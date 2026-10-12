#pragma once

// Composition-time policy; set before constructing views. This is not a DSP
// setting or a user configuration key. Benchmarks override it explicitly.
namespace presentation {
enum class MeshPreparation { Inline, Worker, Pooled };
enum class PlotHost { TextureWidget, NativeWindow };
struct RenderPreparationPolicy {
    static inline PlotHost plot_host = PlotHost::TextureWidget;
    static inline bool snapshot_pool = true;
    static inline MeshPreparation mesh = MeshPreparation::Inline;
};
}
