#include "conversion_execution.h"
#include "frame_access.h"
#include "preparation_support.h"
#include <algorithm>
namespace dataflow_runtime::detail {
namespace {
class ConversionExecution {
    const PlanNode& node;
    State& state;
    const DataFrameView& current;
    const FrameLimits& limits;
    const std::optional<ConversionHistoryOrigin> origin;
    const NumericView& source;
    MutableNumericView output;
    void reset_history() noexcept {
        state.history_origin_valid=false;state.frames=state.oldest=state.progress=0;state.row_ordinal=0;
    }
    void copy_time_coordinate(std::size_t logical,std::size_t physical) noexcept {
        if(source.coordinates.axes[0].empty())return;
        auto& coordinates=state.numeric_outputs[0].coordinates[0];const auto coordinate=source.coordinates.axes[0].subspan(logical*8,8);
        std::copy(coordinate.begin(),coordinate.end(),coordinates.begin()+physical*8);
        std::copy(coordinate.begin(),coordinate.end(),coordinates.begin()+(physical+source.ring.capacity)*8);
    }
    void row_planes(std::size_t physical) noexcept {
        for(std::size_t plane=0;plane<output.planes.size();++plane)state.row_planes[plane]={output.planes[plane].offset+physical*output.planes[plane].stride_bytes[0],{output.planes[plane].stride_bytes[1],0}};
    }
    ConversionExecutionResult convert_row(std::size_t logical) noexcept {
        const auto& plan=node.input_gather->gather;auto contract=node.row_conversion->output.contract();NumericGatherOutput row;
        NumericGatherDestination destination{{&plan.output_contract(),state.input_gather_payload,plan.output_planes(),{source.actual[1],1},{},{}},state.input_gather_metadata};
        const auto gathered=gather_numeric_frame(plan,current,{false,logical},destination,state.input_gather_scratch,row);if(gathered!=Status::ok)return {gathered,true};
        const auto physical=(source.ring.oldest+logical)%source.ring.capacity;row_planes(physical);
        MutableNumericView converted{&contract,output.storage,state.row_planes,{source.actual[1],1},{},std::get<NumericView>(row.frame.payload).coordinates};
        const auto status=node.node.enabled?numeric_kernel_status(dsp_kernels::convert(*node.row_conversion,std::get<NumericView>(row.frame.payload),converted,state.numeric_scratch).status):fill_silence(converted,limits);
        if(status==Status::ok)copy_time_coordinate(logical,physical);
        return {status,false};
    }
    void retain_history() noexcept {
        state.frames=source.actual[0];state.oldest=source.ring.oldest;state.row_ordinal=origin->ordinal;state.progress=origin->progress;
        state.history_origin=origin->node;state.history_origin_valid=true;
    }
    ConversionExecutionResult convert_changed_rows(std::size_t changed) noexcept {
        ConversionExecutionResult result;
        for(std::size_t logical=source.actual[0]-changed;logical<source.actual[0];++logical){result=convert_row(logical);if(result.status!=Status::ok)break;}
        return result;
    }
    ConversionExecutionResult convert_history() noexcept {
        if(!origin)return {Status::unavailable,true};
        const bool rebase=!state.history_origin_valid || state.history_origin!=origin->node || current.stamp.discontinuity || origin->ordinal<state.row_ordinal;
        const auto changed=rebase?source.actual[0]:std::size_t(std::min(std::uint64_t(source.actual[0]),origin->ordinal-state.row_ordinal));
        const auto result=convert_changed_rows(changed);
        if(result.reject)return result;
        if(result.status==Status::ok)retain_history();
        output.ring.silence_mask={};
        if(!source.coordinates.axes[0].empty())output.coordinates.axes[0]={state.numeric_outputs[0].coordinates[0].data()+state.oldest*8,state.frames*8};
        return result;
    }
    void feature_coordinates() noexcept {
        const auto feature=source.ring.capacity?1u:0u;if(source.coordinates.axes[feature].empty())return;
        auto& coordinates=state.numeric_outputs[0].coordinates[feature];
        std::copy(source.coordinates.axes[feature].begin(),source.coordinates.axes[feature].end(),coordinates.begin());output.coordinates.axes[feature]={coordinates.data(),source.coordinates.axes[feature].size()};
    }
public:
    ConversionExecution(const PlanNode& prepared,State& context,const DataFrameView& input,const FrameLimits& bounds,std::optional<ConversionHistoryOrigin> producer) noexcept
        :node(prepared),state(context),current(input),limits(bounds),origin(producer),source(std::get<NumericView>(current.payload)),output(mutable_output(node,state,0)){}
    ConversionExecutionResult process() noexcept {
        output.actual=source.actual;output.coordinates=source.coordinates;output.ring=source.ring;ConversionExecutionResult result;
        if(source.ring.capacity)result=convert_history();
        else {reset_history();result.status=node.node.enabled?numeric_kernel_status(dsp_kernels::convert(*node.conversion,source,output,state.numeric_scratch).status):fill_silence(output,limits);}
        if(result.reject)return result;
        feature_coordinates();if(result.status==Status::ok)state.numeric_frames[0]={current.stamp,output.view()};return result;
    }
};
}
ConversionExecutionResult execute_conversion(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits& limits,std::optional<ConversionHistoryOrigin> origin) noexcept {
    return ConversionExecution(node,state,current,limits,origin).process();
}
}
