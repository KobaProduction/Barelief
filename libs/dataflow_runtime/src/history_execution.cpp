#include "history_execution.h"
#include "frame_access.h"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace dataflow_runtime::detail {
namespace {
class HistoryWriter {
    const PlanNode& node;
    State& state;
    const DataFrameView& current;
    MutableNumericView output;
    const NumericView& source;
    const std::size_t capacity,physical;
    bool scalar() const noexcept {return output.contract->shape.rank==Rank::Vector;}
    Status copy_element(const NumericView& zero,std::size_t plane,std::size_t column) noexcept {
                std::uint64_t bits=0;
                const auto input_index=scalar()?ElementIndex{plane,0,0}:ElementIndex{plane,column,0};
                auto status=load_bits(node.node.enabled?source:zero,input_index,source.contract->dtype,bits);
                if(status!=Status::ok)return status;
                const auto output_index=scalar()?ElementIndex{plane,physical,0}:ElementIndex{plane,physical,column};
                status=store_bits(output,output_index,output.contract->dtype,bits);
                if(status!=Status::ok)return status;
        return Status::ok;
    }
    Status copy_row() noexcept {
        const auto zero=std::get<NumericView>(zero_value(node.numeric_input,state.numeric_zero));
        const auto width=scalar()?std::size_t{1}:source.actual[0];
        for(std::size_t plane=0;plane<output.planes.size();++plane)
            for(std::size_t column=0;column<width;++column){
                const auto status=copy_element(zero,plane,column);if(status!=Status::ok)return status;
            }
        return Status::ok;
    }
    void advance() noexcept {
        state.frames=capacity;state.progress=std::min(state.progress+1,capacity);
        state.oldest=(state.oldest+1)%capacity;++state.row_ordinal;
        output.actual={state.frames,scalar()?std::size_t{1}:source.actual[0]};
        output.ring={capacity,state.oldest,{}};
    }
    void feature_coordinates() noexcept {
        if(scalar() || output.contract->coordinates[1].mode!=CoordinateMode::ExplicitFloat64LE)return;
        auto& coordinates=state.numeric_outputs[0].coordinates[1];
        std::copy(source.coordinates.axes[0].begin(),source.coordinates.axes[0].end(),coordinates.begin());
        output.coordinates.axes[1]={coordinates.data(),source.actual[0]*8};
    }
    Status time_coordinates() noexcept {
        if(output.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)return Status::ok;
        const double coordinate=output.contract->coordinates[0].unit==NumericUnit::Frames?
            double(state.row_ordinal):double(current.stamp.timestamp_ticks)*
            double(source.contract->clock.tick_seconds.numerator)/double(source.contract->clock.tick_seconds.denominator);
        if(!std::isfinite(coordinate))return Status::overflow;
        std::uint64_t bits=0;std::memcpy(&bits,&coordinate,8);
        auto& coordinates=state.numeric_outputs[0].coordinates[0];
        for(std::size_t byte=0;byte<8;++byte)
            coordinates[physical*8+byte]=coordinates[(physical+capacity)*8+byte]=std::byte((bits>>(byte*8))&255);
        output.coordinates.axes[0]={coordinates.data()+state.oldest*8,state.frames*8};
        return Status::ok;
    }
    Status publish() noexcept {
        auto stamp=current.stamp;stamp.sequence=++state.output_sequence;
        state.numeric_frames[0]={stamp,output.view()};
        auto count=mutable_output(node,state,1);
        const auto status=store_element(count,{0,0,0},std::uint64_t(state.progress));
        if(status!=Status::ok)return status;
        state.numeric_frames[1]={stamp,count.view()};
        return Status::ok;
    }
public:
    HistoryWriter(const PlanNode& prepared,State& context,const DataFrameView& input) noexcept
        :node(prepared),state(context),current(input),output(mutable_output(node,state,0)),
         source(std::get<NumericView>(current.payload)),capacity(output.actual[0]),physical(state.oldest){}
    HistoryAppendResult append() noexcept {
        output.ring={capacity,0,{}};
        if(state.row_ordinal==UINT64_MAX || state.output_sequence==UINT64_MAX)return {Status::overflow,false};
        {const auto status=copy_row();if(status!=Status::ok)return {status,true};}
        advance();feature_coordinates();
        {const auto status=time_coordinates();if(status!=Status::ok)return {status,true};}
        const auto status=publish();return {status,status!=Status::ok};
    }
};
}
HistoryAppendResult append_history(const PlanNode& node,State& state,const DataFrameView& current,const FrameLimits&) noexcept {
    return HistoryWriter(node,state,current).append();
}
}
