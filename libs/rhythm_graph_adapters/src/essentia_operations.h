// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "batch_operation_plan.h"
#include "frame_operation_io.h"
#include <rhythm_reference_ports/beat_agreement.h>
#include <rhythm_reference_ports/erb_filter_bank.h>
#include <rhythm_reference_ports/global_onset.h>
#include <rhythm_reference_ports/spectral_onset.h>

namespace rhythm_graph_adapters::detail {
enum class EssentiaKind {Onset,Information,Novelty,Erb,Agreement,CandidateInputs};
struct EssentiaPlan {
    EssentiaKind kind{};
    std::size_t rows=0,bins=1025,inputs=1,outputs=1,method=1;
    float minimum_time=5,frame_rate=44100.f/1024.f;
    std::array<BatchShape,5> shapes{};
    std::array<std::size_t,5> counts{};
};
bool essentia_operation_kind(std::string_view) noexcept;
bool essentia_frequency_contract(const signal_contracts::NumericContract&) noexcept;
bool essentia_frequency_input(const dataflow_runtime::NumericOperationInput&) noexcept;
bool essentia_operation_plan(const domain::ResolvedProcessingNode&,EssentiaPlan&) noexcept;
bool essentia_operation_storage(const EssentiaPlan&,std::size_t&) noexcept;
std::size_t essentia_operation_object_bytes(EssentiaKind) noexcept;
dataflow_runtime::NumericOperationPolicy essentia_operation_policy(const domain::ResolvedProcessingNode&);
domain::ProcessingSupport assess_essentia_operation(const domain::ResolvedProcessingNode&,
    const dataflow_runtime::NumericRuntimeProfile&) noexcept;
std::unique_ptr<AdapterOperation> prepare_essentia_operation(const domain::ConfiguredProcessingNode&);
}
