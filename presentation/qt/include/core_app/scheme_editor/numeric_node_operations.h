#pragma once
#include <domain/processing_contracts.h>
#include <QStringList>
namespace numeric_editor {
domain::ProcessingAdmissionLimits limits();
domain::ConfiguredProcessingNode default_node(const QString &kind);
QStringList catalog();
QString category(const QString &kind);
QString direction_group(const QString &kind);
bool is_graph_view(const QString &kind);
bool view_accepts(const QString&,const signal_contracts::NumericContract&);
QString port_caption(const domain::ProcessingPort &,bool include_extents=true);
QString contract_details(const domain::ProcessingPort &);
QString conversion_effects(signal_contracts::DType, signal_contracts::DType);
QString conversion_policy_caption(std::uint8_t);
QString conversion_policy_details(std::uint8_t);
QString dtype_caption(signal_contracts::DType);
int capture_source_kind(const domain::ConfiguredProcessingNode &);
bool set_sample_rate(domain::ConfiguredProcessingNode &, std::uint32_t);
// These are draft edits; only runtime admission authorizes execution.
bool set_dtype(domain::ConfiguredProcessingNode &, domain::PortDirection, domain::PortId, signal_contracts::DType);
bool set_contract(domain::ConfiguredProcessingNode &, domain::PortDirection, domain::PortId,
                  const signal_contracts::DataContract &);
bool add_input(domain::ConfiguredProcessingNode &);
bool remove_input(domain::ConfiguredProcessingNode &, domain::PortId);
bool set_input_count(domain::ConfiguredProcessingNode &, std::uint16_t);
bool set_output_count(domain::ConfiguredProcessingNode &, std::uint16_t);
} // namespace numeric_editor
