#include "numeric_operation_lifecycle_fixture.h"
#include <dsp_kernels/recursive_filter.h>
#include <rhythm_reference_ports/contrast_peak_picker.h>
#include <rhythm_reference_ports/polar_novelty.h>

namespace {
using namespace operation_lifecycle;
void explicit_eof_and_reset() {
    Running run(contrast_node());rhythm_reference_ports::PreparedContrastPeakAccumulator reference;
    const rhythm_reference_ports::ContrastPeakPickerConfig settings{100,.05f,0,10,40,20};
    require(reference.prepare(32,32,settings),"prepare existing accumulator oracle");
    const std::array<float,8> first{0,0,1,0,0,0,1,0};
    const std::array<float,3> short_chunk{0,1,0};
    require(reference.consume(first),"consume first source chunk");run.push(1,std::span<const float>(first));
    run.capture.matches(reference.onsets());
    require(reference.consume(short_chunk),"consume short source chunk");run.push(1,std::span<const float>(short_chunk));
    run.capture.matches(reference.onsets());require(run.capture.count==reference.onsets().size(),"short packet preserves accumulation count");
    std::array<float,32> final{};std::size_t count=0;
    require(reference.finish(final,count),"finish direct accumulator");run.control(2);
    run.capture.matches(std::span(final).first(count));require(run.capture.count==count,"explicit EOF publishes accumulated count");
    require(reference.finish(final,count),"repeat direct EOF");run.control(2);
    run.capture.matches(std::span(final).first(count));require(count==0 && run.capture.count==0,"repeated EOF does not replay stale events");
    require(reference.consume(first),"consume new stream after EOF");run.push(1,std::span<const float>(first));
    run.capture.matches(reference.onsets());const auto calls=run.capture.calls;run.control(3);reference.reset();
    require(run.capture.calls==calls+1 && !run.capture.last.available && run.capture.last.discontinuity,"reset marker invalidates runtime output");
    require(reference.consume(short_chunk),"consume source after explicit reset");run.push(1,std::span<const float>(short_chunk));
    run.capture.matches(reference.onsets());
}
void apply_copies_latest_compatible_state() {
    Running run(spectral_node("SpectralRmsFlux"));rhythm_reference_ports::PreparedSpectralRmsFlux reference;
    const std::array<float,4> first{1,2,3,4},second{2,3,4,5},third{4,6,8,10},fourth{5,7,9,11};float expected=0;
    require(reference.process(first,expected),"initial RMS source step");run.push(1,std::span<const float>(first));run.capture.matches(std::span(&expected,1));
    auto pending=run.prepare(run.graph);
    require(reference.process(second,expected),"RMS advances while prepared change waits");run.push(1,std::span<const float>(second));
    run.apply(std::move(pending));require(reference.process(third,expected),"latest compatible RMS state");
    run.push(1,std::span<const float>(third));run.capture.matches(std::span(&expected,1));
    auto invalid=run.graph;invalid.nodes[0].outputs[0].contract=contract(DType::UInt8,4);
    const auto* previous_plan=run.executor.plan();const auto* previous_context=run.executor.context();
    application::PreparedExecution rejected;const auto p=profile();
    require(run.executor.prepare_numeric(invalid,{p.admission,p.resources},rejected)!=Status::ok,"incompatible dtype graph rejected");
    require(run.executor.plan()==previous_plan && run.executor.context()==previous_context,"rejected prepare leaves working graph atomically owned");
    require(reference.process(fourth,expected),"source state after rejected prepare");run.push(1,std::span<const float>(fourth));run.capture.matches(std::span(&expected,1));
    run.graph=graph_for(spectral_node("SpectralRmsFlux",8));auto geometry=run.prepare(run.graph);run.apply(std::move(geometry));reference.reset();
    const std::array<float,8> changed{1,2,3,4,5,6,7,8};require(reference.process(changed,expected),"reset RMS geometry source step");
    run.push(1,std::span<const float>(changed));run.capture.matches(std::span(&expected,1));require(expected==0,"incompatible geometry starts fresh state");
}
void clone_owns_independent_filter_state() {
    Operation operation(recursive_node());dsp_kernels::PreparedRecursiveFilter reference;
    const std::array<float,1> numerator{1};const std::array<float,2> denominator{1,-.5f};
    require(reference.prepare(numerator,denominator),"prepare recursive filter oracle");
    const std::array<float,4> first{1,0,0,0},left{0,0,0,0},right{2,1,0,0};std::array<float,4> expected{};
    auto input=operation.input(0,std::span<const float>(first));require(reference.process(first,expected),"initial recursive state");
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"initial recursive operation");operation.matches(0,expected);
    auto cloned=operation.processor->clone();auto clone_reference=reference;
    input=operation.input(0,std::span<const float>(left));require(reference.process(left,expected),"advance original recursive branch");
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"advance original operation only");operation.matches(0,expected);
    input=operation.input(0,std::span<const float>(right));require(clone_reference.process(right,expected),"advance independent recursive source clone");
    require(cloned->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"advance independent operation clone");operation.matches(0,expected);
    require(cloned->copy_state_from(*operation.processor)==Status::incompatible_contract,"common filter declares warm-copy reset policy");
    cloned->reset();clone_reference.reset();require(clone_reference.process(right,expected),"reset source clone");
    require(cloned->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"reset operation clone");operation.matches(0,expected);
    auto invalid=recursive_node();invalid.inputs[0].contract=contract(DType::UInt8,4);auto p=profile();
    require(!p.operation_factory->assess(numeric_operation_node_view(resolved(invalid)),numeric_operation_profile(p)).supported,"recursive filter rejects incompatible primitive contract");
}
}
int main() {
    explicit_eof_and_reset();
    apply_copies_latest_compatible_state();clone_owns_independent_filter_state();
    std::puts("numeric operation lifecycle: 3 scenarios PASS");
}
