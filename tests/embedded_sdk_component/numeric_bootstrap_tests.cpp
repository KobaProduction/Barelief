#include "../../examples/esp32_streamer/main/main.cpp"
#include <map>
#include <cstdio>
#include <cstring>
#include <deque>
static std::size_t available_memory=8*1024*1024;
static std::map<std::string,std::vector<std::uint8_t>> records;
static unsigned writes=0,opens=0;
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
void nvs_close(nvs_handle_t){}
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
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);return 1;}}while(false)
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
static int control_state_both(Device& device) {
    for(const auto mask:{1u,2u}) {
        control::numeric::SessionConfig config;config.endpoint_authorized=true;config.device_identity="numeric-esp32";
        config.codec_mask=mask;config.max_packet_bytes=PacketBytes;config.codec_limits=device.control_limits();
        control::numeric::ControlSession client(config,device.clock);FakeIo a,b;a.other=&b;b.other=&a;b.segment=19;
        unsigned replies=0;bool valid=false;
        struct Reply {unsigned* count;bool* valid;} capture{&replies,&valid};
        client.set_reply_handler([](const nc::Message& message,void* user) {
            auto& c=*static_cast<Reply*>(user);const auto& reply=std::get<nc::Reply>(message.body);
            ++*c.count;*c.valid=reply.error==nc::Error::None && reply.state.applied && reply.state.applied->nodes.size()==(Spectral?7:3);
        },&capture);
        auto drain=[&] {
            for(unsigned i=0;i<10000 && (client.pending_bytes() || device.control->pending_bytes() || !a.input.empty() || !b.input.empty());++i) {
                if(client.poll(a)!=Status::ok || device.control->poll(b)!=Status::ok)return false;
            }
            return !client.pending_bytes() && !device.control->pending_bytes() && a.input.empty() && b.input.empty();
        };
        CHECK(client.open(mask==1?101:7)==Status::ok);CHECK(drain());CHECK(client.state()==control::ControlSessionState::Ready);
        nc::Command command;command.id=nc::CommandId::State;CHECK(client.send(command,1)==Status::ok);CHECK(drain());CHECK(replies==1 && valid);
        CHECK(!device.cold.leased);const auto revision=device.state.graph_revision;
        const auto busy=device.submit(application::GetState{});CHECK(!busy.accepted && busy.error==application::ControlError::limit_exceeded);
        CHECK(device.state.graph_revision==revision && device.state.applied_numeric && !device.local.session);
        const auto observed=device.service->observe_state();CHECK(observed.graph_revision==revision && !observed.own_prepared_token);
        device.control->reset();
    }
    return 0;
}
int main(){
    domain::NumericProcessingGraph saved;
    {Device d;const auto status=d.prepare();std::fprintf(stderr,"stock bootstrap status=%d spectral=%d fft=%u\n",int(status),int(Spectral),unsigned(FftSize));
        std::fprintf(stderr,"stock budget memory=%zu control=%zu adapter=%zu pool=%zu runtime_peak=%zu replay=%zu cold=%zu device=%zu\n",
            d.profile.memory_bytes,d.profile.control_bytes,d.profile.adapter_bytes,d.profile.queue_frames*d.profile.max_frame_bytes,
            d.effective.runtime.resources.max_peak_bytes,d.caps.control.max_replay_bytes_per_session,d.cold.bytes,sizeof(Device));
        {auto stock=embedded_support::default_processing_graph(d.profile,StreamId);std::size_t owned=0;const auto bytes_status=domain::processing_graph_owned_bytes(stock,d.effective.runtime.admission,owned);std::fprintf(stderr,"stock DTO owned=%zu status=%d ceiling=%zu\n",owned,int(bytes_status),d.effective.runtime.admission.max_owned_metadata_bytes);}
        {auto stock=embedded_support::default_processing_graph(d.profile,StreamId);auto diagnostic_caps=d.capabilities();
            diagnostic_caps.profile.max_peak_bytes=SIZE_MAX;
            std::array<std::size_t,16> workspace{};domain::ProcessingGraphAdmission required;
            const auto admitted=domain::admit_processing_graph(stock,d.effective.runtime.admission,diagnostic_caps,workspace,required);
            std::fprintf(stderr,"stock model validation=%d node=%d required_peak=%zu plan=%zu payload=%zu scratch=%zu stack=%zu retained=%zu limit=%zu\n",
                int(admitted.error),int(admitted.node),required.peak_bytes,required.resources.plan_bytes,required.resources.payload_bytes,
                required.resources.scratch_bytes,required.resources.stack_bytes,diagnostic_caps.profile.retained_working_bytes,d.effective.runtime.resources.max_peak_bytes);}
        if(status!=Status::ok && d.service){application::ProcessingDTO probe;probe.numeric=embedded_support::default_processing_graph(d.profile,StreamId);const auto reply=d.submit(application::PrepareProcessing{&probe});std::fprintf(stderr,"prepare status=%d control=%d validation=%d node=%d\n",int(reply.status),int(reply.error),int(reply.numeric_validation.error),int(reply.numeric_validation.node));
            const auto caps=d.capabilities();for(const auto& node:probe.numeric->nodes){domain::ResolvedProcessingNode resolved;domain::resolve_processing_node(node,d.effective.runtime.admission,resolved);const auto s=caps.query(caps.context,resolved,caps.profile);std::fprintf(stderr,"node=%d plan=%zu payload=%zu scratch=%zu stack=%zu\n",int(node.id),s.resources.plan_bytes,s.resources.payload_bytes,s.resources.scratch_bytes,s.resources.stack_bytes);}}
        CHECK(status==Status::ok);
        CHECK(d.control->state()!=control::ControlSessionState::Failed);CHECK(d.state.applied_numeric);CHECK(d.state.run_requested);CHECK(writes==0);
        std::printf("memory=%zu control=%zu adapter=%zu pool=%zu runtime_peak=%zu replay=%zu cold=%zu device=%zu\n",
            d.profile.memory_bytes,d.profile.control_bytes,d.profile.adapter_bytes,d.profile.queue_frames*d.profile.max_frame_bytes,
            d.effective.runtime.resources.max_peak_bytes,d.caps.control.max_replay_bytes_per_session,d.cold.bytes,sizeof(Device));
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
        // No binding/network: the real Device processing entrypoint still advances.
        const auto before_processing=d.processed_frames.load();
        for(unsigned i=0;i<32;++i)CHECK(d.source.tick_source([](void* owner,const nd::Message& input){static_cast<Device*>(owner)->process(input.frame_view());},&d)==Status::ok);
        CHECK(d.processed_frames.load()==before_processing+32);CHECK(d.executor->running());CHECK(!d.data);
        // Queue delivery identity is independent of source epoch/revision=1.
        d.selected_endpoint.store((std::uint64_t(std::uint32_t(d.state.applied_numeric->nodes.back().id))<<16)|0);
        for(unsigned i=0;i<32;++i)CHECK(d.source.tick_source([](void* owner,const nd::Message& input){static_cast<Device*>(owner)->process(input.frame_view());},&d)==Status::ok);
        signal_contracts::OwnedDataFrame owned;domain::NodeId node;domain::PortId port;dataflow_runtime::DeliveryStamp delivery;
        CHECK(d.output_queue.pop(owned,node,port,delivery));CHECK(delivery.epoch==d.state.epoch && delivery.graph_revision==d.state.graph_revision);
        CHECK(owned.view().stamp.producer_epoch==1 && owned.view().stamp.timestamp_ticks>0);
        CHECK(d.output_queue.dropped.load()>0);CHECK(d.processing_us.load()<=d.maximum_processing_us.load()*d.processed_frames.load());
        while(d.output_queue.pop(owned,node,port,delivery)){}owned={};d.selected_endpoint.store(UINT64_MAX);

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
        CHECK(!d.submit(application::StopProcessing{}).accepted);CHECK(!d.submit(application::StartProcessing{}).accepted);
        saved=*d.state.applied_numeric;saved.nodes.back().enabled=false;
        application::ProcessingDTO dto;dto.numeric=saved;auto reply=d.submit(application::PrepareProcessing{&dto});
#ifdef ORDINARY_PROFILE
        CHECK(d.profile.memory_bytes==320*1024);CHECK(Spectral && FftSize==2048);
        if(!reply.accepted) {
            CHECK(reply.status==Status::exhausted);CHECK(d.state.run_requested && d.state.applied_numeric->nodes.back().enabled);
            saved=*d.state.applied_numeric;
            std::puts("ordinary atomic replacement: exhausted, working graph preserved");
        }
#else
        CHECK(reply.accepted);
#endif
        if(reply.accepted){reply=d.submit(application::ApplyProcessing{reply.prepared_token,saved.graph_id});CHECK(reply.accepted);}
        CHECK(writes==0);
        reply=d.submit(application::ExplicitPersist{d.state.graph_revision});CHECK(reply.accepted);CHECK(writes==1);
    }
    {Device d;CHECK(d.prepare()==Status::ok);CHECK(d.state.applied_numeric);CHECK(application::same_numeric_processing_graph(saved,*d.state.applied_numeric));CHECK(writes==1);}
    const auto prior_opens=opens;available_memory=64*1024;
    {Device d;CHECK(d.prepare()==Status::exhausted);CHECK(!d.service);CHECK(opens==prior_opens);CHECK(writes==1);}
    return 0;
}
