// SPDX-License-Identifier: AGPL-3.0-or-later
#include "essentia_operations.h"
#include <bit>
namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
bool frequency_unit(NumericUnit unit) noexcept {
    return unit==NumericUnit::Hertz || unit==NumericUnit::Values || unit==NumericUnit::Frames;
}
double step(const AxisCoordinates& axis) noexcept {return axis.unit==NumericUnit::Hertz?44100./2048:1.;}
double coordinate(std::span<const std::byte> bytes,std::size_t index) noexcept {
    std::uint64_t bits=0;
    for(std::size_t i=0;i<8;++i)bits|=std::uint64_t(std::to_integer<unsigned char>(bytes[8*index+i]))<<(8*i);
    return std::bit_cast<double>(bits);
}
}
bool essentia_frequency_contract(const NumericContract& c) noexcept {
    if(c.clock.sample_rate && c.clock.sample_rate!=44100)return false;
    const auto& axis=c.coordinates[1];
    if(axis.mode==CoordinateMode::None)return true;
    if(!frequency_unit(axis.unit))return false;
    if(axis.mode==CoordinateMode::ExplicitFloat64LE)return true;
    return axis.mode==CoordinateMode::Regular && axis.origin.denominator && axis.step.denominator &&
        !axis.origin.numerator && double(axis.step.numerator)/double(axis.step.denominator)==step(axis);
}
bool essentia_frequency_input(const dataflow_runtime::NumericOperationInput& input) noexcept {
    if(!input.value.contract || !essentia_frequency_contract(*input.value.contract))return false;
    const auto& axis=input.value.contract->coordinates[1];
    if(axis.mode!=CoordinateMode::ExplicitFloat64LE)return true;
    const auto bytes=input.value.coordinates.axes[1];
    if(bytes.size()!=1025*8 || !bytes.data())return false;
    for(std::size_t i=0;i<1025;++i)if(coordinate(bytes,i)!=double(i)*step(axis))return false;
    return true;
}
}
