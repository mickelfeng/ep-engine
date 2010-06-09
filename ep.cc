/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "ep.hh"
#include "flusher.hh"
#include "locks.hh"
#include "dispatcher.hh"

#include <vector>
#include <time.h>
#include <string.h>
#include <iostream>

extern "C" {
    static rel_time_t uninitialized_current_time(void) {
        abort();
        return 0;
    }

    rel_time_t (*ep_current_time)() = uninitialized_current_time;
}

class GetCallback : public DispatcherCallback {
public:
    GetCallback(KVStore *kvs, const std::string &k,
                uint16_t vbid, shared_ptr<Callback<GetValue> > cb) :
        store(kvs), key(k), vbucket(vbid), _callback(cb) {}

    virtual bool callback(Dispatcher &d, TaskId t) {
        (void)d; (void)t;
        store->get(key, vbucket, *_callback);
        return false;
    }

private:
    KVStore *store;
    std::string key;
    uint16_t vbucket;
    shared_ptr<Callback<GetValue> > _callback;
};

EventuallyPersistentStore::EventuallyPersistentStore(KVStore *t,
                                                     bool startVb0) :
    loadStorageKVPairCallback(vbuckets, stats)
{
    stats.min_data_age.set(DEFAULT_MIN_DATA_AGE);
    stats.queue_age_cap.set(DEFAULT_MIN_DATA_AGE_CAP);

    doPersistence = getenv("EP_NO_PERSISTENCE") == NULL;
    dispatcher = new Dispatcher();
    flusher = new Flusher(this, dispatcher);

    setTxnSize(DEFAULT_TXN_SIZE);

    underlying = t;

    if (startVb0) {
        RCPtr<VBucket> vb(new VBucket(0, active));
        vbuckets.addBucket(vb);
    }

    startDispatcher();
    startFlusher();
    assert(underlying);
}

class VerifyStoredVisitor : public HashTableVisitor {
public:
    std::vector<std::string> dirty;
    virtual void visit(StoredValue *v) {
        if (v->isDirty()) {
            dirty.push_back(v->getKey());
        }
    }
};

EventuallyPersistentStore::~EventuallyPersistentStore() {
    stopFlusher();
    dispatcher->stop();

    // Verify that we don't have any dirty objects!
    if (getenv("EP_VERIFY_SHUTDOWN_FLUSH") != NULL) {
        VerifyStoredVisitor walker;
        // TODO: Something smarter for multiple vbuckets.
        RCPtr<VBucket> vb = vbuckets.getBucket(0);
        assert(vb);
        vb->ht.visit(walker);
        if (!walker.dirty.empty()) {
            std::vector<std::string>::const_iterator iter;
            for (iter = walker.dirty.begin();
                 iter != walker.dirty.end();
                 ++iter) {
                std::cerr << "ERROR: Object dirty after flushing: "
                          << iter->c_str() << std::endl;
            }

            throw std::runtime_error("Internal error, objects dirty objects exists");
        }
    }

    delete flusher;
    delete dispatcher;
}

void EventuallyPersistentStore::startDispatcher() {
    dispatcher->start();
}


const Flusher* EventuallyPersistentStore::getFlusher() {
    return flusher;
}

void EventuallyPersistentStore::startFlusher() {
    flusher->start();
}

void EventuallyPersistentStore::stopFlusher() {
    bool rv = flusher->stop();
    if (rv) {
        flusher->wait();
    }
}

bool EventuallyPersistentStore::pauseFlusher() {
    flusher->pause();
    return true;
}

bool EventuallyPersistentStore::resumeFlusher() {
    flusher->resume();
    return true;
}

void EventuallyPersistentStore::set(const Item &item,
                                    Callback<std::pair<bool, int64_t> > &cb) {
    RCPtr<VBucket> vb = vbuckets.getBucket(item.getVBucketId());
    if (!vb) {
        cb.setStatus(static_cast<int>(INVALID_VBUCKET));
        std::pair<bool, int64_t> p(false, 0);
        cb.callback(p);
        return;
    }

    mutation_type_t mtype = vb->ht.set(item);
    bool rv = true;

    if (mtype == INVALID_CAS || mtype == IS_LOCKED) {
        rv = false;
    } else if (mtype == WAS_CLEAN || mtype == NOT_FOUND) {
        queueDirty(item.getKey(), item.getVBucketId());
        if (mtype == NOT_FOUND) {
            stats.curr_items++;
        }
    }

    cb.setStatus((int)mtype);
    std::pair<bool, int64_t> p(rv, 0);
    cb.callback(p);
}

RCPtr<VBucket> EventuallyPersistentStore::getVBucket(uint16_t vbucket) {
    return vbuckets.getBucket(vbucket);
}

void EventuallyPersistentStore::setVBucketState(uint16_t vbid,
                                                vbucket_state_t to) {
    // Lock to prevent a race condition between a failed update and add.
    LockHolder lh(vbsetMutex);
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (vb) {
        vb->setState(to);
    } else {
        RCPtr<VBucket> newvb(new VBucket(vbid, to));
        vbuckets.addBucket(newvb);
    }
}

void EventuallyPersistentStore::get(const std::string &key,
                                    uint16_t vbucket,
                                    Callback<GetValue> &cb) {
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        GetValue rv(ENGINE_NOT_MY_VBUCKET);
        cb.callback(rv);
        return;
    }

    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = vb->ht.unlocked_find(key, bucket_num);

    if (v) {
        // return an invalid cas value if the item is locked
        GetValue rv(new Item(v->getKey(), v->getFlags(), v->getExptime(),
                             v->getValue(),
                             v->isLocked(ep_current_time()) ? -1 : v->getCas()));
        lh.unlock();
        cb.callback(rv);
    } else {
        GetValue rv;
        lh.unlock();
        cb.callback(rv);
    }
}

bool EventuallyPersistentStore::getLocked(const std::string &key,
                                          uint16_t vbucket,
                                          Callback<GetValue> &cb,
                                          rel_time_t currentTime,
                                          uint32_t lockTimeout) {
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    assert(vb);
    if (!vb) {
        GetValue rv(ENGINE_NOT_MY_VBUCKET);
        cb.callback(rv);
        return false;
    }

    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = vb->ht.unlocked_find(key, bucket_num);

    if (v) {
        if (v->isLocked(currentTime)) {
            GetValue rv;
            cb.callback(rv);
            lh.unlock();
            return false;
        }

        // acquire lock and increment cas value

        v->lock(currentTime + lockTimeout);

        Item *it = new Item(v->getKey(), v->getFlags(), v->getExptime(),
                v->getValue(), v->getCas());

         it->setCas();
         v->setCas(it->getCas());

        GetValue rv(it);
        cb.callback(rv);

    } else {
        GetValue rv;
        cb.callback(rv);
    }
    lh.unlock();
    return true;
}

void EventuallyPersistentStore::getFromUnderlying(const std::string &key,
                                                  uint16_t vbucket,
                                                  shared_ptr<Callback<GetValue> > cb) {

    shared_ptr<GetCallback> dcb(new GetCallback(underlying, key, vbucket, cb));
    dispatcher->schedule(dcb, NULL, -1);
}

bool EventuallyPersistentStore::getKeyStats(const std::string &key,
                                            uint16_t vbucket,
                                            struct key_stats &kstats)
{
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    assert(vb);
    if (!vb) {
        return false;
    }

    bool found = false;
    int bucket_num = vb->ht.bucket(key);
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = vb->ht.unlocked_find(key, bucket_num);

    found = (v != NULL);
    if (found) {
        kstats.dirty = v->isDirty();
        kstats.exptime = v->getExptime();
        kstats.flags = v->getFlags();
        kstats.cas = v->getCas();
        kstats.dirtied = v->getDirtied();
        kstats.data_age = v->getDataAge();
    }
    return found;
}

void EventuallyPersistentStore::setMinDataAge(int to) {
    stats.min_data_age.set(to);
}

void EventuallyPersistentStore::setQueueAgeCap(int to) {
    stats.queue_age_cap.set(to);
}

void EventuallyPersistentStore::resetStats(void) {
    stats.tooYoung.set(0);
    stats.tooOld.set(0);
    stats.dirtyAge.set(0);
    stats.dirtyAgeHighWat.set(0);
    stats.flushDuration.set(0);
    stats.flushDurationHighWat.set(0);
    stats.commit_time.set(0);
}

void EventuallyPersistentStore::del(const std::string &key,
                                    uint16_t vbucket,
                                    Callback<bool> &cb) {
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        bool rv(false);
        cb.setStatus(ENGINE_NOT_MY_VBUCKET);
        cb.callback(rv);
        return;
    }

    bool existed = vb->ht.del(key);
    cb.setStatus(existed ? ENGINE_SUCCESS : ENGINE_KEY_ENOENT);

    if (existed) {
        queueDirty(key, vbucket);
        stats.curr_items--;
    }
    cb.callback(existed);
}

void EventuallyPersistentStore::reset() {
    // TODO: Something smarter for multiple vbuckets.
    RCPtr<VBucket> vb = vbuckets.getBucket(0);
    vb->ht.clear();
    queueDirty("", 0);
}

std::queue<QueuedItem>* EventuallyPersistentStore::beginFlush() {
    std::queue<QueuedItem> *rv(NULL);
    if (towrite.empty() && writing.empty()) {
        stats.dirtyAge = 0;
    } else {
        assert(underlying);
        towrite.getAll(writing);
        stats.flusher_todo.set(writing.size());
        stats.queue_size.set(towrite.size());
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Flushing %d items with %d still in queue\n",
                         writing.size(), towrite.size());
        rv = &writing;
    }
    return rv;
}

void EventuallyPersistentStore::completeFlush(std::queue<QueuedItem> *rej,
                                              rel_time_t flush_start) {
    // Requeue the rejects.
    stats.queue_size += rej->size();
    while (!rej->empty()) {
        writing.push(rej->front());
        rej->pop();
    }

    stats.queue_size.set(towrite.size() + writing.size());
    rel_time_t complete_time = ep_current_time();
    stats.flushDuration.set(complete_time - flush_start);
    stats.flushDurationHighWat.set(std::max(stats.flushDuration.get(),
                                            stats.flushDurationHighWat.get()));
}

int EventuallyPersistentStore::flushSome(std::queue<QueuedItem> *q,
                                         std::queue<QueuedItem> *rejectQueue) {
    int tsz = getTxnSize();
    underlying->begin();
    int oldest = stats.min_data_age;
    for (int i = 0; i < tsz && !q->empty(); i++) {
        int n = flushOne(q, rejectQueue);
        if (n != 0 && n < oldest) {
            oldest = n;
        }
    }
    rel_time_t cstart = ep_current_time();
    while (!underlying->commit()) {
        sleep(1);
        stats.commitFailed++;
    }
    rel_time_t complete_time = ep_current_time();

    stats.commit_time.set(complete_time - cstart);

    return oldest;
}


// This class exists to create a closure around a few variables within
// EventuallyPersistentStore::flushOne so that an object can be
// requeued in case of failure to store in the underlying layer.

class Requeuer : public Callback<std::pair<bool, int64_t> >,
                 public Callback<bool> {
public:

    Requeuer(const QueuedItem &qi, std::queue<QueuedItem> *q,
             StoredValue *v, rel_time_t qd, rel_time_t d, struct EPStats *s) :
        queuedItem(qi), rq(q), sval(v), queued(qd), dirtied(d), stats(s) {
        assert(rq);
        assert(s);
    }

    void callback(std::pair<bool, int64_t> &value) {
        if (value.first && sval != NULL && value.second > 0) {
            sval->setId(value.second);
        } else if (!value.first) {
            stats->flushFailed++;
            if (sval != NULL) {
                sval->reDirty(queued, dirtied);
            }
            rq->push(queuedItem);
        }
    }

    void callback(bool &value) {
        if (!value) {
            stats->flushFailed++;
            if (sval != NULL) {
                sval->reDirty(queued, dirtied);
            }
            rq->push(queuedItem);
        }
    }

private:
    const QueuedItem queuedItem;
    std::queue<QueuedItem> *rq;
    StoredValue *sval;
    rel_time_t queued;
    rel_time_t dirtied;
    struct EPStats *stats;
    DISALLOW_COPY_AND_ASSIGN(Requeuer);
};

int EventuallyPersistentStore::flushOne(std::queue<QueuedItem> *q,
                                        std::queue<QueuedItem> *rejectQueue) {

    QueuedItem qi = q->front();
    q->pop();

    // Special case hack:  Flush
    if (qi.getKey().size() == 0) {
        underlying->reset();
        stats.flusher_todo--;
        return 1;
    }

    RCPtr<VBucket> vb = vbuckets.getBucket(qi.getVBucketId());
    assert(vb);
    if (!vb) {
        stats.flusher_todo--;
        return 0;
    }

    int bucket_num = vb->ht.bucket(qi.getKey());
    LockHolder lh(vb->ht.getMutex(bucket_num));
    StoredValue *v = vb->ht.unlocked_find(qi.getKey(), bucket_num);

    bool found = v != NULL;
    bool isDirty = (found && v->isDirty());
    Item *val = NULL;
    rel_time_t queued(0), dirtied(0);

    int ret = 0;

    if (isDirty) {
        v->markClean(&queued, &dirtied);
        assert(dirtied > 0);
        // Calculate stats if this had a positive time.
        rel_time_t now = ep_current_time();
        int dataAge = now - dirtied;
        int dirtyAge = now - queued;
        bool eligible = true;

        if (dirtyAge > stats.queue_age_cap.get()) {
            stats.tooOld++;
        } else if (dataAge < stats.min_data_age.get()) {
            eligible = false;
            // Skip this one.  It's too young.
            ret = stats.min_data_age.get() - dataAge;
            isDirty = false;
            stats.tooYoung++;
            v->reDirty(queued, dirtied);
            rejectQueue->push(qi);
        }

        if (eligible) {
            assert(dirtyAge < (86400 * 30));
            assert(dataAge <= dirtyAge);
            stats.dirtyAge.set(dirtyAge);
            stats.dataAge.set(dataAge);
            stats.dirtyAgeHighWat.set(std::max(stats.dirtyAge.get(),
                                               stats.dirtyAgeHighWat.get()));
            stats.dataAgeHighWat.set(std::max(stats.dataAge.get(),
                                              stats.dataAgeHighWat.get()));
            // Copy it for the duration.
            val = new Item(qi.getKey(), v->getFlags(), v->getExptime(),
                           v->getValue(), v->getCas(), v->getId(),
                           qi.getVBucketId());

            // Consider this persisted as it is our intention, though
            // it may fail and be requeued later.
            stats.totalPersisted++;
        }
    }
    stats.flusher_todo--;
    lh.unlock();

    if (found && isDirty) {
        Requeuer cb(qi, rejectQueue, v, queued, dirtied, &stats);
        underlying->set(*val, cb);
    } else if (!found) {
        Requeuer cb(qi, rejectQueue, v, queued, dirtied, &stats);
        underlying->del(qi.getKey(), qi.getVBucketId(), cb);
    }

    if (val != NULL) {
        delete val;
    }

    return ret;
}

void EventuallyPersistentStore::queueDirty(const std::string &key, uint16_t vbid) {
    if (doPersistence) {
        // Assume locked.
        towrite.push(QueuedItem(key, vbid));
        stats.totalEnqueued++;
        stats.queue_size = towrite.size();
    }
}
