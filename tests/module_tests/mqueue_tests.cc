
#include "config.h"

#include <string>

#include "mqueue.h"

static void checkNextMeta(MutationCursor& cursor, cursor_error_t exp_err,
                          queue_operation exp_op, uint64_t exp_byseqno) {
    //printf("->%d\n", cursor.hasNext());
    assert(cursor.hasNext() == exp_err);
    mutation_item item = cursor.next();
    assert(item->getOperation() == exp_op);
    assert(item->getBySeqno() == exp_byseqno);
}

static void checkNextMutation(MutationCursor& cursor, cursor_error_t exp_err,
                              queue_operation exp_op, uint64_t exp_byseqno,
                              const char* value) {
    //printf("%d\n", cursor.hasNext());
    assert(cursor.hasNext() == exp_err);
    mutation_item item = cursor.next();
    assert(item->getOperation() == exp_op);
    assert(item->getBySeqno() == exp_byseqno);
    //printf("%s,%s,%ld\n", item->getValue()->to_s().c_str(), value->to_s().c_str(),
    //              item->getValue()->length());
    assert(memcmp(item->getValue()->to_s().c_str(), value, strlen(value)) == 0);
}

static void insertMutataion(MutationManager *manager, const char* key,
                            const char* value, queue_operation op,
                            bool newkey = true) {
    std::string mykey(key);
    value_t myval(Blob::New((const char*)value, strlen(value)));
    assert(manager->insert(mykey, myval, op) == newkey);
}

void test_create() {
    MutationManager manager(0);
    std::string key("key");
    value_t val(Blob::New((const char*)"val", 3));
    manager.insert(key, val, queue_op_set);
    assert(manager.getNumItems() == 3);
}

/**
 * Tests that we can insert a cursor to take a snapshot and iterate all items
 */
void test_basic_snapshot_and_iterate() {
    MutationManager manager(0);
    assert(manager.getNumItems() == 2);

    MutationCursor cursor("mycursor");
    manager.registerCursor(cursor, 1);
    assert(cursor.hasNext() == AWAITING_ITEMS);
    assert(cursor.hasNext() == AWAITING_ITEMS);
    assert(cursor.hasNext() == AWAITING_ITEMS);

    insertMutataion(&manager, "key1", "val1", queue_op_set);

    checkNextMeta(cursor, HAS_ITEMS, queue_op_start, 2);

    insertMutataion(&manager, "key2", "val2", queue_op_set);
    insertMutataion(&manager, "key3", "val3", queue_op_set);

    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 3, "val1");
    checkNextMeta(cursor, HAS_ITEMS, queue_op_end, 4);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_empty, 5);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_start, 6);
    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 7, "val2");
    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 8, "val3");
    checkNextMeta(cursor, HAS_ITEMS, queue_op_end, 9);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_empty, 10);

    assert(cursor.hasNext() == AWAITING_ITEMS);
}

/**
 * Tests that items are de-duplicated in the open checkpoint
 *
 * Tests that multiple items inserted consecutively with the same key into the
 * MutationManagers open checkpoint are deduplicated properly.
 */
void test_deduplication_in_open_checkpoint_1() {
    MutationManager manager(0);
    MutationCursor cursor("mycursor");

    insertMutataion(&manager, "key1", "val1", queue_op_set);
    insertMutataion(&manager, "key1", "val2", queue_op_set, false);
    insertMutataion(&manager, "key1", "val3", queue_op_set, false);

    manager.registerCursor(cursor, 1);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_start, 2);
    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 5, "val3");
    checkNextMeta(cursor, HAS_ITEMS, queue_op_end, 6);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_empty, 7);
    assert(cursor.hasNext() == AWAITING_ITEMS);
}

/**
 * Tests that items are de-duplicated in the open checkpoint
 *
 * Tests that multiple items inserted with the same key non-consecutively into
 * the MutationManagers open checkpoint are deduplicated properly.
 */
void test_deduplication_in_open_checkpoint_2() {
    MutationManager manager(0);
    MutationCursor cursor("mycursor");

    insertMutataion(&manager, "key1", "val1", queue_op_set);
    insertMutataion(&manager, "key2", "val2", queue_op_set);
    insertMutataion(&manager, "key3", "val3", queue_op_set);
    insertMutataion(&manager, "key1", "val4", queue_op_set, false);

    manager.registerCursor(cursor, 1);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_start, 2);
    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 4, "val2");
    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 5, "val3");
    checkNextMutation(cursor, HAS_ITEMS, queue_op_set, 6, "val4");
    checkNextMeta(cursor, HAS_ITEMS, queue_op_end, 7);
    checkNextMeta(cursor, HAS_ITEMS, queue_op_empty, 8);
    assert(cursor.hasNext() == AWAITING_ITEMS);
}

/**
 * Test registering a cursor with too large of a sequence number
 *
 * When an attempt is made to register a cursor with a sequence number that is
 * too big the registration should fail and the trying to read from the cursor
 * should return an unregistered error.
 */
void test_register_cursor_too_big() {
    MutationManager manager(0);
    MutationCursor cursor("mycursor");

    insertMutataion(&manager, "key1", "val1", queue_op_set);
    insertMutataion(&manager, "key2", "val2", queue_op_set);
    insertMutataion(&manager, "key3", "val3", queue_op_set);

    assert(!manager.registerCursor(cursor, 10));
    assert(cursor.hasNext() == UNREGISTERED);
}

int main() {
    putenv((char*)"ALLOW_NO_STATS_UPDATE=1");

    test_create();
    test_basic_snapshot_and_iterate();
    test_deduplication_in_open_checkpoint_1();
    test_deduplication_in_open_checkpoint_2();
    test_register_cursor_too_big();
    return 0;
}