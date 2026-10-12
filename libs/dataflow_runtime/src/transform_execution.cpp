#include "transform_execution.h"
#include "preparation_support.h"
#include "signal_time.h"
#include "frame_access.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace dataflow_runtime::detail {
TransformResult execute_transform(const PlanNode& n,State& state,const DataFrameView& current,const FrameLimits& limits,TransformDelivery delivery) noexcept {
    Status status=Status::ok;
    const auto& src=std::get<NumericView>(current.payload);auto dst=mutable_output(n,state,0);
    const auto columns=src.actual[0],count=dst.actual[0];
    if(n.node.kind==NodeKind::PcmResampler){
        auto& resampler=std::get<ResamplerKernel>(state.kernel).operation();
        const auto input_stride=std::size_t(src.contract->shape.extents[0].maximum);
        for(std::size_t p=0;p<src.planes.size();++p)for(std::size_t j=0;j<columns;++j){
            float value=0;
            if(n.node.enabled){status=load_element(src,{p,j,0},value);if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
            state.left[p*input_stride+j]=value;
        }
        if(!resampler.anchored()){
            std::int64_t converted=0;
            if(!convert_rate_ticks(current.stamp.timestamp_ticks,src.contract->clock.sample_rate,dst.contract->clock.sample_rate,converted))return {delivery.reject(delivery.user,Status::overflow),true};
            resampler.anchor(converted);
        }
        const auto ordinal=resampler.produced();std::size_t written=0;
        if(!resampler.process(state.left,columns,input_stride,state.values,count,written)){state.pending_discontinuity=true;return {delivery.reject(delivery.user,Status::overflow),true};}
        if(!resampler.advance_input(current.stamp.timestamp_ticks,columns)){resampler.reset();state.pending_discontinuity=true;return {delivery.reject(delivery.user,Status::overflow),true};}
        if(!written)return {Status::ok,true};
        if(ordinal>std::uint64_t(INT64_MAX) || resampler.anchor_ticks()>INT64_MAX-std::int64_t(ordinal) || state.output_sequence==UINT64_MAX)return {delivery.reject(delivery.user,Status::overflow),true};
        dst.actual[0]=written;
        for(std::size_t p=0;p<dst.planes.size();++p)for(std::size_t j=0;j<written;++j){
            status=store_element(dst,{p,j,0},n.node.enabled?state.values[p*count+j]:0.f);if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};
        }
        const auto first_tick=resampler.anchor_ticks()+std::int64_t(ordinal);
        if(first_tick>INT64_MAX-std::int64_t(written-1))return {delivery.reject(delivery.user,Status::overflow),true};
        auto stamp=current.stamp;stamp.timestamp_ticks=first_tick;stamp.sequence=++state.output_sequence;
        stamp.sample_begin=first_tick;stamp.sample_count=written;stamp.has_sample_interval=true;
        if(src.contract->clock.sample_rate!=dst.contract->clock.sample_rate){
            const auto denominator=current.stamp.latency_seconds.denominator;const auto rate=std::uint64_t(src.contract->clock.sample_rate);const auto common=std::gcd(denominator,rate);
            const auto left_scale=rate/common,right_scale=denominator/common;
            if(denominator>UINT64_MAX/left_scale || std::uint64_t(current.stamp.latency_seconds.numerator)>std::uint64_t(INT64_MAX)/left_scale || right_scale>std::uint64_t(INT64_MAX)/63)return {delivery.reject(delivery.user,Status::overflow),true};
            const auto left=std::uint64_t(current.stamp.latency_seconds.numerator)*left_scale,right=63*right_scale;if(left>std::uint64_t(INT64_MAX)-right)return {delivery.reject(delivery.user,Status::overflow),true};
            const auto n=left+right,d=denominator*left_scale,reduce=std::gcd(n,d);stamp.latency_seconds={std::int64_t(n/reduce),d/reduce};
        }
        std::uint64_t diagnostic=0;if(!divide_products(std::uint64_t(stamp.latency_seconds.numerator),dst.contract->clock.sample_rate,stamp.latency_seconds.denominator,1,diagnostic))return {delivery.reject(delivery.user,Status::overflow),true};stamp.latency_samples=diagnostic;
        stamp.discontinuity=stamp.discontinuity || state.pending_discontinuity;state.pending_discontinuity=false;
        if(!stamp.source_sequence)stamp.source_sequence=current.stamp.sequence;
        state.numeric_frames[0]={stamp,dst.view()};delivery.emit(delivery.user);return {Status::ok,true};
    }
    auto explicit_coordinate=[](std::span<const std::byte> bytes,std::size_t at){std::uint64_t bits=0;
        for(std::size_t byte=0;byte<8;++byte)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[at*8+byte]))<<(byte*8);
        double value=0;std::memcpy(&value,&bits,8);return value;};
    auto write_coordinate=[](std::span<std::byte> bytes,std::size_t at,double value){std::uint64_t bits=0;std::memcpy(&bits,&value,8);
        for(std::size_t byte=0;byte<8;++byte)bytes[at*8+byte]=std::byte((bits>>(byte*8))&255);};
    if(src.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE &&
       (n.node.kind==NodeKind::BarkWeighting || n.node.kind==NodeKind::Mel)){
        double previous_hz=-1;
        for(std::size_t j=0;j<columns;++j){const auto hz=explicit_coordinate(src.coordinates.axes[0],j);
            if(hz<0 || hz<=previous_hz || hz>double(src.contract->clock.sample_rate)*.5)return {delivery.reject(delivery.user,Status::incompatible_contract),true};
            previous_hz=hz;}}
    if(dst.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE && n.node.kind!=NodeKind::Window){
        auto& coords=state.numeric_outputs[0].coordinates[0];
        if(n.node.kind==NodeKind::StereoFFT){for(std::size_t j=0;j<count;++j)write_coordinate(coords,j,double(j)*src.contract->clock.sample_rate/double(columns));}
        else if(n.node.kind==NodeKind::Mel){for(std::size_t j=0;j<count;++j)write_coordinate(coords,j,dst.contract->coordinates[0].unit==NumericUnit::Hertz?state.mel_boundaries[j+1]:double(j));}
        else if(src.coordinates.axes[0].size()==count*8)std::copy(src.coordinates.axes[0].begin(),src.coordinates.axes[0].end(),coords.begin());
        else return {delivery.reject(delivery.user,Status::incompatible_contract),true};
        dst.coordinates.axes[0]={coords.data(),count*8};}
    if(n.node.kind==NodeKind::Window){std::size_t offset=0;
        const bool direct=direct_window(*dst.contract);
        if(direct)dst.storage=std::as_writable_bytes(std::span(state.pcm));
        while(offset<columns){if(!state.hop){state.hop=n.hop_base;
                if(state.remainder>=n.hop_denominator-n.hop_fraction){++state.hop;state.remainder-=n.hop_denominator-n.hop_fraction;}
                else state.remainder+=n.hop_fraction;}
            const auto taken=std::min(columns-offset,state.hop-state.progress),kept=std::min(count,taken);
            if(dst.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE){auto& coords=state.numeric_outputs[0].coordinates[0];
                std::move(coords.begin()+kept*8,coords.end(),coords.begin());
                const auto source=src.coordinates.axes[0].subspan((offset+taken-kept)*8,kept*8);
                std::copy(source.begin(),source.end(),coords.begin()+(count-kept)*8);}
            if(n.node.enabled)for(std::size_t plane=0;plane<src.planes.size();++plane){auto window=std::span(state.pcm).subspan(plane*count,count);
                std::move(window.begin()+kept,window.end(),window.begin());
                for(std::size_t j=0;j<kept;++j){status=load_element(src,{plane,offset+taken-kept+j,0},window[count-kept+j]);if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
            }
            offset+=taken;state.progress+=taken;
            if(state.progress==state.hop){
                if(state.output_sequence==UINT64_MAX)return {delivery.reject(delivery.user,Status::overflow),true};
                const auto& clock=src.contract->clock;
                std::uint64_t ticks=0;
                if(clock.tick_seconds.numerator<=0 || !clock.sample_rate || !divide_products(std::uint64_t(offset),clock.tick_seconds.denominator,
                    clock.sample_rate,std::uint64_t(clock.tick_seconds.numerator),ticks))return {delivery.reject(delivery.user,Status::overflow),true};
                if(ticks>std::uint64_t(INT64_MAX) || current.stamp.timestamp_ticks>INT64_MAX-std::int64_t(ticks))return {delivery.reject(delivery.user,Status::overflow),true};
                if(!direct)for(std::size_t plane=0;plane<dst.planes.size();++plane)for(std::size_t j=0;j<count;++j){
                    status=store_element(dst,{plane,j,0},n.node.enabled?state.pcm[plane*count+j]:0.f);if(status!=Status::ok)return {delivery.reject(delivery.user,status),true};}
                auto stamp=current.stamp;stamp.sequence=++state.output_sequence;
                if(!stamp.source_sequence)stamp.source_sequence=current.stamp.sequence;
                stamp.timestamp_ticks+=std::int64_t(ticks);
                if(current.stamp.has_sample_interval){if(current.stamp.sample_begin>INT64_MAX-std::int64_t(offset))return {delivery.reject(delivery.user,Status::overflow),true};const auto end=current.stamp.sample_begin+std::int64_t(offset);if(end<INT64_MIN+std::int64_t(count))return {delivery.reject(delivery.user,Status::overflow),true};stamp.sample_begin=end-std::int64_t(count);stamp.sample_count=count;stamp.has_sample_interval=true;stamp.timestamp_ticks=end-1;}
                stamp.discontinuity=current.stamp.discontinuity || state.pending_discontinuity;
                auto value=dst.view();
                // The rolling payload stays stable until synchronous descendants
                // and observers finish this emission. Disabled windows borrow the
                // immutable silence pool without modifying the rolling samples.
                if(direct && !n.node.enabled){value.storage=std::as_const(state.numeric_outputs[0]).storage();value.semantic_silence=true;}
                state.pending_discontinuity=false;state.numeric_frames[0]={stamp,value};delivery.emit(delivery.user);state.progress=state.hop=0;
            }
        }
        return {Status::ok,true};
    }
    status=Status::ok;
    if(!n.node.enabled)status=fill_silence(dst,limits);
    else status=validate_values(src,ValueDomain::FiniteOnly,limits);
    if(status==Status::ok && n.node.enabled){
        if(n.node.kind==NodeKind::StereoFFT){
            for(std::size_t p=0;p<dst.planes.size() && status==Status::ok;++p)status=std::get<dsp_kernels::FFT>(state.kernel).process_plane(src,dst,p);
        }else if(n.node.kind==NodeKind::Mel){
            for(std::size_t p=0;p<dst.planes.size() && status==Status::ok;++p){
                for(std::size_t j=0;j<columns;++j){status=load_element(src,{p,j,0},state.left[j]);if(status!=Status::ok)break;}
                if(status!=Status::ok)break;
                bool processed=true;
                if(src.contract->coordinates[0].mode!=CoordinateMode::ExplicitFloat64LE)processed=std::get<dsp_kernels::MelFilterBank>(state.kernel).process(state.left,state.values);
                else for(std::size_t band=0;band<count;++band){double energy=0;
                    const auto left=state.mel_boundaries[band],center=state.mel_boundaries[band+1],right=state.mel_boundaries[band+2];
                    for(std::size_t j=0;j<columns;++j){const auto hz=explicit_coordinate(src.coordinates.axes[0],j);
                        const auto weight=hz<left || hz>right?0.:hz<center?(hz-left)/(center-left):(right-hz)/(right-center);
                        energy+=double(state.left[j])*weight;}
                    state.values[band]=float(energy);}
                if(!processed){status=Status::unavailable;break;}
                for(std::size_t j=0;j<count;++j){if(!std::isfinite(state.values[j])){status=Status::overflow;break;}
                    status=store_element(dst,{p,j,0},state.values[j]);if(status!=Status::ok)break;}
            }
        }else for(std::size_t p=0;p<dst.planes.size() && status==Status::ok;++p)for(std::size_t j=0;j<count;++j){float value=0;
            status=load_element(src,{p,j,0},value);if(status!=Status::ok)break;
            if(n.node.kind==NodeKind::WindowFunction)value*=state.coefficients[j];
            else if(n.node.kind==NodeKind::BarkWeighting){const auto hz=src.contract->coordinates[0].mode==CoordinateMode::ExplicitFloat64LE?explicit_coordinate(src.coordinates.axes[0],j):double(j)*src.contract->clock.sample_rate/double(columns*2);
                value*=float(7.*std::asinh(hz/650.));}
            else if(n.node.kind==NodeKind::SpectrumGain)value*=n.gain;
            else if(n.node.kind==NodeKind::SpectrumMerge){float right=0;status=load_element(src,{1,j,0},right);if(status!=Status::ok)break;
                if(n.merge_mode==0)value=std::midpoint(value,right);
                else if(n.merge_mode==1)value=std::max(value,right);
                else {const float scale=std::max(std::abs(value),std::abs(right));value=scale?float(double(scale)*std::hypot(double(value)/scale,double(right)/scale)/std::sqrt(2.)):0.f;};}
            if(!std::isfinite(value)){status=Status::overflow;break;}
            status=store_element(dst,{p,j,0},n.node.enabled?value:0.f);if(status!=Status::ok)break;
        }
    }
    if(status==Status::ok)state.numeric_frames[0]={current.stamp,dst.view()};
    return {status,false};
}
}
