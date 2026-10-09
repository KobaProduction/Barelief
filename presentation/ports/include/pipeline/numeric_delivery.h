#pragma once
#include <pipeline/owned_frame.h>
#include <domain/processing_contracts.h>
#include <signal_contracts/data_frame.h>
#include <signal_contracts/numeric_frame_gather.h>
#include <memory>
#include <array>
#include <optional>
#include <algorithm>
namespace pipeline {
// Created on the control plane; immutable and shared by every bounded delivery.
struct NumericDeliveryContext {
    EndpointIdentity endpoint;
    domain::OwnedProcessingContract contract;
    // Adapter-created arena owns BufferPools, their slots and backing storage.
    // Contexts retained by GUI deliveries keep that arena alive after endpoint
    // destruction. Populate on the control plane, before publishing the context.
    std::shared_ptr<const void> retention_owner;
};
struct DeliveredNumericFrame {
    // Declaration order releases frame handles before their arena context.
    std::shared_ptr<const NumericDeliveryContext> context;
    DeliveryStamp delivery;
    signal_contracts::OwnedDataFrame frame;
    signal_contracts::Status status=signal_contracts::Status::unavailable;
    DeliveredNumericFrame()=default;
    DeliveredNumericFrame(std::shared_ptr<const NumericDeliveryContext> c,DeliveryStamp d,
        signal_contracts::OwnedDataFrame f,signal_contracts::Status s) noexcept
        :context(std::move(c)),delivery(d),frame(std::move(f)),status(s){}
    DeliveredNumericFrame(const DeliveredNumericFrame&)=default;
    DeliveredNumericFrame(DeliveredNumericFrame&&) noexcept=default;
    DeliveredNumericFrame& operator=(const DeliveredNumericFrame& other) noexcept {
        if(this==&other)return *this;
        // Assignment runs in declaration order too. Pin the previous arena until
        // replacing its last frame handle; destruction order alone is insufficient.
        auto previous_context=context;
        context=other.context;delivery=other.delivery;frame=other.frame;status=other.status;
        return *this;
    }
    DeliveredNumericFrame& operator=(DeliveredNumericFrame&& other) noexcept {
        if(this==&other)return *this;
        auto previous_context=context;
        context=std::move(other.context);delivery=other.delivery;frame=std::move(other.frame);status=other.status;
        return *this;
    }
};
enum class NumericHistoryEvent { Row, Empty, Reset, Unavailable };
struct DeliveredNumericHistoryRow {
    DeliveredNumericFrame compact;
    NumericHistoryEvent event=NumericHistoryEvent::Unavailable;
    std::uint64_t filled_count=0,capacity=0,accepted_row_ordinal=0;
    std::size_t logical_row=0,physical_row=0;
    signal_contracts::AxisCoordinates time_axis;
    std::array<std::byte,8> time_coordinate{};
    bool has_time_coordinate=false,mask_present=false,silenced=false;
    std::byte silence_mask{};
};
// Ring-backed full snapshots use the explicit cold retain_frame boundary.
// A compact History row must opt in; ordinary matrices remain ordinary frames.
inline signal_contracts::Status retain_numeric_delivery(
    std::shared_ptr<const NumericDeliveryContext> context,DeliveryStamp delivery,
    const signal_contracts::DataFrameView& input,signal_contracts::Status status,
    const signal_contracts::FrameLimits& limits,signal_contracts::BufferPool& payload,
    signal_contracts::BufferPool& metadata,DeliveredNumericFrame& out,bool compact_history_row=false) noexcept {
    using namespace signal_contracts;
    if(!context || !context->contract)return Status::invalid_argument;
    if(const auto* numeric=std::get_if<NumericView>(&input.payload))
        if(numeric->ring.capacity)return Status::incompatible_contract;
    const auto expected=context->contract.view();
    if(const auto* numeric=std::get_if<NumericView>(&input.payload)) {
        if(!numeric->contract)return Status::malformed;
        auto projected=expected;
        if(compact_history_row) {
            auto* history=std::get_if<NumericContract>(&projected);
            if(!history || history->shape.rank!=Rank::Matrix)return Status::incompatible_contract;
            history->semantic=history->history_element;history->history_element=Semantic::Numeric;
            history->shape={Rank::Vector,{history->shape.axes[1],Axis::None},{history->shape.extents[1],ExtentBounds{}}};
            history->coordinates={history->coordinates[1],AxisCoordinates{}};
        }
        if(!same_physical_contract(projected,*numeric->contract))return Status::incompatible_contract;
    } else if(const auto* packet=std::get_if<StructuredView>(&input.payload)) {
        if(!packet->contract || !same_contract(expected,*packet->contract))return Status::incompatible_contract;
    }
    DeliveredNumericFrame candidate{std::move(context),delivery,{},status};
    auto result=retain_frame(input,limits,payload,metadata,candidate.frame);
    if(result!=Status::ok)return result;
    out=std::move(candidate);return Status::ok;
}
inline signal_contracts::Status retain_numeric_history_row(
    std::shared_ptr<const NumericDeliveryContext> context,DeliveryStamp delivery,NumericHistoryEvent event,
    const signal_contracts::NumericGatherOutput& input,std::uint64_t filled_count,std::uint64_t capacity,
    std::uint64_t ordinal,const signal_contracts::FrameLimits& limits,signal_contracts::BufferPool& payload,
    signal_contracts::BufferPool& metadata,DeliveredNumericHistoryRow& out) noexcept {
    using namespace signal_contracts;
    if(!context || !context->contract)return Status::invalid_argument;
    const auto contract=context->contract.view();const auto* history=std::get_if<NumericContract>(&contract);
    if(!history || history->shape.rank!=Rank::Matrix ||
       capacity!=history->shape.extents[0].maximum || filled_count>capacity)return Status::incompatible_contract;
    DeliveredNumericHistoryRow candidate;
    candidate.event=event;candidate.filled_count=filled_count;candidate.capacity=capacity;candidate.accepted_row_ordinal=ordinal;
    candidate.compact.context=context;candidate.compact.delivery=delivery;
    if(event==NumericHistoryEvent::Row) {
        if(!input.row.history || !filled_count || input.row.filled_rows<filled_count || input.row.filled_rows>capacity || input.row.capacity!=capacity ||
           input.row.time_coordinate.size()>8 || (!input.row.time_coordinate.empty() && input.row.time_coordinate.size()!=8) ||
           input.row.selected_mask.size()>1 || (input.row.mask_present && input.row.selected_mask.size()!=1) ||
           input.row.logical_row<input.row.filled_rows-filled_count || input.row.logical_row>=input.row.filled_rows || input.row.physical_row>=capacity)return Status::malformed;
        auto status=retain_numeric_delivery(std::move(context),delivery,input.frame,Status::ok,limits,payload,metadata,candidate.compact,true);
        if(status!=Status::ok)return status;
        candidate.logical_row=input.row.logical_row;candidate.physical_row=input.row.physical_row;candidate.time_axis=input.row.time_axis;
        candidate.mask_present=input.row.mask_present;candidate.silenced=input.row.silenced;
        if(candidate.mask_present)candidate.silence_mask=input.row.selected_mask[0];
        candidate.has_time_coordinate=!input.row.time_coordinate.empty();
        if(candidate.has_time_coordinate)std::copy(input.row.time_coordinate.begin(),input.row.time_coordinate.end(),candidate.time_coordinate.begin());
    } else candidate.compact.status=event==NumericHistoryEvent::Unavailable?Status::unavailable:Status::ok;
    out=std::move(candidate);return Status::ok;
}
// A serialized mailbox, used under the owning endpoint's lock/event loop. It
// contains owners only; capacity is fixed before processing, never a borrowed view.
template<class T,std::size_t Capacity> class NumericDeliveryMailbox {
    static_assert(Capacity>0);
public:
    bool push(T value) noexcept {
        if(size_==Capacity){++dropped_;return false;}
        slots_[(head_+size_)%Capacity]=std::move(value);++size_;return true;
    }
    std::optional<T> pop() noexcept {
        if(!size_)return {};
        auto result=std::move(slots_[head_]);slots_[head_].reset();head_=(head_+1)%Capacity;--size_;return result;
    }
    void clear() noexcept {for(auto& slot:slots_)slot.reset();head_=size_=0;}
    std::size_t size() const noexcept {return size_;}
    std::uint64_t dropped() const noexcept {return dropped_;}
private:
    std::array<std::optional<T>,Capacity> slots_{};
    std::size_t head_=0,size_=0;
    std::uint64_t dropped_=0;
};
}
