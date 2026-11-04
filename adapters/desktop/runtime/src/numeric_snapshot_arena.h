#pragma once
#include <signal_contracts/data_frame.h>
#include <algorithm>
#include <array>
#include <memory>
#include <vector>
#include <stdexcept>

namespace pipeline::numeric_engine_detail {
using namespace signal_contracts;
struct SnapshotStorage {
    std::vector<std::byte> payload,metadata;
    std::array<BufferSlot,1> payload_slots{},metadata_slots{};
    BufferPool payload_pool,metadata_pool;
    SnapshotStorage(std::size_t bytes,std::size_t description)
        :payload(std::max(std::size_t{1},bytes)),metadata(std::max(std::size_t{1},description)),
         payload_pool(payload,payload_slots,payload.size()),metadata_pool(metadata,metadata_slots,metadata.size()){}
    bool available() const noexcept {return !payload_pool.in_use() && !metadata_pool.in_use();}
    bool fits(const RetentionRequirements& size) const noexcept {return payload.size()>=size.payload_bytes && metadata.size()>=size.metadata_bytes;}
};
// Ordinary snapshots reuse a resident pool. Larger histories use exact-sized,
// reusable leases allocated only by the cold snapshot query, never DSP ingress.
struct HistorySnapshotArena {
    static constexpr std::size_t slot_count=8,payload_bytes=4*1024*1024,metadata_bytes=48*1024;
    std::vector<std::byte> payload{slot_count*payload_bytes},metadata{slot_count*metadata_bytes};
    std::array<BufferSlot,slot_count> payload_slots{},metadata_slots{};
    BufferPool payload_pool{payload,payload_slots,payload_bytes};
    BufferPool metadata_pool{metadata,metadata_slots,metadata_bytes};
    std::array<std::unique_ptr<SnapshotStorage>,slot_count> extended;
    Status retain_extended(const DataFrameView& frame,const FrameLimits& limits,const RetentionRequirements& size,OwnedDataFrame& out) {
        for(auto& slot:extended) {
            if(slot && !slot->available())continue;
            if(!slot || !slot->fits(size))slot=std::make_unique<SnapshotStorage>(size.payload_bytes,size.metadata_bytes);
            return retain_frame(frame,limits,slot->payload_pool,slot->metadata_pool,out);
        }
        return Status::exhausted;
    }
    Status retain(const DataFrameView& frame,const FrameLimits& limits,OwnedDataFrame& out) noexcept {
        RetentionRequirements size;const auto status=retention_requirements(frame,limits,size);
        if(status!=Status::ok)return status;
        if(size.payload_bytes<=payload_bytes && size.metadata_bytes<=metadata_bytes)
            return retain_frame(frame,limits,payload_pool,metadata_pool,out);
        try{return retain_extended(frame,limits,size,out);}
        catch(const std::bad_alloc&){return Status::exhausted;}
        catch(const std::length_error&){return Status::overflow;}
    }
    std::size_t extra_bytes() const noexcept {
        std::size_t total=0;
        for(const auto& slot:extended)if(slot) {
            if(slot->payload.size()>SIZE_MAX-total)return SIZE_MAX;total+=slot->payload.size();
            if(slot->metadata.size()>SIZE_MAX-total)return SIZE_MAX;total+=slot->metadata.size();
        }
        return total;
    }
};
}
