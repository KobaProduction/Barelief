#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class CurveInterpolation {Linear,Nearest,NearestUp,Zero,Previous,Next,Spline,Quadratic,Cubic};
struct CurveInterpolationConfig {
    CurveInterpolation kind=CurveInterpolation::Linear;
    std::size_t spline_order=1;
    float fill_below=0,fill_above=0;
    bool extrapolate=false,bounds_error=false;
    bool keep_infinite_coordinates=false,source_linear=false;
};
// Numerical interpolation used by tempogram ratio, metrogram and hybrid
// tempograms. Supports interp1d's point/step/linear modes and not-a-knot
// B-splines, including explicit nonnegative integer spline orders.
// Coordinates may ascend or descend; nonfinite coordinates (ACF lag zero)
// are excluded. Finite coordinates must be distinct. Values keep the original
// coordinate-array order. Preparation factors the banded collocation matrix;
// subsequent rows and samples use Float32 without allocation.
// Sampling preserves SciPy-style nonfinite numerical results, including the
// undefined side of Previous/Next extrapolation. Infinite queries use the
// configured bounds/extrapolation; callers select their own NaN/Inf policy.
// Per-instance sampling scratch makes calls serialized, including sample().
class PreparedCurveInterpolation {
public:
    bool prepare(std::span<const float> coordinates,CurveInterpolationConfig={});
    // Maximum input coordinate count, including excluded nonfinite entries.
    // Dynamic owned payload only; no allocation or geometry factorization.
    static bool storage_bytes(std::size_t coordinates,CurveInterpolationConfig,std::size_t& bytes) noexcept;
    // Cold peak bound for prepare_dynamic, including its temporary grid.
    static bool dynamic_storage_bytes(std::size_t coordinates,CurveInterpolationConfig,std::size_t& bytes) noexcept;
    bool prepare_dynamic(std::size_t coordinates,CurveInterpolationConfig={});
    // Rebind a same-width dynamic grid inside prepared capacity. No allocation;
    // failure invalidates the geometry until the next successful rebind.
    // Input must not alias the borrowed coordinates() view.
    bool set_coordinates(std::span<const float>) noexcept;
    bool set_values(std::span<const float> values) noexcept;
    bool sample(float query,float& value) const noexcept;
    std::span<const float> coordinates() const noexcept {return x_;}
    std::span<const float> values() const noexcept {return ready_?std::span<const float>(values_):std::span<const float>{};}
    std::size_t workspace_bytes() const noexcept {
        return indices_.capacity()*sizeof(std::size_t)+
            (x_.capacity()+knots_.capacity()+lu_.capacity()+values_.capacity()+coefficients_.capacity()+scratch_.capacity()+
             basis_.capacity()+left_.capacity()+right_.capacity())*sizeof(float);
    }
private:
    bool configure(std::span<const float>,CurveInterpolationConfig);
    bool coordinates(std::span<const float>) noexcept;
    bool resize_geometry() noexcept;
    bool allocate();
    void knots();
    bool factor();
    bool eliminate(std::size_t pivot) noexcept;
    bool solve() noexcept;
    std::size_t interval(float) const noexcept;
    void basis(float,std::size_t) const noexcept;
    float& matrix(std::size_t row,std::size_t column) noexcept;
    float matrix(std::size_t row,std::size_t column) const noexcept;
    float interpolate(float) const noexcept;
    float nearest(float,std::size_t upper) const noexcept;
    float linear(float,std::size_t upper) const noexcept;
    bool outside(float,float&) const noexcept;
    CurveInterpolationConfig config_;
    std::size_t input_count_=0,degree_=0,width_=0;
    bool spline_=false,ready_=false,geometry_ready_=false;
    std::vector<std::size_t> indices_;
    std::vector<float> x_,knots_,lu_,values_,coefficients_,scratch_;
    mutable std::vector<float> basis_,left_,right_;
};
}
