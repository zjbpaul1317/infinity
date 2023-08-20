//
// Created by jinhai on 23-8-10.
//


#pragma once

#include <utility>

#include "base_entry.h"
#include "column_data_entry.h"
#include "data_access_state.h"

namespace infinity {

class BufferManager;

enum class DataSegmentStatus {
    kOpen,
    kClosed,
    kFlushing,
};

struct SegmentVersion {
    explicit
    SegmentVersion(SizeT capacity) : created_(capacity), deleted_(capacity), txn_ptr_(capacity) {
    }
    Vector<au64> created_{};
    Vector<au64> deleted_{};
    Vector<aptr> txn_ptr_{};
};

struct SegmentEntry : public BaseEntry {
public:
    explicit
    SegmentEntry(const void* table_entry, TxnContext* txn_context)
        : BaseEntry(EntryType::kSegment, txn_context), table_entry_(table_entry) {}

    RWMutex rw_locker_{};

    const void* table_entry_{};

    SharedPtr<String> base_dir_{};

    SizeT row_capacity_{};

    SizeT current_row_{};

    u64 segment_id_{};

    std::atomic<DataSegmentStatus> status_{DataSegmentStatus::kOpen};

    Vector<SharedPtr<ColumnDataEntry>> columns_;

    UniquePtr<SegmentVersion> segment_version_{};

    u64 start_txn_id_{};
    u64 end_txn_id_{};
public:
    inline SizeT
    AvailableCapacity() const {
        return row_capacity_ - current_row_;
    }

public:
    static SharedPtr<SegmentEntry>
    MakeNewSegmentEntry(const void* table_entry,
                        u64 txn_id,
                        TxnContext* txn_context,
                        u64 segment_id,
                        BufferManager* buffer_mgr,
                        SizeT segment_row = DEFAULT_SEGMENT_ROW);

    static void
    AppendData(SegmentEntry* segment_entry, void* txn_ptr, AppendState* append_state_ptr, void* buffer_mgr);

    static void
    CommitAppend(SegmentEntry* segment_entry, void* txn_ptr, u64 start_pos, u64 row_count);

    static bool
    PrepareFlush(SegmentEntry* segment_entry);

    static UniquePtr<String>
    Flush(SegmentEntry* segment_entry);

    inline static ColumnDataEntry*
    GetColumnDataByID(SegmentEntry* segment_entry, u64 column_id) {
        return segment_entry->columns_[column_id].get();
    }
};

}