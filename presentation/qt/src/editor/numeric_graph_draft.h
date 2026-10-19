#pragma once
#include <domain/processing_contracts.h>
#include <functional>
#include <string_view>
namespace numeric_editor {
// Draft transformations only. Admission remains the canonical domain resolver;
// validation supplied by the caller includes its selected backend capabilities.
bool draft_view(std::string_view);
bool draft_view_accepts(std::string_view,const signal_contracts::NumericContract&);
bool set_sample_rate(domain::ConfiguredProcessingNode&,std::uint32_t);
bool set_contract(domain::ConfiguredProcessingNode&,domain::PortDirection,domain::PortId,const signal_contracts::DataContract&);
using NodeCheck=std::function<bool(const domain::ConfiguredProcessingNode&)>;
domain::ProcessingAdmissionLimits limits();
bool clock_node(domain::ConfiguredProcessingNode&,const signal_contracts::ClockContract&,const signal_contracts::ExtentBounds* = nullptr);
bool bind_target_clock(domain::ConfiguredProcessingNode&,const signal_contracts::ClockContract&);
bool adopt_extract_input(domain::ConfiguredProcessingNode&,const signal_contracts::NumericContract&);
bool adopt_matrix_input(domain::ConfiguredProcessingNode&,const signal_contracts::NumericContract&);
bool matrix_operation(std::string_view);
bool adopt_history_input(domain::ConfiguredProcessingNode&,const signal_contracts::NumericContract&);
bool adapt_mel_parameters(domain::ConfiguredProcessingNode&);
bool music_operation(std::string_view);
bool adopt_music_input(domain::ConfiguredProcessingNode&,const signal_contracts::NumericContract&);
bool adopt_sample_input(domain::ConfiguredProcessingNode&,const signal_contracts::NumericContract&);
bool propagate_clock(domain::NumericProcessingGraph&,domain::NodeId,const NodeCheck&);
bool adapt_connection_clock(const domain::Edge&,domain::NumericProcessingGraph&,const NodeCheck&);
}
