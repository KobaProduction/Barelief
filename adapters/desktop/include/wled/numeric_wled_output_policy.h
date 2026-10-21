#pragma once
#include <signal_contracts/data_frame.h>
#include <wled_protocol/wled_packet.h>
#include <cstdint>
#include <cstring>

namespace desktop {
// Adapter policy only: one latest owned packet, one private send counter and an
// externally supplied monotonic clock. No DSP, Qt, socket, queue or allocation.
class NumericWledOutputPolicy final {
public:
    static constexpr std::uint32_t default_cadence_ms=17,stale_threshold_ms=300;
    explicit NumericWledOutputPolicy(std::uint32_t cadence_ms=default_cadence_ms) noexcept
        :cadence_ms_(cadence_ms?cadence_ms:default_cadence_ms) {}

    signal_contracts::Status set_cadence(std::uint32_t value) noexcept {
        if(!value)return signal_contracts::Status::invalid_argument;
        cadence_ms_=value;return signal_contracts::Status::ok;
    }
    void start(std::uint64_t now_ms) noexcept {running_=true;last_attempt_ms_=now_ms;}
    void stop() noexcept {running_=false;}
    void unavailable() noexcept {has_packet_=false;}
    bool running() const noexcept {return running_;}
    std::uint32_t cadence_ms() const noexcept {return cadence_ms_;}
    std::uint8_t next_counter() const noexcept {return counter_;}

    // Invalid input neither replaces a valid packet nor renews its freshness.
    // Known unavailable clears it immediately, before the next timer cadence.
    signal_contracts::Status update(const signal_contracts::DataFrameView& input,std::uint64_t now_ms) noexcept {
        using namespace signal_contracts;
        const auto* packet=std::get_if<StructuredView>(&input.payload);
        if(!valid_contract(packet))return Status::incompatible_contract;
        if(!input.stamp.available){unavailable();return Status::unavailable;}
        const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(packet->bytes.data()),packet->bytes.size());
        if(wled_protocol::validate_packet(bytes,{0,wled_protocol::packet_size})!=wled_protocol::Status::Ok)
            return Status::invalid_argument;
        std::memcpy(latest_.data(),bytes.data(),latest_.size());last_update_ms_=now_ms;has_packet_=true;return Status::ok;
    }

    // Call once per destination write attempt, then note_send_result(). Late
    // timers produce one packet rather than a catch-up burst. Exactly300ms is
    // fresh; >300ms or clock rollback chooses a valid silence packet.
    signal_contracts::Status take_due_packet(std::uint64_t now_ms,wled_protocol::WledPacket& output) noexcept {
        using signal_contracts::Status;
        if(!running_)return Status::unavailable;
        if(reset_rolled_back_clock(now_ms))return Status::would_block;
        if(now_ms-last_attempt_ms_<cadence_ms_)return Status::would_block;
        wled_protocol::WledPacket candidate{};
        if(select_packet(now_ms,candidate)!=Status::ok)return Status::invalid_argument;
        if(wled_protocol::write_counter(candidate,counter_,{0,wled_protocol::packet_size})!=wled_protocol::Status::Ok)
            return Status::invalid_argument;
        commit_attempt(now_ms,candidate,output);return Status::ok;
    }
    void note_send_result(bool complete_write) noexcept {if(!complete_write)running_=false;}
private:
    void commit_attempt(std::uint64_t now_ms,const wled_protocol::WledPacket& candidate,wled_protocol::WledPacket& output) noexcept {
        output=candidate;
        counter_=static_cast<std::uint8_t>(unsigned(counter_)+1);
        last_attempt_ms_=now_ms;
    }
    bool reset_rolled_back_clock(std::uint64_t now_ms) noexcept {
        if(now_ms>=last_attempt_ms_)return false;
        has_packet_=false;last_attempt_ms_=now_ms;return true;
    }
    static bool valid_contract(const signal_contracts::StructuredView* packet) noexcept {
        return packet&&packet->contract&&packet->contract->schema=="wled-audio-sync"&&packet->contract->version==1&&
            packet->contract->encoding=="audio-sync-44-le"&&packet->contract->payload_bytes==signal_contracts::ExtentBounds{44,44};
    }
    signal_contracts::Status select_packet(std::uint64_t now_ms,wled_protocol::WledPacket& candidate) noexcept {
        using signal_contracts::Status;
        if(has_packet_&&now_ms>=last_update_ms_&&now_ms-last_update_ms_<=stale_threshold_ms){
            candidate=latest_;
            return Status::ok;
        }
        has_packet_=false;
        if(wled_protocol::silence(0,candidate,{0,wled_protocol::packet_size}).status!=wled_protocol::Status::Ok)
            return Status::invalid_argument;
        return Status::ok;
    }
    wled_protocol::WledPacket latest_{};
    std::uint64_t last_update_ms_=0,last_attempt_ms_=0;
    std::uint32_t cadence_ms_;
    std::uint8_t counter_=0;
    bool running_=false,has_packet_=false;
};
}
