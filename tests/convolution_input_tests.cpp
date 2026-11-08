#include "numeric_operation_lifecycle_fixture.h"
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace {
// Keep the former cold fallback overrun deterministic, even without a sanitizer.
struct alignas(std::max_align_t) AllocationHeader {std::size_t bytes;};
constexpr std::size_t guard_bytes=32;
constexpr std::byte guard_value{0xa5};
void* guarded_allocate(std::size_t bytes) {
    bytes=std::max(bytes,std::size_t{1});
    if(bytes>std::numeric_limits<std::size_t>::max()-sizeof(AllocationHeader)-guard_bytes)throw std::bad_alloc{};
    auto* raw=std::malloc(sizeof(AllocationHeader)+bytes+guard_bytes);
    if(!raw)throw std::bad_alloc{};
    auto* header=new(raw) AllocationHeader{bytes};
    auto* payload=reinterpret_cast<std::byte*>(header+1);
    std::fill_n(payload+bytes,guard_bytes,guard_value);
    return payload;
}
bool allocation_tail_intact(const void* pointer) noexcept {
    const auto* header=static_cast<const AllocationHeader*>(pointer)-1;
    const auto* end=static_cast<const std::byte*>(pointer)+header->bytes;
    return std::all_of(end,end+guard_bytes,[](std::byte value){return value==guard_value;});
}
void* guarded_allocate_nothrow(std::size_t bytes) noexcept {
    try{return guarded_allocate(bytes);}catch(...){return nullptr;}
}
bool canary_detects_overwrite(void* pointer) noexcept {
    auto* header=static_cast<AllocationHeader*>(pointer)-1;
    auto* tail=reinterpret_cast<std::byte*>(header)+sizeof(AllocationHeader)+header->bytes;
    const auto saved=*tail;*tail=std::byte{0};
    const bool detected=!allocation_tail_intact(pointer);
    *tail=saved;return detected;
}
void guarded_release(void* pointer) noexcept {
    if(!pointer)return;
    auto* header=static_cast<AllocationHeader*>(pointer)-1;
    if(!allocation_tail_intact(pointer)){
        std::fputs("FAIL allocation tail overwritten during convolution preparation\n",stderr);std::abort();
    }
    std::free(header);
}
}
void* operator new(std::size_t bytes) {return guarded_allocate(bytes);}
void operator delete(void* pointer) noexcept {guarded_release(pointer);}
void operator delete(void* pointer,std::size_t) noexcept {guarded_release(pointer);}
void* operator new[](std::size_t bytes) {return guarded_allocate(bytes);}
void operator delete[](void* pointer) noexcept {guarded_release(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept {guarded_release(pointer);}
// MinGW's DLL nothrow operators do not route through the executable's throwing
// replacements. OwnedProcessingContract uses both scalar and array nothrow new.
void* operator new(std::size_t bytes,const std::nothrow_t&) noexcept {return guarded_allocate_nothrow(bytes);}
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {return guarded_allocate_nothrow(bytes);}
void operator delete(void* pointer,const std::nothrow_t&) noexcept {guarded_release(pointer);}
void operator delete[](void* pointer,const std::nothrow_t&) noexcept {guarded_release(pointer);}

namespace {
using namespace operation_lifecycle;
void allocation_canary_controls() {
    auto* scalar=static_cast<std::byte*>(::operator new(8,std::nothrow));
    auto* array=::operator new[](8,std::nothrow);
    require(scalar && array,"nothrow fixture allocations succeed");
    require(allocation_tail_intact(scalar) && allocation_tail_intact(array),"both nothrow allocations carry canaries");
    require(canary_detects_overwrite(scalar),"canary detects a write past the requested extent");
    ::operator delete(scalar,std::nothrow);::operator delete[](array,std::nothrow);
}
template<class T> constexpr DType dtype() {
    return std::is_same_v<T,double>?DType::Float64:DType::Float32;
}
ConfiguredProcessingNode convolution(DType type,bool explicit_kernel) {
    ConfiguredProcessingNode node;node.id=10;node.kind="SameConvolution";
    node.inputs={{0,PortDirection::Input,contract(type,4)}};
    if(explicit_kernel)node.inputs.push_back({1,PortDirection::Input,contract(type,2)});
    node.parameters=TransformProcessingParameters{{{"kernel",explicit_kernel?NumericArray{{9,9,9,9}}:NumericArray{{1,1}}}}};
    std::vector<OwnedProcessingContract> outputs;
    require(common_operation_output_contracts(node,std::get<NumericContract>(node.inputs[0].contract.view()),outputs)==Status::ok,"derive convolution output");
    for(std::size_t i=0;i<outputs.size();++i)node.outputs.push_back({PortId(i),PortDirection::Output,std::move(outputs[i])});
    return node;
}
template<class T> void matches(const Operation& operation) {
    const auto& output=operation.output_storage[0];
    require(output.status==Status::ok && output.value.actual==std::array<std::size_t,2>{4,1},"convolution publishes exact geometry");
    const std::array<T,4> expected{1,3,5,7};
    for(std::size_t i=0;i<expected.size();++i){
        T actual=0;
        require(load_element(output.value.view(),{0,i,0},actual)==Status::ok && actual==expected[i],"convolution uses the selected coefficient source");
    }
}
template<class T> void explicit_kernel_wins() {
    Operation operation(convolution(dtype<T>(),true));
    const std::array<T,4> data{1,2,3,4};const std::array<T,2> kernel{1,1};
    const std::array inputs{operation.input(0,std::span<const T>(data)),operation.input(1,std::span<const T>(kernel))};
    require(operation.processor->process(inputs,operation.outputs(),{})==Status::ok,"explicit kernel overrides oversized fallback");
    matches<T>(operation);
}
template<class T> void fallback_kernel_remains_available() {
    Operation operation(convolution(dtype<T>(),false));
    const std::array<T,4> data{1,2,3,4};auto input=operation.input(0,std::span<const T>(data));
    require(operation.processor->process(std::span(&input,1),operation.outputs(),{})==Status::ok,"parameter-only convolution remains available");
    matches<T>(operation);
}
void runtime_waits_for_explicit_kernel() {
    Running running(convolution(DType::Float32,true));
    const std::array<float,4> data{1,2,3,4};const std::array<float,2> kernel{1,1},updated{0,1};
    running.push(1,std::span<const float>(data));
    require(running.capture.status!=Status::ok,"parameter fallback cannot satisfy a required held kernel input");
    running.push(2,std::span<const float>(kernel));running.capture.matches(std::array{1.f,3.f,5.f,7.f});
    const auto publications=running.capture.calls;
    running.push(2,std::span<const float>(updated));
    require(running.capture.calls==publications,"held kernel update does not replay a consumed data frame");
    running.push(1,std::span<const float>(data));running.capture.matches(std::array{0.f,1.f,2.f,3.f});
}
template<class T> void multiple_kernel_planes_rejected() {
    auto node=numeric_operation_node(convolution(dtype<T>(),true));
    auto p=profile();p.admission.frame.max_planes=2;const auto physical=numeric_operation_profile(p);
    require(p.operation_factory->assess(node,physical).supported,"mono convolution admitted in a two-plane profile");
    node.inputs[1].contract.plane_count=2;
    require(validate_contract(node.inputs[1].contract,physical.admission.frame)==Status::ok,"two-plane kernel is physically valid");
    require(!p.operation_factory->assess(node,physical).supported,"two-plane kernel rejected at admission");
    std::shared_ptr<const NumericOperationDescriptor> descriptor;
    require(p.operation_factory->describe(node,descriptor)==Status::ok,"describe physically valid kernel");
    std::unique_ptr<PreparedNumericOperation> prepared;
    require(p.operation_factory->prepare(std::move(descriptor),physical,prepared)==Status::incompatible_contract && !prepared,"two-plane kernel rejected at preparation");
}
}
int main() {
    allocation_canary_controls();
    explicit_kernel_wins<float>();explicit_kernel_wins<double>();
    fallback_kernel_remains_available<float>();fallback_kernel_remains_available<double>();
    runtime_waits_for_explicit_kernel();
    multiple_kernel_planes_rejected<float>();multiple_kernel_planes_rejected<double>();
    std::puts("convolution input: canary controls and 7 scenarios PASS");
}
