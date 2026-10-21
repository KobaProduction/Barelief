#pragma once
#include "pcm_resampler.h"
#include <signal_contracts/resampling_policy.h>
#include <array>
#include <memory>

namespace dsp_kernels {
using ResamplerMode=signal_contracts::ResamplerMode;
struct ResamplerConfig {
    ResamplerMode mode=ResamplerMode::ExistingFir;
    std::uint32_t input_rate=0, output_rate=0;
    std::size_t channels=1;
    unsigned quality=5;
    bool operator==(const ResamplerConfig&) const = default;
};
struct ResamplerResources {
    // Heap payload and object bytes, excluding allocator bookkeeping. No packet staging.
    std::size_t owned_bytes=0, coefficient_bytes=0, state_bytes=0;
    std::uint32_t fixed_delay_input_samples=0;
    bool frequency_dependent_delay=false, experimental=false, alias_warning=false;
};
// A composition root supplies optional external implementations. The portable
// kernel never includes or links a desktop library. Factory lifetime must exceed
// the prepared operation (including cold copies made while preparing a graph).
class ResamplerBackend {
public:
    virtual ~ResamplerBackend()=default;
    virtual bool process(std::span<const float>,std::size_t,std::size_t,
        std::span<float>,std::size_t,std::size_t&) noexcept=0;
    virtual void reset() noexcept=0;
    virtual bool initial_position(std::uint32_t numerator,std::uint32_t denominator) noexcept {return numerator==0 && denominator!=0;}
    virtual bool copy_state_from(const ResamplerBackend&) noexcept=0;
    virtual ResamplerResources resources() const noexcept=0;
};
class ResamplerBackendFactory {
public:
    virtual ~ResamplerBackendFactory()=default;
    virtual std::unique_ptr<ResamplerBackend> prepare(const ResamplerConfig&) const=0;
    virtual bool query(const ResamplerConfig&,ResamplerResources&) const noexcept {return false;}
};
// Prepared, streaming, planar Float32 resampling. Output timestamps lie on the
// exact rational output grid; delay describes content, not a timestamp shift.
// Packet partitioning does not change samples or phase. All allocation is cold.
// Linear interpolates x(ceil(t)) and x(ceil(t)-1) at t, with zero fixed content
// delay; availability may wait less than one input sample. IIR has nonlinear
// phase, reported separately rather than inventing a constant group delay.
class PreparedResampler {
public:
    static constexpr std::uint32_t design_version=2;
    PreparedResampler()=default;
    PreparedResampler(const PreparedResampler&);
    PreparedResampler& operator=(const PreparedResampler&);
    PreparedResampler(PreparedResampler&&) noexcept=default;
    PreparedResampler& operator=(PreparedResampler&&) noexcept=default;
    bool prepare(const ResamplerConfig&,const ResamplerBackendFactory* external=nullptr);
    bool prepare(std::uint32_t in,std::uint32_t out,std::size_t channels) {
        return prepare({ResamplerMode::ExistingFir,in,out,channels,5});
    }
    static bool supported(std::uint32_t in,std::uint32_t out) noexcept {return PcmResampler::supported(in,out);}
    static bool resource_requirements(const ResamplerConfig&,const ResamplerBackendFactory*,ResamplerResources&) noexcept;
    static std::size_t maximum_output(std::size_t count,std::uint32_t in,std::uint32_t out) noexcept {
        return PcmResampler::maximum_output(count,in,out);
    }
    bool process(std::span<const float>,std::size_t,std::size_t,std::span<float>,std::size_t,std::size_t&) noexcept;
    void reset() noexcept;
    bool copy_state_from(const PreparedResampler&) noexcept;
    ResamplerResources resources() const noexcept;
    const ResamplerConfig& config() const noexcept {return config_;}
    std::uint32_t output_rate() const noexcept {return config_.output_rate;}
    bool input_contiguous(std::int64_t ticks) const noexcept {return !input_expected_ || ticks==next_input_;}
    bool advance_input(std::int64_t ticks,std::size_t count) noexcept;
    bool anchored() const noexcept {return anchored_;}
    void anchor(std::int64_t ticks) noexcept {anchor_=ticks;anchored_=true;}
    bool start_at(std::int64_t output_tick,std::uint32_t input_position_numerator) noexcept;
    std::int64_t anchor_ticks() const noexcept {return anchor_;}
    std::uint64_t produced() const noexcept {return produced_;}
private:
    struct ProcessBuffers {
        std::span<const float> input;std::size_t count,stride;
        std::span<float> output;std::size_t capacity;std::size_t& written;
    };
    bool prepare_mode(const ResamplerBackendFactory*);
    void design_iir() noexcept;
    bool process_non_fir(ProcessBuffers&) noexcept;
    bool process_fir(ProcessBuffers&) noexcept;
    bool process_backend(ProcessBuffers&) noexcept;
    bool process_identity(ProcessBuffers&) noexcept;
    bool process_interpolated(ProcessBuffers&) noexcept;
    bool filter_sample(const ProcessBuffers&,std::size_t,std::array<float,2>&) noexcept;
    float filter_iir(float,std::size_t) noexcept;
    bool emit_sample(ProcessBuffers&,const std::array<float,2>&) noexcept;
    void advance_phase() noexcept;
    bool reject_output(std::size_t&) noexcept;
    struct Biquad {float b0=0,b1=0,b2=0,a1=0,a2=0;};
    ResamplerConfig config_{};
    PcmResampler existing_;
    std::unique_ptr<ResamplerBackend> backend_;
    const ResamplerBackendFactory* factory_=nullptr;
    std::array<Biquad,3> iir_{};
    std::array<std::array<float,6>,2> iir_state_{};
    std::array<float,2> previous_{};
    std::size_t until_=0;
    std::uint32_t fraction_=0;
    std::uint32_t integer_step_=0,fractional_step_=0;
    float inverse_output_=0;
    std::uint64_t produced_=0;
    std::int64_t anchor_=0,next_input_=0;
    bool prepared_=false,anchored_=false,input_expected_=false,has_input_=false;
};
}
