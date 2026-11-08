#include "numeric_operation_api_proofs.h"
#include "numeric_operation_boundary_fixture.h"
#include <new>

namespace {
using namespace operation_boundary;
std::size_t allocations=0;

void standalone_origins() {
    NumericOperationOrigin first,second;
    require(numeric_operation_same_session(first,second),"unspecified standalone sessions match");
    require(numeric_operation_same_step(first,second),"unspecified standalone steps match");
    require(numeric_operation_same_coordinates(first,second),"unspecified standalone coordinates match");
}

void valid_publication_and_retention() {
    operation_boundary::Running run;
    const auto before=allocations;
    require(run.push(1)==Status::ok,"bound origin publication");
    require(run.capture.valid==std::array<std::size_t,2>{1,1},"both outputs publish together");
    require(run.capture.values==std::array<float,4>{1,2,3,4},"numeric buffer payload preserved");
    require(run.capture.axis==std::array<double,4>{0,.25,.5,.75},"write-only axis reaches runtime output");
    const auto first=run.settings->observed_origin;
    run.settings->fault=Fault::RetainedOrigin;
    run.settings->publication=NumericOperationPublication::Batch;
    require(run.push(2)==Status::ok,"same-authority retained origin remains valid");
    require(run.capture.valid==std::array<std::size_t,2>{2,2} && run.capture.last.source_sequence==1,
        "buffered publication preserves its original source step");
    require(numeric_operation_same_session(first,run.settings->observed_origin),
        "different steps retain the same source session");
    require(!numeric_operation_same_step(first,run.settings->observed_origin),
        "origin comparison distinguishes source steps");
    require(allocations==before,"origin retention and coordinate publication allocate no hot storage");
}

void context_clone_preserves_retained_origin() {
    operation_boundary::Running run;require(run.push(1)==Status::ok,"publish before context clone");
    auto cloned=std::make_unique<RuntimeContext>(*run.state);
    run.state=std::move(cloned);
    run.settings->fault=Fault::RetainedOrigin;
    require(run.push(2)==Status::ok,"cloned operation keeps retained origin authority");
    require(run.capture.valid==std::array<std::size_t,2>{2,2} && run.capture.last.source_sequence==1,
        "cloned buffered result preserves original provenance");
}

void rejected_publication_is_atomic(Fault fault) {
    operation_boundary::Running run;require(run.push(1)==Status::ok,"initial valid publication before rejection");
    const auto before=allocations;
    run.settings->fault=fault;
    (void)run.push(2);
    require(run.capture.valid==std::array<std::size_t,2>{1,1},"failed output exposes no partial sibling");
    require(run.capture.invalid[0]>0 && run.capture.invalid[1]>0,"both rejected outputs become unavailable");
    run.settings->fault=Fault::None;
    require(run.push(3)==Status::ok,"publication recovers after rejected output");
    require(run.capture.valid==std::array<std::size_t,2>{2,2} && run.capture.last.source_sequence==3,
        "recovery publishes current values and provenance");
    require(allocations==before,"rejected and recovered publications allocate no hot storage");
}

void foreign_origin_is_rejected() {
    operation_boundary::Running source;require(source.push(1)==Status::ok,"obtain independent operation origin");
    operation_boundary::Running receiver;
    receiver.settings->fault=Fault::ForeignOrigin;
    receiver.settings->foreign_origin=source.settings->observed_origin;
    (void)receiver.push(1);
    require(receiver.capture.valid==std::array<std::size_t,2>{0,0},"foreign authority cannot publish payload");
    require(receiver.capture.invalid[0]>0 && receiver.capture.invalid[1]>0,
        "foreign operation authority rejects both outputs");
}

void expired_writer_is_safe() {
    NumericOperationCoordinateWriter retained;
    {
        operation_boundary::Running run;require(run.push(1)==Status::ok,"obtain borrowed coordinate writer");
        retained=run.settings->observed_writer;
    }
    NumericOperationOutput output;output.coordinates=retained;
    require(numeric_operation_coordinate(output,0,0,1)!=Status::ok,
        "destroyed-context writer rejects coordinate access");
    require(numeric_operation_axis(output,0,4)!=Status::ok,
        "destroyed-context writer rejects axis publication");
}

void resource_underreporting_is_rejected() {
    auto settings=std::make_shared<Settings>();settings->underreport=true;
    auto p=profile();p.operation_factory=std::make_shared<Factory>(settings);
    std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    require(PreparedGraph::prepare_numeric(graph_for(node()),p,plan,state)!=Status::ok && !plan && !state,
        "actual processor resources exceeding assessment reject atomically");
}

void metadata_requirements_reject_incompatible_bindings() {
    for(const auto requirement:{Requirement::SampleRate,Requirement::Unclocked,Requirement::Unit,Requirement::AxisMatch}){
        auto settings=std::make_shared<Settings>();settings->requirement=requirement;
        auto p=profile();p.operation_factory=std::make_shared<Factory>(settings);
        std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
        require(PreparedGraph::prepare_numeric(graph_for(node()),p,plan,state)!=Status::ok && !plan && !state,
            "declared metadata constraint refuses incompatible binding atomically");
    }
}
void metadata_grid_is_checked_before_publication() {
    operation_boundary::Running run(Requirement::Grid);
    (void)run.push(1);
    require(run.capture.valid==std::array<std::size_t,2>{0,0},"wrong explicit coordinate grid exposes no payload");
    require(run.capture.invalid[0]>0 && run.capture.invalid[1]>0,"grid refusal invalidates sibling outputs");
}

void rounded_duplicate_coordinates_are_rejected() {
    operation_boundary::Running run(Requirement::Increasing);
    run.settings->coordinate_origin=1e16;run.settings->coordinate_step=.01;
    (void)run.push(1);
    require(run.capture.valid==std::array<std::size_t,2>{0,0},"rounded duplicate coordinates violate explicit increasing requirement");
}

void declaration_requires_increasing_coordinates() {
    operation_boundary::Running run(Requirement::IncreasingDeclaration);
    const std::array<double,4> increasing{0,.2,.7,2};
    run.settings->explicit_coordinates=increasing;
    const auto before=allocations;
    require(run.push(1)==Status::ok,"increasing declaration accepts an irregular coordinate axis");
    require(run.capture.valid==std::array<std::size_t,2>{1,1} && run.capture.axis==increasing,
        "declaration preserves irregular coordinates without imposing a uniform grid");
    run.settings->explicit_coordinates=std::array<double,4>{0,2,1,3};
    (void)run.push(2);
    require(run.capture.valid==std::array<std::size_t,2>{1,1} &&
        run.capture.invalid==std::array<std::size_t,2>{1,1},"decreasing declaration invalidates both outputs");
    run.settings->explicit_coordinates=std::array<double,4>{0,0,0,0};
    (void)run.push(3);
    require(run.capture.valid==std::array<std::size_t,2>{1,1} &&
        run.capture.invalid==std::array<std::size_t,2>{2,2},"duplicate declaration invalidates both outputs");
    run.settings->explicit_coordinates=increasing;
    require(run.push(4)==Status::ok && run.capture.valid==std::array<std::size_t,2>{2,2},
        "increasing declaration recovers after rejected coordinates");
    require(allocations==before,"increasing declaration validation has no hot allocations");
}

void retained_output(void* context,const NumericPortFrameView& event) {
    if(event.identity.port==0)operation_boundary::Capture::output(context,event);
}

void retained_axis_and_stream_are_delivered() {
    auto n=recursive_node();auto full=std::get<NumericContract>(n.inputs[0].contract.view());
    full.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{},{}};
    OwnedProcessingContract owned;
    require(own_processing_contract(full,profile().admission.frame,65536,owned)==Status::ok,"own explicit retained axis");
    n.inputs[0].contract=owned;full=std::get<NumericContract>(owned.view());
    std::vector<OwnedProcessingContract> derived;
    require(common_operation_output_contracts(n,full,derived)==Status::ok,"derive retained operation outputs from explicit input");
    for(std::size_t i=0;i<derived.size();++i)n.outputs[i].contract=std::move(derived[i]);
    auto graph=graph_for(n);auto p=profile();std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
    require(PreparedGraph::prepare_numeric(graph,p,plan,state)==Status::ok,"prepare explicit retained coordinate route");
    const std::array<float,4> values{1,0,0,0};const std::array<double,4> axis{10,20,30,40};
    const std::array<PlaneLayout,1> planes{{{0,{sizeof(float),0}}}};
    NumericView view{&full,std::as_bytes(std::span(values)),planes,{4,1}};view.coordinates.axes[0]=std::as_bytes(std::span(axis));
    auto time=stamp(1);time.stream_id="test-stream";operation_boundary::Capture capture;
    require(plan->process_numeric_ports(1,0,{time,view},*state,{},NumericRuntimeCallbacks{retained_output,nullptr,nullptr,&capture})==Status::ok,
        "retained explicit coordinates publish on the first call");
    require(capture.valid[0]==1 && capture.axis==axis,"runtime route copies declared input coordinates");
    require(std::string_view(capture.stream.data(),capture.stream_size)==time.stream_id,"explicit stream route preserves delivery label");
}

void explicit_declaration_step_is_exact() {
    for(const auto step:{Ratio{0,1},Ratio{1,1},Ratio{2,2}}){
        auto settings=std::make_shared<Settings>();settings->requirement=Requirement::ExactDeclarationStep;
        auto p=profile();p.operation_factory=std::make_shared<Factory>(settings);
        std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;
        const auto status=PreparedGraph::prepare_numeric(graph_for(declaration_node(step)),p,plan,state);
        const bool expected=step==Ratio{1,1};
        require((status==Status::ok)==expected,"explicit declaration step matches its exact ratio requirement");
        require(expected || (!plan && !state),"incorrect declaration step is rejected atomically");
    }
}

void physical_geometry_is_validated() {
    operation_lifecycle::Operation operation(recursive_node());
    const std::array<float,4> values{1,0,0,0};
    auto input=operation.input(0,std::span<const float>(values));
    auto changed=*input.value.contract;changed.dtype=DType::UInt8;input.value.contract=&changed;
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})!=Status::ok,
        "incorrect physical primitive is rejected before filter state changes");
    input=operation.input(0,std::span<const float>(values));input.value.storage=input.value.storage.first(1);
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})!=Status::ok,
        "short physical storage is rejected before filter state changes");
    input=operation.input(0,std::span<const float>(values));
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,
        "valid physical input accepted after rejection");
    const std::array<float,4> expected{1,.5f,.25f,.125f};operation.matches(0,expected);
}
}

void* operator new(std::size_t count) {
    ++allocations;
    if(auto* value=std::malloc(count?count:1))return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete(void* value,std::size_t) noexcept {std::free(value);}

int main() {
    standalone_origins();valid_publication_and_retention();context_clone_preserves_retained_origin();
    for(const auto fault:{Fault::UnboundOrigin,Fault::StaleWriter,Fault::ForeignWriter,
        Fault::Storage,Fault::Contract,Fault::Extent,Fault::Nonfinite})
        rejected_publication_is_atomic(fault);
    foreign_origin_is_rejected();expired_writer_is_safe();resource_underreporting_is_rejected();
    physical_geometry_is_validated();explicit_declaration_step_is_exact();
    metadata_requirements_reject_incompatible_bindings();metadata_grid_is_checked_before_publication();
    rounded_duplicate_coordinates_are_rejected();declaration_requires_increasing_coordinates();
    retained_axis_and_stream_are_delivered();
    std::puts("numeric operation boundary PASS");
}
