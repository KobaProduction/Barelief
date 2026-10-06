#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace dataflow_runtime {
namespace {
using namespace domain;
using namespace signal_contracts;
constexpr std::size_t absent = std::numeric_limits<std::size_t>::max();
struct PlanNode {
    NodeDefinition node;
    std::size_t parent = absent, clock = absent, ingress = absent;
    PortContract input, output;
    bool known = false, detached = false, reset = true;
    std::size_t samples = 0, history_rows = 0;
    std::vector<std::size_t> children;
};
struct State {
    dsp_kernels::FFT fft;
    dsp_kernels::MelFilterBank mel;
    std::vector<float> left, right, values, coefficients, pcm_left, pcm_right, history, zero;
    std::size_t frames = 0, oldest = 0, progress = 0, remainder = 0, hop = 0;
    std::uint64_t generation = 0, sequence = 0, output_sequence = 0, producer_epoch = 0;
    std::array<std::uint64_t, 16> retired{};
    std::size_t retired_count = 0, retired_next = 0;
    FrameMetadata last_input;
    bool unavailable = false, pending_discontinuity = false, preserve_next_tick = false;
    void clear() noexcept {
        frames = oldest = progress = remainder = hop = 0;
        std::fill(pcm_left.begin(), pcm_left.end(), 0);
        std::fill(pcm_right.begin(), pcm_right.end(), 0);
        pending_discontinuity = true;
    }
};
struct BufferGeometry {
    std::size_t channels = 0, values = 0, zero = 0, pcm = 0, coefficients = 0, history = 0;
};
BufferGeometry buffers(const PlanNode &n) {
    BufferGeometry b;
    if (!n.known)
        return b;
    b.zero = std::max(n.input.columns, n.output.columns);
    switch (n.node.kind) {
    case NodeKind::Window:
        if (n.input.type == FrameType::PCM)
            b.pcm = n.output.columns;
        break;
    case NodeKind::WindowFunction:
        if (n.input.type == FrameType::Window)
            b.channels = b.coefficients = n.input.columns;
        break;
    case NodeKind::StereoFFT:
        if (n.input.type == FrameType::Windowed)
            b.channels = n.output.columns;
        break;
    case NodeKind::BarkWeighting:
        b.channels = b.coefficients = n.input.columns;
        break;
    case NodeKind::SpectrumMerge:
    case NodeKind::SpectrumGain:
    case NodeKind::WLED:
        b.values = n.output.columns;
        break;
    case NodeKind::Mel:
        b.values = n.output.columns;
        break;
    case NodeKind::FFTView:
        if (std::get<FFTViewParameters>(n.node.parameters).channel == Channel::Merged)
            b.values = n.input.columns;
        break;
    default:
        break;
    }
    b.history = n.history_rows * n.output.columns;
    return b;
}
bool local(NodeKind k) {
    return k == NodeKind::AudioSource || k == NodeKind::TestSignal;
}
bool source(const NodeDefinition &n) {
    return !input_type(n);
}
bool structural(const NodeDefinition &a, const NodeDefinition &b) {
    if (a.kind != b.kind)
        return false;
    if (source(a))
        return a.parameters == b.parameters;
    switch (a.kind) {
    case NodeKind::Window:
    case NodeKind::Mel:
        return a.parameters == b.parameters;
    case NodeKind::NetworkOutput:
        return std::get<NetworkParameters>(a.parameters).data_type ==
               std::get<NetworkParameters>(b.parameters).data_type;
    default:
        return true;
    }
}
PortContract transform(const NodeDefinition &n, PortContract f) {
    switch (n.kind) {
    case NodeKind::Window:
        if (f.type == FrameType::PCM || f.type == FrameType::Window) {
            f.type = FrameType::Window;
            f.columns = std::get<WindowParameters>(n.parameters).size;
            f.fft_size = f.columns;
        }
        break;
    case NodeKind::WindowFunction:
        if (f.type == FrameType::Window)
            f.type = FrameType::Windowed;
        break;
    case NodeKind::StereoFFT:
        if (f.type == FrameType::Windowed) {
            f.type = FrameType::StereoSpectrum;
            f.fft_size = f.columns;
            f.columns /= 2;
            f.frequency_step = float(f.sample_rate) / f.fft_size;
            f.bark_weighted = false;
        }
        break;
    case NodeKind::BarkWeighting:
        f.bark_weighted = true;
        break;
    case NodeKind::SpectrumMerge:
        f.type = FrameType::Spectrum;
        break;
    case NodeKind::Mel:
        f.type = FrameType::Mel;
        f.columns = f.bands = std::get<MelParameters>(n.parameters).bands;
        break;
    case NodeKind::Spectrogram:
        f.type = FrameType::History;
        f.bands = f.columns;
        f.history_capacity = std::get<HistoryParameters>(n.parameters).history_size;
        break;
    default:
        break;
    }
    return f;
}
std::size_t index(const std::vector<PlanNode> &nodes, NodeId id) {
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].node.id == id)
            return i;
    return absent;
}
bool retired(const State &s, std::uint64_t epoch) {
    return std::find(s.retired.begin(), s.retired.begin() + s.retired_count, epoch) !=
           s.retired.begin() + s.retired_count;
}
void merge(FrameView &f, State &s, MergeMode mode) noexcept {
    for (std::size_t i = 0; i < f.left.size(); ++i) {
        const float l = f.left[i], r = f.right[i];
        s.values[i] = mode == MergeMode::Mean      ? (l + r) * .5f
                      : mode == MergeMode::Maximum ? std::max(l, r)
                                                   : std::hypot(l, r) / std::sqrt(2.f);
    }
    f.values = {s.values.data(), f.left.size()};
    const auto peak = std::max_element(f.values.begin(), f.values.end());
    f.metadata.magnitude = peak == f.values.end() ? 0 : *peak;
    f.metadata.peak_hz = peak == f.values.end() ? 0 : std::distance(f.values.begin(), peak) * f.metadata.frequency_step;
}
} // namespace
struct RuntimeContext::Impl {
    std::vector<State> states;
    const PreparedGraph *plan = nullptr;
};
struct PreparedGraph::Impl {
    GraphDefinition graph;
    application::MemoryRequirements memory;
    std::vector<PlanNode> nodes;
};
RuntimeContext::RuntimeContext() : impl_(std::make_unique<Impl>()) {}
RuntimeContext::~RuntimeContext() = default;
RuntimeContext::RuntimeContext(const RuntimeContext &c) : impl_(std::make_unique<Impl>(*c.impl_)) {}
RuntimeContext &RuntimeContext::operator=(const RuntimeContext &c) {
    if (this != &c)
        impl_ = std::make_unique<Impl>(*c.impl_);
    return *this;
}
RuntimeContext::RuntimeContext(RuntimeContext &&) noexcept = default;
RuntimeContext &RuntimeContext::operator=(RuntimeContext &&) noexcept = default;
PreparedGraph::PreparedGraph() : impl_(std::make_unique<Impl>()) {}
PreparedGraph::~PreparedGraph() = default;
const GraphDefinition &PreparedGraph::definition() const noexcept {
    return impl_->graph;
}
application::MemoryRequirements PreparedGraph::memory_requirements() const noexcept {
    return impl_->memory;
}
Status PreparedGraph::prepare(const GraphDefinition &g, std::span<const IngressSpec> specs,
                              const application::RuntimeLimits &limits, std::shared_ptr<const PreparedGraph> &result,
                              std::unique_ptr<RuntimeContext> &context, const PreparedGraph *previous,
                              const RuntimeContext *old_context) {
    result.reset();
    context.reset();
    const auto validation = validate_graph(g, desktop_capabilities(), limits.graph);
    if (!validation)
        return Status::invalid_argument;
    if (previous && (!old_context || !old_context->impl_ || old_context->impl_->plan != previous))
        return Status::invalid_argument;
    std::size_t document_bytes = g.graph_id.size();
    for (const auto &node : g.nodes)
        if (const auto *p = std::get_if<NetworkParameters>(&node.parameters)) {
            if (p->stream_id.size() > 256)
                return Status::invalid_argument;
            for (auto bytes : {p->stream_id.size(), p->host.size(), p->provider_uuid.size()}) {
                if (bytes > std::numeric_limits<std::size_t>::max() - document_bytes)
                    return Status::overflow;
                document_bytes += bytes;
            }
        }
    if (document_bytes > limits.max_memory_bytes / 2)
        return Status::exhausted;
    auto p = std::shared_ptr<PreparedGraph>(new PreparedGraph);
    auto &plan = *p->impl_;
    plan.graph = g;
    plan.nodes.reserve(g.nodes.size());
    for (auto id : validation.topological_order) {
        PlanNode n;
        n.node = *std::find_if(g.nodes.begin(), g.nodes.end(), [&](const auto &x) { return x.id == id; });
        const auto edge = std::find_if(g.edges.begin(), g.edges.end(), [&](const auto &e) { return e.to == id; });
        if (edge != g.edges.end())
            n.parent = index(plan.nodes, edge->from);
        n.clock = n.parent;
        n.detached = !source(n.node) && n.parent == absent;
        const auto old = previous ? index(previous->impl_->nodes, id) : absent;
        const bool old_known = old != absent && previous->impl_->nodes[old].known;
        if (source(n.node)) {
            const auto spec = std::find_if(specs.begin(), specs.end(), [&](const auto &s) { return s.node == id; });
            if (spec != specs.end()) {
                if (n.node.kind == NodeKind::NetworkInput && spec->contract.type != *output_type(n.node))
                    return Status::incompatible_contract;
                if (local(n.node.kind) && spec->contract.type != FrameType::PCM &&
                    spec->contract.type != FrameType::StereoSpectrum &&
                    !(spec->allow_legacy_window && spec->contract.type == FrameType::Window))
                    return Status::incompatible_contract;
                if (local(n.node.kind) && spec->contract.type == FrameType::StereoSpectrum &&
                    (spec->contract.fft_size < 4 || spec->contract.fft_size > dsp_kernels::max_fft_samples ||
                     (spec->contract.fft_size & (spec->contract.fft_size - 1)) ||
                     spec->contract.fft_size != 2 * spec->contract.columns || !spec->contract.columns))
                    return Status::incompatible_contract;
                if (local(n.node.kind) && spec->contract.type == FrameType::Window &&
                    (!spec->contract.fft_size || spec->contract.fft_size != spec->contract.columns ||
                     spec->contract.columns > dsp_kernels::max_fft_samples))
                    return Status::incompatible_contract;
                n.input = spec->contract;
                n.known = true;
                n.samples = spec->max_samples ? spec->max_samples : n.input.columns;
                if (n.input.type == FrameType::PCM)
                    n.input.columns = n.samples;
            }
            n.ingress = plan.nodes.size();
        } else if (n.parent != absent) {
            const auto &parent = plan.nodes[n.parent];
            n.known = parent.known;
            n.input = parent.output;
            n.ingress = parent.ingress;
            n.samples = n.input.columns;
        } else if (old_known) {
            const auto &saved = previous->impl_->nodes[old];
            n.input = saved.input;
            n.known = true;
            n.samples = saved.samples;
            n.clock = saved.clock == absent ? absent : std::size_t(previous->impl_->nodes[saved.clock].node.id);
            n.ingress = saved.ingress == absent ? absent : std::size_t(previous->impl_->nodes[saved.ingress].node.id);
        }
        if (n.known) {
            if (int(n.input.type) < 0 || int(n.input.type) > 6 || n.input.fft_size > 65536 || n.input.bands > 4096 ||
                n.input.history_capacity > 2000 || !n.input.sample_rate || n.input.sample_rate > 768000 ||
                !n.input.columns || n.samples > limits.max_frame_samples ||
                n.input.columns > limits.max_frame_samples || n.input.history_capacity > limits.max_history_frames ||
                !std::isfinite(n.input.frequency_step) || n.input.frequency_step < 0)
                return Status::invalid_argument;
            if (n.input.columns > std::numeric_limits<std::size_t>::max() / (sizeof(float) * 2008))
                return Status::overflow;
            if (n.input.fft_size && int(n.input.type) >= 1 && int(n.input.type) <= 4 &&
                n.input.columns != (int(n.input.type) >= 3 ? n.input.fft_size / 2 : n.input.fft_size))
                return Status::incompatible_contract;
            if (n.input.type == FrameType::Mel && n.input.bands && n.input.bands != n.input.columns)
                return Status::incompatible_contract;
            if (n.input.type == FrameType::History && !n.input.history_capacity)
                return Status::incompatible_contract;
            n.output = transform(n.node, n.input);
            if (n.node.kind == NodeKind::Window && n.input.type == FrameType::Window &&
                n.input.columns < n.output.columns)
                return Status::incompatible_contract;
            if (n.output.columns > limits.max_frame_samples || n.output.history_capacity > limits.max_history_frames)
                return Status::exhausted;
            n.history_rows = n.node.kind == NodeKind::Spectrogram               ? n.output.history_capacity
                             : n.detached && n.input.type == FrameType::History ? n.input.history_capacity
                                                                                : 0;
        }
        n.reset = !previous || g.graph_id != previous->impl_->graph.graph_id || old == absent ||
                  !structural(n.node, previous->impl_->nodes[old].node) ||
                  n.known != previous->impl_->nodes[old].known ||
                  (n.known && n.input != previous->impl_->nodes[old].input) ||
                  (n.parent != absent && plan.nodes[n.parent].reset);
        plan.nodes.push_back(std::move(n));
    }
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto at = index(plan.nodes, specs[i].node);
        if (at == absent || !source(plan.nodes[at].node))
            return Status::invalid_argument;
        for (std::size_t j = 0; j < i; ++j)
            if (specs[j].node == specs[i].node)
                return Status::invalid_argument;
    }
    for (auto &n : plan.nodes)
        if (n.detached && n.known) {
            n.clock = index(plan.nodes, NodeId(n.clock));
            n.ingress = index(plan.nodes, NodeId(n.ingress));
            if (n.ingress == absent || !plan.nodes[n.ingress].known) {
                n.known = false;
                n.reset = true;
                n.history_rows = 0;
                n.clock = absent;
            }
        }
    for (auto &n : plan.nodes)
        if (n.parent != absent) {
            if (!plan.nodes[n.parent].known) {
                n.known = false;
                n.reset = true;
                n.history_rows = 0;
            } else if (plan.nodes[n.parent].reset)
                n.reset = true;
        }
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto &n = plan.nodes[i];
        if (n.detached && n.known && n.clock == absent)
            n.clock = n.ingress;
        if (n.clock != absent)
            plan.nodes[n.clock].children.push_back(i);
    }
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto at = i;
        std::size_t steps = 0;
        while (at != absent && steps++ <= plan.nodes.size())
            at = plan.nodes[at].clock;
        if (steps > plan.nodes.size() + 1)
            return Status::incompatible_contract;
    }
    auto &m = plan.memory;
    m.state_bytes = sizeof(PreparedGraph) + sizeof(Impl) + sizeof(RuntimeContext) + sizeof(RuntimeContext::Impl) +
                    plan.nodes.size() * (sizeof(PlanNode) + sizeof(State) + sizeof(NodeDefinition)) +
                    g.edges.size() * sizeof(Edge) + g.graph_id.size();
    for (const auto &n : plan.nodes) {
        m.state_bytes += n.children.size() * sizeof(std::size_t);
        if (const auto *net = std::get_if<NetworkParameters>(&n.node.parameters))
            m.state_bytes += 2 * (net->stream_id.size() + net->host.size() + net->provider_uuid.size());
        if (!n.known)
            continue;
        const auto b = buffers(n);
        const auto bytes =
            (2 * b.channels + b.values + b.zero + 2 * b.pcm + b.coefficients + b.history) * sizeof(float);
        if (bytes > std::numeric_limits<std::size_t>::max() - m.buffer_bytes)
            return Status::overflow;
        m.buffer_bytes += bytes;
        if (n.node.kind == NodeKind::StereoFFT && n.input.type == FrameType::Windowed)
            m.scratch_bytes += 2 * n.input.columns * sizeof(float);
        if (n.node.kind == NodeKind::Mel)
            m.scratch_bytes += (n.output.columns + 2) * sizeof(std::size_t);
    }
    if (!m.fits(limits.max_memory_bytes))
        return Status::exhausted;
    auto c = std::make_unique<RuntimeContext>();
    c->impl_->plan = p.get();
    c->impl_->states.resize(plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto &n = plan.nodes[i];
        auto &s = c->impl_->states[i];
        if (!n.known)
            continue;
        const auto b = buffers(n);
        s.left.resize(b.channels);
        s.right.resize(b.channels);
        s.values.resize(b.values);
        s.zero.resize(b.zero);
        s.history.resize(b.history);
        if (n.node.kind == NodeKind::Window && n.input.type == FrameType::PCM) {
            s.pcm_left.resize(n.output.columns);
            s.pcm_right.resize(n.output.columns);
        }
        if (n.node.kind == NodeKind::WindowFunction && n.input.type == FrameType::Window) {
            s.coefficients.resize(n.input.columns);
            dsp_kernels::window(s.coefficients, static_cast<dsp_kernels::WindowFunction>(
                                                    std::get<WindowFunctionParameters>(n.node.parameters).function));
        }
        if (n.node.kind == NodeKind::BarkWeighting) {
            s.coefficients.resize(n.input.columns);
            dsp_kernels::bark_weights(s.coefficients, n.input.frequency_step);
        }
        if (n.node.kind == NodeKind::StereoFFT && n.input.type == FrameType::Windowed &&
            !s.fft.prepare(n.input.columns, (n.input.columns & 0x55555555) != 0))
            return Status::incompatible_contract;
        if (n.node.kind == NodeKind::Mel && !s.mel.prepare(n.input.columns, n.input.sample_rate, n.output.columns))
            return Status::incompatible_contract;
    }
    if (previous && old_context)
        p->refresh_state(*c, *previous, *old_context);
    // Report actual retained capacities as well as the preflight estimate.
    // Standard vectors may reserve more than their requested element count.
    std::size_t actual_state = sizeof(PreparedGraph) + sizeof(Impl) + sizeof(RuntimeContext) +
                               sizeof(RuntimeContext::Impl) + plan.nodes.capacity() * sizeof(PlanNode) +
                               c->impl_->states.capacity() * sizeof(State) +
                               plan.graph.nodes.capacity() * sizeof(NodeDefinition) +
                               plan.graph.edges.capacity() * sizeof(Edge) + plan.graph.graph_id.capacity() + 1;
    for (const auto &n : plan.nodes) {
        actual_state += n.children.capacity() * sizeof(std::size_t);
        if (const auto *net = std::get_if<NetworkParameters>(&n.node.parameters))
            actual_state += net->stream_id.capacity() + net->host.capacity() + net->provider_uuid.capacity() + 3;
    }
    for (const auto &n : plan.graph.nodes)
        if (const auto *net = std::get_if<NetworkParameters>(&n.parameters))
            actual_state += net->stream_id.capacity() + net->host.capacity() + net->provider_uuid.capacity() + 3;
    m.state_bytes = std::max(m.state_bytes, actual_state);
    m.buffer_bytes = m.scratch_bytes = 0;
    for (const auto &s : c->impl_->states) {
        m.buffer_bytes += (s.left.capacity() + s.right.capacity() + s.values.capacity() + s.coefficients.capacity() +
                           s.pcm_left.capacity() + s.pcm_right.capacity() + s.history.capacity() + s.zero.capacity()) *
                          sizeof(float);
        m.scratch_bytes += s.fft.workspace_bytes() + s.mel.workspace_bytes();
    }
    if (!m.fits(limits.max_memory_bytes))
        return Status::exhausted;
    result = std::move(p);
    context = std::move(c);
    return Status::ok;
}
Status PreparedGraph::process(NodeId ingress, const FrameView &input, RuntimeContext &c, OutputCallback callback,
                              void *user) const noexcept {
    if (!c.impl_ || c.impl_->plan != this)
        return Status::invalid_argument;
    const auto at = index(impl_->nodes, ingress);
    if (at == absent || !source(impl_->nodes[at].node))
        return Status::invalid_argument;
    const auto valid = signal_contracts::validate(input);
    if (valid != Status::ok && valid != Status::unavailable)
        return valid;
    const auto &src = impl_->nodes[at];
    if (!src.known && input.metadata.available)
        return Status::unavailable;
    auto actual = contract(input), expected = src.input;
    if (actual.type == FrameType::PCM) {
        if (actual.columns > expected.columns)
            return Status::buffer_too_small;
        actual.columns = expected.columns;
    }
    if (input.metadata.available && actual != expected)
        return Status::incompatible_contract;
    auto &origin = c.impl_->states[at];
    const auto epoch = input.metadata.producer_epoch ? input.metadata.producer_epoch : input.metadata.epoch;
    const bool new_epoch = epoch != origin.producer_epoch;
    if (new_epoch && retired(origin, epoch))
        return Status::would_block;
    if (!input.metadata.available && !marker_current(ingress, input, c))
        return Status::would_block;
    if (input.metadata.available && !new_epoch &&
        (input.metadata.generation < origin.generation ||
         (input.metadata.generation == origin.generation && origin.sequence &&
          input.metadata.sequence <= origin.sequence && !input.metadata.discontinuity)))
        return Status::would_block;
    if (new_epoch && origin.producer_epoch) {
        origin.retired[origin.retired_next] = origin.producer_epoch;
        origin.retired_next = (origin.retired_next + 1) % origin.retired.size();
        origin.retired_count = std::min(origin.retired_count + 1, origin.retired.size());
    }
    // Prepared adjacency routes each completed window immediately. The bounded
    // DAG depth replaces per-frame scheduling and growing event containers.
    Status processing_status = Status::ok;
    auto unavailable = [&](auto &&self, std::size_t i, FrameMetadata metadata) -> void {
        const auto &n = impl_->nodes[i];
        auto &s = c.impl_->states[i];
        s.clear();
        s.unavailable = true;
        FrameView marker;
        marker.type = output_type(n.node).value_or(input_type(n.node).value_or(n.output.type));
        marker.metadata = metadata;
        marker.metadata.available = false;
        marker.metadata.sequence = 0;
        marker.metadata.discontinuity = true;
        marker.metadata.peak_hz = marker.metadata.magnitude = 0;
        if (n.node.kind == NodeKind::NetworkOutput)
            marker.metadata.stream_id = std::get<NetworkParameters>(n.node.parameters).stream_id;
        if (callback)
            callback(user, {n.node.id, n.node.kind, marker});
        for (auto child : n.children)
            self(self, child, marker.metadata);
    };
    if (!input.metadata.available) {
        if (new_epoch) {
            origin.producer_epoch = epoch;
            origin.generation = input.metadata.generation;
            origin.sequence = 0;
        } else if (input.metadata.generation)
            origin.generation = input.metadata.generation;
        unavailable(unavailable, at, input.metadata);
        return Status::unavailable;
    }
    auto run = [&](auto &&self, std::size_t i, FrameView f) -> void {
        const auto &n = impl_->nodes[i];
        auto &s = c.impl_->states[i];
        if (!n.known)
            return;
        const auto k = n.node.kind;
        std::span<const float> display_values;
        if (n.detached) {
            const auto clock = f.metadata;
            const auto clock_history = f.history;
            f = {};
            f.type = n.input.type;
            f.metadata = s.last_input;
            f.metadata.sequence = clock.sequence;
            f.metadata.source_sequence = clock.source_sequence;
            f.metadata.timestamp_ns = clock.timestamp_ns;
            f.metadata.epoch = clock.epoch;
            f.metadata.producer_epoch = clock.producer_epoch;
            f.metadata.generation = clock.generation;
            f.metadata.discontinuity = false;
            if (int(f.type) <= 3) {
                f.left = {s.zero.data(), n.input.columns};
                f.right = f.left;
            } else if (f.type != FrameType::History)
                f.values = {s.zero.data(), n.input.columns};
            else if (clock_history.frames && clock_history.bands == n.input.columns)
                f.history = clock_history;
            else if (n.history_rows)
                f.history = {s.history, n.input.columns, s.frames, n.history_rows, s.oldest};
            else
                return;
        }
        if (source(n.node) && s.preserve_next_tick) {
            f.metadata.discontinuity = false;
            s.preserve_next_tick = false;
        }
        s.last_input = f.metadata;
        s.last_input.stream_id = {};
        const bool broken = s.unavailable || f.metadata.discontinuity || s.producer_epoch != epoch ||
                            s.generation != f.metadata.generation ||
                            (source(n.node) && s.sequence && f.metadata.sequence != s.sequence + 1);
        s.producer_epoch = epoch;
        s.unavailable = false;
        s.generation = f.metadata.generation;
        s.sequence = f.metadata.sequence;
        f.metadata.producer_epoch = epoch;
        if (broken) {
            s.clear();
            f.metadata.discontinuity = true;
        }
        const bool silence = n.detached || !n.node.enabled;
        if (n.detached && f.type == FrameType::History && f.history.storage.data() == s.history.data()) {
            std::size_t row;
            if (s.frames < n.history_rows) {
                row = (s.oldest + s.frames) % n.history_rows;
                ++s.frames;
            } else {
                row = s.oldest;
                s.oldest = (s.oldest + 1) % n.history_rows;
            }
            std::fill_n(s.history.data() + row * n.input.columns, n.input.columns, 0);
            f.history = {s.history, n.input.columns, s.frames, n.history_rows, s.oldest};
        }
        if (silence) {
            f.left = {s.zero.data(), f.left.size()};
            f.right = {s.zero.data(), f.right.size()};
            f.values = {s.zero.data(), f.values.size()};
            if (f.history.frames)
                f.history.newest_override = {s.zero.data(), f.history.bands};
            f.metadata.peak_hz = f.metadata.magnitude = 0;
        }
        auto emit = [&](FrameView out, bool window_emitted = false) {
            auto finite = [](auto data) {
                for (float v : data)
                    if (!std::isfinite(v))
                        return false;
                return true;
            };
            if (!finite(out.left) || !finite(out.right) || !finite(out.values) ||
                !std::isfinite(out.metadata.peak_hz) || !std::isfinite(out.metadata.magnitude)) {
                processing_status = Status::overflow;
                unavailable(unavailable, i, out.metadata);
                return;
            }
            if (callback)
                callback(user, {n.node.id, k, out, window_emitted, display_values});
            for (auto child : n.children)
                self(self, child, out);
        };
        if (k == NodeKind::Window && f.type == FrameType::PCM) {
            const auto size = n.output.columns;
            std::size_t offset = 0;
            while (offset < f.left.size()) {
                if (!s.hop) {
                    s.hop = std::max(std::size_t{1}, f.metadata.sample_rate / 60);
                    s.remainder += f.metadata.sample_rate % 60;
                    if (s.remainder >= 60) {
                        ++s.hop;
                        s.remainder -= 60;
                    }
                }
                const auto count = std::min(f.left.size() - offset, s.hop - s.progress), kept = std::min(size, count);
                std::move(s.pcm_left.begin() + kept, s.pcm_left.end(), s.pcm_left.begin());
                std::move(s.pcm_right.begin() + kept, s.pcm_right.end(), s.pcm_right.begin());
                std::copy_n(f.left.data() + offset + count - kept, kept, s.pcm_left.data() + size - kept);
                std::copy_n(f.right.data() + offset + count - kept, kept, s.pcm_right.data() + size - kept);
                offset += count;
                s.progress += count;
                if (s.progress == s.hop) {
                    auto w = f;
                    w.type = FrameType::Window;
                    w.left = s.pcm_left;
                    w.right = s.pcm_right;
                    if (!n.node.enabled) {
                        w.left = {s.zero.data(), size};
                        w.right = w.left;
                    }
                    w.metadata.fft_size = size;
                    w.metadata.timestamp_ns =
                        f.metadata.timestamp_ns + std::int64_t(offset * 1000000000ULL / f.metadata.sample_rate);
                    w.metadata.source_sequence =
                        f.metadata.source_sequence ? f.metadata.source_sequence : f.metadata.sequence;
                    w.metadata.sequence = ++s.output_sequence;
                    w.metadata.discontinuity = f.metadata.discontinuity || s.pending_discontinuity;
                    s.pending_discontinuity = false;
                    emit(w, true);
                    f.metadata.discontinuity = false;
                    s.progress = s.hop = 0;
                }
            }
            return;
        }
        switch (k) {
        case NodeKind::Window:
            if (f.type == FrameType::Window) {
                f.left = f.left.last(n.output.columns);
                f.right = f.right.last(n.output.columns);
                f.metadata.fft_size = n.output.columns;
            }
            break;
        case NodeKind::WindowFunction:
            if (f.type == FrameType::StereoSpectrum)
                break;
            if (f.type != FrameType::Window)
                return;
            for (std::size_t j = 0; j < f.left.size(); ++j) {
                s.left[j] = f.left[j] * s.coefficients[j];
                s.right[j] = f.right[j] * s.coefficients[j];
            }
            f.left = {s.left.data(), f.left.size()};
            f.right = {s.right.data(), f.right.size()};
            f.type = FrameType::Windowed;
            break;
        case NodeKind::StereoFFT:
            if (f.type != FrameType::StereoSpectrum) {
                if (f.type != FrameType::Windowed)
                    return;
                const auto bins = f.left.size() / 2;
                if (!s.fft.process(f.left, {s.left.data(), bins}) || !s.fft.process(f.right, {s.right.data(), bins}))
                    return;
                f.metadata.fft_size = f.left.size();
                f.metadata.frequency_step = float(f.metadata.sample_rate) / f.left.size();
                f.metadata.bark_weighted = false;
                f.left = {s.left.data(), bins};
                f.right = {s.right.data(), bins};
                f.type = FrameType::StereoSpectrum;
            }
            break;
        case NodeKind::BarkWeighting:
            if (f.type != FrameType::StereoSpectrum)
                return;
            if (!f.metadata.bark_weighted) {
                for (std::size_t j = 0; j < f.left.size(); ++j) {
                    s.left[j] = f.left[j] * s.coefficients[j];
                    s.right[j] = f.right[j] * s.coefficients[j];
                }
                f.left = {s.left.data(), f.left.size()};
                f.right = {s.right.data(), f.right.size()};
                f.metadata.bark_weighted = true;
            }
            break;
        case NodeKind::SpectrumMerge:
            if (f.type != FrameType::StereoSpectrum)
                return;
            merge(f, s, std::get<MergeParameters>(n.node.parameters).merge);
            f.type = FrameType::Spectrum;
            f.left = {};
            f.right = {};
            break;
        case NodeKind::SpectrumGain:
        case NodeKind::WLED:
            for (std::size_t j = 0; j < f.values.size(); ++j)
                s.values[j] = f.values[j] * float(std::get<GainParameters>(n.node.parameters).gain);
            f.values = {s.values.data(), f.values.size()};
            if (k == NodeKind::SpectrumGain)
                f.metadata.magnitude *= float(std::get<GainParameters>(n.node.parameters).gain);
            break;
        case NodeKind::Mel:
            if (!s.mel.process(f.values, {s.values.data(), n.output.columns}))
                return;
            f.values = {s.values.data(), n.output.columns};
            f.type = FrameType::Mel;
            f.metadata.bands = n.output.columns;
            break;
        case NodeKind::Spectrogram: {
            std::size_t row;
            if (s.frames < n.history_rows) {
                row = (s.oldest + s.frames) % n.history_rows;
                ++s.frames;
            } else {
                row = s.oldest;
                s.oldest = (s.oldest + 1) % n.history_rows;
            }
            std::copy(f.values.begin(), f.values.end(), s.history.data() + row * n.output.columns);
            f.history = {s.history, n.output.columns, s.frames, n.history_rows, s.oldest};
            f.values = {};
            f.type = FrameType::History;
            f.metadata.bands = n.output.columns;
            f.metadata.history_capacity = n.history_rows;
            break;
        }
        case NodeKind::FFTView: {
            const auto p = std::get<FFTViewParameters>(n.node.parameters);
            if (p.channel == Channel::Merged) {
                merge(f, s, p.merge);
                display_values = f.values;
                f.values = {};
            } else
                display_values = p.channel == Channel::Left ? f.left : f.right;
            break;
        }
        case NodeKind::NetworkOutput:
            f.metadata.stream_id = std::get<NetworkParameters>(n.node.parameters).stream_id;
            break;
        default:
            break;
        }
        emit(f);
    };
    run(run, at, input);
    return processing_status;
}
std::vector<NodeId> PreparedGraph::clear_ingress(NodeId ingress, RuntimeContext &c) const {
    std::vector<NodeId> affected;
    if (!c.impl_ || c.impl_->plan != this)
        return affected;
    const auto at = index(impl_->nodes, ingress);
    if (at == absent)
        return affected;
    for (std::size_t i = 0; i < impl_->nodes.size(); ++i)
        if (impl_->nodes[i].ingress == at) {
            affected.push_back(impl_->nodes[i].node.id);
            auto &s = c.impl_->states[i];
            s.clear();
            s.unavailable = true;
        }
    return affected;
}
bool PreparedGraph::marker_current(NodeId ingress, const FrameView &f, const RuntimeContext &c) const noexcept {
    const auto at = index(impl_->nodes, ingress);
    if (at == absent || !c.impl_ || c.impl_->plan != this)
        return true;
    const auto &s = c.impl_->states[at];
    const auto epoch = f.metadata.producer_epoch ? f.metadata.producer_epoch : f.metadata.epoch;
    return !retired(s, epoch) &&
           (epoch != s.producer_epoch || !f.metadata.generation || f.metadata.generation >= s.generation);
}
HistoryView PreparedGraph::history(NodeId id, const RuntimeContext &c) const noexcept {
    const auto requested = index(impl_->nodes, id);
    if (requested == absent || !c.impl_ || c.impl_->plan != this || c.impl_->states[requested].unavailable)
        return {};
    auto at = requested, fallback = absent;
    bool newest_zero = false;
    while (at != absent) {
        const auto &n = impl_->nodes[at];
        const auto &s = c.impl_->states[at];
        if (n.node.kind == NodeKind::Spectrogram) {
            HistoryView h{s.history, n.output.columns, s.frames, n.history_rows, s.oldest};
            if (newest_zero && h.frames)
                h.newest_override = {c.impl_->states[requested].zero.data(), h.bands};
            return h;
        }
        if (n.output.type != FrameType::History)
            return {};
        newest_zero = newest_zero || !n.node.enabled || n.detached;
        if (n.detached && n.history_rows)
            fallback = at;
        if (source(n.node))
            break;
        const auto next = n.clock;
        if (next != absent && impl_->nodes[next].output.type != FrameType::History)
            break;
        at = next;
    }
    if (fallback != absent) {
        const auto &n = impl_->nodes[fallback];
        const auto &s = c.impl_->states[fallback];
        HistoryView h{s.history, n.output.columns, s.frames, n.history_rows, s.oldest};
        if (newest_zero && h.frames)
            h.newest_override = {c.impl_->states[requested].zero.data(), h.bands};
        return h;
    }
    return {}; // External ingress spans expire at the end of their producer tick.
}
NodeId PreparedGraph::history_ingress(NodeId id) const noexcept {
    auto at = index(impl_->nodes, id);
    while (at != absent) {
        const auto &n = impl_->nodes[at];
        if (!n.known || n.output.type != FrameType::History || n.node.kind == NodeKind::Spectrogram)
            return -1;
        if (source(n.node))
            return n.node.id;
        if (n.detached && n.history_rows &&
            (n.clock == absent || impl_->nodes[n.clock].output.type != FrameType::History))
            return -1;
        at = n.clock;
    }
    return -1;
}
void PreparedGraph::reset_history(NodeId id, RuntimeContext &c) const noexcept {
    const auto at = index(impl_->nodes, id);
    if (at != absent && c.impl_ && c.impl_->plan == this)
        c.impl_->states[at].frames = c.impl_->states[at].oldest = 0;
}
void PreparedGraph::rebase_local_producers(RuntimeContext &c, std::uint64_t from, std::uint64_t to,
                                           std::uint64_t generation) const noexcept {
    if (!c.impl_ || c.impl_->plan != this)
        return;
    for (std::size_t i = 0; i < impl_->nodes.size(); ++i) {
        const auto &n = impl_->nodes[i];
        auto &s = c.impl_->states[i];
        if (n.ingress == absent || !local(impl_->nodes[n.ingress].node.kind) || !from || s.producer_epoch != from)
            continue;
        if (from != to && !retired(s, from)) {
            s.retired[s.retired_next] = from;
            s.retired_next = (s.retired_next + 1) % s.retired.size();
            s.retired_count = std::min(s.retired_count + 1, s.retired.size());
        }
        s.producer_epoch = to;
        s.last_input.producer_epoch = s.last_input.epoch = to;
        s.sequence = 0;
        if (generation)
            s.generation = s.last_input.generation = generation;
        if (source(n.node))
            s.preserve_next_tick = true;
    }
}
void PreparedGraph::refresh_state(RuntimeContext &c, const PreparedGraph &previous,
                                  const RuntimeContext &old_context) const noexcept {
    for (std::size_t i = 0; i < impl_->nodes.size(); ++i) {
        const auto &n = impl_->nodes[i];
        auto &s = c.impl_->states[i];
        if (!n.reset) {
            const auto old = index(previous.impl_->nodes, n.node.id);
            const auto &saved = old_context.impl_->states[old];
            s.clear();
            s.generation = saved.generation;
            s.sequence = saved.sequence;
            s.output_sequence = saved.output_sequence;
            s.producer_epoch = saved.producer_epoch;
            s.retired = saved.retired;
            s.retired_count = saved.retired_count;
            s.retired_next = saved.retired_next;
            s.last_input = saved.last_input;
            s.last_input.stream_id = {};
            s.unavailable = saved.unavailable;
            s.pending_discontinuity = saved.pending_discontinuity;
            s.preserve_next_tick = saved.preserve_next_tick;
            s.progress = saved.progress;
            s.remainder = saved.remainder;
            s.hop = saved.hop;
            if (s.pcm_left.size() == saved.pcm_left.size()) {
                std::copy(saved.pcm_left.begin(), saved.pcm_left.end(), s.pcm_left.begin());
                std::copy(saved.pcm_right.begin(), saved.pcm_right.end(), s.pcm_right.begin());
            }
            if (n.history_rows) {
                auto history_owner = old;
                const auto &oldn = previous.impl_->nodes[old];
                if (n.detached && n.input.type == FrameType::History && oldn.clock != absent &&
                    previous.impl_->nodes[oldn.clock].history_rows)
                    history_owner = oldn.clock;
                const auto &history_state = old_context.impl_->states[history_owner];
                const auto &history_plan = previous.impl_->nodes[history_owner];
                s.frames = std::min(n.history_rows, history_state.frames);
                for (std::size_t row = 0; row < s.frames; ++row) {
                    const auto oldrow =
                        (history_state.oldest + history_state.frames - s.frames + row) % history_plan.history_rows;
                    std::copy_n(history_state.history.data() + oldrow * n.output.columns, n.output.columns,
                                s.history.data() + row * n.output.columns);
                }
            }
        }
    }
}
std::vector<NodeId> PreparedGraph::reset_nodes() const {
    std::vector<NodeId> result;
    for (const auto &n : impl_->nodes)
        if (n.reset)
            result.push_back(n.node.id);
    return result;
}
GraphExecutor::GraphExecutor(std::span<const IngressSpec> specs) : specs_(specs.begin(), specs.end()) {}
void GraphExecutor::ingress_specs(std::span<const IngressSpec> specs) {
    specs_.assign(specs.begin(), specs.end());
    prepared_.reset();
}
Status GraphExecutor::prepare(const GraphDefinition &graph, const std::vector<NodeId> &order,
                              const application::RuntimeLimits &limits, application::PreparedExecution &result) {
    result = {};
    const auto validation = validate_graph(graph, desktop_capabilities(), limits.graph);
    if (!validation || validation.topological_order != order)
        return Status::invalid_argument;
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> context;
    const auto status = PreparedGraph::prepare(graph, specs_, limits, plan, context, plan_.get(), context_.get());
    if (status != Status::ok)
        return status;
    prepared_ = plan;
    prepared_context_ = context.get();
    prepared_base_plan_ = plan_.get();
    result.plan = std::move(plan);
    result.context = std::move(context);
    return Status::ok;
}
Status GraphExecutor::apply(application::PreparedExecution &&execution, bool run_requested) {
    const auto expected = prepared_.lock();
    if (!execution || !expected || execution.plan.get() != expected.get() ||
        execution.context.get() != prepared_context_ || plan_.get() != prepared_base_plan_)
        return Status::invalid_argument;
    // Identity is checked before casts, so no RTTI or foreign-plan downcast is
    // needed. The bundle came from this preparation port and remains untouched
    // on failure. Successful application consumes it at the owner's boundary.
    if (plan_)
        expected->refresh_state(*static_cast<RuntimeContext *>(execution.context.get()), *plan_, *context_);
    plan_ = std::static_pointer_cast<const PreparedGraph>(std::move(execution.plan));
    context_.reset(static_cast<RuntimeContext *>(execution.context.release()));
    prepared_.reset();
    prepared_context_ = nullptr;
    running_ = run_requested;
    return Status::ok;
}
Status GraphExecutor::start() {
    if (!plan_)
        return Status::unavailable;
    running_ = true;
    return Status::ok;
}
Status GraphExecutor::stop() {
    running_ = false;
    if (plan_)
        for (const auto &n : plan_->definition().nodes)
            if (!input_type(n))
                plan_->clear_ingress(n.id, *context_);
    return Status::ok;
}
Status GraphExecutor::process(NodeId ingress, const FrameView &input, OutputCallback callback, void *user) noexcept {
    if (!running_ || !plan_)
        return Status::would_block;
    return plan_->process(ingress, input, *context_, callback, user);
}
} // namespace dataflow_runtime
