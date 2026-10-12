#include "sdkconfig.h"
#include "host_hooks.h"
#include <embedded_support/synthetic_pipeline.h>
#include <control/numeric_control_session.h>
#include <control/numeric_processing_message_mapper.h>
#include <esp_idf_adapter/byte_transport.h>
#include <esp_idf_adapter/nvs_persistence.h>
#include <nvs_flash.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <numeric>
using signal_contracts::Status;
extern "C" bool __attribute__((weak)) streamer_host_setup(){return true;}
extern "C" int __attribute__((weak)) streamer_host_acquire_socket(){return -1;}
extern "C" void __attribute__((weak)) streamer_host_release_socket(int){}
extern "C" Status __attribute__((weak)) streamer_host_read_pcm(signal_contracts::DataFrameView*){return Status::would_block;}
extern "C" void __attribute__((weak)) streamer_host_report_processing(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t){}
extern "C" void __attribute__((weak)) streamer_host_report_processing_errors(std::uint64_t,Status){}
extern "C" void __attribute__((weak)) streamer_host_report_node_stats(const dataflow_runtime::NodeStats*,std::size_t){}
extern "C" application::ProcessingPersistencePort* __attribute__((weak)) streamer_host_persistence(){return nullptr;}
extern "C" Status __attribute__((weak)) streamer_host_restore_processing(domain::NumericProcessingGraph*){return Status::unavailable;}
extern "C" std::size_t __attribute__((weak)) streamer_host_memory_budget(){return heap_caps_get_free_size(MALLOC_CAP_8BIT);}
namespace {
namespace nd=bdsp_protocol::numeric_data;
namespace nc=bdsp_protocol::numeric_control;
constexpr const char* Tag="numeric-stream";
constexpr std::size_t BlockSamples=256,FrameBytes=4096;
// Separate paths no longer nest CONTROL codec/persistence frames above DSP.
// Eight admitted DSP nodes reserve16KiB dispatch plus8KiB conversion kernel stack;
// CONTROL uses externally allocated staging rather than large stack arrays.
constexpr std::size_t AcquisitionStackBytes=24*1024,TaskStackBytes=16*1024;
#ifdef CONFIG_STREAMER_CONTROL_WIRE_KIB
constexpr std::size_t ControlWire=CONFIG_STREAMER_CONTROL_WIRE_KIB*1024;
#else
constexpr std::size_t ControlWire=4*1024;
#endif
#ifdef CONFIG_STREAMER_CONTROL_ARENA_KIB
constexpr std::size_t ControlArena=CONFIG_STREAMER_CONTROL_ARENA_KIB*1024;
#else
constexpr std::size_t ControlArena=8*1024;
#endif
constexpr std::size_t PacketBytes=1024;
#ifdef CONFIG_STREAMER_SPECTRAL
constexpr bool Spectral=true;
#else
constexpr bool Spectral=false;
#endif
#ifdef CONFIG_STREAMER_FFT_SIZE
constexpr std::uint32_t FftSize=CONFIG_STREAMER_FFT_SIZE;
#else
constexpr std::uint32_t FftSize=std::min(2048,PORTABLE_DSP_MAX_FFT_SIZE);
#endif
#ifdef CONFIG_STREAMER_MEMORY_KIB
constexpr std::size_t MemoryBytes=CONFIG_STREAMER_MEMORY_KIB*1024;
#else
constexpr std::size_t MemoryBytes=320*1024;
#endif
constexpr const char* StreamId=Spectral?"esp32-mel":"esp32-raw";
embedded_support::Profile example_profile(std::size_t available_memory) {
    embedded_support::Profile p;p.spectral=Spectral;p.fft_size=FftSize;p.memory_bytes=std::min(MemoryBytes,available_memory);p.block_samples=BlockSamples;
    p.channels=1;
#ifdef CONFIG_STREAMER_SAMPLE_RATE
    p.sample_rate=CONFIG_STREAMER_SAMPLE_RATE;
#endif
#if defined(CONFIG_STREAMER_ANALYSIS_RATE_NUMERATOR) && defined(CONFIG_STREAMER_ANALYSIS_RATE_DENOMINATOR)
    constexpr auto divisor=std::gcd(CONFIG_STREAMER_ANALYSIS_RATE_NUMERATOR,CONFIG_STREAMER_ANALYSIS_RATE_DENOMINATOR);
    p.execution_interval={CONFIG_STREAMER_ANALYSIS_RATE_DENOMINATOR/divisor,
        std::uint64_t(CONFIG_STREAMER_ANALYSIS_RATE_NUMERATOR/divisor)};
#else
    p.execution_interval={1,120};
#endif
    // prepare derives the codec/replay/cold-workspace reservations from their
    // actual compiled ABI before allocating them or opening NVS.
    p.control_bytes=0;
    p.adapter_bytes=40*1024;p.task_stack_bytes=AcquisitionStackBytes;p.packet_payload_capacity=PacketBytes;
#ifdef CONFIG_STREAMER_UART
    p.link=embedded_support::Link::uart_8n1;p.uart_baud=CONFIG_STREAMER_UART_BAUD;
#else
    p.link=embedded_support::Link::tcp;p.tcp_bytes_per_second=CONFIG_STREAMER_TCP_BUDGET;
#endif
    return p;
}
struct Clock final:application::ControlClock {
    std::uint64_t now_ms() const noexcept override{return std::uint64_t(esp_timer_get_time()/1000);}
};
// Cold codec staging and persistence never overlap. Primary decoded CONTROL
// views remain separate while the synchronous service performs ExplicitPersist.
struct ColdWorkspace {
    std::unique_ptr<std::max_align_t[]> storage;
    std::size_t bytes=0;
    bool leased=false;
    Status prepare(std::size_t requested) {
        const auto words=(requested+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t);
        storage.reset(new(std::nothrow) std::max_align_t[words]);
        if(!storage)return Status::exhausted;
        bytes=words*sizeof(std::max_align_t);return Status::ok;
    }
    Status acquire(std::size_t requested,std::span<std::byte>& out) noexcept {
        out={};if(leased)return Status::would_block;
        if(requested>bytes || !storage)return Status::exhausted;
        leased=true;out={reinterpret_cast<std::byte*>(storage.get()),requested};return Status::ok;
    }
    void release() noexcept {leased=false;}
};
struct Buffers {
    std::array<std::uint8_t,FrameBytes> tx{},scratch{};
    std::array<std::uint8_t,PacketBytes+8> fragment{};
    alignas(8) std::array<std::byte,2048> decoded{};
    alignas(8) std::array<std::byte,PacketBytes> handshake{},handshake_scratch{};
    std::array<char,256> text{};std::array<std::uint32_t,2> channels{};
    std::array<signal_contracts::PlaneLayout,2> planes{};
    nd::Arena arena(){return {decoded,text,channels,planes};}
    embedded_support::NumericSessionBuffers spans(){return {{},{},{},tx,scratch,fragment,{},arena()};}
};
// Acquisition owns its sample clock and bounded PCM storage independently of
// slow CONTROL, persistence and transport. Its owner is the sole executor task;
// the bounded queue carries immutable output to the transport consumer.
struct OutputQueue {
    static constexpr std::size_t Capacity=2,Slots=Capacity+1;
    std::array<std::byte,Slots*FrameBytes> payload{};
    alignas(std::max_align_t) std::array<std::byte,Slots*1024> metadata{};
    std::array<signal_contracts::BufferSlot,Slots> payload_slots{},metadata_slots{};
    signal_contracts::BufferPool payload_pool{payload,payload_slots,FrameBytes},metadata_pool{metadata,metadata_slots,1024};
    std::array<signal_contracts::OwnedDataFrame,Slots> frames;
    std::array<dataflow_runtime::DeliveryStamp,Slots> deliveries{};
    std::array<domain::NodeId,Slots> nodes{};std::array<domain::PortId,Slots> ports{};
    std::atomic_size_t read{0},write{0};
    bool gap=false; // producer only
    std::atomic_uint64_t dropped{0};
    void push(signal_contracts::DataFrameView view,const signal_contracts::FrameLimits& limits,domain::NodeId node,domain::PortId port,dataflow_runtime::DeliveryStamp delivery) noexcept {
        const auto cursor=write.load(std::memory_order_relaxed),next=(cursor+1)%Slots;
        if(next==read.load(std::memory_order_acquire)){gap=true;dropped.fetch_add(1,std::memory_order_relaxed);return;}
        view.stamp.discontinuity|=gap;
        if(signal_contracts::retain_frame(view,limits,payload_pool,metadata_pool,frames[cursor])!=Status::ok) {
            gap=true;dropped.fetch_add(1,std::memory_order_relaxed);return;
        }
        nodes[cursor]=node;ports[cursor]=port;deliveries[cursor]=delivery;gap=false;write.store(next,std::memory_order_release);
    }
    bool pop(signal_contracts::OwnedDataFrame& out,domain::NodeId& node,domain::PortId& port,dataflow_runtime::DeliveryStamp& delivery) noexcept {
        const auto cursor=read.load(std::memory_order_relaxed);
        if(cursor==write.load(std::memory_order_acquire))return false;
        node=nodes[cursor];port=ports[cursor];delivery=deliveries[cursor];out=std::move(frames[cursor]);read.store((cursor+1)%Slots,std::memory_order_release);return true;
    }
};
struct Device final:application::NumericGraphPreparationPort {
    embedded_support::Profile profile;
    embedded_support::DeviceProfile effective;
    explicit Device(std::size_t available_memory=streamer_host_memory_budget())
        :profile(example_profile(available_memory)),effective(embedded_support::device_profile(profile)){}
    Clock clock;Buffers buffers;OutputQueue output_queue;
    std::atomic_uint64_t processed_frames{0},processing_us{0},maximum_processing_us{0};
    std::atomic_uint64_t processing_errors{0};
    std::atomic<Status> last_processing_error{Status::ok};
    std::uint64_t clock_samples=0,clock_sequence=0; // acquisition owner only
    std::unique_ptr<dataflow_runtime::GraphExecutor> executor;
    std::mutex processing_mutex;
    std::unique_lock<std::mutex> pending_commit;
    std::atomic_uint64_t delivery_revision{0},delivery_epoch{0},selected_endpoint{UINT64_MAX};
    struct Runtime final:application::GraphRuntimePort {
        Device& owner;
        explicit Runtime(Device& value):owner(value){}
        Status apply(application::PreparedExecution&& prepared) override {
            auto lock=owner.pending_commit.owns_lock()?std::move(owner.pending_commit):std::unique_lock<std::mutex>(owner.processing_mutex);
            const auto status=owner.executor->apply(std::move(prepared));
            if(status==Status::ok){owner.fence();owner.pending_commit=std::move(lock);}return status;
        }
        Status shutdown() override {std::lock_guard lock(owner.processing_mutex);return owner.executor->shutdown();}
    };
    std::unique_ptr<Runtime> delivery;
    std::unique_ptr<application::GraphLifecycle> lifecycle;
    std::unique_ptr<application::GraphLifecycleControlPort> runtime;
    application::ProcessingCapabilities caps;
    std::array<application::ControlSessionSlot,1> slots_{};
    std::unique_ptr<application::ProcessingControlService> service;
    static constexpr std::size_t PersistWire=ControlWire,PersistArena=ControlArena;
    struct PersistenceBuffers {
        std::array<std::uint8_t,PersistWire+embedded_support::ProcessingPersistence::record_header_bytes> first{},second{};
        std::array<std::uint8_t,PersistWire> scratch{};
        alignas(std::max_align_t) std::array<std::byte,PersistArena> arena{},staging{};
        std::array<std::size_t,16> workspace{};
    };
    ColdWorkspace cold;
    nvs_handle_t nvs_handle=0;
    nc::CborCodec persistence_codec;
    std::unique_ptr<esp_idf_adapter::NvsProcessingStorage> storage;
    struct StockPersistence final:application::ProcessingPersistencePort {
        Device& owner;
        explicit StockPersistence(Device& d):owner(d){}
        Status persist_applied(const application::ProcessingDTO& dto,std::uint64_t) override {
            if(!dto.numeric)return Status::invalid_argument;
            return owner.with_persistence([&](auto& persistence){return persistence.save(*dto.numeric);});
        }
    } persistence_port{*this};
    application::SessionIdentity local;std::uint64_t request=0;
    application::ControlState state;nc::Capabilities advertised;
    std::unique_ptr<control::numeric::ControlSession> control;
    embedded_support::SyntheticPipeline source;
    domain::OwnedProcessingContract output_contract;
    std::string output_graph,output_stream;
    embedded_support::NumericSessionBinding binding;
    std::unique_ptr<embedded_support::NumericSession> data;
    bool retire=false,ack_pending=false,gap=true;
    nc::Limits control_limits() const noexcept {
        auto limits=embedded_support::numeric_control_limits(profile);
        limits.wire_bytes=ControlWire;limits.arena_bytes=ControlArena;
        limits.total_text_bytes=4096;limits.records=1024;limits.ports=32;limits.parameters=32;
        return limits;
    }
    control::numeric::SessionConfig session_config() {
        control::numeric::SessionConfig config;config.endpoint_authorized=true;config.device_identity="numeric-esp32";
        config.client_identity=2;config.application_capabilities=&caps;config.advertised_capabilities=&advertised;
        config.codec_limits=control_limits();config.runtime_preflights_reply_envelope=true;
        config.max_packet_bytes=PacketBytes;config.read_chunk_bytes=512;
        config.peer_budget_bytes=std::max<std::size_t>(96*1024,4*ControlWire);
        config.acquire_staging=[](void* context,std::size_t bytes,std::span<std::byte>& output) noexcept {
            return static_cast<ColdWorkspace*>(context)->acquire(bytes,output);
        };
        config.release_staging=[](void* context) noexcept {static_cast<ColdWorkspace*>(context)->release();};
        config.staging_context=&cold;
        return config;
    }
    void configure_runtime() {
        auto& r=effective.runtime;r.admission.max_text_bytes=64;r.admission.max_owned_metadata_bytes=8192;
        r.monotonic_nanoseconds=[](void*) noexcept {return std::uint64_t(esp_timer_get_time())*1000;};
        r.admission.max_ports_per_node=2;r.admission.max_parameters_per_node=4;
        r.admission.frame.max_payload_bytes=std::min<std::size_t>(r.admission.frame.max_payload_bytes,
            std::max<std::size_t>(FftSize,BlockSamples)*sizeof(float));
        r.resources.maximum.plan_bytes=std::min<std::size_t>(32768,r.resources.maximum.plan_bytes);
        const auto m=r.resources;advertised.maximum={m.maximum.plan_bytes,m.maximum.payload_bytes,m.maximum.scratch_bytes,m.maximum.stack_bytes,m.max_peak_bytes};
        caps.numeric=application::NumericProcessingCapabilities{capabilities(),{r.admission,r.resources}};
    }
    domain::NumericDeviceCapabilities capabilities() const noexcept {
        return {this,[](const void* c,const domain::ResolvedProcessingNode& node,const domain::ProcessingResourceProfile& resources) noexcept {
            const auto& d=*static_cast<const Device*>(c);
            if(node.kind=="NetworkInput")return domain::ProcessingSupport{};
            if(node.kind=="SampleClock") {
                const auto* clock=std::get_if<domain::SampleClockProcessingParameters>(node.parameters);
                if(!clock || clock->sample_rate!=d.profile.sample_rate)return domain::ProcessingSupport{};
            } else if(node.kind=="SignalGenerator") {
                if(node.outputs.size()!=1)return domain::ProcessingSupport{};
                const auto contract=node.outputs.front().contract.view();
                const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
                if(!numeric || numeric->clock.sample_rate!=d.profile.sample_rate ||
                    numeric->shape.extents[0].minimum>BlockSamples || numeric->shape.extents[0].maximum<BlockSamples)return domain::ProcessingSupport{};
            } else if(node.role==domain::NodeRole::Source && (node.kind!="AudioSource" || node.id!=0 || node.outputs.size()!=1 ||
               !signal_contracts::same_contract(node.outputs.front().contract.view(),d.source.input_contract())))return domain::ProcessingSupport{};
            const auto actual=embedded_support::numeric_capabilities(d.effective);
            auto support=actual.query(actual.context,node,resources);
            // The stock task owns its entire allocated stack, even when a
            // particular graph's reference dispatch estimate is smaller.
            if(support.supported)support.resources.stack_bytes=std::max(support.resources.stack_bytes,d.profile.task_stack_bytes);
            return support;
        },embedded_support::numeric_capabilities(effective).profile};
    }
    bool discard_data=false;
    void fence() noexcept {selected_endpoint.store(UINT64_MAX,std::memory_order_release);if(data)data->stop();data.reset();ack_pending=false;discard_data=true;gap=true;}
    ~Device(){service.reset();storage.reset();if(nvs_handle)nvs_close(nvs_handle);}
    template<class Operation> Status with_persistence(Operation&& operation) {
        std::span<std::byte> workspace;const auto acquired=cold.acquire(sizeof(PersistenceBuffers),workspace);
        if(acquired!=Status::ok)return acquired;
        struct Release {ColdWorkspace& owner;~Release(){owner.release();}} release{cold};
        auto* buffers=new(workspace.data()) PersistenceBuffers{};
        auto limits=control_limits();limits.wire_bytes=PersistWire;limits.arena_bytes=PersistArena;
        embedded_support::ProcessingPersistence persistence(*storage,
            {buffers->first,buffers->second,buffers->scratch,buffers->arena,buffers->staging,buffers->workspace},
            persistence_codec,limits,effective.runtime.admission,capabilities());
        return operation(persistence);
    }
    void publish_state(const application::ControlState& projected) {
        state=projected;
        delivery_revision.store(state.graph_revision,std::memory_order_relaxed);delivery_epoch.store(state.epoch,std::memory_order_relaxed);
        if(pending_commit.owns_lock())pending_commit.unlock();
    }
    void publish_state(const application::ProcessingReply& reply){publish_state(reply.state);}
    application::ProcessingReply submit(const application::ProcessingCommand& command) {
        // The stock adapter has one wire session. Bootstrap/maintenance borrow
        // its slot only while idle; prepared local ownership spans Prepare/Apply.
        if(!local.session){const auto opened=service->open_session(1);if(!opened){application::ProcessingReply busy;busy.state=state;busy.error=opened.error;busy.status=Status::would_block;return busy;}local=opened.identity;request=0;}
        auto reply=service->submit(local,{++request,state.revision},command);publish_state(reply);
        if(!reply.state.own_prepared_token){service->close_session(local);local={};}return reply;
    }
    Status prepare_numeric(const domain::NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,application::PreparedExecution& prepared) override {
        const auto packet_capacity=control && control->state()==control::ControlSessionState::Ready?control->packet_payload_capacity():profile.packet_payload_capacity;
        const auto admitted=embedded_support::graph_link_admission(profile,graph,packet_capacity);
        if(admitted.status!=Status::ok)return admitted.status;
        if(!control::numeric::reply_envelope_fits(graph,control_limits(),advertised))return Status::exhausted;
        auto query=capabilities();
        dataflow_runtime::GraphExecutor::PreparationBase base;
        {std::lock_guard lock(processing_mutex);
            base=executor->snapshot_preparation_base();}
        if(base.plan) {
            const auto memory=base.plan->memory_requirements();
            // Same immutable profile shares the silence pool across old/new
            // plans. Full physical task stack is still charged once by query.
            const auto retained=memory.state_bytes+memory.buffer_bytes+memory.scratch_bytes-effective.runtime.admission.frame.max_payload_bytes;
            if(retained>SIZE_MAX-query.profile.retained_working_bytes)return Status::overflow;
            query.profile.retained_working_bytes+=retained;
        }
        std::array<std::size_t,16> workspace{};domain::ProcessingGraphAdmission checked;
        const auto result=domain::admit_processing_graph(graph,limits.admission,query,workspace,checked);
        if(!result)return result.error==domain::ProcessingError::ResourceLimit || result.error==domain::ProcessingError::Limit?Status::exhausted:Status::incompatible_contract;
        const auto status=executor->prepare_numeric_detached(graph,limits,base,prepared);
        if(status!=Status::ok)return status;
        std::lock_guard lock(processing_mutex);return executor->register_prepared(prepared,base);
    }
    Status prepare() {
        if(embedded_support::admit(profile,std::char_traits<char>::length(StreamId)).status!=Status::ok)return Status::exhausted;
        configure_runtime();
        caps.control.max_sessions=slots_.size();caps.control.max_field_string_bytes=64;caps.control.max_graph_identity_bytes=64;
        const auto snapshot_bytes=application::control_snapshot_bytes(caps);if(!snapshot_bytes)return Status::exhausted;
        caps.control.max_replay_bytes_per_session=*snapshot_bytes;caps.control.max_replay_bytes_total=slots_.size() * *snapshot_bytes;
        caps.control.max_control_scratch_bytes=*snapshot_bytes;
        auto config=session_config();const auto session_bytes=control::numeric::ControlSession::storage_requirements(config);
        if(!session_bytes)return Status::exhausted;
        config.storage_budget_bytes=session_bytes->persistent_bytes;
        const auto cold_bytes=std::max(sizeof(PersistenceBuffers),session_bytes->external_staging_bytes);
        // DTO replay slots include their own objects. DATA tx/scratch are exactly
        // the two max_frame_bytes owners already charged by Profile::admit.
        profile.adapter_bytes=sizeof(Device)-sizeof(slots_)-2*FrameBytes+BlockSamples*profile.channels*sizeof(float)+8*1024+4096+TaskStackBytes+2*sizeof(StaticTask_t);
        profile.control_bytes=session_bytes->persistent_bytes+cold_bytes+(slots_.size()+1) * *snapshot_bytes+
            sizeof(application::ProcessingControlService)+sizeof(application::GraphLifecycle)+
            sizeof(application::GraphLifecycleControlPort)+sizeof(dataflow_runtime::GraphExecutor)+
            sizeof(Runtime)+sizeof(esp_idf_adapter::NvsProcessingStorage)+4096;
        effective=embedded_support::device_profile(profile);
        if(embedded_support::admit(profile,std::char_traits<char>::length(StreamId)).status!=Status::ok)return Status::exhausted;
        configure_runtime();auto& r=effective.runtime;
        executor=std::make_unique<dataflow_runtime::GraphExecutor>(r);
        delivery=std::make_unique<Runtime>(*this);
        const application::NumericRuntimeLimits limits{r.admission,r.resources};
        const auto query=capabilities();
        lifecycle=std::make_unique<application::GraphLifecycle>(*this,*delivery,query,limits);
        runtime=std::make_unique<application::GraphLifecycleControlPort>(*lifecycle);
        caps.numeric=application::NumericProcessingCapabilities{query,limits};
        if(cold.prepare(cold_bytes)!=Status::ok)return Status::exhausted;
        if(source.prepare_source(profile,StreamId)!=Status::ok)return Status::exhausted;
        auto* host_persistence=streamer_host_persistence();
        domain::NumericProcessingGraph restored;auto restore_status=Status::unavailable;
        if(!host_persistence) {
            // Entrypoint owns NVS lifecycle. Never erase or repair a partition.
            if(nvs_flash_init()!=ESP_OK || nvs_open("numeric",NVS_READWRITE,&nvs_handle)!=ESP_OK)return Status::disconnected;
            storage=std::make_unique<esp_idf_adapter::NvsProcessingStorage>(nvs_handle,PersistWire+embedded_support::ProcessingPersistence::record_header_bytes);
            host_persistence=&persistence_port;restore_status=with_persistence([&](auto& persistence){return persistence.restore(restored);});
        } else restore_status=streamer_host_restore_processing(&restored);
        if(restore_status!=Status::ok && restore_status!=Status::unavailable)return restore_status;
        caps.explicit_persist=true;
        service=std::make_unique<application::ProcessingControlService>(*runtime,clock,caps,slots_,clock.now_ms()+1,host_persistence);
        submit(application::GetState{});
        application::ProcessingDTO dto;dto.numeric=restore_status==Status::ok?std::move(restored):embedded_support::default_processing_graph(profile,StreamId);
        auto reply=submit(application::PrepareProcessing{&dto});if(!reply.accepted)return reply.status;
        reply=submit(application::ApplyProcessing{reply.prepared_token,dto.numeric->graph_id});if(!reply.accepted)return reply.status;
        // No speculative Cartesian inventory. Prepare queries actual combinations.
        config.codec_limits=control_limits();
        control=std::make_unique<control::numeric::ControlSession>(config,clock,service.get());
        control->set_command_commit_handler([](const application::ProcessingReply& reply,void* c){static_cast<Device*>(c)->publish_state(reply);},this);
        control->set_packet_handler([](bdsp_protocol::PacketView p,void* c){static_cast<Device*>(c)->packet(p);},this);
        retire=false;discard_data=false;return control->state()==control::ControlSessionState::Failed?Status::exhausted:Status::ok;
    }
    void packet(bdsp_protocol::PacketView packet) {
        if(packet.id!=nd::SubscribeId || packet.payload.size()>FrameBytes || ack_pending || discard_data)return;
        nd::Message selected;if(!nd::decode(std::as_bytes(packet.payload),selected,buffers.arena(),effective.data))return;
        const auto identity=control->identity();
        if(selected.kind!=nd::Kind::Unavailable || selected.stamp.available || !selected.envelope.request || selected.envelope.request>UINT32_MAX ||
           selected.envelope.nonce!=control->nonce() || selected.envelope.session!=identity.session || selected.envelope.delivery_epoch!=identity.epoch ||
           selected.envelope.revision!=state.graph_revision || selected.envelope.delivery_sequence!=1 || !state.applied_numeric ||
           selected.endpoint.graph!=state.applied_numeric->graph_id || selected.endpoint.direction!=nd::Direction::Input)return;
        if(embedded_support::graph_link_admission(profile,*state.applied_numeric,control->packet_payload_capacity()).status!=Status::ok)return;
        for(const auto& node:state.applied_numeric->nodes)if(node.id==selected.endpoint.node && node.kind=="NetworkOutput")
            for(const auto& port:node.inputs)if(port.id==selected.endpoint.port && signal_contracts::same_contract(port.contract.view(),selected.contract)) {
                output_contract=port.contract;output_graph=state.applied_numeric->graph_id;output_stream=std::string(selected.stamp.stream_id);
                binding={{{std::uint32_t(selected.envelope.request),1},{output_graph,node.id,nd::Direction::Input,port.id},output_stream,output_contract.view(),{},selected.values},
                    {nd::PayloadVersion,unsigned(nd::Feature::Numeric)},control->nonce(),identity.session,state.graph_revision,identity.epoch};
                data=std::make_unique<embedded_support::NumericSession>(embedded_support::Role::publisher,binding,buffers.spans(),effective.data,
                    [](void* c,std::uint8_t id,std::span<const std::uint8_t> bytes){return static_cast<Device*>(c)->control->send_packet(id,bytes);},this,control->packet_payload_capacity());
                if(data->begin()!=Status::ok){data.reset();return;}selected_endpoint.store((std::uint64_t(std::uint32_t(node.id))<<16)|port.id,std::memory_order_release);ack_pending=true;gap=true;return;
            }
    }
    void acknowledge() {
        if(!ack_pending || !data)return;
        nd::Message ack;ack.kind=nd::Kind::Unavailable;ack.endpoint=binding.binding.endpoint;ack.contract=binding.binding.contract;
        ack.stamp.available=false;ack.stamp.stream_id=binding.binding.stream;
        ack.envelope={binding.nonce,binding.session,1,binding.publisher_epoch,0,binding.revision,binding.binding.reference.binding};
        const auto result=nd::encode(ack,buffers.handshake,buffers.handshake_scratch,effective.data);if(!result){fence();return;}
        const auto sent=control->send_packet(nd::AckId,{reinterpret_cast<const std::uint8_t*>(buffers.handshake.data()),result.produced});
        if(sent==Status::ok)ack_pending=false;else if(sent!=Status::would_block)fence();
    }
    void process(const signal_contracts::DataFrameView& input) noexcept {
        std::lock_guard lock(processing_mutex);
        if(!executor->running())return;
        const auto* graph=executor->plan()->numeric_definition();
        if(!graph || std::none_of(graph->nodes.begin(),graph->nodes.end(),[](const auto& node){return node.id==0 && node.kind=="AudioSource";}))return;
        const auto began=esp_timer_get_time();
        const auto status=executor->process_numeric_ports(0,input,{delivery_revision.load(std::memory_order_relaxed),delivery_epoch.load(std::memory_order_relaxed)},
            {nullptr,[](void* c,const dataflow_runtime::NumericPortFrameView& out) {
                auto& owner=*static_cast<Device*>(c);
                if(owner.selected_endpoint.load(std::memory_order_acquire)!=((std::uint64_t(std::uint32_t(out.identity.node))<<16)|out.identity.port))return;
                owner.output_queue.push(out.frame,owner.effective.data.frame,out.identity.node,out.identity.port,out.delivery);
            },nullptr,this});
        if(status==Status::ok)processed_frames.fetch_add(1,std::memory_order_relaxed);
        else {
            processing_errors.fetch_add(1,std::memory_order_relaxed);
            last_processing_error.store(status,std::memory_order_relaxed);
        }
        const auto elapsed=std::uint64_t(esp_timer_get_time()-began);processing_us.fetch_add(elapsed,std::memory_order_relaxed);
        auto prior=maximum_processing_us.load(std::memory_order_relaxed);
        while(prior<elapsed&&!maximum_processing_us.compare_exchange_weak(prior,elapsed,std::memory_order_relaxed)){}
    }
    void tick_clocks(bool missing_input) noexcept {
        std::lock_guard lock(processing_mutex);
        if(!executor->running() || clock_sequence==UINT64_MAX || clock_samples>std::uint64_t(INT64_MAX)-BlockSamples)return;
        signal_contracts::FrameStamp event;
        event.sequence=event.source_sequence=++clock_sequence;
        event.epoch=event.producer_epoch=delivery_epoch.load(std::memory_order_relaxed);
        event.generation=event.epoch;event.graph_revision=delivery_revision.load(std::memory_order_relaxed);
        event.timestamp_ticks=event.sample_begin=std::int64_t(clock_samples);
        event.sample_count=BlockSamples;event.has_sample_interval=true;event.stream_id="esp32-clock";
        const dataflow_runtime::DeliveryStamp delivery{event.graph_revision,event.epoch};
        const dataflow_runtime::NumericRuntimeCallbacks callbacks{nullptr,[](void* c,const dataflow_runtime::NumericPortFrameView& out) {
            auto& owner=*static_cast<Device*>(c);
            if(owner.selected_endpoint.load(std::memory_order_acquire)==((std::uint64_t(std::uint32_t(out.identity.node))<<16)|out.identity.port))
                owner.output_queue.push(out.frame,owner.effective.data.frame,out.identity.node,out.identity.port,out.delivery);
        },nullptr,this};
        const auto* graph=executor->plan()->numeric_definition();
        auto observe=[&](Status status) {
            if(status!=Status::ok){processing_errors.fetch_add(1,std::memory_order_relaxed);last_processing_error.store(status,std::memory_order_relaxed);}
        };
        if(graph) {
            if(missing_input)for(const auto& node:graph->nodes)if(node.kind=="AudioSource" && node.id==0)
                observe(executor->advance_unavailable_source(node.id,event,delivery,callbacks));
            for(const auto& node:graph->nodes)if(node.kind=="SampleClock")observe(executor->process_clock(node.id,event,delivery,callbacks));
        }
        clock_samples+=BlockSamples;
    }
    void transmit_outputs() {
        for(std::size_t i=0;i<OutputQueue::Capacity;++i) {
            signal_contracts::OwnedDataFrame retained;domain::NodeId node;domain::PortId port;dataflow_runtime::DeliveryStamp delivery;
            if(!output_queue.pop(retained,node,port,delivery))break;
            const auto frame=retained.view();
            if(!data || ack_pending || retire || discard_data || node!=binding.binding.endpoint.node || port!=binding.binding.endpoint.port ||
               delivery.graph_revision!=state.graph_revision || delivery.epoch!=state.epoch)continue;
            const auto selected=std::find_if(state.applied_numeric->nodes.begin(),state.applied_numeric->nodes.end(),[&](const auto& value){return value.id==node;});
            if(selected==state.applied_numeric->nodes.end()||!selected->enabled)continue;
            nd::Message message;message.endpoint=binding.binding.endpoint;message.contract=binding.binding.contract;message.stamp=frame.stamp;
            message.stamp.stream_id=output_stream;message.stamp.discontinuity|=gap;message.kind=message.stamp.available?nd::Kind::Frame:nd::Kind::Unavailable;
            message.envelope.delivery_generation=message.stamp.generation;
            if(message.stamp.available)if(const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload))
                message.numeric={numeric->storage,numeric->planes,numeric->actual,numeric->ring,numeric->coordinates,numeric->semantic_silence};
            const auto sent=data->publish(message);gap=sent!=Status::ok&&sent!=Status::would_block;
        }
    }

    void report_nodes() {
        // Bounded telemetry snapshot. Host callback runs after serialized access,
        // so diagnostics never call arbitrary firmware code while holding DSP.
        std::array<dataflow_runtime::NodeStats,8> stats{};
        std::size_t count=0;
        {
            std::lock_guard lock(processing_mutex);
            const auto* plan=executor?executor->plan():nullptr;
            const auto* graph=plan?plan->numeric_definition():nullptr;
            if(graph)for(const auto& node:graph->nodes) {
                if(count==stats.size())break;
                if(const auto value=executor->node_stats(node.id))stats[count++]=*value;
            }
        }
        streamer_host_report_node_stats(stats.data(),count);
    }

};
std::unique_ptr<Device> device;
StaticTask_t acquisition_control;
std::unique_ptr<StackType_t[]> acquisition_stack;
void acquire(void*) {
    auto& d=*device;
    auto next=esp_timer_get_time();std::uint64_t remainder=0;
    for(;;) {
        const auto now=esp_timer_get_time();if(now>=next) {
#ifdef CONFIG_STREAMER_EXTERNAL_PCM
            signal_contracts::DataFrameView input;const auto received=streamer_host_read_pcm(&input);
            if(received==Status::ok)d.process(input);
            d.tick_clocks(received!=Status::ok);
#else
            d.source.tick_source([](void* c,const nd::Message& source){static_cast<Device*>(c)->process(source.frame_view());},&d);
            d.tick_clocks(false);
#endif
            // Absolute sample deadlines preserve fractional periods and cannot
            // accumulate transport/task wake-up drift.
            next+=BlockSamples*1000000LL/d.profile.sample_rate;
            remainder+=BlockSamples*1000000LL%d.profile.sample_rate;
            if(remainder>=d.profile.sample_rate){next+=remainder/d.profile.sample_rate;remainder%=d.profile.sample_rate;}
        }
        vTaskDelay(1);
    }
}
StaticTask_t task_control;
std::unique_ptr<StackType_t[]> task_stack;

void worker(void*) {
    auto& d=*device;esp_idf_adapter::TcpTransport tcp;esp_idf_adapter::UartTransport uart;
    QueueHandle_t events=nullptr;bdsp_protocol::ByteTransport* transport=&tcp;int descriptor=-1;
#ifdef CONFIG_STREAMER_UART
    uart_config_t config{};config.baud_rate=CONFIG_STREAMER_UART_BAUD;config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_DEFAULT;
    if(uart_param_config(UART_NUM_1,&config)!=ESP_OK || uart_set_pin(UART_NUM_1,CONFIG_STREAMER_UART_TX,CONFIG_STREAMER_UART_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK ||
       uart_driver_install(UART_NUM_1,8192,0,8,&events,0)!=ESP_OK){ESP_LOGE(Tag,"UART setup failed; processing remains active");}
    transport=&uart;
#endif
    std::uint64_t fence_at=0;
    std::uint64_t reconnect_at=0,report_at=0;
    for(;;) {
        if(d.clock.now_ms()>=report_at) {
            report_at=d.clock.now_ms()+1000;
            streamer_host_report_processing(d.processed_frames.load(std::memory_order_relaxed),d.processing_us.load(std::memory_order_relaxed),
                d.maximum_processing_us.load(std::memory_order_relaxed),d.output_queue.dropped.load(std::memory_order_relaxed));
            streamer_host_report_processing_errors(d.processing_errors.load(std::memory_order_relaxed),d.last_processing_error.load(std::memory_order_relaxed));
            d.report_nodes();
        }
        d.transmit_outputs();
        if(d.discard_data) {
            const auto discarded=d.control->discard_outbound_data();
            if(discarded==Status::ok)d.discard_data=false;
            else if(discarded!=Status::would_block)d.retire=true;
        }
        if(d.retire && transport->state()==bdsp_protocol::TransportState::connected && d.control->state()!=control::ControlSessionState::Failed) {
            // Finish the accepted CONTROL reply while acquisition/processing continues.
            // Then reset the host transport before any new binding is admitted.
            if(!fence_at)fence_at=d.clock.now_ms()+1000;
            d.control->poll(*transport);
            if(d.control->pending_bytes() && d.clock.now_ms()<fence_at){vTaskDelay(1);continue;}
        }
        if(transport->state()!=bdsp_protocol::TransportState::connected || d.control->state()==control::ControlSessionState::Failed || d.retire) {
            d.fence();d.control->reset();tcp.detach();uart.detach();
            if(descriptor>=0){streamer_host_release_socket(descriptor);descriptor=-1;}
#ifdef CONFIG_STREAMER_UART
            if(!events) {
                if(d.clock.now_ms()<reconnect_at){vTaskDelay(1);continue;}
                reconnect_at=d.clock.now_ms()+100;
                if(uart_driver_install(UART_NUM_1,8192,0,8,&events,0)!=ESP_OK){vTaskDelay(1);continue;}
            }
            uart_flush_input(UART_NUM_1);xQueueReset(events);
            if(uart.attach(UART_NUM_1,events)!=Status::ok){vTaskDelay(1);continue;}
            constexpr std::array<std::uint8_t,1> delimiter{0};if(uart.write(delimiter).transferred!=1){vTaskDelay(1);continue;}
#else
            if(d.clock.now_ms()<reconnect_at){vTaskDelay(1);continue;}
            reconnect_at=d.clock.now_ms()+100;
            descriptor=streamer_host_acquire_socket();if(descriptor<0){vTaskDelay(1);continue;}
            if(tcp.attach(descriptor)!=Status::ok){streamer_host_release_socket(descriptor);descriptor=-1;continue;}
#endif
            d.retire=false;d.discard_data=false;fence_at=0;
        }
        const auto status=d.control->poll(*transport);d.publish_state(d.service->observe_state());
        if(status!=Status::ok && status!=Status::would_block){d.retire=true;continue;}
        if(d.retire || d.discard_data)continue;
        d.acknowledge();if(d.data)d.data->drain_packets();
        vTaskDelay(1);
    }
}
}
extern "C" void app_main() {
    if(!streamer_host_setup()){ESP_LOGE(Tag,"Host setup failed");return;}
    const auto available_memory=streamer_host_memory_budget();
    device.reset(new(std::nothrow) Device(available_memory));if(!device){ESP_LOGE(Tag,"Device storage unavailable");return;}const auto status=device->prepare();
    if(status!=Status::ok){ESP_LOGE(Tag,"Numeric profile/control admission refused: %d",int(status));device.reset();return;}
    acquisition_stack.reset(new(std::nothrow) StackType_t[AcquisitionStackBytes/sizeof(StackType_t)]);
    task_stack.reset(new(std::nothrow) StackType_t[TaskStackBytes/sizeof(StackType_t)]);
    if(!task_stack || !acquisition_stack){ESP_LOGE(Tag,"Task storage unavailable");device.reset();task_stack.reset();acquisition_stack.reset();return;}
    const auto producer=xTaskCreateStatic(acquire,"numeric-acquisition",AcquisitionStackBytes,nullptr,6,acquisition_stack.get(),&acquisition_control);
    if(!producer){ESP_LOGE(Tag,"Acquisition task creation failed");device.reset();task_stack.reset();acquisition_stack.reset();return;}
    if(!xTaskCreateStatic(worker,"numeric-device",TaskStackBytes,nullptr,5,task_stack.get(),&task_control)) {
        vTaskDelete(producer);ESP_LOGE(Tag,"Task creation failed");device.reset();task_stack.reset();acquisition_stack.reset();
    }
}
