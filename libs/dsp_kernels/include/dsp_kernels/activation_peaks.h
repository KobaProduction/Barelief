#pragma once
#include <dsp_kernels/convolution.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace dsp_kernels {
struct ActivationPeakConfig {
    float threshold=.5f;
    std::size_t pre_average=0,post_average=0,pre_maximum=1,post_maximum=1;
    SameConvolutionCrop smoothing_crop=SameConvolutionCrop::Largest;
};
// madmom classical peak_picking numerical operation for one explicit channel.
// Uses constant-zero edges, full-window mean denominators, and a maximum of
// thresholded detections. Smoothing is an explicit, unnormalized convolution
// kernel; the source's integer smoothing recipe is symmetric Hamming.
// Default one-dimensional same crop yields max(input,kernel) samples;
// explicit Input crop retains the source matrix time-axis length.
// Set post windows to zero and omit smoothing for the causal operation; its
// history is supplied explicitly, rather than hidden in this offline kernel.
class PreparedActivationPeaks {
public:
    bool prepare(std::size_t maximum_frames,ActivationPeakConfig={},
                 std::span<const float> smoothing_kernel={});
    static bool storage_bytes(std::size_t maximum_frames,ActivationPeakConfig,std::span<const float>,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return (kernel_.capacity()+smoothed_.capacity()+detected_.capacity())*sizeof(float)+result_.capacity();
    }
    std::size_t output_frames(std::size_t input_frames) const noexcept;
    bool process(std::span<const float> activations,std::span<std::uint8_t> peaks) noexcept;
private:
    void detect(std::size_t) noexcept;
    bool smooth(std::span<const float>,std::size_t) noexcept;
    float mean(std::size_t,std::size_t) const noexcept;
    float maximum(std::size_t,std::size_t) const noexcept;
    std::size_t maximum_frames_=0;
    ActivationPeakConfig config_;
    std::vector<float> kernel_,smoothed_,detected_;
    std::vector<std::uint8_t> result_;
};
enum class EventCombination {Mean,Left,Right};
// Sequence preserves supplied ordering, including a prepended last event
// followed by repeated candidates from older buffer context. Default validates sort.
enum class EventOrdering {Nondecreasing,Sequence};
// Same units for events and delta. Sorted finite Float32 times, sized output
// capacity >= input count. A cluster compares against its evolving representative
// (left/right/pairwise mean), not against the immediately preceding raw event.
bool combine_event_times(std::span<const float> events,float delta,EventCombination,
                         std::span<float> output,std::size_t& count,
                         EventOrdering=EventOrdering::Nondecreasing) noexcept;
}
