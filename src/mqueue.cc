
#include "config.h"

#include "mqueue.h"

#define log_level 3

void LOG(int severity, const char *fmt, ...) {
    char buffer[2048];

        if (log_level >= severity) {
            va_list va;
            va_start(va, fmt);
            vsnprintf(buffer, sizeof(buffer) - 1, fmt, va);
            printf("Level(%d): %s\n", severity, buffer);
            va_end(va);
        }
}

cursor_error_t MutationCursor::hasNext() {
    if (!registered) {
        return UNREGISTERED;
    } else if ((*curList)->isOpenSnapshot()) {
        return AWAITING_ITEMS;
    } else {
        return HAS_ITEMS;
    }
}

mutation_item MutationCursor::next() {
    /*if (++curPos != (*curList)->end()) {
        return *curPos;
    }

    assert(!(*curList)->isOpenSnapshot());
    (*curList)->deregisterCursor();
    ++curList;
    curPos = (*curList)->begin();
    (*curList)->registerCursor();
    if ((*curList)->isOpenSnapshot()) {
        manager->snapshot();
    }
    return *curPos;*/
    return manager->nextItemForCursor(&curList, &curPos);
}

Snapshot::Snapshot()
    : isOpen(true), numItems(0), numMutations(0) {
}

bool Snapshot::insert(const mutation_item &itm) {
    assert(isOpen || numCursors == 0);

    if (itm->getOperation() == queue_op_start ||
        itm->getOperation() == queue_op_empty ||
        itm->getOperation() == queue_op_end) {
        LOG(2, "Inserting ('%s') into open checkpoint",
            itm->getKey().c_str(), isOpen ? "open" : "closed");
        mutations.push_back(itm);
        numItems++;
        return true;
    }

    snapshot_index::iterator itr = keyIndex.find(itm->getKey());
    bool deduplicate = itr != keyIndex.end();
    if (deduplicate) {
        mutations.erase(itr->second);
    } else {
        numMutations++;
        numItems++;
    }
    mutations.push_back(itm);
    keyIndex[itm->getKey()] = --(mutations.end());

    LOG(2, "Inserting ('%s','%s') into %s checkpoint%s",
        itm->getKey().c_str(), itm->getValue()->to_s().c_str(),
        isOpen ? "open" : "closed", deduplicate ? " (deduped)" : "");

    return !deduplicate;
}

mitem_itr Snapshot::getIteratorBySeqno(int64_t seqno) {
    assert (numItems > 0);

    mitem_itr itr = mutations.begin();
    for (; itr != mutations.end(); itr++) {
        if (seqno == (*itr)->getBySeqno()) {
            return itr;
        } else if (seqno < (*itr)->getBySeqno()) {
            return --itr;
        }
    }
    abort(); // We should never get here
}

MutationManager::MutationManager(int64_t lastBySeqno)
    : numItems(0), bySeqno(lastBySeqno), numMutations(0) {
    addNewSnapshot();
}

mutation_item MutationManager::nextItemForCursor(mlist_itr* list, mitem_itr* pos) {
    if (++(*pos) != (**list)->end()) {
        return **pos;
    }

    assert(!(**list)->isOpenSnapshot());

    (**list)->deregisterCursor();
    ++(*list);
    *pos = (**list)->begin();
    (**list)->registerCursor();
    if ((**list)->isOpenSnapshot()) {
        snapshot();
    }
    return **pos;
}

bool MutationManager::insert(std::string& key, value_t& val,
                             enum queue_operation op) {
    mutation_item itm(new MutationItem(key, val, 0, op, ++bySeqno));
    bool rv = mutationLists.back()->insert(itm);
    numItems++;
    if (op == queue_op_set || op == queue_op_del) {
        numMutations++;
    }

    snapshot();
    return rv;
}

bool MutationManager::registerCursor(MutationCursor &cursor, int64_t seqno) {
    assert(!mutationLists.empty());
    if (mutationLists.back()->getLowSeqno() < seqno) {
        LOG(1, "Cursor cannot be registered, sequence number is too high");
        return false;
    }

    std::list<Snapshot*>::iterator litr = mutationLists.begin();
    printf("%ld", (*litr)->getLowSeqno());
    if ((*litr)->getLowSeqno() > seqno) {
        // Backfill is required. Insert this cursor at the seqno of the
        // persistence cursor and schedule backfill.
        LOG(1, "Backfill required, this code path is not finished");
        return true;
    }

    for (; (*litr) != mutationLists.back(); ++litr) {
        if ((*litr)->getLowSeqno() <= seqno &&
            (*litr)->getHighSeqno() >= seqno) {
            mitem_itr iitr = (*litr)->getIteratorBySeqno(seqno);

            cursor.setPosition(iitr, litr, this);
            cursor.setRegistered(true);
            LOG(1, "Registering cursor (%s) into a closed snapshot",
                cursor.getName().c_str());
            return true;
        }
    }

    LOG(1, "Registering cursor ('%s') into the open snapshot",
        cursor.getName().c_str());

    (*litr)->registerCursor();
    mitem_itr iitr = (*litr)->getIteratorBySeqno(seqno);
    cursor.setPosition(iitr, litr, this);
    cursor.setRegistered(true);
    snapshot();
    return true;
}

bool MutationManager::snapshot() {
    if (mutationLists.back()->getNumCursors() > 0 &&
        mutationLists.back()->getNumMutations() > 0) {
        LOG(2, "Snapshotting the current open checkpoint");
        mutation_item end(new MutationItem("end", 0, queue_op_end, ++bySeqno));
        mutationLists.back()->insert(end);
        numItems++;
        mutationLists.back()->closeSnapshot();
        addNewSnapshot();
    }
    return true;
}

void MutationManager::addNewSnapshot() {
    LOG(2, "Creating a new open checkpoint");
    Snapshot* openList = new Snapshot();
    mutation_item dummy(new MutationItem("dummy", 0, queue_op_empty, ++bySeqno));
    mutation_item start(new MutationItem("start", 0, queue_op_start, ++bySeqno));
    openList->insert(dummy);
    openList->insert(start);
    numItems+=2;
    mutationLists.push_back(openList);
}
