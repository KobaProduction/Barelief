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
extern "C" bool __attribute__((weak)) streamer_host_setup() {
    return true;
}
extern "C" int __attribute__((weak)) streamer_host_acquire_socket() {
    return -1;
}
extern "C" void __attribute__((weak)) streamer_host_release_socket(int) {}
extern "C" Status __attribute__((weak)) streamer_host_read_pcm(signal_contracts::DataFrameView *) {
    return Status::would_block;
}
extern "C" void __attribute__((weak)) streamer_host_report_processing(std::uint64_t, std::uint64_t, std::uint64_t,
                                                                      std::uint64_t) {}
extern "C" void __attribute__((weak)) streamer_host_report_processing_errors(std::uint64_t, Status) {}
extern "C" void __attribute__((weak)) streamer_host_report_node_stats(const dataflow_runtime::NodeStats *,
                                                                      std::size_t) {}
extern "C" application::ProcessingPersistencePort *__attribute__((weak)) streamer_host_persistence() {
    return nullptr;
}
extern "C" Status __attribute__((weak)) streamer_host_restore_processing(domain::NumericProcessingGraph *) {
    return Status::unavailable;
}
extern "C" std::size_t __attribute__((weak)) streamer_host_memory_budget() {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}
namespace {
namespace nd = bdsp_protocol::numeric_data;
namespace nc = bdsp_protocol::numeric_control;
constexpr const char *Tag = "numeric-stream";
constexpr std::size_t BlockSamples = 256, FrameBytes = 4096;
// Separate paths no longer nest CONTROL codec/persistence frames above DSP.
// Nine admitted nodes reserve 17920 dispatch bytes plus 8192 conversion stack bytes.
// Allocate 32 KiB for that conservative bound and charge the full task stack.
// CONTROL uses externally allocated staging rather than large stack arrays.
constexpr std::size_t AcquisitionStackBytes = 32 * 1024, TaskStackBytes = 16 * 1024;
#ifdef CONFIG_STREAMER_CONTROL_WIRE_KIB
constexpr std::size_t ControlWire = CONFIG_STREAMER_CONTROL_WIRE_KIB * 1024;
#else
constexpr std::size_t ControlWire = 4 * 1024;
#endif
#ifdef CONFIG_STREAMER_CONTROL_ARENA_KIB
constexpr std::size_t ControlArena = CONFIG_STREAMER_CONTROL_ARENA_KIB * 1024;
#else
constexpr std::size_t ControlArena = 8 * 1024;
#endif
constexpr std::size_t PacketBytes = 1024;
#ifdef CONFIG_STREAMER_SPECTRAL
constexpr bool Spectral = true;
#else
constexpr bool Spectral = false;
#endif
#ifdef CONFIG_STREAMER_FFT_SIZE
constexpr std::uint32_t FftSize = CONFIG_STREAMER_FFT_SIZE;
#else
constexpr std::uint32_t FftSize = std::min(2048, PORTABLE_DSP_MAX_FFT_SIZE);
#endif
#ifdef CONFIG_STREAMER_MEMORY_KIB
constexpr std::size_t MemoryBytes = CONFIG_STREAMER_MEMORY_KIB * 1024;
#else
constexpr std::size_t MemoryBytes = 320 * 1024;
#endif
constexpr const char *StreamId = Spectral ? "esp32-mel" : "esp32-raw";
embedded_support::Profile example_profile(std::size_t available_memory) {
    embedded_support::Profile p;
    p.spectral = Spectral;
    p.fft_size = FftSize;
    p.memory_bytes = std::min(MemoryBytes, available_memory);
    p.block_samples = BlockSamples;
    p.channels = 1;
#ifdef CONFIG_STREAMER_SAMPLE_RATE
    p.sample_rate = CONFIG_STREAMER_SAMPLE_RATE;
#endif
#if defined(CONFIG_STREAMER_ANALYSIS_RATE_NUMERATOR) && defined(CONFIG_STREAMER_ANALYSIS_RATE_DENOMINATOR)
    constexpr auto divisor =
        std::gcd(CONFIG_STREAMER_ANALYSIS_RATE_NUMERATOR, CONFIG_STREAMER_ANALYSIS_RATE_DENOMINATOR);
    p.execution_interval = {CONFIG_STREAMER_ANALYSIS_RATE_DENOMINATOR / divisor,
                            std::uint64_t(CONFIG_STREAMER_ANALYSIS_RATE_NUMERATOR / divisor)};
#else
    p.execution_interval = {1, 120};
#endif
    // prepare derives the codec/replay/cold-workspace reservations from their
    // actual compiled ABI before allocating them or opening NVS.
    p.control_bytes = 0;
    p.adapter_bytes = 40 * 1024;
    p.task_stack_bytes = AcquisitionStackBytes;
    p.packet_payload_capacity = PacketBytes;
#ifdef CONFIG_STREAMER_UART
    p.link = embedded_support::Link::uart_8n1;
    p.uart_baud = CONFIG_STREAMER_UART_BAUD;
#else
    p.link = embedded_support::Link::tcp;
    p.tcp_bytes_per_second = CONFIG_STREAMER_TCP_BUDGET;
#endif
    return p;
}
struct Clock final : application::ControlClock {
    std::uint64_t now_ms() const noexcept override {
        return std::uint64_t(esp_timer_get_time() / 1000);
    }
};
// Cold codec staging and persistence never overlap. Primary decoded CONTROL
// views remain separate while the synchronous service performs ExplicitPersist.
struct ColdWorkspace {
    std::unique_ptr<std::max_align_t[]> storage;
    std::size_t bytes = 0;
    bool leased = false;
    Status prepare(std::size_t requested) {
        const auto words = (requested + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
        storage.reset(new (std::nothrow) std::max_align_t[words]);
        if (!storage)
            return Status::exhausted;
        bytes = words * sizeof(std::max_align_t);
        return Status::ok;
    }
    Status acquire(std::size_t requested, std::span<std::byte> &out) noexcept {
        out = {};
        if (leased)
            return Status::would_block;
        if (requested > bytes || !storage)
            return Status::exhausted;
        leased = true;
        out = {reinterpret_cast<std::byte *>(storage.get()), requested};
        return Status::ok;
    }
    void release() noexcept {
        leased = false;
    }
};
struct Buffers {
    std::array<std::uint8_t, FrameBytes> tx{}, scratch{};
    std::array<std::uint8_t, PacketBytes + 8> fragment{};
    alignas(8) std::array<std::byte, 2048> decoded{};
    alignas(8) std::array<std::byte, PacketBytes> handshake{}, handshake_scratch{};
    std::array<char, 256> text{};
    std::array<std::uint32_t, 2> channels{};
    std::array<signal_contracts::PlaneLayout, 2> planes{};
    nd::Arena arena() {
        return {decoded, text, channels, planes};
    }
    embedded_support::NumericSessionBuffers spans() {
        return {{}, {}, {}, tx, scratch, fragment, {}, arena()};
    }
};
// Acquisition owns its sample clock and bounded PCM storage independently of
// slow CONTROL, persistence and transport. Its owner is the sole executor task;
// the bounded queue carries immutable output to the transport consumer.
struct OutputQueue {
    static constexpr std::size_t Capacity = 2, Slots = Capacity + 1;
    std::array<std::byte, Slots * FrameBytes> payload{};
    alignas(std::max_align_t) std::array<std::byte, Slots * 1024> metadata{};
    std::array<signal_contracts::BufferSlot, Slots> payload_slots{}, metadata_slots{};
    signal_contracts::BufferPool payload_pool{payload, payload_slots, FrameBytes},
        metadata_pool{metadata, metadata_slots, 1024};
    std::array<signal_contracts::OwnedDataFrame, Slots> frames;
    std::array<dataflow_runtime::DeliveryStamp, Slots> deliveries{};
    std::array<domain::NodeId, Slots> nodes{};
    std::array<domain::PortId, Slots> ports{};
    std::atomic_size_t read{0}, write{0};
    bool gap = false; // producer only
    std::atomic_uint64_t dropped{0};
    void push(signal_contracts::DataFrameView view, const signal_contracts::FrameLimits &limits, domain::NodeId node,
              domain::PortId port, dataflow_runtime::DeliveryStamp delivery) noexcept {
        const auto cursor = write.load(std::memory_order_relaxed), next = (cursor + 1) % Slots;
        if (next == read.load(std::memory_order_acquire)) {
            gap = true;
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        view.stamp.discontinuity |= gap;
        if (signal_contracts::retain_frame(view, limits, payload_pool, metadata_pool, frames[cursor]) != Status::ok) {
            gap = true;
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        nodes[cursor] = node;
        ports[cursor] = port;
        deliveries[cursor] = delivery;
        gap = false;
        write.store(next, std::memory_order_release);
    }
    bool pop(signal_contracts::OwnedDataFrame &out, domain::NodeId &node, domain::PortId &port,
             dataflow_runtime::DeliveryStamp &delivery) noexcept {
        const auto cursor = read.load(std::memory_order_relaxed);
        if (cursor == write.load(std::memory_order_acquire))
            return false;
        node = nodes[cursor];
        port = ports[cursor];
        delivery = deliveries[cursor];
        out = std::move(frames[cursor]);
        read.store((cursor + 1) % Slots, std::memory_order_release);
        return true;
    }
};
struct Device final : application::NumericGraphPreparationPort {
    embedded_support::Profile profile;
    embedded_support::DeviceProfile effective;
    explicit Device(std::size_t available_memory = streamer_host_memory_budget())
        : profile(example_profile(available_memory)), effective(embedded_support::device_profile(profile)) {}
    Clock clock;
    Buffers buffers;
    OutputQueue output_queue;
    std::atomic_uint64_t processed_frames{0}, processing_us{0}, maximum_processing_us{0};
    std::atomic_uint64_t processing_errors{0};
    std::atomic<Status> last_processing_error{Status::ok};
    std::uint64_t clock_samples = 0, clock_sequence = 0; // acquisition owner only
    std::unique_ptr<dataflow_runtime::GraphExecutor> executor;
    std::mutex processing_mutex;
    std::unique_lock<std::mutex> pending_commit;
    std::atomic_uint64_t delivery_revision{0}, delivery_epoch{0}, selected_endpoint{UINT64_MAX};
    struct Runtime final : application::GraphRuntimePort {
        Device &owner;
        explicit Runtime(Device &value) : owner(value) {}
        Status apply(application::PreparedExecution &&prepared) override {
            auto lock = owner.pending_commit.owns_lock() ? std::move(owner.pending_commit)
                                                         : std::unique_lock<std::mutex>(owner.processing_mutex);
            const auto status = owner.executor->apply(std::move(prepared));
            if (status == Status::ok) {
                owner.fence();
                owner.pending_commit = std::move(lock);
            }
            return status;
        }
        Status shutdown() override {
            std::lock_guard lock(owner.processing_mutex);
            return owner.executor->shutdown();
        }
    };
    std::unique_ptr<Runtime> delivery;
    std::unique_ptr<application::GraphLifecycle> lifecycle;
    std::unique_ptr<application::GraphLifecycleControlPort> runtime;
    application::ProcessingCapabilities caps;
    std::array<application::ControlSessionSlot, 1> slots_{};
    std::unique_ptr<application::ProcessingControlService> service;
    static constexpr std::size_t PersistWire = ControlWire, PersistArena = ControlArena;
    struct PersistenceBuffers {
        std::array<std::uint8_t, PersistWire + embedded_support::ProcessingPersistence::record_header_bytes> first{},
            second{};
        std::array<std::uint8_t, PersistWire> scratch{};
        alignas(std::max_align_t) std::array<std::byte, PersistArena> arena{}, staging{};
        std::array<std::size_t, 2 * bdsp_protocol::numeric_control::device_limits().nodes> workspace{};
    };
    ColdWorkspace cold;
    nvs_handle_t nvs_handle = 0;
    nc::CborCodec persistence_codec;
    std::unique_ptr<esp_idf_adapter::NvsProcessingStorage> storage;
    struct StockPersistence final : application::ProcessingPersistencePort {
        Device &owner;
        explicit StockPersistence(Device &d) : owner(d) {}
        Status persist_applied(const application::ProcessingDTO &dto, std::uint64_t) override {
            if (!dto.numeric)
                return Status::invalid_argument;
            return owner.with_persistence([&](auto &persistence) { return persistence.save(*dto.numeric); });
        }
    } persistence_port{*this};
    application::SessionIdentity local;
    std::uint64_t request = 0;
    application::ControlState state;
    nc::Capabilities advertised;
    std::unique_ptr<control::numeric::ControlSession> control;
    embedded_support::SyntheticPipeline source;
    domain::OwnedProcessingContract output_contract;
    std::string output_graph, output_stream;
    embedded_support::NumericSessionBinding binding;
    std::unique_ptr<embedded_support::NumericSession> data;
    bool retire = false, ack_pending = false, gap = true;
    nc::Limits control_limits() const noexcept {
        auto limits = embedded_support::numeric_control_limits(profile);
        limits.wire_bytes = ControlWire;
        limits.arena_bytes = ControlArena;
        limits.total_text_bytes = 4096;
        limits.records = 1024;
        limits.ports = 32;
        limits.parameters = 32;
        return limits;
    }
    control::numeric::SessionConfig session_config() {
        control::numeric::SessionConfig config;
        config.endpoint_authorized = true;
        config.device_identity = "numeric-esp32";
        config.client_identity = 2;
        config.application_capabilities = &caps;
        config.advertised_capabilities = &advertised;
        config.codec_limits = control_limits();
        config.runtime_preflights_reply_envelope = true;
        config.max_packet_bytes = PacketBytes;
        config.read_chunk_bytes = 512;
        config.peer_budget_bytes = std::max<std::size_t>(96 * 1024, 4 * ControlWire);
        config.acquire_staging = [](void *context, std::size_t bytes, std::span<std::byte> &output) noexcept {
            return static_cast<ColdWorkspace *>(context)->acquire(bytes, output);
        };
        config.release_staging = [](void *context) noexcept { static_cast<ColdWorkspace *>(context)->release(); };
        config.staging_context = &cold;
        return config;
    }
    void configure_runtime() {
        auto &r = effective.runtime;
        r.admission.max_text_bytes = 64;
        r.monotonic_nanoseconds = [](void *) noexcept { return std::uint64_t(esp_timer_get_time()) * 1000; };
        r.admission.max_ports_per_node = 2;
        r.admission.max_parameters_per_node = 5;
        r.admission.frame.max_payload_bytes = std::min<std::size_t>(
            r.admission.frame.max_payload_bytes, std::max<std::size_t>(FftSize, BlockSamples) * sizeof(float));
        const auto m = r.resources;
        advertised.maximum = {m.maximum.plan_bytes, m.maximum.payload_bytes, m.maximum.scratch_bytes,
                              m.maximum.stack_bytes, m.max_peak_bytes};
        caps.numeric = application::NumericProcessingCapabilities{capabilities(), {r.admission, r.resources,{}}};
    }
    domain::NumericDeviceCapabilities capabilities() const noexcept {
        return {this, query_support, embedded_support::numeric_capabilities(effective).profile};
    }
    bool accepts_source(const domain::ResolvedProcessingNode &node) const noexcept {
        if (node.kind == "SampleClock") {
            const auto *clock = std::get_if<domain::SampleClockProcessingParameters>(node.parameters);
            return clock && clock->sample_rate == profile.sample_rate;
        }
        if (node.kind == "SignalGenerator") {
            if (node.outputs.size() != 1)
                return false;
            const auto contract = node.outputs.front().contract.view();
            const auto *numeric = std::get_if<signal_contracts::NumericContract>(&contract);
            return numeric && numeric->clock.sample_rate == profile.sample_rate &&
                   numeric->shape.extents[0].minimum <= BlockSamples &&
                   numeric->shape.extents[0].maximum >= BlockSamples;
        }
        return node.role != domain::NodeRole::Source ||
               (node.kind == "AudioSource" && node.id == 0 && node.outputs.size() == 1 &&
                signal_contracts::same_contract(node.outputs.front().contract.view(), source.input_contract()));
    }
    static domain::ProcessingSupport query_support(const void *context, const domain::ResolvedProcessingNode &node,
                                                   const domain::ProcessingResourceProfile &resources) noexcept {
        const auto &owner = *static_cast<const Device *>(context);
        if (node.kind == "NetworkInput" || !owner.accepts_source(node))
            return {};
        const auto actual = embedded_support::numeric_capabilities(owner.effective);
        auto support = actual.query(actual.context, node, resources);
        // The stock task owns its entire allocated stack, even when a
        // particular graph's reference dispatch estimate is smaller.
        if (support.supported)
            support.resources.stack_bytes = std::max(support.resources.stack_bytes, owner.profile.task_stack_bytes);
        return support;
    }
    bool discard_data = false;
    void fence() noexcept {
        selected_endpoint.store(UINT64_MAX, std::memory_order_release);
        if (data)
            data->stop();
        data.reset();
        ack_pending = false;
        discard_data = true;
        gap = true;
    }
    ~Device() {
        service.reset();
        storage.reset();
        if (nvs_handle)
            nvs_close(nvs_handle);
    }
    template <class Operation> Status with_persistence(Operation &&operation) {
        std::span<std::byte> workspace;
        const auto acquired = cold.acquire(sizeof(PersistenceBuffers), workspace);
        if (acquired != Status::ok)
            return acquired;
        struct Release {
            ColdWorkspace &owner;
            ~Release() {
                owner.release();
            }
        } release{cold};
        auto *buffers = new (workspace.data()) PersistenceBuffers{};
        auto limits = control_limits();
        limits.wire_bytes = PersistWire;
        limits.arena_bytes = PersistArena;
        embedded_support::ProcessingPersistence persistence(
            *storage,
            {buffers->first, buffers->second, buffers->scratch, buffers->arena, buffers->staging, buffers->workspace},
            persistence_codec, limits, effective.runtime.admission, capabilities());
        return operation(persistence);
    }
    void publish_state(const application::ControlState &projected) {
        state = projected;
        delivery_revision.store(state.graph_revision, std::memory_order_relaxed);
        delivery_epoch.store(state.epoch, std::memory_order_relaxed);
        if (pending_commit.owns_lock())
            pending_commit.unlock();
    }
    void publish_state(const application::ProcessingReply &reply) {
        publish_state(reply.state);
    }
    application::ProcessingReply submit(const application::ProcessingCommand &command) {
        // The stock adapter has one wire session. Bootstrap/maintenance borrow
        // its slot only while idle; prepared local ownership spans Prepare/Apply.
        if (!local.session) {
            const auto opened = service->open_session(1);
            if (!opened) {
                application::ProcessingReply busy;
                busy.state = state;
                busy.error = opened.error;
                busy.status = Status::would_block;
                return busy;
            }
            local = opened.identity;
            request = 0;
        }
        auto reply = service->submit(local, {++request, state.revision}, command);
        publish_state(reply);
        if (!reply.state.own_prepared_token) {
            service->close_session(local);
            local = {};
        }
        return reply;
    }
    dataflow_runtime::GraphExecutor::PreparationBase snapshot_base() {
        std::lock_guard lock(processing_mutex);
        return executor->snapshot_preparation_base();
    }
    Status register_candidate(const application::PreparedExecution &prepared,
                              const dataflow_runtime::GraphExecutor::PreparationBase &base) {
        std::lock_guard lock(processing_mutex);
        return executor->register_prepared(prepared, base);
    }
    Status validate_candidate_graph(const domain::NumericProcessingGraph& graph,const application::NumericRuntimeLimits& limits,const domain::NumericDeviceCapabilities& query) const {
        std::array<std::size_t, 2 * bdsp_protocol::numeric_control::device_limits().nodes> workspace{};
        domain::ProcessingGraphAdmission checked;
        const auto result = domain::admit_processing_graph(graph, limits.admission, query, workspace, checked);
        if (!result)
            return result.error == domain::ProcessingError::ResourceLimit ||
                           result.error == domain::ProcessingError::Limit
                       ? Status::exhausted
                       : Status::incompatible_contract;
        return Status::ok;
    }
    Status prepare_numeric(const domain::NumericProcessingGraph &graph, const application::NumericRuntimeLimits &limits,
                           application::PreparedExecution &prepared) override {
        const auto admission = admit_candidate_link(graph);
        if (admission != Status::ok)
            return admission;
        auto query = capabilities();
        const auto base = snapshot_base();
        const auto charged = charge_retained_plan(base, query);
        if (charged != Status::ok)
            return charged;
        const auto admitted=validate_candidate_graph(graph,limits,query);
        if(admitted!=Status::ok)return admitted;
        const auto status = executor->prepare_numeric_detached(graph, limits, base, prepared);
        if (status != Status::ok)
            return status;
        return register_candidate(prepared, base);
    }
    Status admit_candidate_link(const domain::NumericProcessingGraph &graph) const {
        const auto packet_capacity = control && control->state() == control::ControlSessionState::Ready
                                         ? control->packet_payload_capacity()
                                         : profile.packet_payload_capacity;
        const auto admitted = embedded_support::graph_link_admission(profile, graph, packet_capacity);
        if (admitted.status != Status::ok)
            return admitted.status;
        if (!control::numeric::reply_envelope_fits(graph, control_limits(), advertised))
            return Status::exhausted;
        return Status::ok;
    }
    Status charge_retained_plan(const dataflow_runtime::GraphExecutor::PreparationBase &base,
                                domain::NumericDeviceCapabilities &query) const noexcept {
        if (!base.plan)
            return Status::ok;
        const auto memory = base.plan->memory_requirements();
        // Same immutable profile shares the silence pool across old/new plans.
        // Full physical task stack is still charged once by query.
        const auto retained = memory.state_bytes + memory.buffer_bytes + memory.scratch_bytes -
                              effective.runtime.admission.frame.max_payload_bytes;
        if (retained > SIZE_MAX - query.profile.retained_working_bytes)
            return Status::overflow;
        query.profile.retained_working_bytes += retained;
        return Status::ok;
    }
    Status prepare() {
        if (embedded_support::admit(profile, std::char_traits<char>::length(StreamId)).status != Status::ok)
            return Status::exhausted;
        configure_runtime();
        auto config = session_config();
        std::size_t cold_bytes = 0;
        const auto runtime_status = prepare_runtime(config, cold_bytes);
        if (runtime_status != Status::ok)
            return runtime_status;
        domain::NumericProcessingGraph restored;
        Status restore_status = Status::unavailable;
        auto *host_persistence = streamer_host_persistence();
        const auto activation = restore_and_activate(restored, restore_status, host_persistence);
        if (activation != Status::ok)
            return activation;
        return configure_control(config);
    }
    Status prepare_runtime(control::numeric::SessionConfig &config, std::size_t &cold_bytes) {
        const auto budget = prepare_control_budget(config, cold_bytes);
        if (budget != Status::ok)
            return budget;
        const auto runtime_status = create_runtime(cold_bytes);
        if (runtime_status != Status::ok)
            return runtime_status;
        return Status::ok;
    }
    Status restore_and_activate(domain::NumericProcessingGraph &restored, Status &restore_status,
                                decltype(streamer_host_persistence()) host_persistence) {
        const auto restore = restore_graph(restored, restore_status, host_persistence);
        if (restore != Status::ok)
            return restore;
        const auto activation = activate_graph(restored, restore_status, host_persistence);
        if (activation != Status::ok)
            return activation;
        return Status::ok;
    }
    Status configure_control(control::numeric::SessionConfig &config) {
        config.codec_limits = control_limits();
        control = std::make_unique<control::numeric::ControlSession>(config, clock, service.get());
        control->set_command_commit_handler(
            [](const application::ProcessingReply &reply, void *c) { static_cast<Device *>(c)->publish_state(reply); },
            this);
        control->set_packet_handler([](bdsp_protocol::PacketView p, void *c) { static_cast<Device *>(c)->packet(p); },
                                    this);
        retire = false;
        discard_data = false;
        return control->state() == control::ControlSessionState::Failed ? Status::exhausted : Status::ok;
    }
    std::optional<std::size_t> reserve_control_snapshots() {
        caps.control.max_sessions = slots_.size();
        caps.control.max_field_string_bytes = 64;
        caps.control.max_graph_identity_bytes = 64;
        const auto snapshot_bytes = application::control_snapshot_bytes(caps);
        if (!snapshot_bytes)
            return {};
        caps.control.max_replay_bytes_per_session = *snapshot_bytes;
        caps.control.max_replay_bytes_total = slots_.size() * *snapshot_bytes;
        caps.control.max_control_scratch_bytes = *snapshot_bytes;
        return snapshot_bytes;
    }
    void charge_control_storage(std::size_t persistent_bytes,std::size_t cold_bytes,std::size_t snapshot_bytes) {
        // DTO replay slots include their own objects. DATA tx/scratch are exactly
        // the two max_frame_bytes owners already charged by Profile::admit.
        profile.adapter_bytes = sizeof(Device) - sizeof(slots_) - 2 * FrameBytes +
                                BlockSamples * profile.channels * sizeof(float) + 8 * 1024 + 4096 + TaskStackBytes +
                                2 * sizeof(StaticTask_t);
        profile.control_bytes = persistent_bytes + cold_bytes + (slots_.size() + 1) * snapshot_bytes +
                                sizeof(application::ProcessingControlService) + sizeof(application::GraphLifecycle) +
                                sizeof(application::GraphLifecycleControlPort) +
                                sizeof(dataflow_runtime::GraphExecutor) + sizeof(Runtime) +
                                sizeof(esp_idf_adapter::NvsProcessingStorage) + 4096;
        effective = embedded_support::device_profile(profile);
    }
    Status prepare_control_budget(control::numeric::SessionConfig &config, std::size_t &cold_bytes) {
        const auto snapshot_bytes=reserve_control_snapshots();
        if(!snapshot_bytes)return Status::exhausted;
        config = session_config();
        const auto session_bytes = control::numeric::ControlSession::storage_requirements(config);
        if (!session_bytes)
            return Status::exhausted;
        config.storage_budget_bytes = session_bytes->persistent_bytes;
        cold_bytes = std::max(sizeof(PersistenceBuffers), session_bytes->external_staging_bytes);
        charge_control_storage(session_bytes->persistent_bytes,cold_bytes,*snapshot_bytes);
        if (embedded_support::admit(profile, std::char_traits<char>::length(StreamId)).status != Status::ok)
            return Status::exhausted;
        configure_runtime();
        return Status::ok;
    }
    Status create_runtime(std::size_t cold_bytes) {
        auto &r = effective.runtime;
        executor = std::make_unique<dataflow_runtime::GraphExecutor>(r);
        delivery = std::make_unique<Runtime>(*this);
        const application::NumericRuntimeLimits limits{r.admission, r.resources,{}};
        const auto query = capabilities();
        lifecycle = std::make_unique<application::GraphLifecycle>(*this, *delivery, query, limits);
        runtime = std::make_unique<application::GraphLifecycleControlPort>(*lifecycle);
        caps.numeric = application::NumericProcessingCapabilities{query, limits};
        if (cold.prepare(cold_bytes) != Status::ok)
            return Status::exhausted;
        if (source.prepare_source(profile, StreamId) != Status::ok)
            return Status::exhausted;
        return Status::ok;
    }
    Status restore_graph(domain::NumericProcessingGraph &restored, Status &restore_status,
                         application::ProcessingPersistencePort *&host_persistence) {
        if (!host_persistence) {
            // Entrypoint owns NVS lifecycle. Never erase or repair a partition.
            if (nvs_flash_init() != ESP_OK || nvs_open("numeric", NVS_READWRITE, &nvs_handle) != ESP_OK)
                return Status::disconnected;
            storage = std::make_unique<esp_idf_adapter::NvsProcessingStorage>(
                nvs_handle, PersistWire + embedded_support::ProcessingPersistence::record_header_bytes);
            host_persistence = &persistence_port;
            restore_status = with_persistence([&](auto &persistence) { return persistence.restore(restored); });
        } else
            restore_status = streamer_host_restore_processing(&restored);
        if (restore_status != Status::ok && restore_status != Status::unavailable)
            return restore_status;
        return Status::ok;
    }
    Status activate_graph(domain::NumericProcessingGraph &restored, Status restore_status,
                          application::ProcessingPersistencePort *host_persistence) {
        caps.explicit_persist = true;
        service = std::make_unique<application::ProcessingControlService>(*runtime, clock, caps, slots_,
                                                                          clock.now_ms() + 1, host_persistence);
        submit(application::GetState{});
        application::ProcessingDTO dto;
        dto.numeric = restore_status == Status::ok ? std::move(restored)
                                                   : embedded_support::default_processing_graph(profile, StreamId);
        auto reply = submit(application::PrepareProcessing{&dto});
        if (!reply.accepted)
            return reply.status;
        reply = submit(application::ApplyProcessing{reply.prepared_token, dto.numeric->graph_id});
        if (!reply.accepted)
            return reply.status;
        // No speculative Cartesian inventory. Prepare queries actual combinations.
        return Status::ok;
    }
    bool valid_subscription_message(const nd::Message& selected,const application::SessionIdentity& identity) const {
        return !(selected.kind != nd::Kind::Unavailable || selected.stamp.available || !selected.envelope.request ||
            selected.envelope.request > UINT32_MAX || selected.envelope.nonce != control->nonce() ||
            selected.envelope.session != identity.session || selected.envelope.delivery_epoch != identity.epoch ||
            selected.envelope.revision != state.graph_revision || selected.envelope.delivery_sequence != 1 ||
            !state.applied_numeric || selected.endpoint.graph != state.applied_numeric->graph_id ||
            selected.endpoint.direction != nd::Direction::Input);
    }
    void packet(bdsp_protocol::PacketView packet) {
        if (packet.id != nd::SubscribeId || packet.payload.size() > FrameBytes || ack_pending || discard_data)
            return;
        nd::Message selected;
        if (!nd::decode(std::as_bytes(packet.payload), selected, buffers.arena(), effective.data))
            return;
        const auto identity = control->identity();
        if(!valid_subscription_message(selected,identity))return;
        if (embedded_support::graph_link_admission(profile, *state.applied_numeric, control->packet_payload_capacity())
                .status != Status::ok)
            return;
        select_subscription_output(selected,identity);
    }
    bool select_node_subscription(const domain::ConfiguredProcessingNode& node,const nd::Message& selected,const application::SessionIdentity& identity) {
        for(const auto& port:node.inputs) {
            if(port.id!=selected.endpoint.port || !signal_contracts::same_contract(port.contract.view(),selected.contract))continue;
            install_subscription(selected,identity,node,port);return true;
        }
        return false;
    }
    void select_subscription_output(const nd::Message& selected,const application::SessionIdentity& identity) {
        for(const auto& node:state.applied_numeric->nodes)
            if(node.id==selected.endpoint.node && node.kind=="NetworkOutput")
                if(select_node_subscription(node,selected,identity))return;
    }
    bool begin_output_session() {
        data = std::make_unique<embedded_support::NumericSession>(
            embedded_support::Role::publisher, binding, buffers.spans(), effective.data,
            [](void *c, std::uint8_t id, std::span<const std::uint8_t> bytes) {
                return static_cast<Device *>(c)->control->send_packet(id, bytes);
            },
            this, control->packet_payload_capacity());
        if (data->begin() != Status::ok) {
            data.reset();
            return false;
        }
        return true;
    }
    void install_subscription(const nd::Message& selected,const application::SessionIdentity& identity,const domain::ConfiguredProcessingNode& node,const domain::ProcessingPort& port) {
        output_contract = port.contract;
        output_graph = state.applied_numeric->graph_id;
        output_stream = std::string(selected.stamp.stream_id);
        binding = {{{std::uint32_t(selected.envelope.request), 1},
                    {output_graph, node.id, nd::Direction::Input, port.id},
                    output_stream,
                    output_contract.view(),
                    {},
                    selected.values},
                   {nd::PayloadVersion, unsigned(nd::Feature::Numeric)},
                   control->nonce(),
                   identity.session,
                   state.graph_revision,
                   identity.epoch};
        if(!begin_output_session())return;
        selected_endpoint.store((std::uint64_t(std::uint32_t(node.id)) << 16) | port.id,
                                std::memory_order_release);
        ack_pending = true;
        gap = true;
    }
    nd::Message acknowledgement_message() const {
        nd::Message ack;
        ack.kind = nd::Kind::Unavailable;
        ack.endpoint = binding.binding.endpoint;
        ack.contract = binding.binding.contract;
        ack.stamp.available = false;
        ack.stamp.stream_id = binding.binding.stream;
        ack.envelope = {binding.nonce,
                        binding.session,
                        1,
                        binding.publisher_epoch,
                        0,
                        binding.revision,
                        binding.binding.reference.binding};
        return ack;
    }
    void acknowledge() {
        if (!ack_pending || !data)
            return;
        const auto ack=acknowledgement_message();
        const auto result = nd::encode(ack, buffers.handshake, buffers.handshake_scratch, effective.data);
        if (!result) {
            fence();
            return;
        }
        send_acknowledgement(result.produced);
    }
    void send_acknowledgement(std::size_t bytes) {
        const auto sent = control->send_packet(
            nd::AckId, {reinterpret_cast<const std::uint8_t *>(buffers.handshake.data()), bytes});
        if (sent == Status::ok)
            ack_pending = false;
        else if (sent != Status::would_block)
            fence();
    }
    void record_processing_status(Status status) noexcept {
        if (status == Status::ok)
            processed_frames.fetch_add(1, std::memory_order_relaxed);
        else {
            processing_errors.fetch_add(1, std::memory_order_relaxed);
            last_processing_error.store(status, std::memory_order_relaxed);
        }
    }
    void process(const signal_contracts::DataFrameView &input) noexcept {
        std::lock_guard lock(processing_mutex);
        if (!executor->running())
            return;
        const auto *graph = executor->plan()->numeric_definition();
        if (!graph || std::none_of(graph->nodes.begin(), graph->nodes.end(),
                                   [](const auto &node) { return node.id == 0 && node.kind == "AudioSource"; }))
            return;
        const auto began = esp_timer_get_time();
        const auto status = executor->process_numeric_ports(
            0, input,
            {delivery_revision.load(std::memory_order_relaxed), delivery_epoch.load(std::memory_order_relaxed)},
            {nullptr, enqueue_output, nullptr, this});
        record_processing_status(status);
        const auto elapsed = std::uint64_t(esp_timer_get_time() - began);
        record_processing_time(elapsed);
    }
    void record_processing_time(std::uint64_t elapsed) noexcept {
        processing_us.fetch_add(elapsed, std::memory_order_relaxed);
        auto prior = maximum_processing_us.load(std::memory_order_relaxed);
        while (prior < elapsed &&
               !maximum_processing_us.compare_exchange_weak(prior, elapsed, std::memory_order_relaxed)) {
        }
    }
    void tick_clocks(bool missing_input) noexcept {
        std::lock_guard lock(processing_mutex);
        if (!executor->running() || clock_sequence == UINT64_MAX ||
            clock_samples > std::uint64_t(INT64_MAX) - BlockSamples)
            return;
        const auto event = next_clock_stamp();
        const dataflow_runtime::DeliveryStamp delivery{event.graph_revision, event.epoch};
        const dataflow_runtime::NumericRuntimeCallbacks callbacks{nullptr, enqueue_output, nullptr, this};
        const auto *graph = executor->plan()->numeric_definition();
        if (graph)
            advance_clocks(*graph, missing_input, event, delivery, callbacks);
        clock_samples += BlockSamples;
    }
    signal_contracts::FrameStamp next_clock_stamp() noexcept {
        signal_contracts::FrameStamp event;
        event.sequence = event.source_sequence = ++clock_sequence;
        event.epoch = event.producer_epoch = delivery_epoch.load(std::memory_order_relaxed);
        event.generation = event.epoch;
        event.graph_revision = delivery_revision.load(std::memory_order_relaxed);
        event.timestamp_ticks = event.sample_begin = std::int64_t(clock_samples);
        event.sample_count = BlockSamples;
        event.has_sample_interval = true;
        event.stream_id = "esp32-clock";
        return event;
    }
    static void enqueue_output(void *context, const dataflow_runtime::NumericPortFrameView &out) noexcept {
        auto &owner = *static_cast<Device *>(context);
        if (owner.selected_endpoint.load(std::memory_order_acquire) ==
            ((std::uint64_t(std::uint32_t(out.identity.node)) << 16) | out.identity.port))
            owner.output_queue.push(out.frame, owner.effective.data.frame, out.identity.node, out.identity.port,
                                    out.delivery);
    }
    void advance_clocks(const domain::NumericProcessingGraph &graph, bool missing_input,
                        const signal_contracts::FrameStamp &event, const dataflow_runtime::DeliveryStamp &delivery,
                        const dataflow_runtime::NumericRuntimeCallbacks &callbacks) noexcept {
        auto observe = [&](Status status) {
            if (status != Status::ok) {
                processing_errors.fetch_add(1, std::memory_order_relaxed);
                last_processing_error.store(status, std::memory_order_relaxed);
            }
        };
        if (missing_input)
            for (const auto &node : graph.nodes)
                if (node.kind == "AudioSource" && node.id == 0)
                    observe(executor->advance_unavailable_source(node.id, event, delivery, callbacks));
        for (const auto &node : graph.nodes)
            if (node.kind == "SampleClock")
                observe(executor->process_clock(node.id, event, delivery, callbacks));
    }
    void transmit_outputs() {
        for (std::size_t i = 0; i < OutputQueue::Capacity; ++i) {
            signal_contracts::OwnedDataFrame retained;
            domain::NodeId node;
            domain::PortId port;
            dataflow_runtime::DeliveryStamp delivery;
            if (!output_queue.pop(retained, node, port, delivery))
                break;
            transmit_output(retained.view(), node, port, delivery);
        }
    }
    nd::Message output_message(const signal_contracts::DataFrameView& frame) const {
        nd::Message message;
        message.endpoint = binding.binding.endpoint;
        message.contract = binding.binding.contract;
        message.stamp = frame.stamp;
        message.stamp.stream_id = output_stream;
        message.stamp.discontinuity |= gap;
        message.kind = message.stamp.available ? nd::Kind::Frame : nd::Kind::Unavailable;
        message.envelope.delivery_generation = message.stamp.generation;
        if (message.stamp.available)
            if (const auto *numeric = std::get_if<signal_contracts::NumericView>(&frame.payload))
                message.numeric = {numeric->storage, numeric->planes,      numeric->actual,
                                   numeric->ring,    numeric->coordinates, numeric->semantic_silence};
        return message;
    }
    void transmit_output(const signal_contracts::DataFrameView &frame, domain::NodeId node, domain::PortId port,
                         const dataflow_runtime::DeliveryStamp &delivery) {
        if (!data || ack_pending || retire || discard_data || node != binding.binding.endpoint.node ||
            port != binding.binding.endpoint.port || delivery.graph_revision != state.graph_revision ||
            delivery.epoch != state.epoch)
            return;
        const auto selected = std::find_if(state.applied_numeric->nodes.begin(), state.applied_numeric->nodes.end(),
                                           [&](const auto &value) { return value.id == node; });
        if (selected == state.applied_numeric->nodes.end() || !selected->enabled)
            return;
        const auto message=output_message(frame);
        const auto sent = data->publish(message);
        gap = sent != Status::ok && sent != Status::would_block;
    }

    void report_nodes() {
        // Bounded telemetry snapshot. Host callback runs after serialized access,
        // so diagnostics never call arbitrary firmware code while holding DSP.
        std::array<dataflow_runtime::NodeStats, 8> stats{};
        std::size_t count = 0;
        {
            std::lock_guard lock(processing_mutex);
            const auto *plan = executor ? executor->plan() : nullptr;
            const auto *graph = plan ? plan->numeric_definition() : nullptr;
            if (graph)
                for (const auto &node : graph->nodes) {
                    if (count == stats.size())
                        break;
                    if (const auto value = executor->node_stats(node.id))
                        stats[count++] = *value;
                }
        }
        streamer_host_report_node_stats(stats.data(), count);
    }
};
std::unique_ptr<Device> device;
StaticTask_t acquisition_control;
std::unique_ptr<StackType_t[]> acquisition_stack;
void acquire(void *) {
    auto &d = *device;
    auto next = esp_timer_get_time();
    std::uint64_t remainder = 0;
    for (;;) {
        const auto now = esp_timer_get_time();
        if (now >= next) {
#ifdef CONFIG_STREAMER_EXTERNAL_PCM
            signal_contracts::DataFrameView input;
            const auto received = streamer_host_read_pcm(&input);
            if (received == Status::ok)
                d.process(input);
            d.tick_clocks(received != Status::ok);
#else
            d.source.tick_source(
                [](void *c, const nd::Message &source) { static_cast<Device *>(c)->process(source.frame_view()); }, &d);
            d.tick_clocks(false);
#endif
            // Absolute sample deadlines preserve fractional periods and cannot
            // accumulate transport/task wake-up drift.
            next += BlockSamples * 1000000LL / d.profile.sample_rate;
            remainder += BlockSamples * 1000000LL % d.profile.sample_rate;
            if (remainder >= d.profile.sample_rate) {
                next += remainder / d.profile.sample_rate;
                remainder %= d.profile.sample_rate;
            }
        }
        vTaskDelay(1);
    }
}
StaticTask_t task_control;
std::unique_ptr<StackType_t[]> task_stack;

// Fixed ERROR reports call IDF's public log entrypoint without its dynamic-level
// dispatch. Keep the SDK local/master/tag gates and version-specific formatting.
void report_uart_setup_failure() {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("UART setup failed; processing remains active") ESP_LOG_ARGS());
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"UART setup failed; processing remains active"),esp_log_timestamp(),Tag);
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"UART setup failed; processing remains active"),esp_log_timestamp(),Tag);
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"UART setup failed; processing remains active"),esp_log_system_timestamp(),Tag);
#endif
    }
#endif
}
void report_host_setup_failure() {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("Host setup failed") ESP_LOG_ARGS());
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"Host setup failed"),esp_log_timestamp(),Tag);
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"Host setup failed"),esp_log_timestamp(),Tag);
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"Host setup failed"),esp_log_system_timestamp(),Tag);
#endif
    }
#endif
}
void report_device_storage_failure() {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("Device storage unavailable") ESP_LOG_ARGS());
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"Device storage unavailable"),esp_log_timestamp(),Tag);
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"Device storage unavailable"),esp_log_timestamp(),Tag);
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"Device storage unavailable"),esp_log_system_timestamp(),Tag);
#endif
    }
#endif
}
void report_task_storage_failure() {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("Task storage unavailable") ESP_LOG_ARGS());
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"Task storage unavailable"),esp_log_timestamp(),Tag);
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"Task storage unavailable"),esp_log_timestamp(),Tag);
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"Task storage unavailable"),esp_log_system_timestamp(),Tag);
#endif
    }
#endif
}
void report_acquisition_failure() {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("Acquisition task creation failed") ESP_LOG_ARGS());
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"Acquisition task creation failed"),esp_log_timestamp(),Tag);
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"Acquisition task creation failed"),esp_log_timestamp(),Tag);
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"Acquisition task creation failed"),esp_log_system_timestamp(),Tag);
#endif
    }
#endif
}
void report_task_creation_failure() {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("Task creation failed") ESP_LOG_ARGS());
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"Task creation failed"),esp_log_timestamp(),Tag);
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"Task creation failed"),esp_log_timestamp(),Tag);
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"Task creation failed"),esp_log_system_timestamp(),Tag);
#endif
    }
#endif
}
void report_admission_failure(Status status) {
#if ESP_LOG_VERSION == 2
    if(ESP_LOG_ENABLED(ESP_LOG_ERROR))
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR | ESP_LOG_CONFIGS_DEFAULT),Tag,
                ESP_LOG_ATTR_STR("Numeric profile/control admission refused: %d") ESP_LOG_ARGS(int(status)));
#elif NON_OS_BUILD
    if(_ESP_LOG_EARLY_ENABLED(ESP_LOG_ERROR))
        esp_rom_printf(LOG_FORMAT(E,"Numeric profile/control admission refused: %d"),esp_log_timestamp(),Tag, int(status));
#else
    if(_ESP_LOG_ENABLED(ESP_LOG_ERROR)) {
#if CONFIG_LOG_TIMESTAMP_SOURCE_RTOS
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_FORMAT(E,"Numeric profile/control admission refused: %d"),esp_log_timestamp(),Tag, int(status));
#elif CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM
        esp_log(ESP_LOG_CONFIG_INIT(ESP_LOG_ERROR),Tag,LOG_SYSTEM_TIME_FORMAT(E,"Numeric profile/control admission refused: %d"),esp_log_system_timestamp(),Tag, int(status));
#endif
    }
#endif
}

struct WorkerConnection {
    Device &d;
    esp_idf_adapter::TcpTransport tcp{};
    esp_idf_adapter::UartTransport uart{};
    QueueHandle_t events = nullptr;
    bdsp_protocol::ByteTransport *transport = &tcp;
    int descriptor = -1;
    std::uint64_t fence_at = 0, reconnect_at = 0, report_at = 0;
    void initialize_uart() {
#ifdef CONFIG_STREAMER_UART
        uart_config_t config{};
        config.baud_rate = CONFIG_STREAMER_UART_BAUD;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        config.source_clk = UART_SCLK_DEFAULT;
        if (uart_param_config(UART_NUM_1, &config) != ESP_OK ||
            uart_set_pin(UART_NUM_1, CONFIG_STREAMER_UART_TX, CONFIG_STREAMER_UART_RX, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE) != ESP_OK ||
            uart_driver_install(UART_NUM_1, 8192, 0, 8, &events, 0) != ESP_OK)
            report_uart_setup_failure();
#endif
    }
    void initialize() {
#ifdef CONFIG_STREAMER_UART
        initialize_uart();
        transport = &uart;
#endif
    }
    void report() {
        if (d.clock.now_ms() >= report_at) {
            report_at = d.clock.now_ms() + 1000;
            streamer_host_report_processing(d.processed_frames.load(std::memory_order_relaxed),
                                            d.processing_us.load(std::memory_order_relaxed),
                                            d.maximum_processing_us.load(std::memory_order_relaxed),
                                            d.output_queue.dropped.load(std::memory_order_relaxed));
            streamer_host_report_processing_errors(d.processing_errors.load(std::memory_order_relaxed),
                                                   d.last_processing_error.load(std::memory_order_relaxed));
            d.report_nodes();
        }
    }
    void discard_outbound() {
        if (!d.discard_data)
            return;
        const auto discarded = d.control->discard_outbound_data();
        if (discarded == Status::ok)
            d.discard_data = false;
        else if (discarded != Status::would_block)
            d.retire = true;
    }
    bool flush_retiring_reply() {
        if (!d.retire || transport->state() != bdsp_protocol::TransportState::connected ||
            d.control->state() == control::ControlSessionState::Failed)
            return true;
        // Finish the accepted CONTROL reply while acquisition/processing continues.
        if (!fence_at)
            fence_at = d.clock.now_ms() + 1000;
        d.control->poll(*transport);
        if (!d.control->pending_bytes() || d.clock.now_ms() >= fence_at)
            return true;
        vTaskDelay(1);
        return false;
    }
    bool disconnected() const noexcept {
        return transport->state() != bdsp_protocol::TransportState::connected ||
               d.control->state() == control::ControlSessionState::Failed || d.retire;
    }
    bool reconnect() {
        d.fence();
        d.control->reset();
        tcp.detach();
        uart.detach();
        if (descriptor >= 0) {
            streamer_host_release_socket(descriptor);
            descriptor = -1;
        }
        if (!attach_transport())
            return false;
        d.retire = false;
        d.discard_data = false;
        fence_at = 0;
        return true;
    }
    bool ensure_uart_driver() {
#ifdef CONFIG_STREAMER_UART
        if (!events) {
            if (d.clock.now_ms() < reconnect_at) {
                vTaskDelay(1);
                return false;
            }
            reconnect_at = d.clock.now_ms() + 100;
            if (uart_driver_install(UART_NUM_1, 8192, 0, 8, &events, 0) != ESP_OK) {
                vTaskDelay(1);
                return false;
            }
        }
#endif
        return true;
    }
    bool attach_transport() {
#ifdef CONFIG_STREAMER_UART
        if (!ensure_uart_driver())
            return false;
        uart_flush_input(UART_NUM_1);
        xQueueReset(events);
        if (uart.attach(UART_NUM_1, events) != Status::ok) {
            vTaskDelay(1);
            return false;
        }
        constexpr std::array<std::uint8_t, 1> delimiter{0};
        if (uart.write(delimiter).transferred != 1) {
            vTaskDelay(1);
            return false;
        }
#else
        if (d.clock.now_ms() < reconnect_at) {
            vTaskDelay(1);
            return false;
        }
        reconnect_at = d.clock.now_ms() + 100;
        descriptor = streamer_host_acquire_socket();
        if (descriptor < 0) {
            vTaskDelay(1);
            return false;
        }
        if (tcp.attach(descriptor) != Status::ok) {
            streamer_host_release_socket(descriptor);
            descriptor = -1;
            return false;
        }
#endif
        return true;
    }
    void poll_connected() {
        const auto status = d.control->poll(*transport);
        d.publish_state(d.service->observe_state());
        if (status != Status::ok && status != Status::would_block) {
            d.retire = true;
            return;
        }
        if (d.retire || d.discard_data)
            return;
        d.acknowledge();
        if (d.data)
            d.data->drain_packets();
        vTaskDelay(1);
    }
    void cycle() {
        report();
        d.transmit_outputs();
        discard_outbound();
        if (!flush_retiring_reply())
            return;
        if (disconnected() && !reconnect())
            return;
        poll_connected();
    }
};
void worker(void *) {
    WorkerConnection connection{*device};
    connection.initialize();
    for (;;)
        connection.cycle();
}
bool prepare_device() {
    if (!streamer_host_setup()) {
        report_host_setup_failure();
        return false;
    }
    const auto available_memory = streamer_host_memory_budget();
    device.reset(new (std::nothrow) Device(available_memory));
    if (!device) {
        report_device_storage_failure();
        return false;
    }
    const auto status = device->prepare();
    if (status != Status::ok) {
        report_admission_failure(status);
        device.reset();
        return false;
    }
    return true;
}
void release_task_storage() {
    device.reset();
    task_stack.reset();
    acquisition_stack.reset();
}
bool prepare_task_storage() {
    acquisition_stack.reset(new (std::nothrow) StackType_t[AcquisitionStackBytes / sizeof(StackType_t)]);
    task_stack.reset(new (std::nothrow) StackType_t[TaskStackBytes / sizeof(StackType_t)]);
    if (task_stack && acquisition_stack)
        return true;
    report_task_storage_failure();
    release_task_storage();
    return false;
}
void start_tasks() {
    const auto producer = xTaskCreateStatic(acquire, "numeric-acquisition", AcquisitionStackBytes, nullptr, 6,
                                            acquisition_stack.get(), &acquisition_control);
    if (!producer) {
        report_acquisition_failure();
        release_task_storage();
        return;
    }
    if (!xTaskCreateStatic(worker, "numeric-device", TaskStackBytes, nullptr, 5, task_stack.get(), &task_control)) {
        vTaskDelete(producer);
        report_task_creation_failure();
        release_task_storage();
    }
}
} // namespace
extern "C" void app_main() {
    if (!prepare_device() || !prepare_task_storage())
        return;
    start_tasks();
}
