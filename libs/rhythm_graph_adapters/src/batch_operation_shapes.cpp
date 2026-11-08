// SPDX-License-Identifier: AGPL-3.0-or-later
#include "batch_operation_plan.h"
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
BatchShape scalar(DType type=DType::Float32) noexcept {return {Rank::Scalar,1,1,type};}
BatchShape vector(std::size_t size,bool variable=false,ValueDomain values=ValueDomain::FiniteOnly) noexcept {return {Rank::Vector,std::max(std::size_t{1},size),1,DType::Float32,variable,values};}
BatchShape peaks(std::size_t size) noexcept {return {Rank::Matrix,std::max(std::size_t{1},size),2,DType::Float32,true};}
void summary(BatchPlan& p) noexcept {p.shape[0]=p.shape[1]=p.shape[2]=scalar();p.shape[3]=vector(3);}
void pulse_train_shape(BatchPlan& p) noexcept {p.outputs=4;p.shape[0]=scalar();p.shape[1]=p.shape[2]=vector(p.secondary,true);p.shape[3]=vector(p.secondary,true,ValueDomain::AnyIeee);}
void consensus_shape(BatchPlan& p) noexcept {summary(p);p.outputs=7;p.shape[4]=vector(414);p.shape[5]=p.shape[6]=scalar(DType::UInt64);}
bool pulse_shapes(BatchPlan& p) noexcept {
    switch(p.kind){
    case BatchKind::PulseTrain:pulse_train_shape(p);return true;
    case BatchKind::LagConsensus:consensus_shape(p);return true;
    default:return false;}
}
bool novelty_shapes(BatchPlan& p,std::size_t bins) noexcept {
    switch(p.kind){
    case BatchKind::FourierCandidates:
        if(p.histogram.maximum_peaks==SIZE_MAX)return false;
        p.outputs=3;p.shape[0]=peaks(p.histogram.maximum_peaks);p.shape[1]=peaks(p.histogram.maximum_peaks+1);p.shape[2]=vector(bins);return true;
    case BatchKind::WeightedHistogram:p.outputs=3;p.shape[0]=vector(bins);p.shape[1]=peaks(bins);p.shape[2]=scalar(DType::UInt8);return true;
    default:return false;}
}
void path_shape(BatchPlan& p) noexcept {p.outputs=3;p.shape[0]=vector(p.rows,true);p.shape[1]=scalar();p.shape[2]=scalar(DType::UInt8);}
}
namespace {
bool final_batch_shape(BatchPlan& p) noexcept {
    if(p.kind==BatchKind::FramePath){path_shape(p);return true;}
    return false;
}
}
bool batch_operation_shapes(const dataflow_runtime::NumericOperationNodeView&,BatchPlan& p) noexcept {
    if(pulse_shapes(p))return true;
    const float maximum=std::ceil(p.histogram.path.maximum_bpm);
    if(!std::isfinite(maximum) || maximum<0 || double(maximum)>=double(SIZE_MAX/sizeof(float))-2)return false;
    const auto bins=std::size_t(maximum)+1;
    return novelty_shapes(p,bins) || final_batch_shape(p);
}
}
