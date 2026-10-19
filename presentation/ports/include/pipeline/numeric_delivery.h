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
namespace numeric_delivery_detail {
inline signal_contracts::Status project_history_contract(signal_contracts::DataContract& projected) noexcept {
    using namespace signal_contracts;
    auto* history=std::get_if<NumericContract>(&projected);
    if(!history)return Status::incompatible_contract;
    const auto rank=history->shape.rank;
    if(rank!=Rank::Matrix && rank!=Rank::Vector)return Status::incompatible_contract;
    history->semantic=history->history_element;
    history->history_element=Semantic::Numeric;
    if(rank==Rank::Matrix){
        history->shape={Rank::Vector,{history->shape.axes[1],Axis::None},{history->shape.extents[1],ExtentBounds{}}};
        history->coordinates={history->coordinates[1],AxisCoordinates{}};
    }else{
        history->shape={Rank::Scalar,{Axis::None,Axis::None},{ExtentBounds{},ExtentBounds{}}};
        history->coordinates={AxisCoordinates{},AxisCoordinates{}};
    }
    return Status::ok;
}
inline signal_contracts::Status validate_delivery_contract(const signal_contracts::DataContract& expected,
    const signal_contracts::DataFrameView& input,bool compact_history_row) noexcept {
    using namespace signal_contracts;
    if(const auto* numeric=std::get_if<NumericView>(&input.payload)) {
        if(numeric->ring.capacity)return Status::incompatible_contract;
        if(!numeric->contract)return Status::malformed;
        auto projected=expected;
        if(compact_history_row && project_history_contract(projected)!=Status::ok)return Status::incompatible_contract;
        return same_physical_contract(projected,*numeric->contract)?Status::ok:Status::incompatible_contract;
    }
    if(const auto* packet=std::get_if<StructuredView>(&input.payload))
        if(!packet->contract || !same_contract(expected,*packet->contract))return Status::incompatible_contract;
    return Status::ok;
}
inline bool valid_history_geometry(const signal_contracts::NumericGatherOutput& input,std::uint64_t filled_count,std::uint64_t capacity) noexcept {
    const auto& row=input.row;
    return row.history && filled_count && row.filled_rows>=filled_count && row.filled_rows<=capacity && row.capacity==capacity &&
        row.logical_row>=row.filled_rows-filled_count && row.logical_row<row.filled_rows && row.physical_row<capacity;
}
inline bool valid_history_metadata(const signal_contracts::NumericGatherOutput& input) noexcept {
    const auto& row=input.row;
    return (row.time_coordinate.empty() || row.time_coordinate.size()==8) && row.selected_mask.size()<=1 &&
        (!row.mask_present || row.selected_mask.size()==1);
}
inline void copy_history_metadata(const signal_contracts::NumericGatherOutput& input,DeliveredNumericHistoryRow& out) noexcept {
    out.logical_row=input.row.logical_row;
    out.physical_row=input.row.physical_row;
    out.time_axis=input.row.time_axis;
    out.mask_present=input.row.mask_present;
    out.silenced=input.row.silenced;
    if(out.mask_present)out.silence_mask=input.row.selected_mask[0];
    out.has_time_coordinate=!input.row.time_coordinate.empty();
    if(out.has_time_coordinate)std::copy(input.row.time_coordinate.begin(),input.row.time_coordinate.end(),out.time_coordinate.begin());
}
inline bool valid_history_contract(const NumericDeliveryContext& context,std::uint64_t filled_count,std::uint64_t capacity) noexcept {
    const auto contract=context.contract.view();
    const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
    return history && (history->shape.rank==signal_contracts::Rank::Matrix ||
        history->shape.rank==signal_contracts::Rank::Vector) &&
        capacity==history->shape.extents[0].maximum && filled_count<=capacity;
}
inline DeliveredNumericHistoryRow history_event(std::shared_ptr<const NumericDeliveryContext> context,DeliveryStamp delivery,
    NumericHistoryEvent event,std::uint64_t filled_count,std::uint64_t capacity,std::uint64_t ordinal) noexcept {
    DeliveredNumericHistoryRow candidate;
    candidate.event=event;
    candidate.filled_count=filled_count;
    candidate.capacity=capacity;
    candidate.accepted_row_ordinal=ordinal;
    candidate.compact.context=std::move(context);
    candidate.compact.delivery=delivery;
    return candidate;
}
}
inline signal_contracts::Status retain_numeric_delivery(
    std::shared_ptr<const NumericDeliveryContext> context,DeliveryStamp delivery,
    const signal_contracts::DataFrameView& input,signal_contracts::Status status,
    const signal_contracts::FrameLimits& limits,signal_contracts::BufferPool& payload,
    signal_contracts::BufferPool& metadata,DeliveredNumericFrame& out,bool compact_history_row=false) noexcept {
    using namespace signal_contracts;
    if(!context || !context->contract)return Status::invalid_argument;
    const auto contract_status=numeric_delivery_detail::validate_delivery_contract(context->contract.view(),input,compact_history_row);
    if(contract_status!=Status::ok)return contract_status;
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
    if(!numeric_delivery_detail::valid_history_contract(*context,filled_count,capacity))return Status::incompatible_contract;
    auto candidate=numeric_delivery_detail::history_event(context,delivery,event,filled_count,capacity,ordinal);
    if(event==NumericHistoryEvent::Row) {
        if(!numeric_delivery_detail::valid_history_geometry(input,filled_count,capacity) || !numeric_delivery_detail::valid_history_metadata(input))return Status::malformed;
        auto status=retain_numeric_delivery(std::move(context),delivery,input.frame,Status::ok,limits,payload,metadata,candidate.compact,true);
        if(status!=Status::ok)return status;
        numeric_delivery_detail::copy_history_metadata(input,candidate);
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
