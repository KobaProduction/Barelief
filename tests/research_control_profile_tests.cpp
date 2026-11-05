#include <control/numeric_remote_support.h>
#include <dataflow_runtime/runtime.h>
#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
using namespace signal_contracts;
void check(bool valid,const char* message){if(!valid){std::fprintf(stderr,"research CONTROL: %s\n",message);std::exit(1);}}
dataflow_runtime::NumericRuntimeProfile profile() {
    const FrameLimits frame{8,4194304,16777216,65536,256,32768};
    return {{64,128,16,64,1048576,16777216,frame},
        {{16777216,67108864,67108864,1048576},0,536870912},
        {4194304,8,16777216,16777216,16777216},{true,true,true}};
}
domain::NumericProcessingGraph graph(std::size_t count) {
    static constexpr std::uint32_t mono=0;
    NumericContract contract;contract.channel_order={&mono,1};contract.dtype=DType::Float32;
    contract.shape={Rank::Vector,{Axis::Declared,Axis::None},{{{count,count},{1,1}}}};
    contract.clock={"control-profile",{1,44100},44100};
    auto bounds=profile().admission.frame;bounds.max_elements=8388608;bounds.max_payload_bytes=33554432;
    domain::OwnedProcessingContract owned;
    check(domain::own_processing_contract(contract,bounds,65536,owned)==Status::ok,"construct bounded metadata fixture");
    domain::ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";
    source.parameters=domain::EndpointProcessingParameters{{{"sample_rate",NumericLiteral{std::uint64_t{44100}}}}};
    source.outputs={{0,domain::PortDirection::Output,std::move(owned)}};
    domain::NumericProcessingGraph result;result.graph_id="research-control-profile";result.nodes.push_back(std::move(source));return result;
}
void projection_bounds() {
    const auto ordinary=pipeline::remote_numeric::control_limits();
    const auto p=profile();const auto research=pipeline::remote_numeric::control_limits(p.admission.frame);
    check(ordinary.frame.max_elements==desktop::numeric_profile::frame_limits.max_elements && ordinary.frame.max_payload_bytes==desktop::numeric_profile::frame_limits.max_payload_bytes,"default desktop frame ceiling unchanged");
    check(research.wire_bytes==ordinary.wire_bytes && research.arena_bytes==ordinary.arena_bytes && research.total_text_bytes==ordinary.total_text_bytes,"execution frame does not enlarge CONTROL storage envelope");
    control::numeric::GraphProjection desktop_projection,accepted;
    check(bool(desktop_projection.assign(graph(4194305),ordinary)),"desktop metadata uses representation limits");
    check(bool(accepted.assign(graph(1000000),research)),"explicit research frame admits metadata");
    check(!accepted.assign(graph(4194305),research),"over research bound rejected");
    const auto& retained=std::get<NumericContract>(accepted.wire().nodes[0].outputs[0].contract);
    check(retained.shape.extents[0].maximum==1000000,"failed projection retains prior accepted owner");
}
void selected_capabilities() {
    const auto p=profile();application::ProcessingCapabilities capabilities;
    capabilities.numeric=application::NumericProcessingCapabilities{
        dataflow_runtime::PreparedGraph::numeric_capabilities(p),{p.admission,p.resources}};
    pipeline::remote_numeric::CapabilitySnapshot snapshot;
    check(snapshot.assign(graph(1000000),capabilities)==Status::ok && snapshot.rows.size()==1,"capability projection uses explicitly selected runtime frame");
    check(snapshot.assign(graph(4194305),capabilities)!=Status::ok,"capability projection retains declared upper bound");
}
}
int main(){projection_bounds();selected_capabilities();std::puts("research CONTROL profile PASS");}
