// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <domain/graph/processing_model.h>
namespace rhythm_graph_adapters::btrack_ports {
enum class OnsetInput:domain::PortId {Real=0,Imaginary=1};
enum class HistoryOutput:domain::PortId {NewestOnset=0,OnsetWindow=1};
enum class DecisionInput:domain::PortId {Onset=0,TempoObservations=1};
enum class DecisionOutput:domain::PortId {BeatFlag=0,TempoBpm=1,PeriodFrames=2,CumulativeScore=3,CumulativeHistory=4,NextBeat=5,NextPrediction=6,TempoProbabilities=7};
inline constexpr domain::PortId values_input=0,values_output=0;
constexpr domain::PortId port(OnsetInput value) noexcept {return domain::PortId(value);}
constexpr domain::PortId port(HistoryOutput value) noexcept {return domain::PortId(value);}
constexpr domain::PortId port(DecisionInput value) noexcept {return domain::PortId(value);}
constexpr domain::PortId port(DecisionOutput value) noexcept {return domain::PortId(value);}
}
