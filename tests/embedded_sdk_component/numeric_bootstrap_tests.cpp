#include "../../examples/esp32_streamer/main/main.cpp"
#include <map>
#include "../test_assertions.h"
#include <cstdio>
#include <cstring>
#include <deque>
static std::size_t available_memory=8*1024*1024;
static std::map<std::string,std::vector<std::uint8_t>> records;
static unsigned writes=0,opens=0,closes=0;
extern "C" void vTaskDelay(TickType_t){}
extern "C" void vTaskDelete(TaskHandle_t){}
extern "C" TaskHandle_t xTaskCreateStatic(void (*)(void*),const char*,std::uint32_t,void*,unsigned,StackType_t*,StaticTask_t*){return nullptr;}
extern "C" int fcntl(int,int,...){return 0;}
extern "C" int recv(int,void*,std::size_t,int){return -1;}
extern "C" int send(int,const void*,std::size_t,int){return -1;}
extern "C" BaseType_t xQueueReceive(QueueHandle_t,void*,TickType_t){return 0;}
extern "C" BaseType_t xQueueReset(QueueHandle_t){return 1;}
extern "C" int uart_read_bytes(uart_port_t,void*,std::uint32_t,TickType_t){return 0;}
extern "C" int uart_tx_chars(uart_port_t,const char*,std::uint32_t){return 0;}
extern "C" esp_err_t uart_param_config(uart_port_t,const uart_config_t*){return ESP_OK;}
extern "C" esp_err_t uart_set_pin(uart_port_t,int,int,int,int){return ESP_OK;}
extern "C" esp_err_t uart_driver_install(uart_port_t,int,int,int,QueueHandle_t* queue,int){
    static int handle; if(queue)*queue=&handle;return ESP_OK;
}
extern "C" esp_err_t uart_driver_delete(uart_port_t){return ESP_OK;}
extern "C" esp_err_t uart_flush_input(uart_port_t){return ESP_OK;}
std::size_t heap_caps_get_free_size(unsigned){return available_memory;}
extern "C" std::int64_t esp_timer_get_time(){return 1000000;}
esp_err_t nvs_flash_init(){return ESP_OK;}
esp_err_t nvs_open(const char*,nvs_open_mode_t,nvs_handle_t* out){++opens;*out=1;return ESP_OK;}
void nvs_close(nvs_handle_t){++closes;}
esp_err_t nvs_get_blob(nvs_handle_t,const char* key,void* out,std::size_t* size){
    const auto it=records.find(key);if(it==records.end())return ESP_ERR_NVS_NOT_FOUND;
    if(!out){*size=it->second.size();return ESP_OK;}
    if(*size<it->second.size())return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(out,it->second.data(),it->second.size());*size=it->second.size();return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t,const char* key,const void* bytes,std::size_t size){
    const auto* begin=static_cast<const std::uint8_t*>(bytes);records[key]={begin,begin+size};++writes;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t){return ESP_OK;}
#define CHECK(x) do{if(!test_assertions::verify(bool(x),#x,__FILE__,__LINE__))return 1;}while(false)
struct FakeIo final:bdsp_protocol::ByteTransport {
    std::deque<std::uint8_t> input;FakeIo* other=nullptr;std::size_t segment=17;
    bdsp_protocol::TransportState state() const noexcept override{return bdsp_protocol::TransportState::connected;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t> out) noexcept override {
        const auto count=std::min({segment,out.size(),input.size()});
        for(std::size_t i=0;i<count;++i){out[i]=input.front();input.pop_front();}return {Status::ok,count};
    }
    bdsp_protocol::IoResult write(std::span<const std::uint8_t> bytes) noexcept override {
        const auto count=std::min(segment,bytes.size());other->input.insert(other->input.end(),bytes.begin(),bytes.begin()+count);return {Status::ok,count};
    }
};
static bool drain_control(control::numeric::ControlSession& client,Device& device,FakeIo& a,FakeIo& b){
 for(unsigned i=0;i<10000 && (client.pending_bytes()||device.control->pending_bytes()||!a.input.empty()||!b.input.empty());++i)
  if(client.poll(a)!=Status::ok||device.control->poll(b)!=Status::ok)return false;
 return !client.pending_bytes()&&!device.control->pending_bytes()&&a.input.empty()&&b.input.empty();
}
static int control_cold_budget(Device& device){
        CHECK(!device.cold.leased);const auto revision=device.state.graph_revision;
        const auto busy=device.submit(application::GetState{});CHECK(!busy.accepted && busy.error==application::ControlError::limit_exceeded);
        CHECK(device.state.graph_revision==revision && device.state.applied_numeric && !device.local.session);
        const auto observed=device.service->observe_state();CHECK(observed.graph_revision==revision && !observed.own_prepared_token);
        device.control->reset();
    return 0;
}
static int control_handshake(control::numeric::ControlSession& client,Device& device,FakeIo& a,FakeIo& b,unsigned mask){
        CHECK(client.open(mask==1?101:7)==Status::ok);CHECK(drain_control(client,device,a,b));CHECK(client.state()==control::ControlSessionState::Ready);
    return 0;
}
static int control_state_codec(Device& device,unsigned mask){
        control::numeric::SessionConfig config;config.endpoint_authorized=true;config.device_identity="numeric-esp32";
        config.codec_mask=mask;config.max_packet_bytes=PacketBytes;config.codec_limits=device.control_limits();
        control::numeric::ControlSession client(config,device.clock);FakeIo a,b;a.other=&b;b.other=&a;b.segment=19;
        unsigned replies=0;bool valid=false;
        struct Reply {unsigned* count;bool* valid;} capture{&replies,&valid};
        client.set_reply_handler([](const nc::Message& message,void* user) {
            auto& c=*static_cast<Reply*>(user);const auto& reply=std::get<nc::Reply>(message.body);
            ++*c.count;*c.valid=reply.error==nc::Error::None && reply.state.applied && reply.state.applied->nodes.size()==(Spectral?9:3);
        },&capture);
        CHECK(control_handshake(client,device,a,b,mask)==0);
        nc::Command command;command.id=nc::CommandId::State;CHECK(client.send(command,1)==Status::ok);CHECK(drain_control(client,device,a,b));CHECK(replies==1 && valid);
        CHECK(control_cold_budget(device)==0);
    return 0;
}
static int control_state_both(Device& device) {
    for(const auto mask:{1u,2u}) {
        CHECK(control_state_codec(device,mask)==0);
    }
    return 0;
}
static void print_host_model(Device& d){
        {auto stock=embedded_support::default_processing_graph(d.profile,StreamId);std::size_t owned=0;const auto bytes_status=domain::processing_graph_owned_bytes(stock,d.effective.runtime.admission,owned);std::fprintf(stderr,"host fixture DTO owned=%zu status=%d ceiling=%zu\n",owned,int(bytes_status),d.effective.runtime.admission.max_owned_metadata_bytes);}
        {auto stock=embedded_support::default_processing_graph(d.profile,StreamId);auto diagnostic_caps=d.capabilities();
            diagnostic_caps.profile.max_peak_bytes=SIZE_MAX;
            std::array<std::size_t,2*bdsp_protocol::numeric_control::device_limits().nodes> workspace{};domain::ProcessingGraphAdmission required;
            const auto admitted=domain::admit_processing_graph(stock,d.effective.runtime.admission,diagnostic_caps,workspace,required);
            std::fprintf(stderr,"host fixture model validation=%d node=%d required_peak=%zu plan=%zu payload=%zu scratch=%zu stack=%zu retained=%zu limit=%zu\n",
                int(admitted.error),int(admitted.node),required.peak_bytes,required.resources.plan_bytes,required.resources.payload_bytes,
                required.resources.scratch_bytes,required.resources.stack_bytes,diagnostic_caps.profile.retained_working_bytes,d.effective.runtime.resources.max_peak_bytes);}
}
static void print_node_support(Device& d,const domain::ConfiguredProcessingNode& node){
    domain::ResolvedProcessingNode resolved;
    const auto diagnostic=domain::resolve_processing_node(node,d.effective.runtime.admission,resolved);
    if(!diagnostic){std::fprintf(stderr,"node=%d resolution=%d\n",int(node.id),int(diagnostic.error));return;}
    const auto caps=d.capabilities();const auto support=caps.query(caps.context,resolved,caps.profile);
    std::fprintf(stderr,"node=%d plan=%zu payload=%zu scratch=%zu stack=%zu\n",int(node.id),support.resources.plan_bytes,
                 support.resources.payload_bytes,support.resources.scratch_bytes,support.resources.stack_bytes);
}
static void print_prepare_refusal(Device& d,Status status){
    if(status==Status::ok||!d.service)return;
    application::ProcessingDTO probe;probe.numeric=embedded_support::default_processing_graph(d.profile,StreamId);
    const auto reply=d.submit(application::PrepareProcessing{&probe});
    std::fprintf(stderr,"prepare status=%d control=%d validation=%d node=%d\n",int(reply.status),int(reply.error),int(reply.numeric_validation.error),int(reply.numeric_validation.node));
    for(const auto& node:probe.numeric->nodes)print_node_support(d,node);
}
static Status prepare_host_bootstrap(Device& d){
    // Fake-IDF executes on the host stack. Reserve a truthful host fixture budget;
    // the configured acquisition stack is not physically executed by the host fixture.
    d.profile.task_stack_bytes=32*1024;
    d.effective=embedded_support::device_profile(d.profile);
    return d.prepare();
}
static int bootstrap_initial(Device& d){
const auto status=prepare_host_bootstrap(d);std::fprintf(stderr,"host fixture bootstrap status=%d spectral=%d fft=%u\n",int(status),int(Spectral),unsigned(FftSize));
        std::fprintf(stderr,"host fixture budget memory=%zu control=%zu adapter=%zu pool=%zu runtime_peak=%zu replay=%zu cold=%zu device=%zu\n",
            d.profile.memory_bytes,d.profile.control_bytes,d.profile.adapter_bytes,d.profile.queue_frames*d.profile.max_frame_bytes,
            d.effective.runtime.resources.max_peak_bytes,d.caps.control.max_replay_bytes_per_session,d.cold.bytes,sizeof(Device));
        print_host_model(d);
        print_prepare_refusal(d,status);
        CHECK(status==Status::ok);
        CHECK(d.control->state()!=control::ControlSessionState::Failed);CHECK(d.state.applied_numeric);CHECK(d.state.run_requested);CHECK(writes==0);
        std::printf("memory=%zu control=%zu adapter=%zu pool=%zu runtime_peak=%zu replay=%zu cold=%zu device=%zu\n",
            d.profile.memory_bytes,d.profile.control_bytes,d.profile.adapter_bytes,d.profile.queue_frames*d.profile.max_frame_bytes,
            d.effective.runtime.resources.max_peak_bytes,d.caps.control.max_replay_bytes_per_session,d.cold.bytes,sizeof(Device));

 return 0;
}
static int bootstrap_persist_budget(Device& d){
        const auto footprint=control::numeric::reply_envelope_fits(*d.state.applied_numeric,d.control_limits(),d.advertised);CHECK(footprint);
        std::printf("BOTH_reply wire=%zu scratch=%zu arena=%zu text=%zu\n",footprint.required_wire,footprint.required_scratch,footprint.required_arena,footprint.required_text);
        control::numeric::GraphProjection persisted;auto persist_limits=d.control_limits();persist_limits.wire_bytes=Device::PersistWire;persist_limits.arena_bytes=Device::PersistArena;
        CHECK(persisted.assign(*d.state.applied_numeric,persist_limits));nc::Command persist;persist.id=nc::CommandId::Prepare;persist.graph=persisted.wire();nc::Message persist_message;persist_message.body=persist;
        nc::CborCodec cbor;nc::ProtobufCodec protobuf;
        for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cbor),static_cast<const nc::Codec*>(&protobuf)}) {
            const auto measured=codec->measure(persist_message,persist_limits);CHECK(measured);
            std::printf("persist codec=%u wire=%zu scratch=%zu arena=%zu text=%zu\n",unsigned(codec->id()),measured.required_wire,measured.required_scratch,measured.required_arena,measured.required_text);
        }
        CHECK(control_state_both(d)==0);

 return 0;
}
static int bootstrap_processing(Device& d){
        // No binding/network: the real Device processing entrypoint still advances.
        const auto before_processing=d.processed_frames.load();
        for(unsigned i=0;i<32;++i)CHECK(d.source.tick_source([](void* owner,const nd::Message& input){static_cast<Device*>(owner)->process(input.frame_view());},&d)==Status::ok);
        CHECK(d.processed_frames.load()==before_processing+32);CHECK(d.executor->running());CHECK(!d.data);

 return 0;
}
static int find_bootstrap_terminal(const domain::NumericProcessingGraph& graph,domain::NodeId& terminal_id){
    const auto terminal=std::find_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& candidate){return candidate.kind=="NetworkOutput";});
    CHECK(terminal!=graph.nodes.end());
    CHECK(std::count_if(graph.nodes.begin(),graph.nodes.end(),[](const auto& candidate){return candidate.kind=="NetworkOutput";})==1);
    CHECK(terminal->inputs.size()==1&&terminal->inputs.front().id==0);
    terminal_id=terminal->id;
    return 0;
}
static int select_queue_terminal(Device& d,domain::NodeId& terminal_id){
    if(const int failure=find_bootstrap_terminal(*d.state.applied_numeric,terminal_id);failure)return failure;
    d.selected_endpoint.store((std::uint64_t(std::uint32_t(terminal_id))<<16)|0);
    return 0;
}
static int disable_bootstrap_terminal(domain::NumericProcessingGraph& graph){
    domain::NodeId terminal_id=-1;
    if(const int failure=find_bootstrap_terminal(graph,terminal_id);failure)return failure;
    for(auto& node:graph.nodes)if(node.id==terminal_id)node.enabled=false;
    return 0;
}
static bool bootstrap_terminal_enabled(const domain::NumericProcessingGraph& graph){
    return std::any_of(graph.nodes.begin(),graph.nodes.end(),[](const auto& node){return node.kind=="NetworkOutput"&&node.enabled;});
}
static int produce_queue_frames(Device& d){
    for(unsigned i=0;i<32;++i)CHECK(d.source.tick_source([](void* owner,const nd::Message& input){static_cast<Device*>(owner)->process(input.frame_view());},&d)==Status::ok);
    return 0;
}
static int verify_queue_identity(Device& d,domain::NodeId terminal_id){
    signal_contracts::OwnedDataFrame owned;domain::NodeId node;domain::PortId port;dataflow_runtime::DeliveryStamp delivery;
    CHECK(d.output_queue.pop(owned,node,port,delivery));CHECK(node==terminal_id&&port==0);
    CHECK(delivery.epoch==d.state.epoch && delivery.graph_revision==d.state.graph_revision);
    CHECK(owned.view().stamp.producer_epoch==1 && owned.view().stamp.timestamp_ticks>0);
    CHECK(d.output_queue.dropped.load()>0);CHECK(d.processing_us.load()<=d.maximum_processing_us.load()*d.processed_frames.load());
    while(d.output_queue.pop(owned,node,port,delivery)){}owned={};d.selected_endpoint.store(UINT64_MAX);
    return 0;
}
static int bootstrap_queue_identity(Device& d){
    // Queue delivery identity is independent of source epoch/revision=1.
    domain::NodeId terminal_id=-1;
    if(const int failure=select_queue_terminal(d,terminal_id);failure)return failure;
    if(const int failure=produce_queue_frames(d);failure)return failure;
    return verify_queue_identity(d,terminal_id);
}
static int bootstrap_executor_frames(Device& d){
        struct Run {Device* device;std::size_t outputs=0;bool valid=true;} run{&d};
        for(unsigned i=0;i<128;++i)CHECK(d.source.tick_source([](void* user,const nd::Message& source) {
            auto& run=*static_cast<Run*>(user);auto& d=*run.device;
            const auto status=d.executor->process_numeric_ports(0,source.frame_view(),{d.state.graph_revision,d.state.epoch},
                {nullptr,[](void* user,const dataflow_runtime::NumericPortFrameView& frame) {
                    auto& run=*static_cast<Run*>(user);++run.outputs;
                    run.valid &= frame.status==Status::ok && signal_contracts::validate_view(frame.frame,run.device->effective.runtime.admission.frame)==Status::ok;
                },nullptr,&run});
            run.valid &= status==Status::ok;
        },&run)==Status::ok);
        CHECK(run.valid && run.outputs>0);

 return 0;
}
static int host_spectral_refusal_status(Device& d,const auto& reply){
    CHECK(d.profile.memory_bytes==1024*1024);
    CHECK(!reply.accepted&&reply.status==Status::exhausted);
    CHECK(!reply.prepared_token&&!reply.state.own_prepared_token);
    return 0;
}
static int verify_working_replacement(Device& d,const auto& before,const auto& working,const auto* working_plan){
    CHECK(d.state.revision==before.revision&&d.state.graph_revision==before.graph_revision&&d.state.epoch==before.epoch);
    CHECK(d.state.run_requested==before.run_requested&&d.executor->running());
    CHECK(d.executor->plan()==working_plan);
    CHECK(d.state.applied_numeric&&application::same_numeric_processing_graph(working,*d.state.applied_numeric));
    return 0;
}
static int host_spectral_replacement_refusal(Device& d,domain::NumericProcessingGraph& saved,const auto& reply,
    const application::ControlState& before,const domain::NumericProcessingGraph& working,const dataflow_runtime::PreparedGraph* working_plan){
    if(const int failure=host_spectral_refusal_status(d,reply);failure)return failure;
    if(const int failure=verify_working_replacement(d,before,working,working_plan);failure)return failure;
    saved=working;
    std::printf("host64 spectral%u: initial preparation positive; 1MiB detached replacement exhausted atomically, old graph retained\n",unsigned(FftSize));
    return 0;
}
#ifdef ORDINARY_PROFILE
static int ordinary_replacement_admission(Device& d,domain::NumericProcessingGraph& saved,const auto& reply){
    CHECK(d.profile.memory_bytes==320*1024);CHECK(Spectral && FftSize==2048);
    if(!reply.accepted) {
        CHECK(reply.status==Status::exhausted);CHECK(d.state.run_requested && bootstrap_terminal_enabled(*d.state.applied_numeric));
        saved=*d.state.applied_numeric;
        std::puts("ordinary atomic replacement: exhausted, working graph preserved");
    }
    return 0;
}
#endif
static int replacement_admission(Device& d,domain::NumericProcessingGraph& saved,const auto& reply,
    const application::ControlState& before,const domain::NumericProcessingGraph& working,const dataflow_runtime::PreparedGraph* working_plan){
    if constexpr(sizeof(void*)==8&&Spectral&&MemoryBytes==1024*1024&&(FftSize==2048||FftSize==4096))
        return host_spectral_replacement_refusal(d,saved,reply,before,working,working_plan);
    static_cast<void>(before);static_cast<void>(working);static_cast<void>(working_plan);
#ifdef ORDINARY_PROFILE
    return ordinary_replacement_admission(d,saved,reply);
#else
    static_cast<void>(d);static_cast<void>(saved);
    CHECK(reply.accepted);
    return 0;
#endif
}
static int persist_replacement(Device& d){
        CHECK(writes==0);
        const auto reply=d.submit(application::ExplicitPersist{d.state.graph_revision});CHECK(reply.accepted);CHECK(writes==1);
 return 0;
}
static int bootstrap_replacement(Device& d,domain::NumericProcessingGraph& saved){
        CHECK(!d.submit(application::StopProcessing{}).accepted);CHECK(!d.submit(application::StartProcessing{}).accepted);
        const auto before=d.state;
        const auto working=*d.state.applied_numeric;
        const auto* working_plan=d.executor->plan();
        saved=working;
        if(const int failure=disable_bootstrap_terminal(saved);failure)return failure;
        application::ProcessingDTO dto;dto.numeric=saved;auto reply=d.submit(application::PrepareProcessing{&dto});
        CHECK(replacement_admission(d,saved,reply,before,working,working_plan)==0);
        if(reply.accepted){reply=d.submit(application::ApplyProcessing{reply.prepared_token,saved.graph_id});CHECK(reply.accepted);}
        CHECK(persist_replacement(d)==0);
 return 0;
}
static int bootstrap_reload(const domain::NumericProcessingGraph& saved){
    {Device d;CHECK(prepare_host_bootstrap(d)==Status::ok);CHECK(d.state.applied_numeric);CHECK(application::same_numeric_processing_graph(saved,*d.state.applied_numeric));CHECK(writes==1);}

    return 0;
}
static int bootstrap_memory_refusal(){
    const auto prior_opens=opens;available_memory=64*1024;
    {Device d;CHECK(prepare_host_bootstrap(d)==Status::exhausted);CHECK(!d.service);CHECK(opens==prior_opens);CHECK(writes==1);}
    return 0;
}
static int ordinary_host_refusal_admission(Device& d){
    CHECK(d.profile.memory_bytes==320*1024&&Spectral&&FftSize==2048);
    const auto status=prepare_host_bootstrap(d);
    std::fprintf(stderr,"ordinary host resource admission status=%d memory=%zu runtime_peak=%zu\n",int(status),d.profile.memory_bytes,d.effective.runtime.resources.max_peak_bytes);
    CHECK(status==Status::exhausted);
    CHECK(!d.state.applied_numeric&&!d.state.run_requested);
    CHECK(!d.executor||!d.executor->running());
    return 0;
}
static int ordinary_host_refusal_side_effects(const Device& d,unsigned prior_opens,unsigned prior_writes){
    CHECK(!d.data&&!d.control&&!d.local.session);
    CHECK(d.processed_frames.load()==0);
    CHECK(writes==prior_writes&&records.empty());
    CHECK(opens>=prior_opens&&opens<=prior_opens+1);
    return 0;
}
static int ordinary_host_resource_refusal(){
    const auto prior_opens=opens,prior_writes=writes;
    {Device d;CHECK(ordinary_host_refusal_admission(d)==0);CHECK(ordinary_host_refusal_side_effects(d,prior_opens,prior_writes)==0);}
    CHECK(closes==opens&&writes==prior_writes);
    std::puts("ordinary 320KiB host64 budget: exhausted atomically; no write, binding, execution; restore read cleaned up");
    return 0;
}
int main(){
#ifdef ORDINARY_PROFILE
 constexpr bool ordinary_host_budget_refusal=sizeof(void*)==8;
#else
 constexpr bool ordinary_host_budget_refusal=false;
#endif
 if(ordinary_host_budget_refusal)return ordinary_host_resource_refusal();
 domain::NumericProcessingGraph saved;
 {Device d;
  CHECK(bootstrap_initial(d)==0);CHECK(bootstrap_persist_budget(d)==0);
  CHECK(bootstrap_processing(d)==0);CHECK(bootstrap_queue_identity(d)==0);CHECK(bootstrap_executor_frames(d)==0);
  CHECK(bootstrap_replacement(d,saved)==0);
 }
    CHECK(bootstrap_reload(saved)==0);
    CHECK(bootstrap_memory_refusal()==0);
    return 0;
}
