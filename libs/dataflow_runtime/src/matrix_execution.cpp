#include "matrix_execution.h"
#include "frame_access.h"
#include <dsp_kernels/correlation.h>
#include <dsp_kernels/sequence_statistics.h>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dataflow_runtime::detail {
namespace {
Status copy_axis_coordinates(const PlanNode& node,const NumericView& source,std::size_t axis,std::size_t bytes,std::span<std::byte> destination) noexcept {
            const auto source_axis = node.operation == ProcessingOperation::MatrixTranspose ? 1 - axis : axis;
            if (source.coordinates.axes[source_axis].size() != bytes) return Status::incompatible_contract;
            std::copy_n(source.coordinates.axes[source_axis].begin(), bytes, destination.begin());
    return Status::ok;
}
Status coordinates(const PlanNode& node, State& state, const NumericView& source,
                   MutableNumericView& output, Ratio,std::size_t port=0) noexcept {
    for (std::size_t axis = 0; axis < 2; ++axis) {
        if (output.contract->coordinates[axis].mode != CoordinateMode::ExplicitFloat64LE) continue;
        auto& destination = state.numeric_outputs[port].coordinates[axis];
        const auto bytes = output.actual[axis] * sizeof(double);
        const auto status=copy_axis_coordinates(node,source,axis,bytes,destination);
        if(status!=Status::ok)return status;
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

Status window_element(const NumericView& source,MutableNumericView& output,std::size_t row,std::size_t column,float coefficient) noexcept {
            float value = 0;
            auto status = load_element(source, {0, row, column}, value);
            if (status != Status::ok) return status;
            if (!std::isfinite(value)) return Status::invalid_argument;
            const auto windowed = value * coefficient;
            if (!std::isfinite(windowed)) return Status::overflow;
            status = store_element(output, {0, row, column}, windowed);
            if (status != Status::ok) return status;
    return Status::ok;
}
Status window_rows(const PlanNode& node, State& state, const NumericView& source,
                   MutableNumericView& output) noexcept {
    std::span<const float> coefficients;
    auto status = window_coefficients(node, state, source.actual[1], coefficients);
    if (status != Status::ok) return status;
    for (std::size_t row = 0; row < source.actual[0]; ++row) {
        for (std::size_t column = 0; column < source.actual[1]; ++column) {
            status=window_element(source,output,row,column,coefficients[column]);if(status!=Status::ok)return status;
        }
    }
    return Status::ok;
}

Status measured_row_center(const PlanNode& node,const NumericView& source,std::size_t row,float& center) noexcept {
    float sum=0,correction=0,minimum=std::numeric_limits<float>::max(),maximum=-minimum;
    for (std::size_t column = 0; column < source.actual[1]; ++column) {
        float value = 0;
        const auto status = load_element(source, {0, row, column}, value);
        if (status != Status::ok) return status;
        if (!std::isfinite(value)) return Status::invalid_argument;
        if(node.center_method==0){if(!dsp_kernels::accumulate_scaled_mean_float32(value,float(source.actual[1]),sum,correction))return Status::overflow;}
        else {minimum=std::min(minimum,value);maximum=std::max(maximum,value);}
    }
    center=node.center_method==0?sum:std::midpoint(minimum,maximum);
    return std::isfinite(center) ? Status::ok : Status::overflow;
}
Status row_center(const PlanNode& node, const NumericView& source, std::size_t row, float& center) noexcept {
    if (node.center_method == 2) { center = node.center_offset; return Status::ok; }
    if (node.center_method > 2) return Status::invalid_argument;
    if (!source.actual[1]) { center = 0; return Status::ok; }
    return measured_row_center(node,source,row,center);
}

Status center_element(const NumericView& source,MutableNumericView& output,std::size_t row,std::size_t column,float center) noexcept {
            float value = 0;
            auto status = load_element(source, {0, row, column}, value);
            if (status != Status::ok) return status;
            if (!std::isfinite(value)) return Status::invalid_argument;
            const float centered=value-center;
            if (!std::isfinite(centered) || std::abs(centered) > std::numeric_limits<float>::max()) return Status::overflow;
            status = store_element(output, {0, row, column}, float(centered));
            if (status != Status::ok) return status;
    return Status::ok;
}
Status center_rows(const PlanNode& node, const NumericView& source, MutableNumericView& output) noexcept {
    for (std::size_t row = 0; row < source.actual[0]; ++row) {
        float center = 0;
        auto status = row_center(node, source, row, center);
        if (status != Status::ok) return status;
        for (std::size_t column = 0; column < source.actual[1]; ++column) {
            status=center_element(source,output,row,column,center);if(status!=Status::ok)return status;
        }
    }
    return Status::ok;
}

Status load_fft_row(State& state,const NumericView& source,std::size_t row,std::size_t first,std::size_t used) noexcept {
        std::fill(state.left.begin(), state.left.end(), 0.f);
        for (std::size_t column = 0; column < used; ++column) {
            float value = 0;
            const auto status = load_element(source, {0, row, first + column}, value);
            if (status != Status::ok) return status;
            if (!std::isfinite(value)) return Status::invalid_argument;
            state.left[column] = value;
        }
    return Status::ok;
}
Status store_fft_row(std::span<const float> values,std::size_t row,MutableNumericView& output) noexcept {
    for(std::size_t bin=0;bin<values.size();++bin) {
        const auto status=store_element(output,{0,row,bin},values[bin]);
        if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status store_complex_row(std::span<const float> values,std::size_t row,MutableNumericView& real,MutableNumericView& imaginary) noexcept {
    for(std::size_t bin=0;bin<values.size()/2;++bin){
        auto status=store_element(real,{0,row,bin},values[2*bin]);if(status!=Status::ok)return status;
        status=store_element(imaginary,{0,row,bin},values[2*bin+1]);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
Status fft_rows(State& state,const NumericView& source,MutableNumericView& real,MutableNumericView& imaginary) noexcept {
    const auto used=std::min(source.actual[1],state.left.size()),first=source.actual[1]-used;
    auto& fft=std::get<dsp_kernels::PreparedRfft>(state.kernel);
    for(std::size_t row=0;row<source.actual[0];++row){
        const auto loaded=load_fft_row(state,source,row,first,used);if(loaded!=Status::ok)return loaded;
        if(!fft.process(state.left,state.values))return Status::overflow;
        const auto stored=store_complex_row(state.values,row,real,imaginary);if(stored!=Status::ok)return stored;
    }
    return Status::ok;
}
Status correlation_rows(State& state,const NumericView& source,MutableNumericView& output) noexcept {
    for(std::size_t row=0;row<source.actual[0];++row){
        const auto loaded=load_fft_row(state,source,row,0,source.actual[1]);
        if(loaded!=Status::ok)return loaded;
        auto values=std::span(state.values).first(output.actual[1]);
        if(!dsp_kernels::linear_autocorrelation(std::span(state.left).first(source.actual[1]),values))return Status::invalid_argument;
        const auto stored=store_fft_row(values,row,output);if(stored!=Status::ok)return stored;
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
        case ProcessingOperation::MatrixAutocorrelation: return correlation_rows(state, source, output);
        default: return Status::invalid_argument;
    }
}
Status silence_fft(MutableNumericView& magnitude,MutableNumericView& phase,const FrameLimits& limits) noexcept {
    const auto status=fill_silence(magnitude,limits);
    return status==Status::ok?fill_silence(phase,limits):status;
}
Status render_fft(const PlanNode& node,State& state,const DataFrameView& current,const NumericView& source,
                  MutableNumericView& output,const FrameLimits& limits,Ratio interval) noexcept {
    auto phase=mutable_output(node,state,1);phase.actual=output.actual;
    auto status=coordinates(node,state,source,phase,interval,1);
    if(status!=Status::ok)return status;
    status=node.node.enabled?fft_rows(state,source,output,phase):silence_fft(output,phase,limits);
    if(status==Status::ok){
        state.numeric_frames[0]={current.stamp,output.view()};
        state.numeric_frames[1]={current.stamp,phase.view()};
    }
    return status;
}
Status render_matrix(const PlanNode& node,State& state,const DataFrameView& current,const NumericView& source,MutableNumericView& output,const FrameLimits& limits,Ratio execution_interval) noexcept {
    auto status = coordinates(node, state, source, output, execution_interval);
    if (status != Status::ok) return status;
    if(node.operation==ProcessingOperation::MatrixFFT)return render_fft(node,state,current,source,output,limits,execution_interval);
    if (!node.node.enabled) {
        // A compatible ring bypass preserves logical values and coordinates,
        // but publishes an ordinary matrix owned by this transforming node.
        status = node.identity_bypass ? copy_values(source, output, false) : fill_silence(output, limits);
    }
    else status=matrix_operation(node,state,source,output);
    if (status == Status::ok) state.numeric_frames[0] = {current.stamp, output.view()};
    return status;
}
}
Status execute_matrix(const PlanNode& node, State& state, const DataFrameView& current,
                      const FrameLimits& limits, Ratio execution_interval) noexcept {
    const auto& source = std::get<NumericView>(current.payload);
    auto output = mutable_output(node, state, 0);
    output.actual = source.actual;
    if (node.operation == ProcessingOperation::MatrixTranspose) std::swap(output.actual[0], output.actual[1]);
    if (node.operation == ProcessingOperation::MatrixFFT) output.actual[1] = state.values.size()/2;
    if (node.operation == ProcessingOperation::MatrixAutocorrelation) output.actual[1] = std::min(source.actual[1],state.values.size());

    return render_matrix(node,state,current,source,output,limits,execution_interval);
}
} // namespace dataflow_runtime::detail
