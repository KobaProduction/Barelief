#pragma once
#include <domain/graph/processing_model.h>
namespace rhythm_graph_adapters {
enum class SpectralRowsInput:domain::PortId {Magnitude=0,Phase=1};
enum class OnsetRowsOutput:domain::PortId {Curve=0,SampleTimes=1,Intermediate=2};
enum class CandidateCurve:domain::PortId {Complex=0,Rms=1,MelFlux=2,Emphasis=3,Information=4};
enum class HarmonicCombInput:domain::PortId {Correlation=0,Weights=1};
enum class PeriodPathInput:domain::PortId {Observations=0};
enum class PeriodPathOutput:domain::PortId {Periods=0,Ends=1,NoisyObservations=2,StatePath=3};
enum class SummaryInput:domain::PortId {Ticks=0,Confidence=1};
enum class AgreementOutput:domain::PortId {Ticks=0,Confidence=1,Selected=2,Scores=3,Pairwise=4};
enum class RenewalInput:domain::PortId {OnsetCurve=0,Periods=1,Ends=2};
enum class RenewalOutput:domain::PortId {Mask=0,StatePath=1,Times=2};
}
