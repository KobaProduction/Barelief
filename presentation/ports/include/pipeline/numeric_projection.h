#pragma once
#include <signal_contracts/data_frame.h>
#include <signal_contracts/numeric_policy.h>
#include <array>
#include <string>
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
inline Status project_literal(const NumericLiteral& literal,bool allow_loss,float& out,bool& inexact) noexcept {
    return std::visit([&](auto value) {
        using T=decltype(value); bool exact=true;
        if constexpr(std::is_integral_v<T>) {
            std::uint64_t magnitude;
            if constexpr(std::is_signed_v<T>) magnitude=value<0?std::uint64_t(-(value+1))+1:std::uint64_t(value);
            else magnitude=value;
            exact=float_exact_integer(magnitude);
        } else {
            if(!std::isfinite(value) || value>std::numeric_limits<float>::max() || value<-std::numeric_limits<float>::max())return Status::malformed;
            exact=T(float(value))==value;
        }
        if(!exact && !allow_loss)return Status::incompatible_contract;
        out=float(value);inexact=!exact;return Status::ok;
    },literal);
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
inline Status read_numeric_literal(const signal_contracts::NumericView& view,signal_contracts::ElementIndex index,NumericLiteral& out) noexcept {
    if(!view.contract)return Status::malformed;
    using signal_contracts::DType;
    switch(view.contract->dtype) {
    case DType::Int8:return read_literal_as<std::int8_t>(view,index,out);
    case DType::UInt8:return read_literal_as<std::uint8_t>(view,index,out);
    case DType::Int16:return read_literal_as<std::int16_t>(view,index,out);
    case DType::UInt16:return read_literal_as<std::uint16_t>(view,index,out);
    case DType::Int32:return read_literal_as<std::int32_t>(view,index,out);
    case DType::UInt32:return read_literal_as<std::uint32_t>(view,index,out);
    case DType::Int64:return read_literal_as<std::int64_t>(view,index,out);
    case DType::UInt64:return read_literal_as<std::uint64_t>(view,index,out);
    case DType::Float32:return read_literal_as<float>(view,index,out);
    case DType::Float64:return read_literal_as<double>(view,index,out);
    }
    return Status::malformed;
}
// One selected plane; old->new logical order. Preflight prevents partial publication.
inline Status project_numeric_float(const signal_contracts::NumericView& view,std::size_t plane,
    FloatProjectionPolicy policy,std::span<float> destination,FloatProjectionReport& report) noexcept {
    if(!view.contract || plane>=view.planes.size())return Status::malformed;
    using signal_contracts::Rank;
    const auto rows=view.actual[0],columns=view.contract->shape.rank==Rank::Matrix?view.actual[1]:std::size_t{1};
    if(columns && rows>policy.max_elements/columns)return Status::buffer_too_small;
    const auto count=rows*columns;if(count>destination.size())return Status::buffer_too_small;
    FloatProjectionReport candidate{count,0};
    for(std::size_t i=0;i<count;++i) {
        NumericLiteral literal;auto s=read_numeric_literal(view,{plane,i/columns,i%columns},literal);if(s!=Status::ok)return s;
        float projected;bool loss; s=project_literal(literal,policy.allow_precision_loss,projected,loss);if(s!=Status::ok)return s;
        candidate.inexact_elements+=loss;
    }
    for(std::size_t i=0;i<count;++i) {
        NumericLiteral literal;read_numeric_literal(view,{plane,i/columns,i%columns},literal);
        bool loss;project_literal(literal,policy.allow_precision_loss,destination[i],loss);
    }
    report=candidate;return Status::ok;
}
// Fixed ring retains exact tagged values; first sequence=0 is a valid first frame.
class ScalarPresentation {
public:
    bool accept(const signal_contracts::FrameStamp& stamp, NumericLiteral value) noexcept {
        if(!stamp.available)return false;
        if(seen_ && stamp.epoch<delivery_epoch_)return false;
        // Delivery fences reject stale queued work, but a newer graph fence alone
        // does not reset an exact compatible producer stream.
        const bool same_stream=seen_ && epoch_==stamp.producer_epoch && generation_==stamp.generation;
        // Test duplicates before applying a latched discontinuity bit.
        if(same_stream && stamp.sequence<=sequence_)return false;
        // Presentation delivery may skip producer frames. Retain sparse samples;
        // source timestamps preserve the real interval between them.
        if(!same_stream || stamp.discontinuity)clear();
        const auto slot=(start_+count_)%values_.size();
        values_[slot]=value;times_[slot]=stamp.timestamp_ticks;
        if(count_<values_.size())++count_;else start_=(start_+1)%values_.size();
        seen_=true;sequence_=stamp.sequence;epoch_=stamp.producer_epoch;generation_=stamp.generation;delivery_epoch_=stamp.epoch;
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
        for(std::size_t i=0;i<count_;++i) {
            const auto time=times_[(start_+i)%times_.size()];
            // Subtract integer ticks before conversion: on MSVC long double
            // is double, so converting near INT64_MAX first loses short deltas.
            const long double delta=time<=newest
                ?-static_cast<long double>(std::uint64_t(newest)-std::uint64_t(time))
                :static_cast<long double>(std::uint64_t(time)-std::uint64_t(newest));
            result[i]=double(delta*tick);
            if(!std::isfinite(result[i]) || (i && result[i]<=result[i-1]))return {};
        }
        return result;
    }

    Status project(FloatProjectionPolicy policy,std::span<float> destination,FloatProjectionReport& report) const noexcept {
        if(count_>policy.max_elements || count_>destination.size())return Status::buffer_too_small;
        FloatProjectionReport candidate{count_,0};
        for(std::size_t i=0;i<count_;++i){float x;bool loss;auto s=project_literal(at(i),policy.allow_precision_loss,x,loss);if(s!=Status::ok)return s;candidate.inexact_elements+=loss;}
        for(std::size_t i=0;i<count_;++i){bool loss;project_literal(at(i),policy.allow_precision_loss,destination[i],loss);}
        report=candidate;return Status::ok;
    }
private:
    std::array<NumericLiteral,128> values_{};
    std::array<std::int64_t,128> times_{};
    std::size_t start_=0,count_=0;
    std::uint64_t sequence_=0,epoch_=0,generation_=0,delivery_epoch_=0;
    bool seen_=false;
};
// Float peak includes retained ring + replacement/render snapshot + staged row.
// Row metadata/container bookkeeping are separately bounded by max_rows.
struct HistoryPresentationLimits { std::size_t max_rows=0,max_columns=0,max_float_bytes=0; FloatProjectionPolicy projection; };
enum class HistoryColumnAxis { Band, Bin, Sample, Value, Hertz };
enum class HistoryValueAxis { Amplitude, Frames, Value };
struct HistoryAxes {
    HistoryColumnAxis column_axis=HistoryColumnAxis::Value;
    HistoryValueAxis value_axis=HistoryValueAxis::Value;
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
public:
    explicit HistoryPresentation(HistoryPresentationLimits limits):limits_(limits){}
    // Explicit cold resynchronization. The full owner is projected atomically,
    // then released; subsequent hot deliveries still contain one compact row.
    Status accept_snapshot(const DeliveredNumericFrame& input,std::uint64_t ordinal,std::uint64_t filled_count,std::size_t plane=0) {
        using namespace signal_contracts;
        if(!input.context || !input.context->contract)return Status::invalid_argument;
        if(fence_seen_ && delivery_precedes(input.delivery,delivery_fence_))return Status::would_block;
        const auto contract=input.context->contract.view();const auto* history=std::get_if<NumericContract>(&contract);
        if(!history || history->shape.rank!=Rank::Matrix)return Status::incompatible_contract;
        if(filled_count>ordinal || filled_count>history->shape.extents[0].maximum)return Status::malformed;
        const auto frame=input.frame?input.frame.view():DataFrameView{};
        const auto unavailable_without_payload=[&] {
            // Without an authoritative payload there are no display samples.
            // Capacity metadata alone does not authorize GUI-generated silence.
            clear();context_=input.context;plane_=plane;authoritative_snapshot_=true;
            delivery_fence_=input.delivery;fence_seen_=true;return Status::unavailable;
        };
        if(!input.frame && input.status==Status::unavailable)return unavailable_without_payload();
        if(!input.frame)return Status::invalid_argument;
        const auto* numeric=std::get_if<NumericView>(&frame.payload);
        if(!numeric || !numeric->contract || !same_contract(*history,*numeric->contract))return Status::incompatible_contract;
        if(input.status!=Status::ok && input.status!=Status::unavailable)return input.status;
        const bool same_stream=context_ && context_->endpoint==input.context->endpoint && plane_==plane &&
            same_contract(context_->contract.view(),contract) && seen_ && epoch_==frame.stamp.producer_epoch && generation_==frame.stamp.generation;
        if(same_stream && ordinal<ordinal_)return Status::would_block;
        if((input.status==Status::unavailable || !frame.stamp.available) && numeric->storage.empty())
            return unavailable_without_payload();
        const auto capacity=history->shape.extents[0].maximum, rows=numeric->actual[0],columns=numeric->actual[1];
        if(rows!=capacity || filled_count>rows || filled_count>ordinal || columns<history->shape.extents[1].minimum ||
           columns>history->shape.extents[1].maximum)return Status::malformed;
        if(!capacity || capacity>limits_.max_rows || !columns || columns>limits_.max_columns || plane>=numeric->planes.size() ||
           columns>limits_.projection.max_elements || limits_.max_float_bytes/sizeof(float)/columns<1 ||
           capacity>(limits_.max_float_bytes/sizeof(float)/columns-1)/2)return Status::buffer_too_small;
        std::vector<float> replacement(std::size_t(capacity*columns),0.f);
        std::vector<bool> losses(std::size_t(capacity),false);std::vector<RowTime> times(std::size_t(capacity),RowTime{});
        std::size_t loss_count=0;
        const auto real_rows=std::size_t(filled_count),padding=rows-real_rows;
        for(std::size_t row=0;row<rows;++row) {
            for(std::size_t column=0;column<columns;++column) {
                NumericLiteral literal;auto status=read_numeric_literal(*numeric,{plane,row,column},literal);if(status!=Status::ok)return status;
                bool loss=false;status=project_literal(literal,limits_.projection.allow_precision_loss,replacement[row*columns+column],loss);
                if(status!=Status::ok)return status;losses[row]=losses[row] || loss;
            }
            loss_count+=losses[row];
            if(row<padding)continue; // Padding has values, but no invented sample times.
            times[row].ordinal=ordinal-real_rows+(row-padding);
            if(history->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE) {
                if(numeric->coordinates.axes[0].size()!=rows*8)return Status::malformed;
                std::copy_n(numeric->coordinates.axes[0].begin()+row*8,8,times[row].coordinate.begin());times[row].has_coordinate=true;
            } else if(history->coordinates[0].mode==CoordinateMode::Regular) {
                const auto& axis=history->coordinates[0];if(!axis.origin.denominator || !axis.step.denominator)return Status::malformed;
                const double value=double(axis.origin.numerator)/axis.origin.denominator+double(row)*axis.step.numerator/axis.step.denominator;
                const auto bits=std::bit_cast<std::uint64_t>(value);
                for(unsigned byte=0;byte<8;++byte)times[row].coordinate[byte]=std::byte((bits>>(byte*8))&255);
                times[row].has_coordinate=true;
            }
        }
        std::vector<double> coordinates;
        if(history->coordinates[1].mode==CoordinateMode::ExplicitFloat64LE) {
            if(numeric->coordinates.axes[1].size()!=columns*8)return Status::malformed;
            coordinates.resize(columns);
            for(std::size_t column=0;column<columns;++column)coordinates[column]=decode_coordinate(numeric->coordinates.axes[1].subspan(column*8,8));
        }
        ring_.swap(replacement);row_losses_.swap(losses);row_times_.swap(times);explicit_columns_.swap(coordinates);
        latest_={};context_=input.context;plane_=plane;start_=padding;count_=real_rows;capacity_=capacity;columns_=columns;authoritative_snapshot_=true;
        filled_count_=filled_count;loss_count_=loss_count;loss_=loss_count!=0;ordinal_=ordinal;
        epoch_=frame.stamp.producer_epoch;generation_=frame.stamp.generation;delivery_fence_=input.delivery;
        seen_=real_rows!=0;fence_seen_=true;available_=input.status==Status::ok && frame.stamp.available;return available_?Status::ok:Status::unavailable;
    }
    Status accept(const DeliveredNumericHistoryRow& event,std::size_t plane=0) {
        if(!event.compact.context || !event.compact.context->contract)return Status::invalid_argument;
        auto contract=event.compact.context->contract.view();const auto* history=std::get_if<signal_contracts::NumericContract>(&contract);
        if(!history || history->shape.rank!=signal_contracts::Rank::Matrix)return Status::incompatible_contract;
        if(event.capacity!=history->shape.extents[0].maximum || event.filled_count>event.capacity)return Status::malformed;
        signal_contracts::DataFrameView frame;
        const signal_contracts::NumericView* numeric=nullptr;
        auto columns=history->shape.extents[1].maximum;
        if(event.event==NumericHistoryEvent::Row) {
            if(!event.accepted_row_ordinal || !event.filled_count || event.filled_count>event.accepted_row_ordinal)return Status::malformed;
            if(!event.compact.frame || event.compact.status!=Status::ok)return Status::unavailable;
            frame=event.compact.frame.view();numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);
            if(!numeric || !numeric->contract || !frame.stamp.available || numeric->actual[1]!=1)return Status::malformed;
            auto expected=*history;expected.semantic=history->history_element;expected.history_element=signal_contracts::Semantic::Numeric;
            expected.shape={signal_contracts::Rank::Vector,{history->shape.axes[1],signal_contracts::Axis::None},{history->shape.extents[1],signal_contracts::ExtentBounds{}}};
            expected.coordinates={history->coordinates[1],signal_contracts::AxisCoordinates{}};
            if(!signal_contracts::same_contract(expected,*numeric->contract))return Status::incompatible_contract;
            columns=numeric->actual[0];
            if(columns<history->shape.extents[1].minimum || columns>history->shape.extents[1].maximum)return Status::malformed;
        }
        if(!event.capacity || event.capacity>limits_.max_rows || !columns || columns>limits_.max_columns ||
           limits_.max_float_bytes/sizeof(float)/columns<1 ||
           event.capacity>(limits_.max_float_bytes/sizeof(float)/columns-1)/2 || plane>=history->channel_order.size())return Status::buffer_too_small;
        if(fence_seen_ && delivery_precedes(event.compact.delivery,delivery_fence_))return Status::would_block;
        const bool changed=!context_ || capacity_!=event.capacity || columns_!=columns || context_->endpoint!=event.compact.context->endpoint || plane_!=plane ||
            !signal_contracts::same_contract(context_->contract.view(),contract);
        if(event.event==NumericHistoryEvent::Unavailable){
            if(changed){
                const bool preserve=compatible_capacity_change(event.compact.context,plane,columns);
                resize_storage(std::size_t(event.capacity),std::size_t(columns),preserve);
                context_=event.compact.context;plane_=plane;
                if(!preserve)filled_count_=event.filled_count;
            }
            delivery_fence_=event.compact.delivery;fence_seen_=true;available_=false;return Status::unavailable;
        }
        if(event.event==NumericHistoryEvent::Empty || event.event==NumericHistoryEvent::Reset) {
            authoritative_snapshot_=false;
            if(changed){std::vector<float> replacement(std::size_t(event.capacity*columns),0.f);std::vector<bool> losses(std::size_t(event.capacity),false);std::vector<RowTime> times(std::size_t(event.capacity));ring_.swap(replacement);row_losses_.swap(losses);row_times_.swap(times);capacity_=std::size_t(event.capacity);columns_=std::size_t(columns);}
            clear_rows();context_=event.compact.context;plane_=plane;delivery_fence_=event.compact.delivery;fence_seen_=true;available_=true;return Status::ok;
        }
        const bool preserve_resize=changed && compatible_capacity_change(event.compact.context,plane,columns);
        const bool same_stream=(!changed || preserve_resize) && seen_ && epoch_==frame.stamp.producer_epoch && generation_==frame.stamp.generation;
        if(same_stream && event.accepted_row_ordinal<=ordinal_){delivery_fence_=event.compact.delivery;fence_seen_=true;return Status::would_block;}
        // Stage a row before changing ring, so conversion refusal keeps prior rows.
        staged_row_.resize(std::size_t(columns));FloatProjectionReport report;
        auto status=project_numeric_float(*numeric,plane,limits_.projection,staged_row_,report);if(status!=Status::ok){delivery_fence_=event.compact.delivery;fence_seen_=true;available_=false;return status;}
        authoritative_snapshot_=false;
        if(changed){
            resize_storage(std::size_t(event.capacity),std::size_t(columns),preserve_resize);
            context_=event.compact.context;plane_=plane;
        }
        if(!same_stream || frame.stamp.discontinuity || (seen_ && event.accepted_row_ordinal-ordinal_!=1))clear_rows();
        // Count shrink can be a producer reset even if its epoch is unchanged.
        if(event.filled_count<filled_count_)clear_rows();
        const auto row=(start_+count_)%capacity_;std::copy(staged_row_.begin(),staged_row_.end(),ring_.begin()+row*columns_);
        if(count_==capacity_ && row_losses_[row])--loss_count_;
        row_losses_[row]=report.inexact_elements!=0;
        loss_count_+=row_losses_[row];
        row_times_[row]={event.time_coordinate,event.has_time_coordinate,event.accepted_row_ordinal-1};
        if(count_<capacity_)++count_;else start_=(start_+1)%capacity_;
        ordinal_=event.accepted_row_ordinal;epoch_=frame.stamp.producer_epoch;generation_=frame.stamp.generation;delivery_fence_=event.compact.delivery;
        filled_count_=event.filled_count;seen_=fence_seen_=available_=true;latest_=event;context_=event.compact.context;loss_=loss_count_!=0;
        return Status::ok;
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
        using namespace signal_contracts;
        HistoryAxes result;if(!context_ || !columns_)return result;
        auto contract=context_->contract.view();const auto& history=std::get<NumericContract>(contract);
        result.value_axis=history.quantity==Quantity::FilledFrameCount || history.unit==NumericUnit::Frames?HistoryValueAxis::Frames:
            history.quantity==Quantity::Amplitude || history.unit==NumericUnit::NormalizedAmplitude || history.unit==NumericUnit::RawAmplitude?HistoryValueAxis::Amplitude:HistoryValueAxis::Value;
        result.column_axis=history.shape.axes[1]==Axis::Band?HistoryColumnAxis::Band:history.shape.axes[1]==Axis::Bin?HistoryColumnAxis::Bin:history.shape.axes[1]==Axis::Sample?HistoryColumnAxis::Sample:HistoryColumnAxis::Value;
        result.columns.resize(columns_);for(std::size_t i=0;i<columns_;++i)result.columns[i]=double(i);
        const auto& coordinates=history.coordinates[1];
        if(coordinates.unit==NumericUnit::Hertz && coordinates.mode!=CoordinateMode::None) {
            std::vector<double> hz(columns_,std::numeric_limits<double>::quiet_NaN());
            if(coordinates.mode==CoordinateMode::Regular && coordinates.origin.denominator && coordinates.step.denominator)
                for(std::size_t i=0;i<columns_;++i)hz[i]=double(coordinates.origin.numerator)/double(coordinates.origin.denominator)+double(i)*double(coordinates.step.numerator)/double(coordinates.step.denominator);
            else if(coordinates.mode==CoordinateMode::ExplicitFloat64LE && latest_.compact.frame) {
                const auto frame=latest_.compact.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
                if(numeric && numeric->coordinates.axes[0].size()==columns_*8)
                    for(std::size_t i=0;i<columns_;++i)hz[i]=decode_coordinate(numeric->coordinates.axes[0].subspan(i*8,8));
            }
            else if(coordinates.mode==CoordinateMode::ExplicitFloat64LE && explicit_columns_.size()==columns_)hz=explicit_columns_;
            bool valid=true;for(std::size_t i=0;i<hz.size();++i)valid=valid && std::isfinite(hz[i]) && hz[i]>=0 && (!i || hz[i]>hz[i-1]);
            if(valid){result.columns=std::move(hz);result.column_axis=HistoryColumnAxis::Hertz;}
        }
        const auto displayed_rows=authoritative_snapshot_ && !ring_.empty()?capacity_:count_;
        result.ages.resize(displayed_rows);for(std::size_t age=0;age<displayed_rows;++age)result.ages[age]=double(age);
        // Until every slot has an actual timestamp, expose the complete frame
        // age axis. Partial timestamps cannot define the older zero tail in seconds.
        if(authoritative_snapshot_ && count_!=capacity_)return result;
        const bool explicit_seconds=history.coordinates[0].mode!=CoordinateMode::None && history.coordinates[0].unit==NumericUnit::Seconds;
        if(!count_){result.seconds=explicit_seconds;return result;}
        const auto& newest=row_times_[(start_+count_-1)%capacity_];
        // None means row indices, consistently for compact and full deliveries.
        // One snapshot stamp cannot reconstruct individual row times, and the
        // PCM clock/sample rate is not the analysis cadence.
        if(!explicit_seconds)return result;
        std::vector<double> ages(count_);bool valid=true;
        for(std::size_t age=0;age<count_;++age) {
            const auto& time=row_times_[(start_+count_-1-age)%capacity_];
            if(history.coordinates[0].mode==CoordinateMode::Regular){
                // A rolling snapshot reuses logical origin; retained rows need
                // their producer ordinal to keep spacing after capacity wraps.
                const auto& step=history.coordinates[0].step;
                valid=valid && step.denominator && step.numerator>0 && newest.ordinal>=time.ordinal;
                ages[age]=step.denominator?double(newest.ordinal-time.ordinal)*double(step.numerator)/double(step.denominator):0;
            }else{
                valid=valid && newest.has_coordinate && time.has_coordinate;
                ages[age]=decode_coordinate(newest.coordinate)-decode_coordinate(time.coordinate);
            }
            valid=valid && std::isfinite(ages[age]) && ages[age]>=0 && (!age || ages[age]>ages[age-1]);
        }
        if(valid){ages[0]=0;result.ages=std::move(ages);result.seconds=true;}return result;
    }
    std::vector<std::vector<float>> render_snapshot() const {
        std::vector<std::vector<float>> rows;render_snapshot_into(rows);return rows;
    }
    // Presentation-only copy into reusable owning storage; processing memory
    // remains borrowed only within the synchronous copy.
    void render_snapshot_into(std::vector<std::vector<float>>& rows) const {
        if(authoritative_snapshot_) {
            if(ring_.empty()){rows.clear();return;}
            rows.resize(capacity_);for(auto& row:rows)row.resize(columns_);
            for(std::size_t row=0;row<capacity_;++row)
                std::copy_n(ring_.begin()+row*columns_,columns_,rows[row].begin());
            return;
        }
        // The complete represented history exists immediately. Unknown older
        // slots contain projected typed silence (unsigned PCM uses its midpoint);
        // only the newest suffix carries accepted samples.
        rows.resize(capacity_);for(auto& row:rows){row.resize(columns_);std::fill(row.begin(),row.end(),padding_value());}
        for(std::size_t i=0;i<count_;++i)std::copy_n(ring_.begin()+((start_+i)%capacity_)*columns_,columns_,rows[capacity_-count_+i].begin());
    }
private:
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
    void resize_storage(std::size_t capacity,std::size_t columns,bool preserve) {
        std::vector<float> replacement(capacity*columns,0.f);
        std::vector<bool> losses(capacity,false);std::vector<RowTime> times(capacity);
        const auto retained=preserve?std::min(count_,capacity):0;
        std::size_t loss_count=0;
        for(std::size_t i=0;i<retained;++i) {
            const auto source=(start_+count_-retained+i)%capacity_;
            std::copy_n(ring_.begin()+source*columns_,columns,replacement.begin()+i*columns);
            losses[i]=row_losses_[source];loss_count+=losses[i];times[i]=row_times_[source];
        }
        ring_.swap(replacement);row_losses_.swap(losses);row_times_.swap(times);
        if(!preserve)clear_rows();
        start_=0;count_=retained;capacity_=capacity;columns_=columns;
        filled_count_=preserve?std::min<std::uint64_t>(filled_count_,capacity):0;
        loss_count_=loss_count;loss_=loss_count!=0;
    }
    void clear_rows() noexcept {start_=count_=loss_count_=0;seen_=false;available_=false;filled_count_=0;latest_={};loss_=false;explicit_columns_.clear();}
    struct RowTime {std::array<std::byte,8> coordinate{};bool has_coordinate=false;std::uint64_t ordinal=0;};
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
