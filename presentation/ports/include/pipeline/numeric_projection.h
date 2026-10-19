#pragma once
#include <signal_contracts/data_frame.h>
#include <signal_contracts/numeric_policy.h>
#include <array>
#include <string>
#include <type_traits>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <span>
#include <pipeline/numeric_delivery.h>
#include <vector>
#include <algorithm>
namespace pipeline {
using signal_contracts::Status;
using signal_contracts::NumericLiteral;
// Renderer conversion is explicit, bounded and never part of DSP compatibility.
struct FloatProjectionPolicy { bool allow_precision_loss=false; std::size_t max_elements=0; };
struct FloatProjectionReport { std::size_t elements=0, inexact_elements=0; };
inline bool float_exact_integer(std::uint64_t magnitude) noexcept {
    const auto bits=std::bit_width(magnitude);
    return bits<=24 || (magnitude & ((std::uint64_t{1}<<(bits-24))-1))==0;
}
template<class T> inline std::uint64_t projection_integer_magnitude(T value) noexcept {
    if constexpr(std::is_signed_v<T>)return value<0?std::uint64_t(-(value+1))+1:std::uint64_t(value);
    else return value;
}
template<class T> inline Status projection_exactness(T value,bool& exact) noexcept {
    if constexpr(std::is_integral_v<T>)exact=float_exact_integer(projection_integer_magnitude(value));
    else {
        if(!std::isfinite(value) || value>std::numeric_limits<float>::max() || value<-std::numeric_limits<float>::max())
            return Status::malformed;
        exact=T(float(value))==value;
    }
    return Status::ok;
}
template<class T> inline Status project_typed_literal(T value,bool allow_loss,float& out,bool& inexact) noexcept {
    bool exact=true;
    const auto status=projection_exactness(value,exact);
    if(status!=Status::ok)return status;
    if(!exact && !allow_loss)return Status::incompatible_contract;
    out=float(value);
    inexact=!exact;
    return Status::ok;
}
inline Status project_literal(const NumericLiteral& literal,bool allow_loss,float& out,bool& inexact) noexcept {
    return std::visit([&](auto value){return project_typed_literal(value,allow_loss,out,inexact);},literal);
}
inline std::string exact_scalar_text(const NumericLiteral& literal) {
    char buffer[128];
    return std::visit([&](auto value)->std::string {
        using T=decltype(value);
        std::to_chars_result result;
        if constexpr(std::is_integral_v<T>)result=std::to_chars(buffer,buffer+sizeof(buffer),value);
        else result=std::to_chars(buffer,buffer+sizeof(buffer),value,std::chars_format::general,std::numeric_limits<T>::max_digits10);
        return result.ec==std::errc{}?std::string(buffer,result.ptr):std::string{};
    },literal);
}
template<class T> inline Status read_literal_as(const signal_contracts::NumericView& view,signal_contracts::ElementIndex index,NumericLiteral& out) noexcept {
    T value{};auto status=signal_contracts::load_element(view,index,value);if(status!=Status::ok)return status;
    if constexpr(std::is_integral_v<T> && std::is_signed_v<T>)out=std::int64_t(value);
    else if constexpr(std::is_integral_v<T>)out=std::uint64_t(value);
    else out=value;
    return Status::ok;
}
inline Status read_signed_literal(const signal_contracts::NumericView& view,signal_contracts::ElementIndex index,NumericLiteral& out) noexcept {
    using signal_contracts::DType;
    switch(view.contract->dtype) {
    case DType::Int8:return read_literal_as<std::int8_t>(view,index,out);
    case DType::Int16:return read_literal_as<std::int16_t>(view,index,out);
    case DType::Int32:return read_literal_as<std::int32_t>(view,index,out);
    case DType::Int64:return read_literal_as<std::int64_t>(view,index,out);
    default:return Status::malformed;
    }
}
inline Status read_unsigned_literal(const signal_contracts::NumericView& view,signal_contracts::ElementIndex index,NumericLiteral& out) noexcept {
    using signal_contracts::DType;
    switch(view.contract->dtype) {
    case DType::UInt8:return read_literal_as<std::uint8_t>(view,index,out);
    case DType::UInt16:return read_literal_as<std::uint16_t>(view,index,out);
    case DType::UInt32:return read_literal_as<std::uint32_t>(view,index,out);
    case DType::UInt64:return read_literal_as<std::uint64_t>(view,index,out);
    default:return Status::malformed;
    }
}
inline Status read_numeric_literal(const signal_contracts::NumericView& view,signal_contracts::ElementIndex index,NumericLiteral& out) noexcept {
    if(!view.contract)return Status::malformed;
    const auto* descriptor=signal_contracts::dtype_descriptor(view.contract->dtype);
    if(!descriptor)return Status::malformed;
    using signal_contracts::DType;
    switch(view.contract->dtype) {
    case DType::Float32:return read_literal_as<float>(view,index,out);
    case DType::Float64:return read_literal_as<double>(view,index,out);
    default:return descriptor->signed_value?read_signed_literal(view,index,out):read_unsigned_literal(view,index,out);
    }
}
template<class Read> inline Status preflight_float_projection(std::size_t count,const Read& read,bool allow_loss,FloatProjectionReport& candidate) noexcept {
    for(std::size_t i=0;i<count;++i) {
        NumericLiteral literal;
        const auto status=read(i,literal);
        if(status!=Status::ok)return status;
        float projected;
        bool loss=false;
        const auto conversion=project_literal(literal,allow_loss,projected,loss);
        if(conversion!=Status::ok)return conversion;
        candidate.inexact_elements+=loss;
    }
    return Status::ok;
}
template<class Read> inline void publish_float_projection(std::size_t count,const Read& read,bool allow_loss,std::span<float> destination) noexcept {
    for(std::size_t i=0;i<count;++i) {
        NumericLiteral literal;
        (void)read(i,literal);
        bool loss=false;
        (void)project_literal(literal,allow_loss,destination[i],loss);
    }
}
template<class Read> inline Status project_float_values(std::size_t count,const Read& read,FloatProjectionPolicy policy,
    std::span<float> destination,FloatProjectionReport& report) noexcept {
    if(count>policy.max_elements || count>destination.size())return Status::buffer_too_small;
    FloatProjectionReport candidate{count,0};
    const auto status=preflight_float_projection(count,read,policy.allow_precision_loss,candidate);
    if(status!=Status::ok)return status;
    publish_float_projection(count,read,policy.allow_precision_loss,destination);
    report=candidate;
    return Status::ok;
}
// One selected plane; old->new logical order. Preflight prevents partial publication.
inline Status project_numeric_float(const signal_contracts::NumericView& view,std::size_t plane,
    FloatProjectionPolicy policy,std::span<float> destination,FloatProjectionReport& report) noexcept {
    if(!view.contract || plane>=view.planes.size())return Status::malformed;
    const auto rows=view.actual[0];
    const auto columns=view.contract->shape.rank==signal_contracts::Rank::Matrix?view.actual[1]:std::size_t{1};
    if(columns && rows>policy.max_elements/columns)return Status::buffer_too_small;
    const auto read=[&](std::size_t i,NumericLiteral& literal){return read_numeric_literal(view,{plane,i/columns,i%columns},literal);};
    return project_float_values(rows*columns,read,policy,destination,report);
}
// Fixed ring retains exact tagged values; first sequence=0 is a valid first frame.
class ScalarPresentation {
public:
    bool accept(const signal_contracts::FrameStamp& stamp, NumericLiteral value) noexcept {
        if(!stamp.available)return false;
        if(seen_ && stamp.epoch<delivery_epoch_)return false;
        // Delivery fences reject stale queued work, but a newer graph fence alone
        // does not reset an exact compatible producer stream.
        const bool same_stream=same_scalar_stream(stamp);
        // Test duplicates before applying a latched discontinuity bit.
        if(same_stream && stamp.sequence<=sequence_)return false;
        // Presentation delivery may skip producer frames. Retain sparse samples;
        // source timestamps preserve the real interval between them.
        if(!same_stream || stamp.discontinuity)clear();
        append_scalar(value,stamp.timestamp_ticks);
        remember_scalar_stamp(stamp);
        return true;
    }
    void clear() noexcept {start_=count_=0;seen_=false;}
    std::size_t size() const noexcept { return count_; }
    const NumericLiteral& at(std::size_t index) const {return values_.at((start_+index)%values_.size());}
    std::string latest_text() const {return count_?exact_scalar_text(at(count_-1)):std::string{};}
    // Source-clock coordinates, oldest to newest; newest is exactly zero.
    // Unknown, repeated or non-monotonic clocks are honestly shown as frames.
    std::vector<double> relative_seconds(const signal_contracts::ClockContract& clock) const {
        if(!count_ || clock.domain.empty() || clock.tick_seconds.numerator<=0 ||
           !clock.tick_seconds.denominator)return {};
        std::vector<double> result(count_);
        const auto newest=times_[(start_+count_-1)%times_.size()];
        const long double tick=static_cast<long double>(clock.tick_seconds.numerator)/clock.tick_seconds.denominator;
        if(!project_relative_times(newest,tick,result))return {};
        return result;
    }

    Status project(FloatProjectionPolicy policy,std::span<float> destination,FloatProjectionReport& report) const noexcept {
        const auto read=[&](std::size_t i,NumericLiteral& literal){literal=at(i);return Status::ok;};
        return project_float_values(count_,read,policy,destination,report);
    }
private:
    bool same_scalar_stream(const signal_contracts::FrameStamp& stamp) const noexcept {
        return seen_ && epoch_==stamp.producer_epoch && generation_==stamp.generation;
    }
    void append_scalar(NumericLiteral value,std::int64_t time) noexcept {
        const auto slot=(start_+count_)%values_.size();
        values_[slot]=value;
        times_[slot]=time;
        if(count_<values_.size())++count_;
        else start_=(start_+1)%values_.size();
    }
    void remember_scalar_stamp(const signal_contracts::FrameStamp& stamp) noexcept {
        seen_=true;
        sequence_=stamp.sequence;
        epoch_=stamp.producer_epoch;
        generation_=stamp.generation;
        delivery_epoch_=stamp.epoch;
    }
    static long double relative_tick_delta(std::int64_t time,std::int64_t newest) noexcept {
        // Subtract ticks before conversion: MSVC long double cannot retain a
        // short delta if near-INT64_MAX absolute timestamps are converted first.
        return time<=newest?-static_cast<long double>(std::uint64_t(newest)-std::uint64_t(time))
            :static_cast<long double>(std::uint64_t(time)-std::uint64_t(newest));
    }
    bool project_relative_times(std::int64_t newest,long double tick,std::span<double> result) const noexcept {
        for(std::size_t i=0;i<count_;++i) {
            const auto time=times_[(start_+i)%times_.size()];
            result[i]=double(relative_tick_delta(time,newest)*tick);
            if(!std::isfinite(result[i]) || (i && result[i]<=result[i-1]))return false;
        }
        return true;
    }
    std::array<NumericLiteral,128> values_{};
    std::array<std::int64_t,128> times_{};
    std::size_t start_=0,count_=0;
    std::uint64_t sequence_=0,epoch_=0,generation_=0,delivery_epoch_=0;
    bool seen_=false;
};
// Float peak includes retained ring + replacement/render snapshot + staged row.
// Row metadata/container bookkeeping are separately bounded by max_rows.
struct HistoryPresentationLimits { std::size_t max_rows=0,max_columns=0,max_float_bytes=0; FloatProjectionPolicy projection; };
struct HistoryAxes {
    bool column_coordinates=false;
    signal_contracts::NumericUnit column_unit=signal_contracts::NumericUnit::Unspecified;
    bool seconds=false;
    std::vector<double> ages,columns; // ages newest->oldest; newest is exactly zero
};
inline double decode_coordinate(std::span<const std::byte> bytes) noexcept {
    if(bytes.size()!=8)return std::numeric_limits<double>::quiet_NaN();
    std::uint64_t bits=0;for(unsigned i=0;i<8;++i)bits|=std::uint64_t(std::to_integer<unsigned>(bytes[i]))<<(8*i);
    return std::bit_cast<double>(bits);
}
// Bounded projection of an explicitly supplied history. Graphs use full snapshots;
// compact-row reconstruction is available only to transport consumers.
class HistoryPresentation {
    struct RowTime {std::array<std::byte,8> coordinate{};bool has_coordinate=false;std::uint64_t ordinal=0;};
    struct SnapshotInput {
        signal_contracts::DataContract contract;
        signal_contracts::DataFrameView frame;
        bool has_payload=false;
        const signal_contracts::NumericContract& history() const {return std::get<signal_contracts::NumericContract>(contract);}
        const signal_contracts::NumericView& numeric() const {return std::get<signal_contracts::NumericView>(frame.payload);}
    };
    struct SnapshotGeometry {std::uint64_t capacity=0,rows=0,columns=0,real_rows=0,padding=0;};
    struct CompactInput {
        signal_contracts::DataContract contract;
        signal_contracts::DataFrameView frame;
        std::size_t columns=0;
        bool changed=false;
        const signal_contracts::NumericContract& history() const {return std::get<signal_contracts::NumericContract>(contract);}
        const signal_contracts::NumericView& numeric() const {return std::get<signal_contracts::NumericView>(frame.payload);}
    };
    struct SnapshotProjection {
        std::vector<float> values;
        std::vector<bool> losses;
        std::vector<RowTime> times;
        std::vector<double> coordinates;
        std::size_t loss_count=0;
    };
public:
    explicit HistoryPresentation(HistoryPresentationLimits limits):limits_(limits){}
    // Explicit cold resynchronization. The full owner is projected atomically,
    // then released; subsequent hot deliveries still contain one compact row.
    Status accept_snapshot(const DeliveredNumericFrame& input,std::uint64_t ordinal,std::uint64_t filled_count,std::size_t plane=0) {
        SnapshotInput snapshot;
        auto status=validate_snapshot_input(input,ordinal,filled_count,plane,snapshot);
        if(status!=Status::ok)return status;
        if(!snapshot.has_payload)return unavailable_snapshot(input,plane);
        SnapshotGeometry geometry;
        status=snapshot_geometry(snapshot,filled_count,plane,geometry);
        if(status!=Status::ok)return status;
        SnapshotProjection projection;
        status=project_snapshot(snapshot,geometry,plane,ordinal,projection);
        if(status!=Status::ok)return status;
        install_snapshot_storage(projection,geometry);
        install_snapshot_delivery(input,snapshot.frame.stamp,plane,ordinal,filled_count,projection.loss_count);
        return available_?Status::ok:Status::unavailable;
    }
    Status accept(const DeliveredNumericHistoryRow& event,std::size_t plane=0) {
        CompactInput input;
        const auto status=validate_compact_input(event,plane,input);
        if(status!=Status::ok)return status;
        switch(event.event) {
        case NumericHistoryEvent::Unavailable:return compact_unavailable(event,input,plane);
        case NumericHistoryEvent::Empty:
        case NumericHistoryEvent::Reset:return compact_reset(event,input,plane);
        default:return accept_compact_row(event,input,plane);
        }
    }
    void clear() noexcept {
        clear_rows();context_.reset();capacity_=columns_=0;fence_seen_=false;authoritative_snapshot_=false;
        ring_.clear();row_losses_.clear();row_times_.clear();staged_row_.clear();explicit_columns_.clear();
    }
    std::size_t size() const noexcept{return count_;}
    std::size_t capacity() const noexcept{return capacity_;}
    std::size_t columns() const noexcept{return columns_;}
    std::uint64_t filled_count() const noexcept{return filled_count_;}
    bool available() const noexcept{return available_;}
    bool precision_loss() const noexcept{return loss_;}
    const DeliveredNumericHistoryRow& latest() const noexcept{return latest_;}
    HistoryAxes render_axes() const {
        HistoryAxes result;
        if(!context_ || !columns_)return result;
        const auto contract=context_->contract.view();
        const auto& history=std::get<signal_contracts::NumericContract>(contract);
        result.columns=index_coordinates(columns_);
        project_column_axis(history.coordinates[1],result);
        project_age_axis(history.coordinates[0],result);
        return result;
    }
    std::vector<std::vector<float>> render_snapshot() const {
        std::vector<std::vector<float>> rows;render_snapshot_into(rows);return rows;
    }
    // Presentation-only copy into reusable owning storage; processing memory
    // remains borrowed only within the synchronous copy.
    void render_snapshot_into(std::vector<std::vector<float>>& rows) const {
        if(authoritative_snapshot_) {
            render_authoritative_snapshot(rows);
            return;
        }
        // The complete represented history exists immediately. Unknown older
        // slots contain projected typed silence (unsigned PCM uses its midpoint);
        // only the newest suffix carries accepted samples.
        render_compact_snapshot(rows);
    }
private:
    void render_authoritative_snapshot(std::vector<std::vector<float>>& rows) const {
        if(ring_.empty()){rows.clear();return;}
        rows.resize(capacity_);
        for(auto& row:rows)row.resize(columns_);
        for(std::size_t row=0;row<capacity_;++row)
            std::copy_n(ring_.begin()+row*columns_,columns_,rows[row].begin());
    }
    void render_compact_snapshot(std::vector<std::vector<float>>& rows) const {
        rows.resize(capacity_);
        for(auto& row:rows) {
            row.resize(columns_);
            std::fill(row.begin(),row.end(),padding_value());
        }
        for(std::size_t i=0;i<count_;++i)
            std::copy_n(ring_.begin()+((start_+i)%capacity_)*columns_,columns_,rows[capacity_-count_+i].begin());
    }
    static Status validate_compact_contract(const DeliveredNumericHistoryRow& event,CompactInput& input) {
        if(!event.compact.context || !event.compact.context->contract)return Status::invalid_argument;
        input.contract=event.compact.context->contract.view();
        const auto* history=std::get_if<signal_contracts::NumericContract>(&input.contract);
        if(!history || history->shape.rank!=signal_contracts::Rank::Matrix)return Status::incompatible_contract;
        if(event.capacity!=history->shape.extents[0].maximum || event.filled_count>event.capacity)return Status::malformed;
        input.columns=history->shape.extents[1].maximum;
        return Status::ok;
    }
    static signal_contracts::NumericContract compact_row_contract(const signal_contracts::NumericContract& history) {
        auto expected=history;
        expected.semantic=history.history_element;
        expected.history_element=signal_contracts::Semantic::Numeric;
        expected.shape={signal_contracts::Rank::Vector,{history.shape.axes[1],signal_contracts::Axis::None},
            {history.shape.extents[1],signal_contracts::ExtentBounds{}}};
        expected.coordinates={history.coordinates[1],signal_contracts::AxisCoordinates{}};
        return expected;
    }
    static Status validate_compact_payload(const DeliveredNumericHistoryRow& event,CompactInput& input) {
        if(!event.accepted_row_ordinal || !event.filled_count || event.filled_count>event.accepted_row_ordinal)return Status::malformed;
        if(!event.compact.frame || event.compact.status!=Status::ok)return Status::unavailable;
        input.frame=event.compact.frame.view();
        const auto* numeric=std::get_if<signal_contracts::NumericView>(&input.frame.payload);
        if(!numeric || !numeric->contract || !input.frame.stamp.available || numeric->actual[1]!=1)return Status::malformed;
        if(!signal_contracts::same_contract(compact_row_contract(input.history()),*numeric->contract))return Status::incompatible_contract;
        input.columns=numeric->actual[0];
        const auto& extent=input.history().shape.extents[1];
        return input.columns<extent.minimum || input.columns>extent.maximum?Status::malformed:Status::ok;
    }
    Status validate_compact_resources(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane) const {
        if(!event.capacity || event.capacity>limits_.max_rows || !input.columns || input.columns>limits_.max_columns
            || limits_.max_float_bytes/sizeof(float)/input.columns<1
            || event.capacity>(limits_.max_float_bytes/sizeof(float)/input.columns-1)/2
            || plane>=input.history().channel_order.size())return Status::buffer_too_small;
        return Status::ok;
    }
    bool compact_layout_changed(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane) const {
        return !context_ || capacity_!=event.capacity || columns_!=input.columns
            || context_->endpoint!=event.compact.context->endpoint || plane_!=plane
            || !signal_contracts::same_contract(context_->contract.view(),input.contract);
    }
    Status validate_compact_input(const DeliveredNumericHistoryRow& event,std::size_t plane,CompactInput& input) const {
        const auto contract_status=validate_compact_contract(event,input);
        if(contract_status!=Status::ok)return contract_status;
        if(event.event==NumericHistoryEvent::Row) {
            const auto status=validate_compact_payload(event,input);
            if(status!=Status::ok)return status;
        }
        const auto status=validate_compact_resources(event,input,plane);
        if(status!=Status::ok)return status;
        if(fence_seen_ && delivery_precedes(event.compact.delivery,delivery_fence_))return Status::would_block;
        input.changed=compact_layout_changed(event,input,plane);
        return Status::ok;
    }
    void remember_delivery(const DeliveryStamp& delivery) {
        delivery_fence_=delivery;
        fence_seen_=true;
    }
    void install_compact_layout(const DeliveredNumericHistoryRow& event,std::size_t plane,std::size_t columns,bool preserve) {
        resize_storage(std::size_t(event.capacity),columns,preserve);
        // resize_storage packs retained rows into compact-ring order.
        authoritative_snapshot_=false;
        context_=event.compact.context;
        plane_=plane;
    }
    Status compact_unavailable(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane) {
        if(input.changed) {
            const bool preserve=compatible_capacity_change(event.compact.context,plane,input.columns);
            install_compact_layout(event,plane,input.columns,preserve);
            if(!preserve)filled_count_=event.filled_count;
        }
        remember_delivery(event.compact.delivery);
        available_=false;
        return Status::unavailable;
    }
    Status compact_reset(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane) {
        authoritative_snapshot_=false;
        if(input.changed)install_compact_layout(event,plane,input.columns,false);
        clear_rows();
        context_=event.compact.context;
        plane_=plane;
        remember_delivery(event.compact.delivery);
        available_=true;
        return Status::ok;
    }
    Status project_compact_row(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane,FloatProjectionReport& report) {
        staged_row_.resize(input.columns);
        const auto status=project_numeric_float(input.numeric(),plane,limits_.projection,staged_row_,report);
        if(status!=Status::ok) {
            remember_delivery(event.compact.delivery);
            available_=false;
        }
        return status;
    }
    void append_compact_row(const DeliveredNumericHistoryRow& event,const FloatProjectionReport& report) {
        const auto row=(start_+count_)%capacity_;
        std::copy(staged_row_.begin(),staged_row_.end(),ring_.begin()+row*columns_);
        if(count_==capacity_ && row_losses_[row])--loss_count_;
        row_losses_[row]=report.inexact_elements!=0;
        loss_count_+=row_losses_[row];
        row_times_[row]={event.time_coordinate,event.has_time_coordinate,event.accepted_row_ordinal-1};
        if(count_<capacity_)++count_;
        else start_=(start_+1)%capacity_;
    }
    void install_compact_delivery(const DeliveredNumericHistoryRow& event,const signal_contracts::FrameStamp& stamp) {
        ordinal_=event.accepted_row_ordinal;
        epoch_=stamp.producer_epoch;
        generation_=stamp.generation;
        remember_delivery(event.compact.delivery);
        filled_count_=event.filled_count;
        seen_=available_=true;
        latest_=event;
        context_=event.compact.context;
        loss_=loss_count_!=0;
    }
    void prepare_compact_append(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane,
        bool preserve,bool same_stream) {
        authoritative_snapshot_=false;
        if(input.changed)install_compact_layout(event,plane,input.columns,preserve);
        if(!same_stream || input.frame.stamp.discontinuity || (seen_ && event.accepted_row_ordinal-ordinal_!=1))clear_rows();
        // Count shrink can represent a producer reset without a new epoch.
        if(event.filled_count<filled_count_)clear_rows();
    }
    Status accept_compact_row(const DeliveredNumericHistoryRow& event,const CompactInput& input,std::size_t plane) {
        const bool preserve=input.changed && compatible_capacity_change(event.compact.context,plane,input.columns);
        const bool same_stream=(!input.changed || preserve) && seen_
            && epoch_==input.frame.stamp.producer_epoch && generation_==input.frame.stamp.generation;
        if(same_stream && event.accepted_row_ordinal<=ordinal_) {
            remember_delivery(event.compact.delivery);
            return Status::would_block;
        }
        // Conversion refusal preserves all previously accepted rows.
        FloatProjectionReport report;
        const auto status=project_compact_row(event,input,plane,report);
        if(status!=Status::ok)return status;
        prepare_compact_append(event,input,plane,preserve,same_stream);
        append_compact_row(event,report);
        install_compact_delivery(event,input.frame.stamp);
        return Status::ok;
    }
    static std::vector<double> index_coordinates(std::size_t count) {
        std::vector<double> result(count);
        for(std::size_t i=0;i<count;++i)result[i]=double(i);
        return result;
    }
    static bool increasing_finite(std::span<const double> values) {
        for(std::size_t i=0;i<values.size();++i)
            if(!std::isfinite(values[i]) || (i && values[i]<=values[i-1]))return false;
        return true;
    }
    static bool increasing_nonnegative(std::span<const double> values) {
        return increasing_finite(values) && std::all_of(values.begin(),values.end(),[](double value){return value>=0;});
    }
    void explicit_column_coordinates(std::vector<double>& out) const {
        if(!latest_.compact.frame) {
            if(explicit_columns_.size()==columns_)out=explicit_columns_;
            return;
        }
        const auto frame=latest_.compact.frame.view();
        const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);
        if(!numeric || numeric->coordinates.axes[0].size()!=columns_*8)return;
        for(std::size_t i=0;i<columns_;++i)
            out[i]=decode_coordinate(numeric->coordinates.axes[0].subspan(i*8,8));
    }
    void project_column_axis(const signal_contracts::AxisCoordinates& axis,HistoryAxes& result) const {
        using signal_contracts::CoordinateMode;
        if(axis.mode==CoordinateMode::None)return;
        std::vector<double> values(columns_,std::numeric_limits<double>::quiet_NaN());
        if(axis.mode==CoordinateMode::Regular && axis.origin.denominator && axis.step.denominator)
            for(std::size_t i=0;i<columns_;++i)
                values[i]=double(axis.origin.numerator)/double(axis.origin.denominator)
                    +double(i)*double(axis.step.numerator)/double(axis.step.denominator);
        else if(axis.mode==CoordinateMode::ExplicitFloat64LE)explicit_column_coordinates(values);
        if(!increasing_finite(values))return;
        result.columns=std::move(values);
        result.column_coordinates=true;
        result.column_unit=axis.unit;
    }
    static bool row_age(const signal_contracts::AxisCoordinates& axis,const RowTime& newest,const RowTime& time,double& out) {
        if(axis.mode==signal_contracts::CoordinateMode::Regular) {
            // Producer ordinals retain spacing after the rolling capacity wraps.
            if(!axis.step.denominator || axis.step.numerator<=0 || newest.ordinal<time.ordinal)return false;
            out=double(newest.ordinal-time.ordinal)*double(axis.step.numerator)/double(axis.step.denominator);
            return true;
        }
        if(!newest.has_coordinate || !time.has_coordinate)return false;
        out=decode_coordinate(newest.coordinate)-decode_coordinate(time.coordinate);
        return true;
    }
    bool project_row_ages(const signal_contracts::AxisCoordinates& axis,std::vector<double>& ages) const {
        const auto& newest=row_times_[(start_+count_-1)%capacity_];
        for(std::size_t age=0;age<count_;++age) {
            const auto& time=row_times_[(start_+count_-1-age)%capacity_];
            if(!row_age(axis,newest,time,ages[age]))return false;
        }
        return increasing_nonnegative(ages);
    }
    void project_age_axis(const signal_contracts::AxisCoordinates& axis,HistoryAxes& result) const {
        const auto displayed=authoritative_snapshot_ && !ring_.empty()?capacity_:count_;
        result.ages=index_coordinates(displayed);
        // Unknown older slots cannot be assigned invented seconds.
        if(authoritative_snapshot_ && count_!=capacity_)return;
        const bool seconds=axis.mode!=signal_contracts::CoordinateMode::None && axis.unit==signal_contracts::NumericUnit::Seconds;
        if(!count_) {result.seconds=seconds;return;}
        if(!seconds)return;
        std::vector<double> ages(count_);
        if(!project_row_ages(axis,ages))return;
        ages[0]=0;
        result.ages=std::move(ages);
        result.seconds=true;
    }
    bool same_snapshot_stream(const DeliveredNumericFrame& input,const SnapshotInput& snapshot,std::size_t plane) const {
        return context_ && context_->endpoint==input.context->endpoint && plane_==plane
            && signal_contracts::same_contract(context_->contract.view(),snapshot.contract) && seen_
            && epoch_==snapshot.frame.stamp.producer_epoch && generation_==snapshot.frame.stamp.generation;
    }
    Status validate_snapshot_payload(const DeliveredNumericFrame& input,std::uint64_t ordinal,std::size_t plane,SnapshotInput& snapshot) const {
        const auto* numeric=std::get_if<signal_contracts::NumericView>(&snapshot.frame.payload);
        if(!numeric || !numeric->contract || !signal_contracts::same_contract(snapshot.history(),*numeric->contract))
            return Status::incompatible_contract;
        if(input.status!=Status::ok && input.status!=Status::unavailable)return input.status;
        if(same_snapshot_stream(input,snapshot,plane) && ordinal<ordinal_)return Status::would_block;
        snapshot.has_payload=!((input.status==Status::unavailable || !snapshot.frame.stamp.available) && numeric->storage.empty());
        return Status::ok;
    }
    Status validate_snapshot_input(const DeliveredNumericFrame& input,std::uint64_t ordinal,std::uint64_t filled,std::size_t plane,SnapshotInput& snapshot) const {
        if(!input.context || !input.context->contract)return Status::invalid_argument;
        if(fence_seen_ && delivery_precedes(input.delivery,delivery_fence_))return Status::would_block;
        snapshot.contract=input.context->contract.view();
        const auto* history=std::get_if<signal_contracts::NumericContract>(&snapshot.contract);
        if(!history || history->shape.rank!=signal_contracts::Rank::Matrix)return Status::incompatible_contract;
        if(filled>ordinal || filled>history->shape.extents[0].maximum)return Status::malformed;
        snapshot.frame=input.frame?input.frame.view():signal_contracts::DataFrameView{};
        if(!input.frame && input.status==Status::unavailable)return Status::ok;
        if(!input.frame)return Status::invalid_argument;
        return validate_snapshot_payload(input,ordinal,plane,snapshot);
    }
    Status unavailable_snapshot(const DeliveredNumericFrame& input,std::size_t plane) {
        // A capacity declaration alone never creates synthetic display samples.
        clear();
        context_=input.context;
        plane_=plane;
        authoritative_snapshot_=true;
        delivery_fence_=input.delivery;
        fence_seen_=true;
        return Status::unavailable;
    }
    Status snapshot_geometry(const SnapshotInput& snapshot,std::uint64_t filled,std::size_t plane,SnapshotGeometry& out) const {
        const auto& history=snapshot.history();
        const auto& numeric=snapshot.numeric();
        const auto capacity=history.shape.extents[0].maximum;
        const auto rows=numeric.actual[0],columns=numeric.actual[1];
        if(rows!=capacity || filled>rows || columns<history.shape.extents[1].minimum || columns>history.shape.extents[1].maximum)
            return Status::malformed;
        if(!capacity || capacity>limits_.max_rows || !columns || columns>limits_.max_columns || plane>=numeric.planes.size()
            || columns>limits_.projection.max_elements || limits_.max_float_bytes/sizeof(float)/columns<1
            || capacity>(limits_.max_float_bytes/sizeof(float)/columns-1)/2)return Status::buffer_too_small;
        out={capacity,rows,columns,filled,rows-filled};
        return Status::ok;
    }
    Status project_snapshot_values(const signal_contracts::NumericView& numeric,std::size_t plane,std::size_t row,
        std::span<float> destination,bool& row_loss) const {
        for(std::size_t column=0;column<destination.size();++column) {
            NumericLiteral literal;
            const auto read=read_numeric_literal(numeric,{plane,row,column},literal);
            if(read!=Status::ok)return read;
            bool loss=false;
            const auto converted=project_literal(literal,limits_.projection.allow_precision_loss,destination[column],loss);
            if(converted!=Status::ok)return converted;
            row_loss=row_loss || loss;
        }
        return Status::ok;
    }
    static Status regular_row_coordinate(const signal_contracts::AxisCoordinates& axis,std::size_t row,RowTime& time) {
        if(!axis.origin.denominator || !axis.step.denominator)return Status::malformed;
        const double value=double(axis.origin.numerator)/axis.origin.denominator+double(row)*axis.step.numerator/axis.step.denominator;
        const auto bits=std::bit_cast<std::uint64_t>(value);
        for(unsigned byte=0;byte<8;++byte)time.coordinate[byte]=std::byte((bits>>(byte*8))&255);
        time.has_coordinate=true;
        return Status::ok;
    }
    static Status snapshot_row_coordinate(const SnapshotInput& snapshot,std::size_t row,std::uint64_t rows,RowTime& time) {
        const auto& axis=snapshot.history().coordinates[0];
        if(axis.mode==signal_contracts::CoordinateMode::Regular)return regular_row_coordinate(axis,row,time);
        if(axis.mode!=signal_contracts::CoordinateMode::ExplicitFloat64LE)return Status::ok;
        const auto bytes=snapshot.numeric().coordinates.axes[0];
        if(bytes.size()!=rows*8)return Status::malformed;
        std::copy_n(bytes.begin()+row*8,8,time.coordinate.begin());
        time.has_coordinate=true;
        return Status::ok;
    }
    Status project_snapshot_row(const SnapshotInput& snapshot,const SnapshotGeometry& geometry,std::size_t plane,
        std::uint64_t ordinal,std::size_t row,SnapshotProjection& projection) const {
        bool loss=false;
        const auto destination=std::span(projection.values).subspan(row*geometry.columns,geometry.columns);
        const auto status=project_snapshot_values(snapshot.numeric(),plane,row,destination,loss);
        if(status!=Status::ok)return status;
        projection.losses[row]=loss;
        projection.loss_count+=loss;
        if(row<geometry.padding)return Status::ok;
        auto& time=projection.times[row];
        time.ordinal=ordinal-geometry.real_rows+(row-geometry.padding);
        return snapshot_row_coordinate(snapshot,row,geometry.rows,time);
    }
    Status project_snapshot_rows(const SnapshotInput& snapshot,const SnapshotGeometry& geometry,std::size_t plane,
        std::uint64_t ordinal,SnapshotProjection& projection) const {
        for(std::size_t row=0;row<geometry.rows;++row) {
            const auto status=project_snapshot_row(snapshot,geometry,plane,ordinal,row,projection);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    static Status snapshot_column_coordinates(const SnapshotInput& snapshot,std::size_t columns,std::vector<double>& out) {
        if(snapshot.history().coordinates[1].mode!=signal_contracts::CoordinateMode::ExplicitFloat64LE)return Status::ok;
        const auto bytes=snapshot.numeric().coordinates.axes[1];
        if(bytes.size()!=columns*8)return Status::malformed;
        out.resize(columns);
        for(std::size_t column=0;column<columns;++column)out[column]=decode_coordinate(bytes.subspan(column*8,8));
        return Status::ok;
    }
    Status project_snapshot(const SnapshotInput& snapshot,const SnapshotGeometry& geometry,std::size_t plane,
        std::uint64_t ordinal,SnapshotProjection& projection) const {
        projection.values.assign(std::size_t(geometry.capacity*geometry.columns),0.f);
        projection.losses.assign(std::size_t(geometry.capacity),false);
        projection.times.assign(std::size_t(geometry.capacity),RowTime{});
        const auto status=project_snapshot_rows(snapshot,geometry,plane,ordinal,projection);
        if(status!=Status::ok)return status;
        return snapshot_column_coordinates(snapshot,std::size_t(geometry.columns),projection.coordinates);
    }
    void install_snapshot_storage(SnapshotProjection& projection,const SnapshotGeometry& geometry) {
        ring_.swap(projection.values);
        row_losses_.swap(projection.losses);
        row_times_.swap(projection.times);
        explicit_columns_.swap(projection.coordinates);
        latest_={};
        start_=std::size_t(geometry.padding);
        count_=std::size_t(geometry.real_rows);
        capacity_=std::size_t(geometry.capacity);
        columns_=std::size_t(geometry.columns);
        authoritative_snapshot_=true;
    }
    void install_snapshot_delivery(const DeliveredNumericFrame& input,const signal_contracts::FrameStamp& stamp,
        std::size_t plane,std::uint64_t ordinal,std::uint64_t filled,std::size_t loss_count) {
        context_=input.context;
        plane_=plane;
        filled_count_=filled;
        loss_count_=loss_count;
        loss_=loss_count!=0;
        ordinal_=ordinal;
        epoch_=stamp.producer_epoch;
        generation_=stamp.generation;
        delivery_fence_=input.delivery;
        seen_=count_!=0;
        fence_seen_=true;
        available_=input.status==Status::ok && stamp.available;
    }
    float padding_value() const noexcept {
        if(!context_)return 0;
        const auto contract=context_->contract.view();const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
        if(!history)return 0;
        const auto* dtype=signal_contracts::dtype_descriptor(history->dtype);
        std::uint64_t bits=0;
        if(history->representation.normalization==signal_contracts::Normalization::UnsignedUnit && dtype && !dtype->signed_value && !dtype->ieee) {
            const unsigned width=history->representation.valid_bits?history->representation.valid_bits:unsigned(dtype->bytes)*8;
            bits=std::uint64_t{1}<<(width-1);
        }
        float value=0;bool loss=false;
        (void)project_literal(NumericLiteral{bits},limits_.projection.allow_precision_loss,value,loss);
        return value;
    }
    bool compatible_capacity_change(const std::shared_ptr<const NumericDeliveryContext>& context,std::size_t plane,std::size_t columns) const {
        if(!context_ || context_->endpoint!=context->endpoint || plane_!=plane || columns_!=columns)return false;
        const auto before=context_->contract.view(),after=context->contract.view();
        const auto* old=std::get_if<signal_contracts::NumericContract>(&before);
        const auto* next=std::get_if<signal_contracts::NumericContract>(&after);
        if(!old || !next)return false;
        auto comparable=*next;comparable.shape.extents[0]=old->shape.extents[0];
        return signal_contracts::same_contract(*old,comparable);
    }
    SnapshotProjection retained_storage(std::size_t capacity,std::size_t columns,std::size_t retained) const {
        SnapshotProjection replacement;
        replacement.values.assign(capacity*columns,0.f);
        replacement.losses.assign(capacity,false);
        replacement.times.assign(capacity,RowTime{});
        for(std::size_t i=0;i<retained;++i) {
            const auto source=(start_+count_-retained+i)%capacity_;
            std::copy_n(ring_.begin()+source*columns_,columns,replacement.values.begin()+i*columns);
            replacement.losses[i]=row_losses_[source];
            replacement.loss_count+=replacement.losses[i];
            replacement.times[i]=row_times_[source];
        }
        return replacement;
    }
    void resize_storage(std::size_t capacity,std::size_t columns,bool preserve) {
        const auto retained=preserve?std::min(count_,capacity):0;
        auto replacement=retained_storage(capacity,columns,retained);
        ring_.swap(replacement.values);
        row_losses_.swap(replacement.losses);
        row_times_.swap(replacement.times);
        if(!preserve)clear_rows();
        start_=0;count_=retained;capacity_=capacity;columns_=columns;
        filled_count_=preserve?std::min<std::uint64_t>(filled_count_,capacity):0;
        loss_count_=replacement.loss_count;
        loss_=loss_count_!=0;
    }
    void clear_rows() noexcept {start_=count_=loss_count_=0;seen_=false;available_=false;filled_count_=0;latest_={};loss_=false;explicit_columns_.clear();}
    HistoryPresentationLimits limits_;
    std::shared_ptr<const NumericDeliveryContext> context_;
    std::vector<float> ring_;
    std::vector<float> staged_row_;
    std::vector<bool> row_losses_;
    std::vector<RowTime> row_times_;
    std::vector<double> explicit_columns_;
    std::size_t start_=0,count_=0,capacity_=0,columns_=0,plane_=0,loss_count_=0;
    std::uint64_t ordinal_=0,epoch_=0,generation_=0,filled_count_=0;
    DeliveryStamp delivery_fence_;
    bool seen_=false,fence_seen_=false,available_=false,loss_=false,authoritative_snapshot_=false;
    DeliveredNumericHistoryRow latest_;
};
}
