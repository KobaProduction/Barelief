#include "matrix_execution.h"
#include "frame_access.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dataflow_runtime::detail {
namespace {
void write_coordinate(std::span<std::byte> bytes, std::size_t index, double value) noexcept {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t byte = 0; byte < sizeof(bits); ++byte)
        bytes[index * sizeof(bits) + byte] = std::byte((bits >> (byte * 8)) & 255);
}

Status frequency_coordinates(std::span<std::byte> destination, std::size_t bins,
                             std::size_t samples, Ratio interval) noexcept {
    const auto cadence = double(interval.denominator) / double(interval.numerator);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        const auto hz = double(bin) * cadence / double(samples);
        if (!std::isfinite(hz)) return Status::overflow;
        write_coordinate(destination, bin, hz);
    }
    return Status::ok;
}

Status coordinates(const PlanNode& node, State& state, const NumericView& source,
                   MutableNumericView& output, Ratio interval) noexcept {
    for (std::size_t axis = 0; axis < 2; ++axis) {
        if (output.contract->coordinates[axis].mode != CoordinateMode::ExplicitFloat64LE) continue;
        auto& destination = state.numeric_outputs[0].coordinates[axis];
        const auto bytes = output.actual[axis] * sizeof(double);
        if (node.operation == ProcessingOperation::MatrixFFT && axis == 1) {
            const auto status = frequency_coordinates(destination, output.actual[axis], state.left.size(), interval);
            if (status != Status::ok) return status;
        } else {
            const auto source_axis = node.operation == ProcessingOperation::MatrixTranspose ? 1 - axis : axis;
            if (source.coordinates.axes[source_axis].size() != bytes) return Status::incompatible_contract;
            std::copy_n(source.coordinates.axes[source_axis].begin(), bytes, destination.begin());
        }
        output.coordinates.axes[axis] = {destination.data(), bytes};
    }
    return Status::ok;
}

Status copy_values(const NumericView& source, MutableNumericView& output, bool transpose) noexcept {
    for (std::size_t row = 0; row < source.actual[0]; ++row) {
        for (std::size_t column = 0; column < source.actual[1]; ++column) {
            std::uint64_t bits = 0;
            auto status = load_bits(source, {0, row, column}, source.contract->dtype, bits);
            if (status != Status::ok) return status;
            status = store_bits(output, {0, transpose ? column : row, transpose ? row : column}, output.contract->dtype, bits);
            if (status != Status::ok) return status;
        }
    }
    return Status::ok;
}

Status window_coefficients(const PlanNode& node, State& state, std::size_t columns,
                           std::span<const float>& coefficients) noexcept {
    if (!node.windows || !node.windows->prepared() || columns > state.coefficients.size())
        return Status::incompatible_contract;
    coefficients = node.windows->coefficients(node.window_function);
    if (columns == coefficients.size()) return Status::ok;

    auto variable = std::span(state.coefficients).first(columns);
    if (state.window_extent != columns) {
        if (!node.windows->coefficients(node.window_function, variable)) return Status::incompatible_contract;
        state.window_extent = columns;
    }
    coefficients = variable;
    return Status::ok;
}

Status window_rows(const PlanNode& node, State& state, const NumericView& source,
                   MutableNumericView& output) noexcept {
    std::span<const float> coefficients;
    auto status = window_coefficients(node, state, source.actual[1], coefficients);
    if (status != Status::ok) return status;
    for (std::size_t row = 0; row < source.actual[0]; ++row) {
        for (std::size_t column = 0; column < source.actual[1]; ++column) {
            float value = 0;
            status = load_element(source, {0, row, column}, value);
            if (status != Status::ok) return status;
            if (!std::isfinite(value)) return Status::invalid_argument;
            const auto windowed = value * coefficients[column];
            if (!std::isfinite(windowed)) return Status::overflow;
            status = store_element(output, {0, row, column}, windowed);
            if (status != Status::ok) return status;
        }
    }
    return Status::ok;
}

Status row_center(const PlanNode& node, const NumericView& source, std::size_t row, double& center) noexcept {
    if (node.center_method == 2) { center = node.center_offset; return Status::ok; }
    if (node.center_method > 2) return Status::invalid_argument;
    if (!source.actual[1]) { center = 0; return Status::ok; }
    double sum = 0, minimum = std::numeric_limits<float>::max(), maximum = -minimum;
    for (std::size_t column = 0; column < source.actual[1]; ++column) {
        float value = 0;
        const auto status = load_element(source, {0, row, column}, value);
        if (status != Status::ok) return status;
        if (!std::isfinite(value)) return Status::invalid_argument;
        if (node.center_method == 0) sum += double(value);
        else { minimum = std::min(minimum, double(value)); maximum = std::max(maximum, double(value)); }
    }
    // Float32 values are accumulated and averaged in double, including the
    // midpoint: two equal FLT_MAX inputs must center to zero without overflow.
    center = node.center_method == 0 ? sum / double(source.actual[1]) : (minimum + maximum) * .5;
    return std::isfinite(center) ? Status::ok : Status::overflow;
}

Status center_rows(const PlanNode& node, const NumericView& source, MutableNumericView& output) noexcept {
    for (std::size_t row = 0; row < source.actual[0]; ++row) {
        double center = 0;
        auto status = row_center(node, source, row, center);
        if (status != Status::ok) return status;
        for (std::size_t column = 0; column < source.actual[1]; ++column) {
            float value = 0;
            status = load_element(source, {0, row, column}, value);
            if (status != Status::ok) return status;
            if (!std::isfinite(value)) return Status::invalid_argument;
            const auto centered = double(value) - center;
            if (!std::isfinite(centered) || std::abs(centered) > std::numeric_limits<float>::max()) return Status::overflow;
            status = store_element(output, {0, row, column}, float(centered));
            if (status != Status::ok) return status;
        }
    }
    return Status::ok;
}

Status fft_rows(State& state, const NumericView& source, MutableNumericView& output) noexcept {
    const auto used = std::min(source.actual[1], state.left.size());
    const auto first = source.actual[1] - used;
    auto& fft = std::get<dsp_kernels::MagnitudeSpectrum>(state.kernel);
    for (std::size_t row = 0; row < source.actual[0]; ++row) {
        std::fill(state.left.begin(), state.left.end(), 0.f);
        for (std::size_t column = 0; column < used; ++column) {
            float value = 0;
            const auto status = load_element(source, {0, row, first + column}, value);
            if (status != Status::ok) return status;
            if (!std::isfinite(value)) return Status::invalid_argument;
            state.left[column] = value;
        }
        if (!fft.process(state.left, state.values)) return Status::overflow;
        for (std::size_t bin = 0; bin < state.values.size(); ++bin) {
            const auto status = store_element(output, {0, row, bin}, state.values[bin]);
            if (status != Status::ok) return status;
        }
    }
    return Status::ok;
}
} // namespace

namespace {
Status matrix_operation(const PlanNode& node,State& state,const NumericView& source,MutableNumericView& output) noexcept {
    switch (node.operation) {
        case ProcessingOperation::MatrixTranspose: return copy_values(source, output, true);
        case ProcessingOperation::MatrixWindowFunction: return window_rows(node, state, source, output);
        case ProcessingOperation::MatrixCenter: return center_rows(node, source, output);
        case ProcessingOperation::MatrixFFT: return fft_rows(state, source, output);
        default: return Status::invalid_argument;
    }
}
}
Status execute_matrix(const PlanNode& node, State& state, const DataFrameView& current,
                      const FrameLimits& limits, Ratio execution_interval) noexcept {
    const auto& source = std::get<NumericView>(current.payload);
    auto output = mutable_output(node, state, 0);
    output.actual = source.actual;
    if (node.operation == ProcessingOperation::MatrixTranspose) std::swap(output.actual[0], output.actual[1]);
    if (node.operation == ProcessingOperation::MatrixFFT) output.actual[1] = state.values.size();

    auto status = coordinates(node, state, source, output, execution_interval);
    if (status != Status::ok) return status;
    if (!node.node.enabled) {
        // A compatible ring bypass preserves logical values and coordinates,
        // but publishes an ordinary matrix owned by this transforming node.
        status = node.identity_bypass ? copy_values(source, output, false) : fill_silence(output, limits);
    }
    else status=matrix_operation(node,state,source,output);
    if (status == Status::ok) state.numeric_frames[0] = {current.stamp, output.view()};
    return status;
}
} // namespace dataflow_runtime::detail
