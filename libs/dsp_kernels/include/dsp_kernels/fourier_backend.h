#pragma once
#include <cstddef>
#include <memory>
#include <span>

namespace dsp_kernels {
enum class FourierBackendTransform {RealForward=0,RealInverse=1,ComplexForward=2,ComplexInverse=3};
struct FourierBackendRequest {
    std::size_t samples=0;
    FourierBackendTransform transform=FourierBackendTransform::RealForward;
    bool operator==(const FourierBackendRequest&) const = default;
};
struct FourierBackendResources {
    // Dynamic payload including the heap-allocated backend object, aligned
    // plans and all staging. The inline owner and allocator bookkeeping are excluded.
    std::size_t owned_bytes=0,cold_peak_bytes=0,shared_bytes=0,hot_stack_bytes=0;
};
// Forward transforms are unnormalized. Inverses include exactly one 1/N.
// Real transforms require even N: forward N -> N+2 interleaved values;
// inverse N+2 -> N real values. Complex transforms use 2*N interleaved values.
// Real inverse DC/Nyquist imaginary components must be zero (either sign).
bool fourier_backend_sizes(FourierBackendRequest,std::size_t& input,std::size_t& output) noexcept;
// Providers validate the entire request before publishing any caller output.
// Spans must be finite, correctly sized and disjoint. No input is retained.
bool fourier_backend_call_valid(FourierBackendRequest,std::span<const float>,std::span<float>) noexcept;
class FourierBackend {
public:
    virtual ~FourierBackend()=default;
    virtual bool process(std::span<const float>,std::span<float>) noexcept=0;
    virtual FourierBackendResources resources() const noexcept=0;
    // Cold, independent copy of the owned plan; no borrowed factory or shared
    // mutable scratch. Raw transforms have no semantic inter-call history.
    virtual std::unique_ptr<FourierBackend> clone() const=0;
};
class FourierBackendFactory {
public:
    virtual ~FourierBackendFactory()=default;
    virtual const char* id() const noexcept=0;
    // No allocation. Cold peak excludes a previously retained owner, which the
    // runtime charges separately. Shared/stack storage is reported separately.
    virtual bool query(FourierBackendRequest,FourierBackendResources&) const noexcept=0;
    virtual std::unique_ptr<FourierBackend> prepare(FourierBackendRequest) const=0;
};
// Optional numerical adapter owner. The factory is borrowed only while calling
// query/prepare; cold copies use the owned backend's clone(). Moves are cheap.
class PreparedFourierBackend {
public:
    PreparedFourierBackend()=default;
    PreparedFourierBackend(const PreparedFourierBackend&);
    PreparedFourierBackend& operator=(const PreparedFourierBackend&);
    PreparedFourierBackend(PreparedFourierBackend&&) noexcept=default;
    PreparedFourierBackend& operator=(PreparedFourierBackend&&) noexcept=default;
    // Preparation invalidates any previous owner before querying. Processing
    // failures leave caller output unchanged; cold copy assignment is strong.
    bool prepare(FourierBackendRequest,const FourierBackendFactory&);
    static bool query(FourierBackendRequest,const FourierBackendFactory&,FourierBackendResources&) noexcept;
    bool process(std::span<const float>,std::span<float>) noexcept;
    bool ready() const noexcept {return bool(backend_);}
    FourierBackendResources resources() const noexcept;
    std::size_t workspace_bytes() const noexcept {return resources().owned_bytes;}
private:
    std::unique_ptr<FourierBackend> backend_;
};
}
