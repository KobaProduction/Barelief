#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include "numeric_graph_draft.h"
#include "barelief/ui_text.h"
#include <core_app/spectrum_projection_ui.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cmath>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include "node_field_builder.h"
namespace {
void write_history_capacity(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericLiteral& literal) {
    using namespace signal_contracts;
    const auto* capacity=std::get_if<std::uint64_t>(&literal);
    if(!capacity || !*capacity)throw std::invalid_argument("capacity type");
    std::get<domain::HistoryProcessingParameters>(node.parameters).capacity=*capacity;
}

}
void NodeParameterModel::append_accumulation_fields() {
    using namespace signal_contracts;using namespace domain;
    if (std::holds_alternative<HistoryProcessingParameters>(_node.parameters)){
        literal(
            _node.kind=="NumberAccumulator"?"History capacity, values":"History capacity, frames", "numericCapacity",
            [](const auto &n) { return NumericLiteral{std::get<HistoryProcessingParameters>(n.parameters).capacity}; },
            write_history_capacity,1);
        _fields.back().numeric_minimum=1;
        boolean("Only filled history rows","numericFilledOnly",
            [](const auto& n){return std::get<HistoryProcessingParameters>(n.parameters).filled_only;},
            [](auto& n,bool value){std::get<HistoryProcessingParameters>(n.parameters).filled_only=value;});
    }
    if (std::holds_alternative<ReduceProcessingParameters>(_node.parameters))
        choice(
            "Operation", "numericReduction", {"Maximum absolute", "Mean", "RMS"},
            [](const auto &n) { return int(std::get<ReduceProcessingParameters>(n.parameters).operation); },
            [](auto &n, int v) { std::get<ReduceProcessingParameters>(n.parameters).operation = NumericReduction(v); });
}
