// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tracking_operations.h"
#include <dsp_kernels/prepared_storage.h>
#include <cmath>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;using namespace rhythm_reference_ports;
namespace {
BatchShape scalar(DType t=DType::Float32) noexcept {return {Rank::Scalar,1,1,t};}
BatchShape vector(std::size_t n,DType t=DType::Float32) noexcept {return {Rank::Vector,std::max(std::size_t{1},n),1,t,true};}
BatchShape matrix(std::size_t n,std::size_t c,DType t=DType::Float32) noexcept {return {Rank::Matrix,std::max(std::size_t{1},n),c,t,true};}
bool basic_global_shapes(TrackingPlan& p) noexcept {
    switch(p.kind){
    case TrackingKind::InformationGain:p.outputs=1;p.shape[0]=scalar();return true;
    case TrackingKind::Erb:p.outputs=3;p.shape[0]=p.shape[1]=vector(p.erb.bands);p.shape[2]=matrix(p.erb.bands,p.rows);return true;
    default:return false;
    }
}
bool global_shapes(TrackingPlan& p) noexcept {
    return basic_global_shapes(p);
}
bool path_shapes(TrackingPlan& p) noexcept {
    p.outputs=4;p.shape[0]=p.shape[1]=vector(p.rows);p.shape[2]=matrix(p.rows,p.path.hop);
    p.shape[3]=vector(p.rows,DType::UInt32);return true;
}
bool agreement_shapes(TrackingPlan& p) noexcept {
    p.outputs=4;p.shape[0]=scalar(DType::UInt64);p.shape[1]=scalar();p.shape[2]=vector(p.rows);p.shape[3]=matrix(p.rows,p.rows);return true;
}
bool basic_period_shapes(TrackingPlan& p) noexcept {
    switch(p.kind){
    case TrackingKind::Renewal:p.outputs=3;p.shape[0]=vector(p.rows,DType::UInt8);p.shape[1]=vector(p.rows,DType::UInt32);p.shape[2]=vector(p.rows);return true;
    default:return false;
    }
}
bool period_shapes(TrackingPlan& p) noexcept {
    if(p.kind==TrackingKind::Agreement)return agreement_shapes(p);
    if(p.kind==TrackingKind::PeriodPath)return path_shapes(p);
    return basic_period_shapes(p);
}
void phase_tick_shapes(TrackingPlan& p) noexcept {
    p.outputs=5;p.shape[0]=vector(p.maximum_ticks);p.shape[1]=vector(p.maximum_matches);
    p.shape[2]=scalar(DType::Int32);p.shape[3]=scalar(DType::UInt64);p.shape[4]=scalar(DType::UInt8);
}
void band_feature_shapes(TrackingPlan& p) noexcept {
    p.outputs=2;p.shape[0]=vector(p.rows);p.shape[1]=scalar();
}
bool tempo_feature_shapes(TrackingPlan& p) noexcept {
    switch(p.kind){
    case TrackingKind::BandFeatures:band_feature_shapes(p);return true;
    case TrackingKind::PhaseTicks:phase_tick_shapes(p);return true;
    default:return false;
    }
}
void silence_shapes(TrackingPlan& p) noexcept {
    p.outputs=4;p.shape[0]=vector(p.rows);p.shape[1]=p.shape[2]=p.shape[3]=scalar(DType::UInt64);
}
void lag_pool_shapes(TrackingPlan& p) noexcept {
    p.outputs=4;p.shape[0]=p.shape[1]=vector(p.maximum_matches);p.shape[2]=p.shape[3]=scalar(DType::UInt64);
}
void tick_refinement_shapes(TrackingPlan& p) noexcept {
    p.outputs=3;p.shape[0]=p.shape[1]=vector(p.maximum_ticks);p.shape[2]=scalar(DType::UInt8);
}
void lag_vote_shapes(TrackingPlan& p) noexcept {
    p.outputs=2;p.shape[0]=scalar();p.shape[1]=vector(p.rows);
}
bool observation_shapes(TrackingPlan& p) noexcept {
    switch(p.kind){
    case TrackingKind::Silence:silence_shapes(p);return true;
    case TrackingKind::LagPool:lag_pool_shapes(p);return true;
    case TrackingKind::TickRefinement:tick_refinement_shapes(p);return true;
    case TrackingKind::LagVote:lag_vote_shapes(p);return true;
    default:return false;
    }
}
void legacy_shapes(TrackingPlan& p) noexcept {
    if(tempo_feature_shapes(p) || observation_shapes(p))return;
    if(p.kind==TrackingKind::HarmonicComb || p.kind==TrackingKind::PhaseResponse){p.outputs=1;p.shape[0]=p.matrix?matrix(p.rows,p.response_length):vector(p.response_length);}
}

}
bool tracking_operation_shapes(const dataflow_runtime::NumericOperationNodeView&,TrackingPlan& p) noexcept {
    if(global_shapes(p) || period_shapes(p))return true;legacy_shapes(p);return p.outputs!=0;
}
} // namespace rhythm_graph_adapters::detail
