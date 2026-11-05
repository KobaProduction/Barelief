#pragma once
#include <cstddef>
#include <span>
#include <vector>
#include <dsp_kernels/numerical_precision.h>
namespace dsp_kernels {
enum class CombDirection {Forward,Backward};
// Explicit-channel comb bank, source madmom audio/comb_filters.pyx.
// PortableFloat32 is the default; DesktopReference retains Double history and
// values with the original Cython Float32 alpha promoted in the recurrence.
// Delays are positive sample counts. One alpha broadcasts, otherwise one per
// filter. Output layout [channel][filter]. History is prepared once; process()
// consumes exactly one multichannel sample. Forward stores x, backward y.
class PreparedCombFilterBank {
public:
    static bool storage_bytes(std::span<const std::size_t>,std::span<const float>,CombDirection,std::size_t channels,std::size_t&) noexcept;
    static bool storage_bytes(std::span<const std::size_t>,std::span<const float>,CombDirection,std::size_t,NumericalPrecision,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t filters,std::size_t maximum_delay,CombDirection,std::size_t channels,std::size_t&) noexcept;
    static bool storage_bytes(std::size_t,std::size_t,CombDirection,std::size_t,NumericalPrecision,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return delays_.capacity()*sizeof(std::size_t)+(alpha_.capacity()+history_.capacity()+values_.capacity())*sizeof(float)+(reference_history_.capacity()+reference_values_.capacity())*sizeof(double);}
    bool prepare(std::span<const std::size_t> delays,std::span<const float> alpha,
                 CombDirection=CombDirection::Backward,std::size_t channels=1,NumericalPrecision=NumericalPrecision::PortableFloat32);
    void reset() noexcept;
    bool process(std::span<const float> input,std::span<float> output) noexcept;
    bool process(std::span<const double> input,std::span<double> output) noexcept;
    std::size_t channels() const noexcept {return channels_;}
    std::size_t filters() const noexcept {return delays_.size();}
private:
    bool configure(std::span<const std::size_t>,std::span<const float>,CombDirection,std::size_t,NumericalPrecision);
    void allocate(std::span<const float> alpha);
    template<typename Scalar> Scalar delayed(std::size_t,std::size_t,std::span<const Scalar>,std::size_t) const noexcept;
    template<typename Scalar> void advance(std::span<const Scalar>,std::span<const Scalar>,std::span<Scalar>) noexcept;
    template<typename Scalar> bool respond(std::span<const Scalar>,std::span<Scalar>,std::span<const Scalar>) noexcept;
    template<typename Scalar> bool evaluate(std::span<const Scalar>,std::span<Scalar>,std::span<Scalar>,std::span<Scalar>) noexcept;
    std::size_t channels_=0,capacity_=0,cursor_=0;
    CombDirection direction_=CombDirection::Backward;
    std::vector<std::size_t> delays_;
    std::vector<float> alpha_,history_,values_;
    std::vector<double> reference_history_,reference_values_;
    NumericalPrecision precision_=NumericalPrecision::PortableFloat32;
};
}
