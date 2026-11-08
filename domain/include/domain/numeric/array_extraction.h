#pragma once
#include <domain/graph/processing_model.h>
#include <domain/numeric/array_processing_ports.h>
#include <span>

namespace domain {
// Stable selectors. Element preserves IndexExtract's original scalar contract.
enum class ExtractionMode : std::uint8_t { Element=0, Axis=1 };
struct AxisExtraction {
    std::size_t axis=0,first=0,length=0,before=0,after=0,output_length=0;
    ArrayPadding padding=ArrayPadding::Constant;
    signal_contracts::NumericLiteral constant{0.0};
    std::span<const float> indices;
};
bool extraction_mode(const ResolvedProcessingNode&,ExtractionMode&) noexcept;
bool axis_extraction(const ResolvedProcessingNode&,const signal_contracts::NumericContract&,AxisExtraction&) noexcept;
std::size_t extraction_selected(const AxisExtraction&,std::size_t actual) noexcept;
std::size_t extraction_length(const AxisExtraction&,std::size_t actual) noexcept;
signal_contracts::Status extraction_output_contract(const ConfiguredProcessingNode&,const signal_contracts::NumericContract&,
    signal_contracts::NumericContract&) noexcept;
}
