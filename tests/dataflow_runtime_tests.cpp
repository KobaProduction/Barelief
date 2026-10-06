#include <dataflow_runtime/runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <new>
#include <limits>
static bool guard = false;
static std::size_t allocations = 0;
#ifdef DATAFLOW_RUNTIME_WRAP_ALLOCATOR
extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void *__wrap_malloc(std::size_t size) {
    if (guard)
        ++allocations;
    return __real_malloc(size);
}
extern "C" void *__wrap_calloc(std::size_t n, std::size_t size) {
    if (guard)
        ++allocations;
    return __real_calloc(n, size);
}
extern "C" void *__wrap_realloc(void *p, std::size_t size) {
    if (guard)
        ++allocations;
    return __real_realloc(p, size);
}
#endif
void *operator new(std::size_t size) {
    if (guard)
        ++allocations;
    if (auto p = std::malloc(size))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
using namespace dataflow_runtime;
using namespace domain;
using namespace signal_contracts;
static NodeDefinition node(int id, NodeKind k) {
    return {id, k, default_parameters(k), true};
}
struct Result {
    std::size_t windows = 0, rows = 0;
    float last = 0;
    std::uint64_t sequence = 0;
};
static void output(void *user, const OutputView &out) {
    auto &r = *static_cast<Result *>(user);
    if (out.window_emitted)
        ++r.windows;
    if (out.kind == NodeKind::SpectrogramView) {
        r.rows = out.frame.history.frames;
        r.last = out.frame.history.row(r.rows - 1)[0];
        r.sequence = out.frame.metadata.sequence;
    }
}
static GraphDefinition mel_graph() {
    GraphDefinition g;
    g.nodes = {node(0, NodeKind::NetworkInput), node(1, NodeKind::Spectrogram), node(2, NodeKind::SpectrogramView)};
    g.nodes[0].parameters = NetworkParameters{FrameType::Mel, "", "mel"};
    g.nodes[1].parameters = HistoryParameters{4};
    g.edges = {{0, 1}, {1, 2}};
    return g;
}
static void pending_execution_test() {
    auto g = mel_graph();
    std::array<float, 3> data{1, 2, 3};
    FrameView f;
    f.type = FrameType::Mel;
    f.values = data;
    f.metadata.sample_rate = 48000;
    f.metadata.bands = 3;
    f.metadata.sequence = 1;
    std::array<IngressSpec, 1> specs{{{0, contract(f), 3}}};
    GraphExecutor executor(specs);
    const auto order = validate_graph(g, desktop_capabilities()).topological_order;
    application::PreparedExecution pending;
    assert(executor.prepare(g, order, {}, pending) == Status::ok);
    assert(executor.apply(std::move(pending), true) == Status::ok);
    assert(executor.process(0, f) == Status::ok);
    assert(executor.prepare(g, order, {}, pending) == Status::ok);
    f.metadata.sequence = 2;
    assert(executor.process(0, f) == Status::ok);
    assert(executor.apply(std::move(pending), true) == Status::ok && !pending);
    assert(executor.plan()->history(1, *executor.context()).frames == 2);
    assert(executor.prepare(g, order, {}, pending) == Status::ok);
    assert(executor.stop() == Status::ok);
    assert(executor.apply(std::move(pending), false) == Status::ok && !pending);
    assert(executor.plan()->history(1, *executor.context()).frames == 0);
    assert(executor.prepare(g, order, {}, pending) == Status::ok);
    assert(executor.apply(std::move(pending), false) == Status::ok);
    assert(executor.start() == Status::ok);
    f.metadata.sequence = 3;
    assert(executor.process(0, f) == Status::ok);
    assert(executor.plan()->history(1, *executor.context()).frames == 1);
    GraphExecutor live(specs);
    application::GraphLifecycle lifecycle(live, live, desktop_capabilities());
    assert(lifecycle.prepare(g) && lifecycle.apply());
    f.metadata.sequence = 1;
    assert(live.process(0, f) == Status::ok);
    auto changed = g;
    changed.nodes[1].parameters = HistoryParameters{2};
    changed.edges.pop_back(); // detached History view needs the latest producer ring
    assert(lifecycle.prepare(changed));
    data[0] = 4;
    f.metadata.sequence = 2;
    assert(live.process(0, f) == Status::ok);
    assert(lifecycle.apply());
    auto latest = live.plan()->history(1, *live.context());
    assert(latest.frames == 2 && latest.row(1)[0] == 4);
    latest = live.plan()->history(2, *live.context());
    assert(latest.frames == 2 && latest.row(0)[0] == 1 && latest.row(1)[0] == 0);
    assert(lifecycle.prepare(g));
    assert(lifecycle.stop() == Status::ok);
    assert(lifecycle.apply() && !live.running());
    assert(live.plan()->history(1, *live.context()).frames == 0);
    assert(lifecycle.start() == Status::ok);
    f.metadata.sequence = 3;
    assert(live.process(0, f) == Status::ok);
    assert(live.plan()->history(1, *live.context()).frames == 1);
}
static void marker_identity_test() {
    auto g = mel_graph();
    std::array<float, 3> data{1, 2, 3};
    FrameView f;
    f.type = FrameType::Mel;
    f.values = data;
    f.metadata.sample_rate = 48000;
    f.metadata.bands = 3;
    f.metadata.sequence = 1;
    f.metadata.epoch = 999;
    f.metadata.producer_epoch = 100;
    std::array<IngressSpec, 1> specs{{{0, contract(f), 3}}};
    std::shared_ptr<const PreparedGraph> p;
    std::unique_ptr<RuntimeContext> c;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::ok);
    assert(p->process(0, f, *c) == Status::ok);
    auto marker = f;
    marker.values = {};
    marker.metadata.sequence = 0;
    marker.metadata.available = false;
    f.metadata.producer_epoch = 101;
    f.metadata.sequence = 2;
    assert(p->process(0, f, *c) == Status::ok);
    assert(!p->marker_current(0, marker, *c));
    assert(p->process(0, marker, *c) == Status::would_block && p->history(2, *c).frames == 1);
}
static void source_semantics_test() {
    GraphDefinition g;
    g.nodes = {node(0, NodeKind::AudioSource), node(1, NodeKind::AudioView)};
    g.edges = {{0, 1}};
    std::array<IngressSpec, 1> specs{{{0, {FrameType::Mel, 48000, 0, 3, 0, 3}, 3}}};
    std::shared_ptr<const PreparedGraph> p;
    std::unique_ptr<RuntimeContext> c;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::incompatible_contract && !p && !c);
    specs[0].contract = {FrameType::Window, 48000, 512, 0, 0, 512};
    specs[0].max_samples = 512;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::incompatible_contract);
    specs[0].allow_legacy_window = true;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::ok);
    specs[0].contract.type = FrameType::Mel;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::incompatible_contract);
    specs[0].allow_legacy_window = false;
    specs[0].contract = {FrameType::StereoSpectrum, 48000, 512, 0, 0, 256};
    specs[0].max_samples = 256;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::ok);
    specs[0].contract.fft_size = 513;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::incompatible_contract);
}
static void unavailable_tick_test() {
    auto g = mel_graph();
    std::array<float, 3> data{1, 2, 3};
    FrameView f;
    f.type = FrameType::Mel;
    f.values = data;
    f.metadata.sample_rate = 48000;
    f.metadata.bands = 3;
    f.metadata.sequence = 1;
    std::array<IngressSpec, 1> specs{{{0, contract(f), 3}}};
    GraphExecutor executor(specs);
    application::GraphLifecycle lifecycle(executor, executor, desktop_capabilities());
    assert(lifecycle.prepare(g) && lifecycle.apply());
    assert(executor.process(0, f) == Status::ok);
    auto marker = f;
    marker.values = {};
    marker.metadata.sequence = 0;
    marker.metadata.available = false;
    std::size_t observed = 0;
    auto inspect = [](void *u, const OutputView &o) {
        ++*static_cast<std::size_t *>(u);
        assert(validate(o.frame) == Status::unavailable);
    };
    allocations = 0;
    guard = true;
    const auto status = executor.process(0, marker, inspect, &observed);
    guard = false;
    assert(status == Status::unavailable && !allocations && observed == 3 &&
           executor.plan()->history(2, *executor.context()).frames == 0);
    marker.metadata.producer_epoch = 100;
    assert(executor.process(0, marker) == Status::unavailable);
    f.metadata.producer_epoch = 100;
    f.metadata.sequence = 1;
    assert(executor.process(0, f) == Status::ok);
    marker.metadata.producer_epoch = 101;
    assert(executor.process(0, marker) == Status::unavailable);
    f.metadata.sequence = 2;
    assert(executor.process(0, f) == Status::would_block);
    f.metadata.producer_epoch = 101;
    f.metadata.sequence = 1;
    assert(executor.process(0, f) == Status::ok);
}
static void history_fallback_tick_test() {
    auto g = mel_graph();
    std::array<float, 3> data{1, 2, 3};
    FrameView f;
    f.type = FrameType::Mel;
    f.values = data;
    f.metadata.sample_rate = 48000;
    f.metadata.bands = 3;
    f.metadata.sequence = 1;
    std::array<IngressSpec, 1> specs{{{0, contract(f), 3}}};
    std::shared_ptr<const PreparedGraph> p, q;
    std::unique_ptr<RuntimeContext> c, d;
    assert(PreparedGraph::prepare(g, specs, {}, p, c) == Status::ok);
    assert(p->process(0, f, *c) == Status::ok);
    g.nodes.erase(g.nodes.begin() + 1);
    g.edges.clear();
    assert(PreparedGraph::prepare(g, specs, {}, q, d, p.get(), c.get()) == Status::ok);
    f.metadata.sequence = 2;
    Result r;
    allocations = 0;
    guard = true;
    const auto status = q->process(0, f, *d, output, &r);
    guard = false;
    assert(status == Status::ok && !allocations && r.rows == 2 && r.last == 0 && q->history(2, *d).row(0)[0] == 1);
    auto marker = f;
    marker.values = {};
    marker.metadata.available = false;
    marker.metadata.sequence = 0;
    assert(q->process(0, marker, *d) == Status::unavailable && q->history(2, *d).frames == 0);
    f.metadata.sequence = 3;
    r = {};
    assert(q->process(0, f, *d, output, &r) == Status::ok && r.rows == 1 && r.last == 0);
    q->reset_history(2, *d);
    f.metadata.sequence = 4;
    r = {};
    assert(q->process(0, f, *d, output, &r) == Status::ok && r.rows == 1 && r.last == 0);
}
int main(int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "pending_execution") {
        pending_execution_test();
        return 0;
    }
    if (argc == 2) {
        const std::string_view selected = argv[1];
        if (selected == "marker_identity")
            marker_identity_test();
        else if (selected == "source_semantics")
            source_semantics_test();
        else if (selected == "unavailable_tick")
            unavailable_tick_test();
        else if (selected == "history_fallback_tick")
            history_fallback_tick_test();
        else
            assert(false);
        return 0;
    }
    GraphDefinition g;
    for (auto [id, k] : std::array<std::pair<int, NodeKind>, 9>{{{0, NodeKind::NetworkInput},
                                                                 {1, NodeKind::Window},
                                                                 {2, NodeKind::WindowFunction},
                                                                 {3, NodeKind::StereoFFT},
                                                                 {4, NodeKind::BarkWeighting},
                                                                 {5, NodeKind::SpectrumMerge},
                                                                 {6, NodeKind::Mel},
                                                                 {7, NodeKind::Spectrogram},
                                                                 {8, NodeKind::SpectrogramView}}})
        g.nodes.push_back(node(id, k));
    g.nodes[0].parameters = NetworkParameters{FrameType::PCM, "", "source"};
    g.nodes[1].parameters = WindowParameters{512};
    g.nodes[6].parameters = MelParameters{3};
    g.nodes[7].parameters = HistoryParameters{4};
    for (int i = 0; i < 8; ++i)
        g.edges.push_back({i, i + 1});
    // Every canonical operation is present in the real route, including sinks,
    // independent local sources, and branch-specific gain/channel selection.
    for (auto [id, k] : std::array<std::pair<int, NodeKind>, 10>{{{9, NodeKind::FFTView},
                                                                  {10, NodeKind::AudioView},
                                                                  {11, NodeKind::WindowView},
                                                                  {12, NodeKind::WindowedView},
                                                                  {13, NodeKind::SpectrumGain},
                                                                  {14, NodeKind::MelView},
                                                                  {15, NodeKind::WLED},
                                                                  {16, NodeKind::NetworkOutput},
                                                                  {17, NodeKind::TestSignal},
                                                                  {18, NodeKind::AudioSource}}})
        g.nodes.push_back(node(id, k));
    g.nodes[16].parameters = NetworkParameters{FrameType::History, "", "history"};
    g.nodes[13].parameters = GainParameters{2};
    for (auto [from, to] :
         std::array<std::pair<int, int>, 8>{{{4, 9}, {0, 10}, {1, 11}, {2, 12}, {5, 13}, {6, 14}, {6, 15}, {7, 16}}})
        g.edges.push_back({from, to});
    std::array<IngressSpec, 1> specs{{{0, {FrameType::PCM, 44100, 0, 0, 0, 4096}, 4096}}};
    std::shared_ptr<const PreparedGraph> p;
    std::unique_ptr<RuntimeContext> c;
    application::RuntimeLimits limits;
    limits.max_memory_bytes = 1;
    assert(PreparedGraph::prepare(g, specs, limits, p, c) == Status::exhausted && !p && !c);
    limits.max_memory_bytes = 64 * 1024 * 1024;
    assert(PreparedGraph::prepare(g, specs, limits, p, c) == Status::ok);
    assert(p->memory_requirements().fits(limits.max_memory_bytes));
    std::array<float, 4096> pcm{};
    for (std::size_t i = 0; i < pcm.size(); ++i)
        for (int t = 0; t < 8; ++t)
            pcm[i] += std::sin(2 * 3.141592653589793 * (13 + t * 19) * i / 512) / 16;
    FrameView f;
    f.left = pcm;
    f.right = pcm;
    f.metadata.sample_rate = 44100;
    f.metadata.sequence = 1;
    Result r;
    guard = true;
    const auto status = p->process(0, f, *c, output, &r);
    guard = false;
    assert(status == Status::ok && allocations == 0 && r.windows == 5 && r.rows == 4 && r.last > 0);
    assert(p->history(8, *c).frames == 4 && p->history(16, *c).row(3)[0] == p->history(7, *c).row(3)[0]);
    assert(p->history_ingress(8) == -1);
    // Duplicate/retired deliveries cannot revive stale state.
    assert(p->process(0, f, *c, output, &r) == Status::would_block);
    f.metadata.sequence = 2;
    f.metadata.producer_epoch = 100;
    assert(p->process(0, f, *c) == Status::ok);
    f.metadata.producer_epoch = 101;
    assert(p->process(0, f, *c) == Status::ok);
    f.metadata.producer_epoch = 100;
    assert(p->process(0, f, *c) == Status::would_block);
    f.metadata.producer_epoch = 101;
    f.metadata.sequence = 3;
    const auto affected = p->clear_ingress(0, *c);
    assert(affected.size() == 17 && p->history(7, *c).frames == 0);
    assert(p->process(0, f, *c) == Status::ok);
    // Detached known-clock routes append typed silence without losing old rows.
    g.edges.erase(g.edges.begin() + 1);
    std::shared_ptr<const PreparedGraph> next;
    std::unique_ptr<RuntimeContext> nc;
    assert(PreparedGraph::prepare(g, specs, limits, next, nc, p.get(), c.get()) == Status::ok);
    f.metadata.sequence = 4;
    r = {};
    allocations = 0;
    guard = true;
    const auto silent = next->process(0, f, *nc, output, &r);
    guard = false;
    assert(silent == Status::ok && !allocations && r.rows == 4 && r.last == 0);
    assert(next->history(8, *nc).row(3)[0] == 0);
    auto removed = g;
    std::erase_if(removed.nodes, [](const auto &n) { return n.id == 7; });
    std::erase_if(removed.edges, [](const auto &e) { return e.from == 7 || e.to == 7; });
    std::shared_ptr<const PreparedGraph> deleted;
    std::unique_ptr<RuntimeContext> dc;
    assert(PreparedGraph::prepare(removed, specs, limits, deleted, dc, next.get(), nc.get()) == Status::ok);
    f.metadata.sequence = 5;
    r = {};
    allocations = 0;
    guard = true;
    const auto removed_status = deleted->process(0, f, *dc, output, &r);
    guard = false;
    assert(removed_status == Status::ok && !allocations && r.rows == 4 && r.last == 0 &&
           deleted->history(8, *dc).row(3)[0] == 0);
    auto missing_clock = removed;
    std::erase_if(missing_clock.nodes, [](const auto &n) { return n.id == 0; });
    std::erase_if(missing_clock.edges, [](const auto &e) { return e.from == 0; });
    std::shared_ptr<const PreparedGraph> clockless;
    std::unique_ptr<RuntimeContext> cc;
    assert(PreparedGraph::prepare(missing_clock, {}, limits, clockless, cc, deleted.get(), dc.get()) == Status::ok);
    const auto resets = clockless->reset_nodes();
    assert(std::find(resets.begin(), resets.end(), 8) != resets.end() && clockless->history(8, *cc).frames == 0);
    // Bad dimensions are rejected before mutating the working ring.
    f.right = {};
    assert(next->process(0, f, *nc) == Status::invalid_argument);
    // Snapshotting a context on Apply is a deep copy. Explicit clears and
    // subsequent ticks must not mutate a retained transaction snapshot.
    auto saved = *nc;
    const auto retained_rows = next->history(7, saved).frames;
    next->reset_history(7, *nc);
    assert(next->history(7, *nc).frames == 0 && next->history(7, saved).frames == retained_rows);
    std::array<IngressSpec, 3> all_specs{{specs[0],
                                          {17, {FrameType::PCM, 44100, 0, 0, 0, 4096}, 4096},
                                          {18, {FrameType::PCM, 44100, 0, 0, 0, 4096}, 4096}}};
    assert(PreparedGraph::prepare(g, all_specs, limits, next, nc, p.get(), c.get()) == Status::ok);
    f.right = pcm;
    f.metadata.producer_epoch = 0;
    f.metadata.sequence = 1;
    allocations = 0;
    guard = true;
    const auto test_source = next->process(17, f, *nc);
    const auto audio_source = next->process(18, f, *nc);
    guard = false;
    assert(test_source == Status::ok && audio_source == Status::ok && !allocations);
    f.metadata.sequence = 2;
    f.metadata.epoch = f.metadata.producer_epoch = 20;
    assert(next->process(17, f, *nc) == Status::ok);
    next->rebase_local_producers(*nc, 20, 21, 0);
    f.metadata.sequence = 3;
    assert(next->process(17, f, *nc) == Status::would_block && !next->marker_current(17, f, *nc));
    f.metadata.sequence = 1;
    f.metadata.epoch = f.metadata.producer_epoch = 21;
    assert(next->process(17, f, *nc) == Status::ok);
    // Unknown contracts report unavailable and never produce untyped stale data.
    assert(PreparedGraph::prepare(g, {}, limits, next, nc) == Status::ok);
    assert(next->process(0, f, *nc) == Status::unavailable);
    // Gating an external wrapped history overlays just its newest row. No
    // history materialization, allocation, or mutation of producer storage.
    GraphDefinition hg;
    hg.nodes = {node(0, NodeKind::NetworkInput), node(1, NodeKind::SpectrogramView), node(2, NodeKind::NetworkOutput)};
    hg.nodes[0].parameters = NetworkParameters{FrameType::History, "", "input"};
    hg.nodes[2].parameters = NetworkParameters{FrameType::History, "", "output"};
    hg.nodes[1].enabled = hg.nodes[2].enabled = false;
    hg.edges = {{0, 1}, {0, 2}};
    std::array<float, 6> ring{1, 2, 3, 4, 5, 6};
    FrameView history;
    history.type = FrameType::History;
    history.metadata.sequence = 1;
    history.metadata.sample_rate = 48000;
    history.metadata.bands = 2;
    history.metadata.history_capacity = 3;
    history.history = {ring, 2, 3, 3, 1};
    std::array<IngressSpec, 1> hs{{{0, contract(history), 2}}};
    assert(PreparedGraph::prepare(hg, hs, limits, next, nc) == Status::ok);
    std::size_t overlays = 0;
    auto inspect = [](void *user, const OutputView &out) {
        if (out.node == 0)
            return;
        auto &count = *static_cast<std::size_t *>(user);
        ++count;
        assert(validate(out.frame) == Status::ok && out.frame.history.row(0)[0] == 3 &&
               out.frame.history.row(1)[0] == 5 && out.frame.history.row(2)[0] == 0);
    };
    allocations = 0;
    guard = true;
    const auto history_status = next->process(0, history, *nc, inspect, &overlays);
    guard = false;
    assert(history_status == Status::ok && overlays == 2 && !allocations && ring[0] == 1);
    assert(next->history(1, *nc).storage.empty() && next->history_ingress(1) == 0);
    // Preparation fails atomically on graph and resource limits.
    limits.graph.max_nodes = 1;
    assert(PreparedGraph::prepare(hg, hs, limits, next, nc) == Status::invalid_argument && !next && !nc);
    limits.graph.max_nodes = 128;
    limits.max_history_frames = 2;
    assert(PreparedGraph::prepare(hg, hs, limits, next, nc) == Status::invalid_argument && !next && !nc);
    // Real application ports prepare/apply this executor and preserve state
    // during disconnect. A stopped application cannot process a tick.
    GraphDefinition lifecycle_graph;
    lifecycle_graph.nodes = {node(0, NodeKind::NetworkInput), node(1, NodeKind::Spectrogram),
                             node(2, NodeKind::SpectrogramView)};
    lifecycle_graph.nodes[0].parameters = NetworkParameters{FrameType::Mel, "", "mel"};
    lifecycle_graph.nodes[1].parameters = HistoryParameters{4};
    lifecycle_graph.edges = {{0, 1}, {1, 2}};
    std::array<float, 3> mel{1, 2, 3};
    FrameView mf;
    mf.type = FrameType::Mel;
    mf.values = mel;
    mf.metadata.sample_rate = 48000;
    mf.metadata.bands = 3;
    mf.metadata.sequence = 1;
    std::array<IngressSpec, 1> ms{{{0, contract(mf), 3}}};
    GraphExecutor executor(ms);
    application::GraphLifecycle lifecycle(executor, executor, desktop_capabilities());
    assert(lifecycle.prepare(lifecycle_graph) && lifecycle.apply() && executor.running());
    r = {};
    assert(executor.process(0, mf, output, &r) == Status::ok && r.rows == 1 && r.last == 1);
    assert(lifecycle.disconnect(1));
    mf.metadata.sequence = 2;
    r = {};
    allocations = 0;
    guard = true;
    const auto disconnected = executor.process(0, mf, output, &r);
    guard = false;
    assert(disconnected == Status::ok && !allocations && r.rows == 2 && r.last == 0);
    assert(lifecycle.stop() == Status::ok && executor.process(0, mf) == Status::would_block);
    assert(executor.plan()->history(1, *executor.context()).frames == 0);
    assert(lifecycle.start() == Status::ok);
    mf.metadata.sequence = 3;
    assert(executor.process(0, mf) == Status::ok);
    auto invalid_graph = lifecycle_graph;
    invalid_graph.edges.push_back({0, 2});
    const auto *retained_plan = executor.plan();
    assert(!lifecycle.prepare(invalid_graph) && executor.plan() == retained_plan);
    GraphDefinition overflow_graph;
    overflow_graph.nodes = {node(0, NodeKind::NetworkInput), node(1, NodeKind::SpectrumGain),
                            node(2, NodeKind::NetworkOutput)};
    overflow_graph.nodes[0].parameters = NetworkParameters{FrameType::Spectrum, "", "spectrum"};
    overflow_graph.nodes[1].parameters = GainParameters{100};
    overflow_graph.nodes[2].parameters = NetworkParameters{FrameType::Spectrum, "", "gain"};
    overflow_graph.edges = {{0, 1}, {1, 2}};
    std::array<float, 1> huge{std::numeric_limits<float>::max()};
    FrameView sf;
    sf.type = FrameType::Spectrum;
    sf.values = huge;
    sf.metadata.sample_rate = 48000;
    sf.metadata.sequence = 1;
    std::array<IngressSpec, 1> os{{{0, contract(sf), 1}}};
    limits = {};
    assert(PreparedGraph::prepare(overflow_graph, os, limits, next, nc) == Status::ok);
    std::size_t observed = 0;
    auto count = [](void *p, const OutputView &out) {
        ++*static_cast<std::size_t *>(p);
        assert(out.node == 0 || (!out.frame.metadata.available && validate(out.frame) == Status::unavailable));
    };
    allocations = 0;
    guard = true;
    const auto overflow = next->process(0, sf, *nc, count, &observed);
    guard = false;
    assert(overflow == Status::overflow && observed == 3 && !allocations);
    GraphDefinition oversized;
    oversized.nodes = {node(0, NodeKind::AudioSource)};
    std::array<IngressSpec, 1> impossible{{{0,
                                            {FrameType::PCM, 48000, 0, 0, 0, std::numeric_limits<std::size_t>::max()},
                                            std::numeric_limits<std::size_t>::max()}}};
    limits.max_frame_samples = std::numeric_limits<std::size_t>::max();
    assert(PreparedGraph::prepare(oversized, impossible, limits, next, nc) == Status::overflow && !next && !nc);
    GraphExecutor foreign(ms);
    application::PreparedExecution foreign_bundle;
    const auto order = validate_graph(lifecycle_graph, desktop_capabilities()).topological_order;
    assert(foreign.prepare(lifecycle_graph, order, {}, foreign_bundle) == Status::ok);
    assert(executor.apply(std::move(foreign_bundle), true) == Status::invalid_argument && foreign_bundle &&
           executor.plan() == retained_plan);
    assert(foreign.apply(std::move(foreign_bundle), true) == Status::ok && !foreign_bundle);
    GraphDefinition reset_window;
    reset_window.nodes = {node(0, NodeKind::AudioSource), node(1, NodeKind::Window)};
    reset_window.nodes[1].parameters = WindowParameters{512};
    reset_window.edges = {{0, 1}};
    std::array<IngressSpec, 1> ws{{{0, {FrameType::PCM, 4800, 0, 0, 0, 80}, 80}}};
    limits = {};
    assert(PreparedGraph::prepare(reset_window, ws, limits, next, nc) == Status::ok);
    FrameView wf;
    wf.metadata.sample_rate = 4800;
    wf.metadata.sequence = 1;
    wf.left = std::span<const float>(pcm).first(80);
    wf.right = wf.left;
    auto peak = [](void *user, const OutputView &out) {
        if (out.node != 1)
            return;
        for (float v : out.frame.left)
            *static_cast<float *>(user) = std::max(*static_cast<float *>(user), std::abs(v));
    };
    float window_peak = 0;
    assert(next->process(0, wf, *nc, peak, &window_peak) == Status::ok && window_peak > 0);
    next->clear_ingress(0, *nc);
    std::array<float, 80> zeros{};
    wf.left = zeros;
    wf.right = zeros;
    wf.metadata.sequence = 2;
    window_peak = 0;
    assert(next->process(0, wf, *nc, peak, &window_peak) == Status::ok && window_peak == 0);
    pending_execution_test();
    marker_identity_test();
    source_semantics_test();
    unavailable_tick_test();
    history_fallback_tick_test();
}
