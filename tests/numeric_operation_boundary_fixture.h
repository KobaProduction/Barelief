#pragma once
#include "numeric_operation_lifecycle_fixture.h"
#include <limits>
#include <optional>

namespace operation_boundary {
using namespace operation_lifecycle;

enum class Fault { None, UnboundOrigin, ForeignOrigin, RetainedOrigin, StaleWriter,
    ForeignWriter, Storage, Contract, Extent, Nonfinite };

enum class Requirement { None, SampleRate, Unclocked, Unit, AxisMatch, Grid, Increasing, IncreasingDeclaration, ExactDeclarationStep };
inline NumericOperationAxisRule grid_rule(Requirement requirement) {
    NumericOperationAxisRule rule;rule.direction=PortDirection::Output;rule.port=0;rule.axis=0;
    rule.kind=NumericOperationAxisKind::Uniform;rule.unit=NumericUnit::Seconds;
    rule.step=requirement==Requirement::Grid?.5:.01;rule.any_origin=requirement==Requirement::Increasing;
    rule.require_increasing=requirement==Requirement::Increasing;return rule;
}
inline NumericOperationAxisRule increasing_declaration_rule() {
    NumericOperationAxisRule rule;rule.direction=PortDirection::Output;rule.port=0;rule.axis=0;
    rule.kind=NumericOperationAxisKind::Declaration;rule.unit=NumericUnit::Seconds;
    rule.mode=CoordinateMode::ExplicitFloat64LE;rule.allow_absent=false;rule.require_increasing=true;
    return rule;
}
inline NumericOperationAxisRule exact_step_rule() {
    auto rule=increasing_declaration_rule();rule.require_increasing=false;rule.exact_step=Ratio{1,1};return rule;
}
inline void append_axis_requirement(Requirement requirement,NumericOperationPolicy& policy) {
    switch(requirement){
    case Requirement::Grid:
    case Requirement::Increasing:policy.axes.push_back(grid_rule(requirement));break;
    case Requirement::IncreasingDeclaration:policy.axes.push_back(increasing_declaration_rule());break;
    case Requirement::ExactDeclarationStep:policy.axes.push_back(exact_step_rule());break;
    default:break;
    }
}
inline NumericOperationPolicy policy_for(Requirement requirement) {
    NumericOperationPolicy policy;policy.mode=NumericOperationMode::ContinuingStream;
    policy.inputs={{0,NumericInputUse::Frame,true,ValueDomain::FiniteOnly}};
    switch(requirement){
    case Requirement::SampleRate:policy.clocks.push_back({PortDirection::Input,0,0,NumericOperationClockKind::SampleRate,48000,false});break;
    case Requirement::Unclocked:policy.clocks.push_back({PortDirection::Output,0,0,NumericOperationClockKind::Unclocked});break;
    case Requirement::Unit:policy.meanings.push_back({PortDirection::Input,0,{}, {NumericUnit::Hertz}});break;
    case Requirement::AxisMatch:policy.axis_matches.push_back({PortDirection::Output,0,0,0,0,true,true});break;
    default:append_axis_requirement(requirement,policy);break;
    }
    return policy;
}

struct Settings {
    Fault fault=Fault::None;
    Requirement requirement=Requirement::None;
    NumericOperationPublication publication=NumericOperationPublication::Frame;
    NumericOperationOrigin foreign_origin,observed_origin;
    NumericOperationCoordinateWriter observed_writer;
    bool underreport=false;
    double coordinate_origin=0,coordinate_step=.25;
    std::optional<std::array<double,4>> explicit_coordinates;
};

inline ConfiguredProcessingNode node() {
    ConfiguredProcessingNode result;result.id=10;result.kind="SpectralRmsFlux";
    result.parameters=TransformProcessingParameters{};
    result.inputs={{0,PortDirection::Input,contract(DType::Float32,4)}};
    auto full=std::get<NumericContract>(result.inputs[0].contract.view());
    full.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{},{}};
    OwnedProcessingContract output;
    require(own_processing_contract(full,profile().admission.frame,65536,output)==Status::ok,
        "own boundary explicit coordinate contract");
    result.outputs={{0,PortDirection::Output,output},{1,PortDirection::Output,output}};
    return result;
}

inline ConfiguredProcessingNode declaration_node(Ratio step) {
    auto result=node();auto full=std::get<NumericContract>(result.outputs[0].contract.view());full.coordinates[0].step=step;
    OwnedProcessingContract output;
    require(own_processing_contract(full,profile().admission.frame,65536,output)==Status::ok,"own explicit declaration step");
    for(auto& port:result.outputs)port.contract=output;
    return result;
}

class Operation final:public PreparedNumericOperation {
public:
    Operation(std::shared_ptr<const NumericOperationDescriptor> descriptor,
        std::shared_ptr<Settings> settings,ProcessingAdmissionLimits limits)
        :descriptor_(std::move(descriptor)),settings_(std::move(settings)),limits_(limits){}
    std::unique_ptr<PreparedNumericOperation> clone() const override {
        return std::make_unique<Operation>(*this);
    }
    Status copy_state_from(const PreparedNumericOperation& source) noexcept override {
        const auto* other=dynamic_cast<const Operation*>(&source);
        if(!other || descriptor_->node.kind!=other->descriptor_->node.kind)return Status::incompatible_contract;
        saved_origin_=other->saved_origin_;captured_=other->captured_;saved_writer_={};
        return Status::ok;
    }
    void reset() noexcept override {saved_origin_={};saved_writer_={};captured_=false;}
    bool owned_resources(ProcessingResources& resources) const noexcept override {
        resources={};resources.stack_bytes=4096;
        return numeric_operation_descriptor_owned_bytes(*descriptor_,limits_,resources.plan_bytes) &&
            numeric_operation_add_bytes(resources.plan_bytes,sizeof(Operation)+sizeof(Settings)) &&
            numeric_operation_add_bytes(resources.plan_bytes,settings_->underreport?65536:0);
    }
    Status process(std::span<const NumericOperationInput> inputs,
        std::span<NumericOperationOutput> outputs,NumericOperationControl) noexcept override {
        if(inputs.size()!=1 || outputs.size()!=2)return Status::invalid_argument;
        if(!captured_){saved_origin_=inputs[0].origin;saved_writer_=outputs[1].coordinates;captured_=true;}
        settings_->observed_origin=inputs[0].origin;settings_->observed_writer=outputs[1].coordinates;
        for(auto& output:outputs){
            const auto status=write(output,inputs[0]);
            if(status!=Status::ok)return status;
        }
        return corrupt(outputs);
    }
private:
    Status write(NumericOperationOutput& output,const NumericOperationInput& input) noexcept {
        output.value.actual=input.value.actual;
        output.origin=numeric_operation_origin(input.origin,settings_->publication);
        output.status=Status::ok;
        for(std::size_t i=0;i<input.value.actual[0];++i){
            float value=0;const auto loaded=load_element(input.value,{0,i,0},value);
            if(loaded!=Status::ok)return loaded;
            const auto stored=store_element(output.value,{0,i,0},value);
            if(stored!=Status::ok)return stored;
        }
        return coordinates(output);
    }
    Status coordinates(NumericOperationOutput& output) noexcept {
        for(std::size_t i=0;i<output.value.actual[0];++i){
            const double value=settings_->explicit_coordinates?(*settings_->explicit_coordinates)[i]:
                settings_->coordinate_origin+double(i)*settings_->coordinate_step;
            const auto status=numeric_operation_coordinate(output,0,i,value);
            if(status!=Status::ok)return status;
        }
        return numeric_operation_axis(output,0,output.value.actual[0]);
    }
    Status corrupt(std::span<NumericOperationOutput> outputs) noexcept {
        auto& target=outputs[1];
        switch(settings_->fault){
        case Fault::UnboundOrigin:target.origin={};break;
        case Fault::ForeignOrigin:target.origin=settings_->foreign_origin;break;
        case Fault::RetainedOrigin:
            for(auto& output:outputs)output.origin=numeric_operation_origin(saved_origin_,settings_->publication);
            break;
        case Fault::StaleWriter:target.coordinates=saved_writer_;return coordinates(target);
        case Fault::ForeignWriter:target.coordinates=outputs[0].coordinates;return coordinates(target);
        default:return corrupt_buffer(target);
        }
        return Status::ok;
    }
    Status corrupt_buffer(NumericOperationOutput& target) noexcept {
        switch(settings_->fault){
        case Fault::Storage:target.value.storage=replacement_storage_;break;
        case Fault::Contract:
            replacement_contract_=*target.value.contract;replacement_contract_.dtype=DType::Int16;
            target.value.contract=&replacement_contract_;break;
        case Fault::Extent:target.value.actual[0]=5;break;
        case Fault::Nonfinite:return store_element(target.value,{0,0,0},std::numeric_limits<float>::infinity());
        default:break;
        }
        return Status::ok;
    }
    std::shared_ptr<const NumericOperationDescriptor> descriptor_;
    std::shared_ptr<Settings> settings_;
    ProcessingAdmissionLimits limits_;
    NumericOperationOrigin saved_origin_;
    NumericOperationCoordinateWriter saved_writer_;
    NumericBufferContract replacement_contract_;
    alignas(float) std::array<std::byte,4*sizeof(float)> replacement_storage_{};
    bool captured_=false;
};

class Factory final:public NumericOperationFactory {
public:
    explicit Factory(std::shared_ptr<Settings> settings):settings_(std::move(settings)){}
    Status describe(const NumericOperationNodeView& source,
        std::shared_ptr<const NumericOperationDescriptor>& result) const override {
        if(source.kind!="SpectralRmsFlux")return Status::unavailable;
        auto policy=policy_for(settings_->requirement);
        return own_numeric_operation_descriptor(source,std::move(policy),result,this);
    }
    ProcessingSupport assess(const NumericOperationNodeView& source,
        const NumericOperationProfile& profile) const noexcept override {
        if(source.kind!="SpectralRmsFlux" || source.inputs.size()!=1 || source.outputs.size()!=2)return {};
        ProcessingSupport result;
        if(!numeric_operation_descriptor_required_bytes(source,profile.admission,result.resources.plan_bytes) ||
            !numeric_operation_add_bytes(result.resources.plan_bytes,sizeof(Operation)+sizeof(Settings)))return {};
        result.resources.stack_bytes=4096;result.supported=true;return result;
    }
    Status prepare(std::shared_ptr<const NumericOperationDescriptor> descriptor,
        const NumericOperationProfile& profile,std::unique_ptr<PreparedNumericOperation>& result) const override {
        if(!descriptor || descriptor->factory_identity!=this)return Status::incompatible_contract;
        auto prepared=std::make_unique<Operation>(std::move(descriptor),settings_,profile.admission);
        result=std::move(prepared);return Status::ok;
    }
private:
    std::shared_ptr<Settings> settings_;
};

struct Capture {
    std::array<std::size_t,2> valid{},invalid{};
    std::array<float,4> values{};
    std::array<double,4> axis{};
    FrameStamp last;
    std::array<char,32> stream{};std::size_t stream_size=0;
    static void output(void* context,const NumericPortFrameView& event) {
        if(event.identity.node!=10 || event.identity.port>=2)return;
        auto& self=*static_cast<Capture*>(context);
        const auto port=event.identity.port;
        if(event.status!=Status::ok || !event.frame.stamp.available){++self.invalid[port];return;}
        ++self.valid[port];self.last=event.frame.stamp;self.last.stream_id={};
        self.stream_size=event.frame.stamp.stream_id.size();
        require(self.stream_size<=self.stream.size(),"bounded stream label");
        std::copy(event.frame.stamp.stream_id.begin(),event.frame.stamp.stream_id.end(),self.stream.begin());
        self.read_values(std::get<NumericView>(event.frame.payload));
    }
    void read_values(const NumericView& value) {
        for(std::size_t i=0;i<4;++i)
            require(load_element(value,{0,i,0},values[i])==Status::ok,"read boundary payload");
        require(value.coordinates.axes[0].size()==sizeof(axis),"coordinate writer publishes full axis");
        std::memcpy(axis.data(),value.coordinates.axes[0].data(),sizeof(axis));
    }
};

struct Running {
    std::shared_ptr<Settings> settings=std::make_shared<Settings>();
    NumericProcessingGraph graph=graph_for(node());
    NumericRuntimeProfile runtime_profile=profile();
    std::shared_ptr<const PreparedGraph> plan;
    std::unique_ptr<RuntimeContext> state;
    NumericContract input_contract=std::get<NumericContract>(graph.nodes[0].outputs[0].contract.view());
    Capture capture;
    explicit Running(Requirement requirement=Requirement::None) {
        settings->requirement=requirement;
        runtime_profile.operation_factory=std::make_shared<Factory>(settings);
        require(PreparedGraph::prepare_numeric(graph,runtime_profile,plan,state)==Status::ok,
            "prepare restricted boundary factory");
    }
    Status push(std::uint64_t sequence) {
        const std::array<float,4> values{1,2,3,4};
        const std::array<PlaneLayout,1> plane{{{0,{sizeof(float),0}}}};
        const NumericView view{&input_contract,std::as_bytes(std::span(values)),plane,{4,1}};
        return plan->process_numeric_ports(1,0,{stamp(sequence),view},*state,{},
            NumericRuntimeCallbacks{Capture::output,nullptr,nullptr,&capture});
    }
};
} // namespace operation_boundary
