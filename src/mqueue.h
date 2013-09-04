
#ifndef SRC_MQUEUE_H_
#define SRC_MQUEUE_H_ 1

#include "config.h"

#include <list>
#include <string>
#include <map>

#include "locks.h"
#include "mitem.h"
#include "mutex.h"

extern void LOG(int severity, const char *fmt, ...);

class MutationManager;
class Snapshot;

typedef std::list<mutation_item>::iterator mitem_itr;
typedef std::list<Snapshot*>::iterator mlist_itr;
typedef unordered_map<std::string, mitem_itr> snapshot_index;

typedef enum {
    HAS_ITEMS,
    AWAITING_ITEMS,
    UNREGISTERED
} cursor_error_t;

class MutationCursor {
public:
    MutationCursor(const std::string &cursorName)
        : name(cursorName), registered(false) {}

    MutationCursor(const std::string &cursorName, mitem_itr iitr,
                   mlist_itr litr)
        : name(cursorName), curPos(iitr), curList(litr), registered(false) {}

    ~MutationCursor() {}

    const std::string& getName() { return name; }

    cursor_error_t hasNext();

    mutation_item next();

    bool isRegistered() {
        return registered;
    }

    void setRegistered(bool r) {
        registered = r;
    }

private:
friend class MutationManager;

    void setPosition(mitem_itr position, mlist_itr list, MutationManager* mgr) {
        curPos = position;
        curList = list;
        manager = mgr;
    }

    std::string name;
    mitem_itr   curPos;
    mlist_itr   curList;
    MutationManager* manager;
    bool registered;
};

class Snapshot {
public:

    Snapshot();
    ~Snapshot() {}

    /**
     * Inserts an operation into this snapshot
     *
     * @param itm The item to insert into the snapshot
     * @returns True if a new item was inserted, false if the item was deduped
     */
    bool insert(const mutation_item &itm);

    size_t getNumItems() {
        return numItems;
    }

    size_t getNumMutations() {
        return numMutations;
    }

    int64_t getLowSeqno() {
        assert(numItems > 0);
        return mutations.front()->getBySeqno();
    }

    int64_t getHighSeqno() {
        assert(numItems > 0);
        return mutations.back()->getBySeqno();
    }

    size_t getNumCursors() {
        return numCursors;
    }

    mitem_itr begin() {
        return mutations.begin();
    }

    mitem_itr end() {
        return mutations.end();
    }

    void registerCursor() {
        numCursors++;
    }

    void deregisterCursor() {
        numCursors--;
    }

    bool isOpenSnapshot() {
        return isOpen;
    }

    void closeSnapshot() {
        isOpen = false;
    }

    /**
     * Gets an iterator to the item in this list that has this bySequence
     * number. If no item has this bySequence number than the item with the next
     * smallest bySequence number is returned. This seqno must be in the range
     * of this list
     * @param The sequence number to get an iterator to
     */
    mitem_itr getIteratorBySeqno(int64_t seqno);

private:
    bool isOpen;
    size_t numItems;
    size_t numMutations;
    size_t numCursors;
    std::list<mutation_item> mutations;
    snapshot_index keyIndex;
};

class MutationManager {
public:
    MutationManager(int64_t lastBySeqno);
    ~MutationManager() {}


    bool insert(std::string& key, value_t& val, enum queue_operation op);

    bool registerCursor(MutationCursor &cursor, int64_t seqno);

    mutation_item nextItemForCursor(mlist_itr* list, mitem_itr* pos);

    size_t getNumItems() {
        return numItems;
    }

private:

    bool snapshot();

    void addNewSnapshot();

    size_t numItems;
    size_t numMutations;
    int64_t bySeqno;
    std::list<Snapshot*> mutationLists;
};

#endif  // SRC_MQUEUE_H_
