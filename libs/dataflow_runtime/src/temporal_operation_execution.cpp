#include "temporal_operation_execution.h"
#include "frame_access.h"
#include "preparation_support.h"
#include <domain/numeric/temporal_operations.h>
#include <dsp_kernels/tempogram_sequence.h>
#include <dsp_kernels/prepared_storage.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace dataflow_runtime::detail {
namespace {
namespace d=dsp_kernels;
namespace storage=dsp_kernels::prepared_storage;
using P=domain::TemporalOperationParameters;
enum class Kind {Frame,Correlation,Fourier,Autocorrelation,Sequence};
using Kernel=std::variant<std::monostate,d::PreparedTemporalWindow,d::PreparedSpectralAutocorrelation,
    d::PreparedFourierTempogram,d::PreparedAutocorrelationTempogram,d::PreparedTempogramSequence>;
Kind operation_kind(std::string_view value) noexcept {
    if(value=="TemporalFrame")return Kind::Frame;
    if(value=="SpectralAutocorrelation")return Kind::Correlation;
    if(value=="FourierTempogram")return Kind::Fourier;
    return value=="AutocorrelationTempogram"?Kind::Autocorrelation:Kind::Sequence;
}
d::TemporalWindowConfig window_config(const P& p) noexcept {
    return {p.window_length,p.hop,p.function,p.periodic,p.center,d::TemporalTail(p.tail),p.odd_center_ceil,p.empty_centered_frame};
}
d::SpectralAutocorrelationConfig correlation_config(const P& p) noexcept {
    return {p.fft_size,p.spectrum_divisor,p.spectral_exponent,p.inverse_scale,d::CorrelationNormalization(p.correlation_normalization),
        d::AutocorrelationInverseOutput(p.inverse_output),d::AutocorrelationTransform(p.transform),d::AutocorrelationFftLength(p.fft_length)};
}
d::AutocorrelationTempogramConfig autocorrelation_config(const P& p) noexcept {
    return {window_config(p),d::VectorNormalization(p.normalization),p.norm_exponent,correlation_config(p),d::TemporalPadding(p.padding)};
}
d::TempogramSequenceConfig sequence_config(const P& p) noexcept {
    const d::CurveInterpolationConfig interpolation{d::CurveInterpolation(p.interpolation_kind),p.spline_order,p.fill_below,p.fill_above,
        p.extrapolate,p.bounds_error,p.keep_infinite_coordinates,p.source_linear};
    return {d::TempogramMethod(p.method),float(p.input_rate),autocorrelation_config(p),interpolation};
}
bool prepare_kernel(Kernel& result,Kind kind,const P& p) {
    if(kind==Kind::Frame)return result.emplace<d::PreparedTemporalWindow>().prepare(window_config(p),p.coefficients);
    if(kind==Kind::Correlation)return result.emplace<d::PreparedSpectralAutocorrelation>().prepare(p.samples,correlation_config(p));
    if(kind==Kind::Fourier)return result.emplace<d::PreparedFourierTempogram>().prepare_size(window_config(p),p.coefficients,p.fft_size?p.fft_size:p.window_length,p.zero_phase);
    if(kind==Kind::Autocorrelation)return result.emplace<d::PreparedAutocorrelationTempogram>().prepare(autocorrelation_config(p),p.coefficients);
    return result.emplace<d::PreparedTempogramSequence>().prepare(sequence_config(p),p.coefficients);
}
std::size_t kernel_bytes(const Kernel& kernel) noexcept {
    return std::visit([](const auto& value)->std::size_t{
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,std::monostate>)return 0;
        else return value.workspace_bytes();
    },kernel);
}
bool resolve_fft(Kind kind,const P& p,std::size_t& result) noexcept {
    if(kind==Kind::Correlation)return d::autocorrelation_fft_size(p.samples,correlation_config(p),result);
    result=kind==Kind::Fourier && p.fft_size?p.fft_size:p.window_length;return true;
}
std::size_t complex_bins(Kind kind,const P& p,std::size_t fft) noexcept {
    if(kind!=Kind::Correlation)return fft/2+1;
    const bool native=p.transform==0 && fft>=8 && fft<=d::max_fft_samples && !(fft&(fft-1));
    return p.transform==2 || native?fft/2+1:fft;
}
bool kernel_storage(Kind kind,const P& p,std::size_t& bytes) noexcept {
    if(kind==Kind::Frame)return d::PreparedTemporalWindow::storage_bytes(window_config(p),p.coefficients,bytes);
    if(kind==Kind::Correlation)return d::PreparedSpectralAutocorrelation::storage_bytes(p.samples,correlation_config(p),bytes);
    if(kind==Kind::Fourier)return d::PreparedFourierTempogram::storage_bytes(window_config(p),p.coefficients,p.fft_size?p.fft_size:p.window_length,p.zero_phase,bytes);
    if(kind==Kind::Autocorrelation)return d::PreparedAutocorrelationTempogram::storage_bytes(autocorrelation_config(p),p.coefficients,bytes);
    return d::PreparedTempogramSequence::storage_bytes(sequence_config(p),p.coefficients,bytes);
}
struct StoragePlan {
    P parameters;Kind kind=Kind::Frame;
    std::size_t input_count=0,row_count=0,bpm_count=0,fft=0,bins=0,kernel_payload=0,own_payload=0;
    bool bpm_lags=false;
};
bool storage_geometry(StoragePlan& plan) noexcept {
    const auto& p=plan.parameters;
    if(!resolve_fft(plan.kind,p,plan.fft) || !numeric_mul(plan.fft,2,plan.row_count))return false;
    plan.row_count=std::max(plan.row_count,p.window_length);plan.bins=complex_bins(plan.kind,p,plan.fft);
    plan.bpm_lags=plan.kind==Kind::Correlation || plan.kind==Kind::Autocorrelation || (plan.kind==Kind::Sequence && p.method==0);
    if(plan.kind!=Kind::Frame)plan.bpm_count=plan.kind==Kind::Correlation?p.lag_count:plan.bpm_lags?p.window_length:plan.bins;
    return true;
}
bool describe_storage(const ResolvedProcessingNode& node,StoragePlan& output) noexcept {
    StoragePlan plan;
    if(node.inputs.size()!=1 || node.inputs[0].id!=0 || !domain::temporal_operation_settings(node,plan.parameters))return false;
    const auto value=node.inputs[0].contract.view();const auto* input=std::get_if<NumericContract>(&value);
    if(!input || !domain::temporal_input_contract(*input,plan.parameters,node.kind))return false;
    if(!plan.parameters.use_first_sample && plan.parameters.frame_index>std::size_t(PTRDIFF_MAX)/plan.parameters.hop)return false;
    plan.kind=operation_kind(node.kind);plan.input_count=std::size_t(input->shape.extents[0].maximum);
    if(!storage_geometry(plan) || !kernel_storage(plan.kind,plan.parameters,plan.kernel_payload))return false;
    if(!storage::add_array(plan.own_payload,plan.input_count,sizeof(float)) || !storage::add_array(plan.own_payload,plan.row_count,sizeof(float)) ||
       !storage::add_array(plan.own_payload,plan.bpm_count,sizeof(float)))return false;
    output=plan;return true;
}
std::ptrdiff_t frame_first(const P& p) noexcept {
    if(p.use_first_sample)return std::ptrdiff_t(p.first_sample);
    const auto offset=p.window_length/2+std::size_t(p.odd_center_ceil && p.window_length%2);
    return std::ptrdiff_t(p.frame_index*p.hop)-(p.center?std::ptrdiff_t(offset):0);
}
double decode_coordinate(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[index*8+byte]))<<(8*byte);
    double value=0;std::memcpy(&value,&bits,8);return value;
}
void encode_coordinate(std::span<std::byte> bytes,std::size_t index,double value) noexcept {
    std::uint64_t bits=0;std::memcpy(&bits,&value,8);
    for(std::size_t byte=0;byte<8;++byte)bytes[index*8+byte]=std::byte((bits>>(8*byte))&255);
}
bool near(double actual,double expected) noexcept {
    return std::isfinite(actual) && std::abs(actual-expected)<=8*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(actual),std::abs(expected)});
}
bool input_coordinates(const NumericView& source,const P& p) noexcept {
    const auto& axis=source.contract->coordinates[0];
    if(axis.mode==CoordinateMode::None)return true;
    const bool seconds=axis.unit==NumericUnit::Seconds;
    const double origin=seconds?p.input_origin_s:0,step=seconds?1/p.input_rate:1;
    if(axis.mode==CoordinateMode::Regular)return axis.origin.denominator && axis.step.denominator &&
        near(double(axis.origin.numerator)/double(axis.origin.denominator),origin) && near(double(axis.step.numerator)/double(axis.step.denominator),step);
    const auto bytes=source.coordinates.axes[0];if(bytes.size()!=source.actual[0]*8)return false;
    for(std::size_t i=0;i<source.actual[0];++i)if(!near(decode_coordinate(bytes,i),origin+double(i)*step))return false;
    return true;
}
Status store_row(MutableNumericView& destination,std::size_t row,std::span<const float> values,std::size_t stride=1,std::size_t component=0) noexcept {
    if(!stride || component>=stride || values.size()%stride || values.size()/stride!=destination.actual[1])return Status::incompatible_contract;
    for(std::size_t column=0;column<destination.actual[1];++column){
        const auto status=store_element(destination,{0,row,column},values[column*stride+component]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status store_values(MutableNumericView& destination,std::span<const float> values) noexcept {
    const auto width=destination.contract->shape.rank==Rank::Matrix?destination.actual[1]:1;
    if(values.size()!=destination.actual[0]*width)return Status::incompatible_contract;
    for(std::size_t i=0;i<values.size();++i){
        const auto status=store_element(destination,{0,i/width,i%width},values[i]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
struct Grid {double origin=0,step=1;std::size_t wrap=SIZE_MAX,period=0;};
double grid_value(Grid grid,std::size_t index) noexcept {
    return grid.origin+(index<grid.wrap?double(index):double(index)-double(grid.period))*grid.step;
}
Grid lag_grid(const P& p,bool skip_zero=false) noexcept {return {skip_zero?1/p.input_rate:0,1/p.input_rate};}
Grid frequency_grid(const P& p,std::size_t fft,bool full=false) noexcept {
    return {0,p.input_rate/double(fft),full?(fft+1)/2:SIZE_MAX,full?fft:0};
}
Grid window_grid(const P& p,bool relative=false) noexcept {
    return {relative?double(frame_first(p))/p.input_rate:p.input_origin_s+double(frame_first(p))/p.input_rate,1/p.input_rate};
}
Grid correlation_grid(const P& p,std::size_t fft,std::size_t bins,PortId port) noexcept {
    if(port==1 || port==2)return frequency_grid(p,fft,port==2 || (p.transform!=2 && bins==fft));
    if(port==4)return {p.input_origin_s,1/p.input_rate};
    return lag_grid(p,port==5);
}
Grid fourier_grid(const P& p,std::size_t fft,PortId port) noexcept {
    if(port==0 || port==3)return frequency_grid(p,fft);
    return port==1?window_grid(p):lag_grid(p);
}
Grid standalone_grid(Kind kind,const P& p,std::size_t fft,std::size_t bins,PortId port) noexcept {
    if(kind==Kind::Frame)return port?lag_grid(p):window_grid(p);
    if(kind==Kind::Correlation)return correlation_grid(p,fft,bins,port);
    if(kind==Kind::Fourier)return fourier_grid(p,fft,port);
    return port==2?window_grid(p):lag_grid(p,port==3);
}
Grid sequence_grid(const P& p,PortId port,std::size_t dimension) noexcept {
    if(port==7)return p.method?frequency_grid(p,p.window_length):lag_grid(p,true);
    if(dimension==0)return {p.input_origin_s,double(p.hop)/p.input_rate};
    if(port==3 || port==4 || (port==0 && p.method))return frequency_grid(p,p.window_length);
    if(port==5 || port==6)return window_grid(p,true);
    return lag_grid(p);
}
Status fill_coordinates(State& state,PortId port,MutableNumericView& destination,std::size_t dimension,Grid grid) noexcept {
    if(destination.contract->coordinates[dimension].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
    const auto count=destination.actual[dimension];auto& bytes=state.numeric_outputs[port].coordinates[dimension];
    if(count>bytes.size()/8)return Status::exhausted;
    for(std::size_t i=0;i<count;++i){const auto value=grid_value(grid,i);if(!std::isfinite(value))return Status::overflow;encode_coordinate(bytes,i,value);}
    destination.coordinates.axes[dimension]={bytes.data(),count*8};return Status::ok;
}
void unavailable(State& state,FrameStamp stamp) noexcept {
    stamp.available=false;
    for(PortId port=0;port<state.numeric_frames.size();++port){
        auto& frame=state.numeric_frames[port];frame.stamp=stamp;
        if(auto* value=std::get_if<NumericView>(&frame.payload)){value->storage={};value->coordinates={};}
        else std::get<StructuredView>(frame.payload).bytes={};
    }
}
Status acf_sequence_row(std::array<MutableNumericView,8>& destination,std::size_t time,const d::AutocorrelationTempogramView& view) noexcept {
    auto status=store_row(destination[1],time,view.raw_autocorrelation);if(status!=Status::ok)return status;
    status=store_row(destination[2],time,view.normalized_values);if(status!=Status::ok)return status;
    return store_row(destination[5],time,view.windowed_input);
}
Status fourier_sequence_row(std::array<MutableNumericView,8>& destination,std::size_t time,const d::FourierTempogramView& view) noexcept {
    auto status=store_row(destination[3],time,view.complex_values,2);if(status!=Status::ok)return status;
    status=store_row(destination[4],time,view.complex_values,2,1);if(status!=Status::ok)return status;
    return store_row(destination[6],time,view.windowed_input);
}
Status publish(State&,std::span<MutableNumericView>,const FrameStamp&,const FrameLimits&) noexcept;
}
struct PreparedTemporalOperation::Impl {
    Kind kind=Kind::Frame;P parameters;
    domain::OwnedProcessingContract input_contract;
    Kernel kernel;
    std::vector<float> input,row,bpm;
    std::size_t fft=0,bins=0,active=0,outputs=0;
    bool bpm_lags=false;

    bool prepare_bpm(std::size_t size) {
        if(kind==Kind::Frame)return true;
        bpm.resize(size);
        return bpm_lags?d::autocorrelation_tempo_frequencies(float(parameters.input_rate),bpm):d::fourier_tempo_frequencies(float(parameters.input_rate),fft,bpm);
    }
    Status write_bpm(MutableNumericView& destination) const noexcept {
        return store_values(destination,std::span(bpm).subspan(std::size_t(bpm_lags)));
    }

    bool configure(const ResolvedProcessingNode& node,const StoragePlan& plan) {
        parameters=plan.parameters;kind=plan.kind;input_contract=node.inputs[0].contract;outputs=domain::temporal_operation_output_count(node.kind);
        if(!prepare_kernel(kernel,kind,parameters))return false;
        fft=plan.fft;bins=plan.bins;bpm_lags=plan.bpm_lags;
        input.resize(plan.input_count);row.resize(plan.row_count);
        parameters.coefficients={};return prepare_bpm(plan.bpm_count);
    }
    bool fits(const StoragePlan& plan) const noexcept {
        std::size_t actual=0;
        return storage::add_array(actual,input.capacity(),sizeof(float)) && storage::add_array(actual,row.capacity(),sizeof(float)) &&
            storage::add_array(actual,bpm.capacity(),sizeof(float)) && actual<=plan.own_payload && kernel_bytes(kernel)<=plan.kernel_payload;
    }
    std::span<const float> values() const noexcept {return std::span(input).first(active);}
    Status read(const NumericView& source,const FrameLimits& limits,bool enabled) noexcept {
        const auto prepared=input_contract.view();
        if(!source.contract || !same_physical_contract(prepared,DataContract{*source.contract}) || source.actual[0]>input.size() || source.actual[1]!=1)return Status::incompatible_contract;
        const auto status=validate_values(source,enabled?ValueDomain::FiniteOnly:ValueDomain::AnyIeee,limits);if(status!=Status::ok)return status;
        if(!input_coordinates(source,parameters))return Status::incompatible_contract;
        if(enabled)for(std::size_t i=0;i<source.actual[0];++i){const auto loaded=load_element(source,{0,i,0},input[i]);if(loaded!=Status::ok)return loaded;}
        active=source.actual[0];return Status::ok;
    }
    std::size_t sequence_frames() const noexcept {return std::get<d::PreparedTempogramSequence>(kernel).frames(active);}
    std::array<std::size_t,2> dimensions(PortId port) const noexcept {
        const auto n=parameters.window_length;
        if(kind==Kind::Frame)return {n,1};
        if(kind==Kind::Correlation){
            if(port==0 || port==5)return {port?parameters.lag_count-1:parameters.lag_count,1};
            return {port==1?bins:fft,port==1 || port==3?2u:1u};
        }
        if(kind==Kind::Fourier)return {port==0 || port==3?bins:port==1?n:fft,port==0?2u:1u};
        if(kind==Kind::Autocorrelation)return {port==3?n-1:n,1};
        if(port==7)return {parameters.method?bins:n-1,1};
        const auto columns=port==0?(parameters.method?bins:n):(port==3 || port==4)?bins:n;
        return {sequence_frames(),columns};
    }
    Status destinations(const PlanNode& node,State& state,std::array<MutableNumericView,8>& result,const FrameLimits& limits) const noexcept {
        for(PortId port=0;port<outputs;++port){
            result[port]=mutable_output(node,state,port);result[port].actual=dimensions(port);
            if(!result[port].actual[0])return Status::unavailable;
            for(std::size_t axis=0;axis<2;++axis){
                const auto grid=kind==Kind::Sequence?sequence_grid(parameters,port,axis):standalone_grid(kind,parameters,fft,bins,port);
                const auto status=fill_coordinates(state,port,result[port],axis,grid);if(status!=Status::ok)return status;
            }
            const auto status=fill_silence(result[port],limits);if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status frame(std::array<MutableNumericView,8>& destination) noexcept {
        auto& operation=std::get<d::PreparedTemporalWindow>(kernel);const auto& p=parameters;
        auto output=std::span(row).first(p.window_length);
        if(!p.use_first_sample && p.frame_index>=operation.frames(active))return Status::unavailable;
        if(!operation.read_at(values(),frame_first(p),output,d::TemporalPadding(p.padding)))return Status::invalid_argument;
        const auto status=store_values(destination[0],output);
        return status==Status::ok?store_values(destination[1],operation.coefficients()):status;
    }
    Status correlation(std::array<MutableNumericView,8>& destination) noexcept {
        auto& operation=std::get<d::PreparedSpectralAutocorrelation>(kernel);
        if(!operation.process(values(),std::span(row).first(parameters.lag_count)))return Status::invalid_argument;
        const auto view=operation.view();
        const std::array<std::span<const float>,5> rows{view.values,view.complex_spectrum,view.power_spectrum,view.complex_inverse,view.padded_input};
        for(PortId port=0;port<rows.size();++port){const auto status=store_values(destination[port],rows[port]);if(status!=Status::ok)return status;}
        return write_bpm(destination[5]);
    }
    Status fourier(std::array<MutableNumericView,8>& destination) noexcept {
        auto& operation=std::get<d::PreparedFourierTempogram>(kernel);const auto& p=parameters;
        if(!p.use_first_sample && p.frame_index>=operation.frames(active))return Status::unavailable;
        if(!operation.process_at(values(),frame_first(p),std::span(row).first(2*bins),std::ptrdiff_t(p.rotation),d::TemporalPadding(p.padding)))return Status::invalid_argument;
        const auto view=operation.view();const std::array<std::span<const float>,3> rows{view.complex_values,view.windowed_input,view.transform_input};
        for(PortId port=0;port<rows.size();++port){const auto status=store_values(destination[port],rows[port]);if(status!=Status::ok)return status;}
        return write_bpm(destination[3]);
    }
    Status autocorrelation(std::array<MutableNumericView,8>& destination) noexcept {
        auto& operation=std::get<d::PreparedAutocorrelationTempogram>(kernel);const auto& p=parameters;
        if(p.frame_index>=operation.frames(active))return Status::unavailable;
        if(!operation.process_frame(values(),p.frame_index,std::span(row).first(p.window_length)))return Status::invalid_argument;
        const auto view=operation.view();const std::array<std::span<const float>,3> rows{view.normalized_values,view.raw_autocorrelation,view.windowed_input};
        for(PortId port=0;port<rows.size();++port){const auto status=store_values(destination[port],rows[port]);if(status!=Status::ok)return status;}
        return write_bpm(destination[3]);
    }
    Status sequence_row(std::array<MutableNumericView,8>& destination,std::size_t time) noexcept {
        const auto view=std::get<d::PreparedTempogramSequence>(kernel).view();const auto method=parameters.method;
        auto status=store_row(destination[0],time,view.values,method==1?2:1);if(status!=Status::ok)return status;
        if(method!=1){
            status=acf_sequence_row(destination,time,view.autocorrelation);if(status!=Status::ok)return status;
        }
        if(method!=0)return fourier_sequence_row(destination,time,view.fourier);
        return Status::ok;
    }
    Status sequence(std::array<MutableNumericView,8>& destination) noexcept {
        auto& operation=std::get<d::PreparedTempogramSequence>(kernel);
        for(std::size_t time=0;time<sequence_frames();++time){
            if(!operation.process_frame(values(),time,std::span(row).first(operation.columns())))return Status::invalid_argument;
            const auto status=sequence_row(destination,time);if(status!=Status::ok)return status;
        }
        return write_bpm(destination[7]);
    }
    Status calculate(std::array<MutableNumericView,8>& destination) noexcept {
        if(kind==Kind::Frame)return frame(destination);
        if(kind==Kind::Correlation)return correlation(destination);
        if(kind==Kind::Fourier)return fourier(destination);
        return kind==Kind::Autocorrelation?autocorrelation(destination):sequence(destination);
    }
    Status run(const PlanNode& node,State& state,const NumericView& source,const FrameStamp& stamp,const FrameLimits& limits) noexcept {
        auto status=read(source,limits,node.node.enabled);if(status!=Status::ok)return status;
        std::array<MutableNumericView,8> destination{};
        status=destinations(node,state,destination,limits);if(status!=Status::ok)return status;
        if(node.node.enabled){status=calculate(destination);if(status!=Status::ok)return status;}
        return publish(state,std::span(destination).first(outputs),stamp,limits);
    }
};
namespace {
Status publish(State& state,std::span<MutableNumericView> destinations,const FrameStamp& stamp,const FrameLimits& limits) noexcept {
    for(auto& value:destinations){const auto status=validate_values(value.view(),ValueDomain::FiniteOnly,limits);if(status!=Status::ok)return status;}
    for(std::size_t port=0;port<destinations.size();++port){
        state.numeric_outputs[port].actual=destinations[port].actual;state.numeric_frames[port]={stamp,destinations[port].view()};
    }
    return Status::ok;
}
}
PreparedTemporalOperation::PreparedTemporalOperation()=default;
PreparedTemporalOperation::~PreparedTemporalOperation()=default;
PreparedTemporalOperation::PreparedTemporalOperation(const PreparedTemporalOperation& other):value_(other.value_?std::make_unique<Impl>(*other.value_):nullptr){}
PreparedTemporalOperation& PreparedTemporalOperation::operator=(const PreparedTemporalOperation& other){if(this!=&other)value_=other.value_?std::make_unique<Impl>(*other.value_):nullptr;return *this;}
PreparedTemporalOperation::PreparedTemporalOperation(PreparedTemporalOperation&&) noexcept=default;
PreparedTemporalOperation& PreparedTemporalOperation::operator=(PreparedTemporalOperation&&) noexcept=default;
void PreparedTemporalOperation::reset() noexcept {if(value_)value_->active=0;}
bool PreparedTemporalOperation::prepare(const ResolvedProcessingNode& node) {
    StoragePlan plan;if(!describe_storage(node,plan))return false;
    auto next=std::make_unique<Impl>();if(!next->configure(node,plan) || !next->fits(plan))return false;
    value_=std::move(next);return true;
}
Status PreparedTemporalOperation::execute(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits) noexcept {
    unavailable(state,current.stamp);reset();
    if(!value_ || state.numeric_frames.size()!=value_->outputs || state.numeric_outputs.size()!=value_->outputs || node.numeric_outputs.size()!=value_->outputs)return Status::invalid_argument;
    if(!current.stamp.available)return Status::unavailable;
    const auto valid=validate_view(current,limits);if(valid!=Status::ok)return valid;
    const auto* source=std::get_if<NumericView>(&current.payload);if(!source)return Status::incompatible_contract;
    return value_->run(node,state,*source,current.stamp,limits);
}
bool PreparedTemporalOperation::resources(const ResolvedProcessingNode& node,ProcessingResources& result) noexcept {
    StoragePlan plan;if(!describe_storage(node,plan))return false;
    return numeric_add(result.plan_bytes,sizeof(Impl)) && numeric_add(result.scratch_bytes,plan.kernel_payload) && numeric_add(result.payload_bytes,plan.own_payload);
}
}
