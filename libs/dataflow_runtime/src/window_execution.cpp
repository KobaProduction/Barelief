#include "window_execution.h"
#include "frame_access.h"
#include <algorithm>
namespace dataflow_runtime::detail {
namespace {
class WindowExecution {
    State& state;
    const DataFrameView& current;
    const NumericView& source;
    MutableNumericView output;
    const std::size_t columns,count,kept;
    Status copy_element(const NumericView& input,std::size_t plane,std::size_t from,std::size_t to) noexcept {
        std::uint64_t bits=0;
        const auto status=load_bits(input,{plane,from,0},input.contract->dtype,bits);
        return status==Status::ok?store_bits(output,{plane,to,0},output.contract->dtype,bits):status;
    }
    Status shift_plane(std::size_t plane) noexcept {
        const auto previous=output.view();
        for(std::size_t column=0;column<count-kept;++column){
            const auto status=copy_element(previous,plane,column+kept,column);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status append_plane(std::size_t plane) noexcept {
        for(std::size_t column=0;column<kept;++column){
            const auto status=copy_element(source,plane,columns-kept+column,count-kept+column);
            if(status!=Status::ok)return status;
        }
        return Status::ok;
    }
    Status retain() noexcept {
        for(std::size_t plane=0;plane<output.planes.size();++plane){
            const auto shifted=shift_plane(plane);if(shifted!=Status::ok)return shifted;
            const auto appended=append_plane(plane);if(appended!=Status::ok)return appended;
        }
        return Status::ok;
    }
    Status publish() noexcept {
        auto stamp=current.stamp;
        if(stamp.has_sample_interval){
            if(stamp.sample_begin>INT64_MAX-std::int64_t(columns))return Status::overflow;
            const auto end=stamp.sample_begin+std::int64_t(columns);
            if(end<INT64_MIN+std::int64_t(count))return Status::overflow;
            stamp.sample_begin=end-std::int64_t(count);stamp.sample_count=count;
        }
        state.numeric_frames[0]={stamp,output.view()};return Status::ok;
    }
public:
    WindowExecution(const PlanNode& node,State& context,const DataFrameView& input) noexcept
        :state(context),current(input),source(std::get<NumericView>(input.payload)),output(mutable_output(node,state,0)),
         columns(source.actual[0]),count(output.actual[0]),kept(std::min(columns,count)){}
    Status process() noexcept {
        const auto status=retain();return status==Status::ok?publish():status;
    }
};
}
TransformResult execute_window(const PlanNode& node,State& state,const DataFrameView& current,TransformDelivery delivery) noexcept {
    const auto status=WindowExecution(node,state,current).process();
    return status==Status::ok?TransformResult{Status::ok,false}:TransformResult{delivery.reject(delivery.user,status),true};
}
}
