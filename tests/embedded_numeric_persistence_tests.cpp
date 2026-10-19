#include <embedded_support/persistence.h>
#include <embedded_support/profile.h>
#include <embedded_support/synthetic_pipeline.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <dsp_kernels/dsp.h>
#include "numeric_test_builders.h"
using namespace signal_contracts;
using namespace domain;
namespace es=embedded_support;
namespace nc=bdsp_protocol::numeric_control;
namespace {
int failures=0,checks=0;
void expect(bool valid,int line,const char* expression) {
    ++checks;
    if(valid)return;
    ++failures;
    std::printf("FAIL %d: %s\n",line,expression);
    std::fflush(stdout);
}
#define CHECK(x) expect(bool(x),__LINE__,#x)
struct Storage final:es::ProcessingStoragePort {
    std::array<std::vector<std::uint8_t>,2> slots;
    int writes=0;bool interrupt=false;Status read_failure=Status::ok;
    Status read(unsigned slot,std::span<std::uint8_t> out,std::size_t& n) noexcept override {
        if(read_failure!=Status::ok)return read_failure;
        if(slots[slot].empty())return Status::unavailable;
        if(out.size()<slots[slot].size())return Status::buffer_too_small;
        std::copy(slots[slot].begin(),slots[slot].end(),out.begin());n=slots[slot].size();return Status::ok;
    }
    Status commit(unsigned slot,std::span<const std::uint8_t> bytes) noexcept override {
        ++writes;slots[slot].assign(bytes.begin(),bytes.end());
        if(interrupt){slots[slot].resize(bytes.size()/2);return Status::disconnected;}return Status::ok;
    }
};
struct Buffers {
    std::array<std::uint8_t,16432> a{},b{},scratch{};
    alignas(std::max_align_t) std::array<std::byte,16384> arena{},stage{};
    std::array<std::size_t,2*nc::device_limits().nodes> workspace{};
    es::PersistenceBuffers view(){return {a,b,scratch,arena,stage,workspace};}
};
NumericProcessingGraph graph(std::string id="saved") {
    NumericContract c;c.dtype=DType::UInt64;c.semantic=Semantic::Numeric;c.shape.rank=Rank::Vector;
    c.representation.encoding=StorageEncoding::FixedLittleEndian;
    c.shape.axes={Axis::Declared,Axis::None};c.shape.extents={ExtentBounds{2,2},ExtentBounds{1,1}};
    static constexpr std::array<std::uint32_t,1> channels{42};c.channel_order=channels;c.clock.domain="device";
    // Exact64 coordinate metadata must survive persistence without double.
    c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Values,{INT64_MIN+1,UINT64_MAX},{1,UINT64_MAX}};
    const auto owned=numeric_test_builders::contract(c,{2,4096,32768,2048,64,256},
        16384,[](bool okay,const char* reason){expect(okay,__LINE__,reason);},"persistence contract");
    auto source=numeric_test_builders::node(1,"NetworkInput",{},
        {},{numeric_test_builders::port(0,PortDirection::Output,owned)});
    auto sink=numeric_test_builders::node(2,"NetworkOutput",{},
        {numeric_test_builders::port(0,PortDirection::Input,owned)},{});
    return numeric_test_builders::graph(std::move(id),{1,120}, {std::move(source),std::move(sink)},
        {numeric_test_builders::edge(1,0,2,0)});
}
void check_fft_graph(const NumericProcessingGraph& g,std::uint32_t fft) {
    CHECK(g.nodes.size()==9);
    if(g.nodes.size()!=9)return;
    CHECK(g.nodes[2].kind=="ArrayMultiply" && g.nodes[2].inputs.size()==2);
    CHECK(g.nodes[7].kind=="ArrayLength" && g.nodes[8].kind=="WindowFunction");
    const auto scalar=std::get<NumericContract>(g.nodes[8].inputs[0].contract.view());
    CHECK(scalar.dtype==DType::UInt32 && scalar.shape.rank==Rank::Scalar);
    const auto values=std::get<NumericContract>(g.nodes[8].outputs[0].contract.view());
    CHECK(values.dtype==DType::Float32 && values.shape.extents[0]==(ExtentBounds{fft,fft}));
}
NumericProcessingGraph fft_graph(std::uint32_t fft) {
    es::Profile profile;
    profile.spectral=true;
    profile.fft_size=fft;
    NumericProcessingGraph graph;
    CHECK(es::default_processing_graph(profile,"device-fft",graph)==Status::ok);
    check_fft_graph(graph,fft);
    return graph;
}
void fix_crc(std::vector<std::uint8_t>& b) {
    std::uint32_t crc=~0u;for(std::size_t i=0;i<b.size();++i){crc^=i>=40&&i<44?0:b[i];
        for(int k=0;k<8;++k)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1)));}crc=~crc;
    for(int k=0;k<4;++k)b[40+k]=std::uint8_t(crc>>(8*k));
}
void future_record_versions(Storage& storage,es::ProcessingPersistence& saved,es::ProcessingPersistence& reboot) {
    const auto original=storage.slots[0];const auto writes=storage.writes;
    for(auto offset:{12u,24u,44u}){
        storage.slots[0]=original;storage.slots[0][offset]=99;fix_crc(storage.slots[0]);
        auto output=graph("retained");
        CHECK(reboot.restore(output)==Status::unsupported_version);
        CHECK(output.graph_id=="retained");
        CHECK(saved.save(graph("refused"))==Status::unsupported_version);
        CHECK(storage.writes==writes);
    }
    storage.slots[0]=original;
}

void record_editions(const Storage& storage) {
    const auto& record=storage.slots[0];
    for(auto [offset,edition]:{std::pair{12u,4u},{16u,5u},{20u,4u},{24u,5u},{44u,4u}}) {
        CHECK(record.size()>=es::ProcessingPersistence::record_header_bytes);
        CHECK(record[offset]==edition && record[offset+1]==0 && record[offset+2]==0 && record[offset+3]==0);
    }
}
void interrupted_save_preserves_good(Storage& storage,es::ProcessingPersistence& saved,
                                      es::ProcessingPersistence& reboot,NumericProcessingGraph& out) {
    storage.interrupt=true;
    CHECK(saved.save(graph("interrupted"))==Status::disconnected);
    CHECK(reboot.restore(out)==Status::ok && out.graph_id=="last-good");
    CHECK(storage.writes==2);
    storage.interrupt=false;
    CHECK(saved.save(graph("new-good"))==Status::ok);
    CHECK(reboot.restore(out)==Status::ok && out.graph_id=="new-good");
    if(storage.slots[1].empty()){CHECK(false);return;}
    storage.slots[1].back()^=1;
    CHECK(reboot.restore(out)==Status::ok && out.graph_id=="last-good");
}
void future_graph_versions(Storage& storage,es::ProcessingPersistence& saved,int writes) {
    for(auto boundary:{0,1}) {
        auto future=graph("future-boundary");
        if(boundary==0)future.versions.graph=99;
        if(boundary==1)future.versions.processing=99;
        CHECK(saved.save(future)==Status::unsupported_version && storage.writes==writes);
    }
}
void future_save_is_atomic(Storage& storage,es::ProcessingPersistence& saved,
                           es::ProcessingPersistence& reboot,NumericProcessingGraph& out,
                           const NumericProcessingGraph& good) {
    auto writes=storage.writes;
    auto invalid=graph("future");
    invalid.versions.processing=99;
    CHECK(saved.save(invalid)!=Status::ok && storage.writes==writes);
    future_graph_versions(storage,saved,writes);
    future_record_versions(storage,saved,reboot);
    CHECK(saved.save(graph("future-record"))==Status::ok);
    storage.slots[1][4]=2;
    fix_crc(storage.slots[1]);
    writes=storage.writes;
    CHECK(reboot.restore(out)==Status::ok && out.graph_id=="last-good");
    CHECK(saved.save(good)==Status::unsupported_version && storage.writes==writes);
    storage.slots[0].back()^=1;
    out.graph_id="unchanged";
    CHECK(reboot.restore(out)!=Status::ok && out.graph_id=="unchanged");
    storage.read_failure=Status::disconnected;
    CHECK(saved.save(good)==Status::disconnected && storage.writes==writes);
}
void persistence(const nc::Codec& codec) {
    Storage storage;Buffers buffers;es::Profile profile;auto device=es::device_profile(profile);auto& runtime=device.runtime;
    es::ProcessingPersistence saved(storage,buffers.view(),codec,es::numeric_control_limits(profile),runtime.admission,es::numeric_capabilities(device));
    auto out=graph("unchanged");CHECK(saved.restore(out)==Status::unavailable);CHECK(out.graph_id=="unchanged" && storage.writes==0);
    auto g=graph("last-good");auto first_save=saved.save(g);if(first_save!=Status::ok)std::printf("first save status %d\n",int(first_save));CHECK(first_save==Status::ok);if(first_save!=Status::ok)return;CHECK(storage.writes==1);
    if(first_save==Status::ok)record_editions(storage);
    // Fresh instance simulates reboot: no process-local active-slot state.
    es::ProcessingPersistence reboot(storage,buffers.view(),codec,es::numeric_control_limits(profile),runtime.admission,es::numeric_capabilities(device));
    const auto restored=reboot.restore(out);CHECK(restored==Status::ok);if(restored!=Status::ok)return;CHECK(out.graph_id=="last-good");
    CHECK(same_contract(out.nodes[0].outputs[0].contract.view(),g.nodes[0].outputs[0].contract.view()));
    CHECK(out.nodes[0].outputs[0].contract.view().index()==0);CHECK(storage.writes==1);
    interrupted_save_preserves_good(storage,saved,reboot,out);
    future_save_is_atomic(storage,saved,reboot,out,g);
}
void profile_limits() {
    es::Profile p;CHECK(p.fft_size==2048 && p.channels==1 && !es::numeric_runtime_profile(p).history_enabled);
    auto a=es::admit(p,3);CHECK(a.status==Status::ok);
    p.channels=2;CHECK(es::admit(p,3).status==Status::invalid_argument);p=es::Profile{};p.spectral=true;CHECK(es::admit(p,3).status==Status::ok);
    p=es::Profile{};p.fft_size=4096;CHECK(es::admit(p,3).status==Status::ok);CHECK(p.fft_size==4096);
    p.control_bytes=p.memory_bytes;CHECK(es::admit(p,3).status==Status::exhausted);CHECK(es::numeric_runtime_profile(p).resources.max_peak_bytes==0);
    p.control_bytes=SIZE_MAX;CHECK(es::admit(p,3).status==Status::exhausted);
    p=es::Profile{};p.tcp_bytes_per_second=1000000;const auto large_packet=es::admit(p,3);
    p.packet_payload_capacity=64;const auto small_packet=es::admit(p,3);
    CHECK(large_packet.status==Status::ok && small_packet.status==Status::ok);
    CHECK(small_packet.frame_bytes==large_packet.frame_bytes && small_packet.wire_bytes>large_packet.wire_bytes);
    p.packet_payload_capacity=8;CHECK(es::admit(p,3).status==Status::invalid_argument);
}
void profile_contract_admission() {
    es::Profile p;
    auto device=es::device_profile(p);auto& runtime=device.runtime;auto caps=es::numeric_capabilities(device);auto g=graph();
    std::array<std::size_t,2*nc::device_limits().nodes> workspace{};ProcessingGraphAdmission result;
    CHECK(admit_processing_graph(g,runtime.admission,caps,workspace,result));
    auto reject_contract=[&](NumericContract c){
        auto candidate=g;OwnedProcessingContract owned;
        CHECK(own_processing_contract(c,runtime.admission.frame,runtime.admission.max_owned_metadata_bytes,owned)==Status::ok);
        candidate.nodes[0].outputs[0].contract=owned;candidate.nodes[1].inputs[0].contract=owned;
        CHECK(!admit_processing_graph(candidate,runtime.admission,caps,workspace,result));
    };
    auto unsupported=std::get<NumericContract>(g.nodes[0].outputs[0].contract.view());
    unsupported.representation.encoding=StorageEncoding::Native;reject_contract(unsupported);
    unsupported.representation.encoding=StorageEncoding::FixedLittleEndian;unsupported.shape.extents[0]={1024,1024};reject_contract(unsupported);
    auto identity=g;identity.graph_id=std::string(65,'g');CHECK(!admit_processing_graph(identity,runtime.admission,caps,workspace,result));
    CHECK(!runtime.capabilities.float64_arithmetic);
    runtime.resources.max_peak_bytes=1;caps=es::numeric_capabilities(device);CHECK(!admit_processing_graph(g,runtime.admission,caps,workspace,result));
}
void profile_fft_admission() {
    std::array<std::size_t,2*nc::device_limits().nodes> workspace{};
    ProcessingGraphAdmission result;
    for(auto fft:{2048u,4096u}) {
        es::Profile p;p.spectral=true;p.fft_size=fft;p.memory_bytes=2*1024*1024;p.task_stack_bytes=1024*1024;
        auto actual_device=es::device_profile(p);auto& actual=actual_device.runtime;auto actual_caps=es::numeric_capabilities(actual_device);auto dsp=fft_graph(fft);
        auto accepted=admit_processing_graph(dsp,actual.admission,actual_caps,workspace,result);
        CHECK(bool(accepted)==(fft<=dsp_kernels::max_fft_samples));CHECK(p.fft_size==fft);
        if(accepted) {
            std::shared_ptr<const dataflow_runtime::PreparedGraph> prepared;std::unique_ptr<dataflow_runtime::RuntimeContext> context;
            CHECK(dataflow_runtime::PreparedGraph::prepare_numeric(dsp,actual,prepared,context)==Status::ok);
            CHECK(prepared && prepared->numeric_definition()->nodes.size()==dsp.nodes.size());
        }
    }
}
void profile_history_rejection() {
    auto device=es::device_profile(es::Profile{});
    auto& runtime=device.runtime;
    auto caps=es::numeric_capabilities(device);
    std::array<std::size_t,2*nc::device_limits().nodes> workspace{};
    ProcessingGraphAdmission result;
    auto history=graph("history-off");auto c=std::get<NumericContract>(history.nodes[0].outputs[0].contract.view());auto h=c;
    h.semantic=Semantic::History;h.history_element=c.semantic;h.shape.rank=Rank::Matrix;
    h.shape.axes={Axis::Time,c.shape.axes[0]};h.shape.extents={ExtentBounds{0,2},c.shape.extents[0]};h.coordinates={AxisCoordinates{},c.coordinates[0]};
    OwnedProcessingContract hc;CHECK(own_processing_contract(h,{2,4096,32768,2048,64,256},16384,hc)==Status::ok);
    history.nodes[1].kind="Spectrogram";history.nodes[1].parameters=HistoryProcessingParameters{2};history.nodes[1].outputs={{0,PortDirection::Output,hc}};
    device=es::device_profile(es::Profile{});caps=es::numeric_capabilities(device);
    CHECK(!admit_processing_graph(history,runtime.admission,caps,workspace,result));
    CHECK(!runtime.history_enabled);
    std::shared_ptr<const dataflow_runtime::PreparedGraph> no_history;std::unique_ptr<dataflow_runtime::RuntimeContext> no_history_context;
    CHECK(dataflow_runtime::PreparedGraph::prepare_numeric(history,runtime,no_history,no_history_context)!=Status::ok);
}
void profile_tests() {
    profile_limits();
    profile_contract_admission();
    profile_fft_admission();
    profile_history_rejection();
}
void explicit_graph_capacity() {
    es::Profile profile;profile.spectral=true;profile.fft_size=512;
    profile.memory_bytes=2*1024*1024;profile.task_stack_bytes=1024*1024;
    auto device=es::device_profile(profile);auto graph=fft_graph(profile.fft_size);
    const auto capabilities=es::numeric_capabilities(device);
    std::array<std::size_t,2*nc::device_limits().nodes> workspace{};
    ProcessingGraphAdmission result;
    CHECK(device.runtime.admission.max_nodes==nc::device_limits().nodes);
    CHECK(admit_processing_graph(graph,device.runtime.admission,capabilities,workspace,result));
    const auto accepted=result;
    auto extra=graph.nodes.front();extra.id=9;graph.nodes.push_back(std::move(extra));
    CHECK(admit_processing_graph(graph,device.runtime.admission,capabilities,workspace,result).error==ProcessingError::Limit);
    CHECK(result.node_count==accepted.node_count && result.peak_bytes==accepted.peak_bytes);
    graph.nodes.pop_back();device.runtime.resources.max_peak_bytes=1;
    CHECK(!admit_processing_graph(graph,device.runtime.admission,es::numeric_capabilities(device),workspace,result));
    CHECK(result.node_count==accepted.node_count && result.peak_bytes==accepted.peak_bytes);
}
void native_dsp_case(const nc::Codec& codec,std::uint32_t fft) {
    Storage storage;Buffers buffers;
    es::Profile profile;profile.spectral=true;profile.fft_size=fft;
    profile.memory_bytes=2*1024*1024;profile.task_stack_bytes=1024*1024;
    auto device=es::device_profile(profile);
    auto& runtime=device.runtime;
    auto dto=fft_graph(fft);
    es::ProcessingPersistence store(storage,buffers.view(),codec,es::numeric_control_limits(profile),
        runtime.admission,es::numeric_capabilities(device));
    const auto status=store.save(dto);
    if(status!=Status::ok)std::printf("native FFT%u save status%d\n",fft,int(status));
    CHECK(status==Status::ok);
    NumericProcessingGraph restored;
    const auto restored_status=store.restore(restored);
    if(restored_status!=Status::ok)
        std::printf("native FFT%u restore status%d codec%d bytes%zu\n",
                    fft,int(restored_status),int(codec.id()),storage.slots[0].size());
    CHECK(restored_status==Status::ok);
    if(restored.nodes.empty())return;
    CHECK(restored.nodes.size()==dto.nodes.size());
    CHECK(std::get<NumericContract>(restored.nodes[1].outputs[0].contract.view()).representation.encoding
          ==StorageEncoding::Native);
    std::shared_ptr<const dataflow_runtime::PreparedGraph> prepared;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context;
    CHECK(dataflow_runtime::PreparedGraph::prepare_numeric(restored,runtime,prepared,context)==Status::ok);
    CHECK(storage.writes==1);
}
void native_dsp_persistence(const nc::Codec& codec) {
    for(auto fft:{512u,2048u,4096u})
        if(fft<=dsp_kernels::max_fft_samples)native_dsp_case(codec,fft);
}
NumericProcessingGraph retag_spectrum(const NumericProcessingGraph& raw) {
    auto retagged_raw=raw;
    for(auto& n:retagged_raw.nodes)for(auto* ports:{&n.inputs,&n.outputs})for(auto& port:*ports){
        auto c=std::get<NumericContract>(port.contract.view());c.semantic=Semantic::SpectrumMagnitude;c.history_element=Semantic::Scalar;
        OwnedProcessingContract owned;CHECK(own_processing_contract(c,bdsp_protocol::numeric_control::desktop_limits().frame,16384,owned)==Status::ok);port.contract=std::move(owned);
    }
    return retagged_raw;
}
void raw_bandwidth(const es::Profile& profile,const NumericProcessingGraph& raw) {
    CHECK(es::graph_link_admission(profile,raw,4096).status==Status::ok);
    const auto retagged_raw=retag_spectrum(raw);
    const auto raw_budget=es::graph_link_admission(profile,raw,4096);
    const auto retagged_budget=es::graph_link_admission(profile,retagged_raw,4096);
    CHECK(retagged_budget.status==Status::ok && retagged_budget.required_bytes_per_second==raw_budget.required_bytes_per_second);
}
NumericProcessingGraph spectral_wire_graph(const es::Profile& profile) {
    auto spectral=fft_graph(512);
    CHECK(es::graph_link_admission(profile,spectral,4096).status==Status::ok);
    // Same raw source/profile, but an admitted FFT512 mono spectrum produces
    // the same 1024-byte payload at 120 Hz rather than raw block cadence31.25 Hz.
    const auto native=spectral.nodes[3].outputs[0].contract;
    auto wire=std::get<NumericContract>(native.view());wire.representation.encoding=StorageEncoding::FixedLittleEndian;
    const auto device=es::device_profile(profile);OwnedProcessingContract owned;
    CHECK(own_processing_contract(wire,device.runtime.admission.frame,device.runtime.admission.max_owned_metadata_bytes,owned)==Status::ok);
    auto converter=spectral.nodes[5];converter.id=4;converter.inputs[0].contract=native;converter.outputs[0].contract=owned;
    auto sink=spectral.nodes[6];sink.id=5;sink.inputs[0].contract=owned;
    spectral.nodes.erase(spectral.nodes.begin()+4,spectral.nodes.begin()+7);
    spectral.nodes.push_back(std::move(converter));spectral.nodes.push_back(std::move(sink));
    spectral.edges={{0,1,0,0},{1,2,0,0},{2,3,0,0},{3,4,0,0},{4,5,0,0},{1,7,0,0},{7,8,0,0},{8,2,0,1}};
    return spectral;
}
void spectral_bandwidth(es::Profile& profile) {
    auto spectral=spectral_wire_graph(profile);
    const auto device=es::device_profile(profile);
    std::array<std::size_t,2*nc::device_limits().nodes> workspace{};ProcessingGraphAdmission result;
    CHECK(admit_processing_graph(spectral,device.runtime.admission,es::numeric_capabilities(device),workspace,result));
    profile.tcp_bytes_per_second=es::graph_link_admission(profile,spectral,4096).required_bytes_per_second/2;
    const auto rejected=es::graph_link_admission(profile,spectral,4096);
    CHECK(rejected.status==Status::exhausted && rejected.required_bytes_per_second>rejected.allowed_bytes_per_second);
    profile.tcp_bytes_per_second=1000000;
    const auto accepted=es::graph_link_admission(profile,spectral,4096);CHECK(accepted.status==Status::ok);
    const auto fragmented=es::graph_link_admission(profile,spectral,64);
    CHECK(fragmented.status==Status::ok && fragmented.required_bytes_per_second>accepted.required_bytes_per_second);
}
void sink_bandwidth(es::Profile& profile,const NumericProcessingGraph& raw) {
    const auto raw_budget=es::graph_link_admission(profile,raw,4096);
    auto multi=raw;auto second=multi.nodes.back();second.id=3;second.enabled=false;multi.nodes.push_back(second);multi.edges.push_back({1,3,0,0});
    profile.tcp_bytes_per_second=raw_budget.required_bytes_per_second*100/profile.utilization_percent+1;
    CHECK(es::graph_link_admission(profile,multi,4096).status==Status::exhausted); // disabled sink still receives typed silence
    CHECK(es::graph_link_admission(profile,raw,8).status==Status::invalid_argument);
    CHECK(es::graph_link_admission(profile,graph(),4096).status==Status::incompatible_contract);
}
void graph_bandwidth() {
    es::Profile profile;profile.memory_bytes=2*1024*1024;profile.task_stack_bytes=1024*1024;
    const auto raw=es::default_processing_graph(profile,"source");
    raw_bandwidth(profile,raw);
    spectral_bandwidth(profile);
    sink_bandwidth(profile,raw);
}
struct Clock final:application::ControlClock {std::uint64_t now_ms() const noexcept override{return 10;}};
void control_explicit_persist(application::ProcessingControlService& service,
    application::SessionIdentity identity,Storage& storage,
    es::ProcessingPersistence& saved,dataflow_runtime::GraphExecutor& executor,
    const application::ProcessingReply& applied) {
    const auto revision=applied.state.revision;
    const auto graph_revision=applied.state.graph_revision;
    const auto state=service.submit(identity,{5,revision},application::GetState{});
    CHECK(state.accepted && state.state.applied_numeric && storage.writes==0);
    const auto persist=service.submit(identity,{6,revision},application::ExplicitPersist{graph_revision});
    CHECK(persist.accepted && storage.writes==1 && persist.state.persisted_graph_revision==graph_revision);
    const auto replay=service.submit(identity,{6,revision},application::ExplicitPersist{graph_revision});
    CHECK(replay.accepted && storage.writes==1);
    auto out=graph("empty");
    CHECK(saved.restore(out)==Status::ok && out.graph_id=="working" && storage.writes==1);
    storage.interrupt=true;
    const auto failure=service.submit(identity,{7,persist.state.revision},application::ExplicitPersist{graph_revision});
    CHECK(!failure.accepted && executor.running() && failure.state.graph_revision==graph_revision);
    CHECK(saved.restore(out)==Status::ok && out.graph_id=="working");
}
void control_commands(application::ProcessingControlService& service,Storage& storage,
    es::ProcessingPersistence& saved,dataflow_runtime::GraphExecutor& executor) {
    const auto session=service.open_session(42);
    CHECK(session);
    if(!session)return;
    application::ProcessingDTO dto;
    dto.numeric=graph("working");
    const auto prepared=service.submit(session.identity,{1,0},application::PrepareProcessing{&dto});
    CHECK(prepared.accepted && prepared.prepared_token && storage.writes==0);
    if(!prepared.accepted)return;
    auto bad=dto;bad.numeric->versions.processing=99;
    const auto rejected=service.submit(session.identity,{2,0},application::PrepareProcessing{&bad});
    CHECK(!rejected.accepted && rejected.state.own_prepared_token==prepared.prepared_token && storage.writes==0);
    const auto wrong=service.submit(session.identity,{3,0},application::ApplyProcessing{prepared.prepared_token+1,"working"});
    CHECK(!wrong.accepted && wrong.state.own_prepared_token==prepared.prepared_token);
    const auto applied=service.submit(session.identity,{4,0},application::ApplyProcessing{prepared.prepared_token,"working"});
    CHECK(applied.accepted && executor.running() && storage.writes==0);
    if(applied.accepted)control_explicit_persist(service,session.identity,storage,saved,executor,applied);
}
void control_persistence() {
    Storage storage;Buffers buffers;es::Profile p;auto profile=es::device_profile(p);auto& runtime=profile.runtime;auto device=es::numeric_capabilities(profile);
    nc::CborCodec codec;es::ProcessingPersistence saved(storage,buffers.view(),codec,es::numeric_control_limits(p),runtime.admission,device);
    es::AppliedProcessingPersistence persistence(saved);dataflow_runtime::GraphExecutor executor(runtime);
    application::NumericRuntimeLimits limits{runtime.admission,runtime.resources};
    application::GraphLifecycle lifecycle(executor,executor,device,limits);application::GraphLifecycleControlPort port(lifecycle);
    Clock clock;application::ProcessingCapabilities caps;caps.control.max_sessions=1;
    caps.explicit_persist=true;caps.numeric=application::NumericProcessingCapabilities{device,limits};
    std::array<application::ControlSessionSlot,1> slots;
    application::ProcessingControlService service(port,clock,caps,slots,123,&persistence);
    control_commands(service,storage,saved,executor);
}
}
int main(){nc::CborCodec c;nc::ProtobufCodec p;profile_tests();explicit_graph_capacity();graph_bandwidth();persistence(c);persistence(p);control_persistence();native_dsp_persistence(c);native_dsp_persistence(p);
    std::printf("embedded numeric/persistence: %d checks, %d failures\n",checks,failures);return failures?1:0;}
