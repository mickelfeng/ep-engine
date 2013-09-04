/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef SRC_MITEM_H_
#define SRC_MITEM_H_ 1

#include "config.h"

#include <string>

#include "common.h"
#include "item.h"
#include "stats.h"

enum queue_operation {
    queue_op_set,
    queue_op_del,
    queue_op_empty,
    queue_op_start,
    queue_op_end
};

/**
 * Representation of an item queued for persistence or tap.
 */
class MutationItem : public RCValue {
public:
    MutationItem(const std::string &k, const uint16_t vb,
               enum queue_operation o, const int64_t bySeq,
               const uint64_t revSeq = 1)
        : key(k), bySeqno(bySeq), revSeqno(revSeq), queued(ep_current_time()),
          op(static_cast<uint16_t>(o)), vbucket(vb) {
        value.reset();
    }

    MutationItem(const std::string &k, const value_t v, const uint16_t vb,
                enum queue_operation o, const int64_t bySeq,
               const uint64_t revSeq = 1)
        : key(k), bySeqno(bySeq), revSeqno(revSeq), queued(ep_current_time()),
          op(static_cast<uint16_t>(o)), vbucket(vb), value(v) {
    }

    ~MutationItem() {}

    const std::string &getKey(void) const { return key; }
    uint16_t getVBucketId(void) const { return vbucket; }
    uint32_t getQueuedTime(void) const { return queued; }
    enum queue_operation getOperation(void) const {
        return static_cast<enum queue_operation>(op);
    }

    int64_t getBySeqno() const { return bySeqno; }

    uint64_t getRevSeqno() const { return revSeqno; }

    const value_t &getValue() const {
        return value;
    }

    void setQueuedTime(uint32_t queued_time) {
        queued = queued_time;
    }

    void setOperation(enum queue_operation o) {
        op = static_cast<uint16_t>(o);
    }

    bool operator <(const MutationItem &other) const {
        return getVBucketId() == other.getVBucketId() ?
            getKey() < other.getKey() : getVBucketId() < other.getVBucketId();
    }

    size_t size() {
        return sizeof(MutationItem) + getKey().size();
    }

private:

    std::string key;
    int64_t  bySeqno;
    uint64_t revSeqno;
    uint32_t queued;
    uint16_t op;
    uint16_t vbucket;
    value_t value;

    DISALLOW_COPY_AND_ASSIGN(MutationItem);
};

typedef SingleThreadedRCPtr<MutationItem> mutation_item;

#endif  // SRC_MITEM_H_
