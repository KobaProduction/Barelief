#include "runtime_internal.h"

namespace dataflow_runtime {
using namespace detail;
using namespace domain;
using namespace signal_contracts;
std::optional<NodeStats> PreparedGraph::node_stats(domain::NodeId node,const RuntimeContext& context) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return {};
    const auto at=index(impl_->nodes,node);
    if(at==absent)return {};
    auto stats=context.impl_->states[at].stats;stats.node=node;return stats;
}
std::optional<MixerInputStats> PreparedGraph::mixer_input_stats(domain::NodeId node,domain::PortId port,const RuntimeContext& context) const noexcept {
    if(!context.impl_ || context.impl_->plan!=this)return {};
    const auto at=index(impl_->nodes,node);
    if(at==absent || impl_->nodes[at].operation!=ProcessingOperation::Mixer)return {};
    const auto& n=impl_->nodes[at];for(std::size_t q=0;q<n.endpoint_plan->inputs.size();++q)if(n.endpoint_plan->inputs[q].port==port){const auto& queue=context.impl_->states[at].mixer_queues[q];return MixerInputStats{port,queue.count,queue.samples.size(),queue.dropped,queue.underrun};}return {};
}

} // namespace dataflow_runtime
