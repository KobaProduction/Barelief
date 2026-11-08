#include <dataflow_runtime/runtime.h>
#include <domain/graph/node_factory.h>
#include <domain/numeric/array_extraction.h>
#include "numeric_test_builders.h"
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
using namespace signal_contracts;using namespace domain;using namespace dataflow_runtime;
std::size_t allocations=0;
void check(bool condition,const char* reason){if(!condition){std::fprintf(stderr,"index extraction: %s\n",reason);std::exit(1);}}
NumericRuntimeProfile profile(){
    return {{32,64,8,32,1048576,16777216,{8,800000,3200000,65536,4096,65536}},
        {{16777216,67108864,33554432,1048576},0,268435456},{800000,8,3200000,3200000,16777216},{true,true,true}};
}
OwnedProcessingContract own(const NumericContract& contract){
    OwnedProcessingContract result;check(own_processing_contract(contract,profile().admission.frame,65536,result)==Status::ok,"own input");return result;
}
ProcessingParameter size(std::string key,std::size_t value){return {std::move(key),NumericLiteral{std::uint64_t(value)}};}
NumericContract input_contract(DType type,bool matrix){
    static constexpr std::uint32_t mono=0;NumericContract result;result.dtype=type;result.channel_order={&mono,1};result.semantic=Semantic::Numeric;
    result.shape={matrix?Rank::Matrix:Rank::Vector,{Axis::Declared,matrix?Axis::Component:Axis::None},{{{0,2},{matrix?3u:1u,matrix?3u:1u}}}};
    result.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{1,1},{2,1}};
    if(matrix)result.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    result.clock={"extract",{1,44100},44100};return result;
}
NumericProcessingGraph graph(const NumericContract& input,TransformProcessingParameters parameters){
    ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};
    source.parameters=EndpointProcessingParameters{{size("sample_rate",44100)}};
    ConfiguredProcessingNode extraction;check(bool(make_processing_node("IndexExtract",2,profile().admission,extraction)),"create existing extractor");
    extraction.parameters=std::move(parameters);extraction.inputs={{0,PortDirection::Input,source.outputs[0].contract}};
    check(bool(derive_processing_node_contracts(extraction,profile().admission)),"derive axis output");
    NumericProcessingGraph result;result.graph_id="extract.axis";result.nodes={source,extraction};result.edges={{1,2,0,0}};return result;
}
std::uint64_t value_bits(DType type,std::size_t row){
    if(type==DType::Float32)return std::bit_cast<std::uint32_t>(row?1.75f:1.00000011920928955078125f);
    if(type==DType::Float64)return std::bit_cast<std::uint64_t>(row?1.75:1.0000000000000002);
    return type==DType::UInt64?UINT64_MAX-row:71+row;
}
struct Seen {
    DType type;std::size_t columns=1,calls=0;
    static void output(void* opaque,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& self=*static_cast<Seen*>(opaque);
        check(event.status==Status::ok && event.frame.stamp.available,"axis selection published");
        const auto& value=std::get<NumericView>(event.frame.payload);
        check(value.actual==std::array<std::size_t,2>{5,self.columns},"actual rows use current input");
        check(value.contract->coordinates[0].mode==CoordinateMode::None,"selected axis uses plain indices");
        if(self.columns>1)check(value.coordinates.axes[1].size()==3*sizeof(double),"unselected coordinates retained");
        for(std::size_t row=0;row<5;++row)for(std::size_t column=0;column<self.columns;++column){
            std::uint64_t bits=0;check(load_bits(value,{0,row,column},self.type,bits)==Status::ok,"read exact dtype");
            check(bits==value_bits(self.type,row==2?0:1),"relative indices, repeats and edge padding preserve bits");
        }
        ++self.calls;
    }
};
struct IngressStorage {
    alignas(std::max_align_t) std::array<std::byte,1024> payload{};
    alignas(std::max_align_t) std::array<std::byte,32768> metadata{};
    std::array<BufferSlot,4> payload_slots,metadata_slots;
    BufferPool payload_pool{payload,payload_slots,256},metadata_pool{metadata,metadata_slots,8192};
};
void exact_dtype_case(DType type,bool matrix,bool ring=false){
    auto input=input_contract(type,matrix);if(ring)input.semantic=Semantic::History;
    const auto g=graph(input,TransformProcessingParameters{{size("mode",1),size("axis",0),
        {"indices",NumericArray{{-1,0,-1}}},size("pad_before",1),size("pad_after",1),size("padding",1)}});
    auto arena=std::make_shared<IngressStorage>();
    std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),prepared,state)==Status::ok,"prepare extraction");
    std::size_t width=0;check(storage_width(input,width)==Status::ok,"dtype width");const auto columns=matrix?3u:1u;
    const PlaneLayout plane{0,{8*width,matrix?2*width:0}};alignas(16) std::array<std::byte,128> bytes{};
    MutableNumericView data{&input,bytes,std::span(&plane,1),{2,columns},{},{}};
    if(ring)data.ring={2,1,{}};
    for(std::size_t row=0;row<2;++row)for(std::size_t column=0;column<columns;++column)
        check(store_bits(data,{0,row,column},type,value_bits(type,row))==Status::ok,"write strided input");
    const std::array<double,3> coordinates{20,200,2000};if(matrix)data.coordinates.axes[1]=std::as_bytes(std::span(coordinates));
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;Seen seen{type,columns};const auto before=allocations;
    const NumericRuntimeCallbacks callbacks{Seen::output,nullptr,nullptr,&seen};
    if(ring){
        OwnedDataFrame retained;check(retain_frame({stamp,data.view()},profile().admission.frame,arena->payload_pool,arena->metadata_pool,retained)==Status::ok,"retain external ring");
        check(prepared->process_numeric_owned_ports(1,retained,*state,{},callbacks,2,arena,2)==Status::ok,"extract logical ring rows");
    }else check(prepared->process_numeric_ports(1,0,{stamp,data.view()},*state,{},callbacks)==Status::ok,"execute exact selection");
    check(allocations==before && seen.calls==1,"one output without hot allocations");
}
void empty_and_bounds(){
    const auto input=input_contract(DType::Float32,false);const auto g=graph(input,TransformProcessingParameters{{size("mode",1),size("pad_after",2)}});
    std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),prepared,state)==Status::ok,"prepare empty constant padding");
    const PlaneLayout plane{0,{4,0}};std::array<float,2> storage{};FrameStamp stamp;stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;
    struct Zero {unsigned calls=0;static void output(void* opaque,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& seen=*static_cast<Zero*>(opaque);check(event.status==Status::ok,"empty pad available");
        const auto& value=std::get<NumericView>(event.frame.payload);check(value.actual[0]==2,"explicit pad has two elements");
        for(std::size_t i=0;i<2;++i){float v=1;check(load_element(value,{0,i,0},v)==Status::ok && v==0,"empty pad is zero");}++seen.calls;
    }} seen;
    const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(storage)),std::span(&plane,1),{0,1});
    const auto before=allocations;check(prepared->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{Zero::output,nullptr,nullptr,&seen})==Status::ok,"empty input processed");
    check(seen.calls==1 && allocations==before,"empty pad no allocation");
    auto invalid=g.nodes[1];std::get<TransformProcessingParameters>(invalid.parameters).values.push_back({"indices",NumericArray{{2}}});
    check(!derive_processing_node_contracts(invalid,profile().admission),"index equal to capacity rejected before execution");
}
struct Refused {
    unsigned calls=0;
    static void output(void* opaque,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;
        check(event.status==Status::invalid_argument && !event.frame.stamp.available,"invalid actual publishes refusal");
        ++static_cast<Refused*>(opaque)->calls;
    }
};
void actual_refusal(TransformProcessingParameters parameters,std::size_t actual){
    const auto input=input_contract(DType::Float32,false);const auto g=graph(input,std::move(parameters));
    std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),prepared,state)==Status::ok,"prepare capacity-valid selection");
    const PlaneLayout plane{0,{4,0}};const std::array<float,2> values{5,7};FrameStamp stamp;stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;
    const auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(values)),std::span(&plane,1),{actual,1});
    Refused seen;const auto before=allocations;
    check(prepared->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{Refused::output,nullptr,nullptr,&seen})==Status::invalid_argument,"actual-dependent bounds refused");
    check(allocations==before && seen.calls==1,"one refusal without allocations");
}
struct RangeSeen {
    unsigned calls=0;
    static void output(void* opaque,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;check(event.status==Status::ok,"second-axis range available");
        const auto& value=std::get<NumericView>(event.frame.payload);
        check(value.actual==std::array<std::size_t,2>{2,3},"range plus explicit padding shape");
        check(value.coordinates.axes[0].size()==2*sizeof(double) && value.contract->coordinates[1].mode==CoordinateMode::None,"only selected coordinates replaced");
        for(std::size_t row=0;row<2;++row)for(std::size_t column=0;column<3;++column){
            float number=0;check(load_element(value,{0,row,column},number)==Status::ok,"read ranged value");
            check(number==(column==1?float(row*3+2):0.f),"range selects column and constant-pads explicitly");
        }
        ++static_cast<RangeSeen*>(opaque)->calls;
    }
};
void second_axis_range(){
    auto input=input_contract(DType::Float32,true);input.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};input.coordinates[1]={};
    const auto g=graph(input,TransformProcessingParameters{{size("mode",1),size("axis",1),size("first",1),size("length",1),size("pad_before",1),size("pad_after",1)}});
    const auto output=std::get<NumericContract>(g.nodes[1].outputs[0].contract.view());
    check(output.shape.extents[1]==ExtentBounds{3,3},"fixed input columns give a fixed selected range for preparation measurements");
    std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),prepared,state)==Status::ok,"prepare selected matrix columns");
    const std::array<float,6> values{1,2,3,4,5,6};const PlaneLayout plane{0,{12,4}};const std::array<double,2> times{.25,.5};
    FrameStamp stamp;stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;
    auto frame=numeric_test_builders::frame(stamp,input,std::as_bytes(std::span(values)),std::span(&plane,1),{2,3});
    std::get<NumericView>(frame.payload).coordinates.axes[0]=std::as_bytes(std::span(times));RangeSeen seen;const auto before=allocations;
    check(prepared->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{RangeSeen::output,nullptr,nullptr,&seen})==Status::ok,"execute columns independently of rows");
    check(seen.calls==1 && allocations==before,"matrix range no hot allocations");
}
struct PaddingSeen {
    std::uint64_t expected=0;unsigned calls=0;
    static void output(void* opaque,const NumericPortFrameView& event){
        if(event.identity.node!=2)return;auto& seen=*static_cast<PaddingSeen*>(opaque);
        check(event.status==Status::ok,"typed padding available");const auto& value=std::get<NumericView>(event.frame.payload);
        check(value.actual==std::array<std::size_t,2>{2,1},"typed padding shape");
        for(std::size_t i=0;i<2;++i){
            std::uint64_t bits=0;check(load_bits(value,{0,i,0},value.contract->dtype,bits)==Status::ok && bits==seen.expected,"constant literal keeps exact primitive bits");
        }
        ++seen.calls;
    }
};
void typed_padding(DType type,NumericLiteral constant,std::uint64_t expected,RepresentationContract representation={}){
    auto input=input_contract(type,false);input.representation=representation;
    const auto g=graph(input,TransformProcessingParameters{{size("mode",1),size("pad_before",2),{"constant",constant}}});
    std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),prepared,state)==Status::ok,"prepare exact constant literal");
    std::size_t width=0;check(storage_width(input,width)==Status::ok,"constant dtype width");const PlaneLayout plane{0,{width,0}};
    alignas(16) const std::array<std::byte,16> values{};FrameStamp stamp;stamp.sequence=stamp.source_sequence=stamp.producer_epoch=1;
    const auto frame=numeric_test_builders::frame(stamp,input,values,std::span(&plane,1),{0,1});PaddingSeen seen{expected};const auto before=allocations;
    check(prepared->process_numeric_ports(1,0,frame,*state,{},NumericRuntimeCallbacks{PaddingSeen::output,nullptr,nullptr,&seen})==Status::ok,"execute exact typed padding");
    check(seen.calls==1 && allocations==before,"typed padding no hot allocations");
}
void rejected_padding(DType type,NumericLiteral constant,RepresentationContract representation={}){
    auto input=input_contract(type,false);input.representation=representation;
    const auto g=graph(input,TransformProcessingParameters{{size("mode",1),size("pad_before",2),{"constant",constant}}});
    std::shared_ptr<const PreparedGraph> prepared;std::unique_ptr<RuntimeContext> state;
    check(PreparedGraph::prepare_numeric(g,profile(),prepared,state)!=Status::ok && !prepared && !state,"constant range refusal is atomic before execution");
}
void narrow_integer_padding(){
    RepresentationContract representation;representation.valid_bits=8;
    typed_padding(DType::Int16,NumericLiteral{std::int64_t{-63}},std::uint16_t(0xffc1),representation);
    representation.placement=BitPlacement::MostSignificant;
    typed_padding(DType::Int16,NumericLiteral{std::int64_t{-63}},std::uint16_t(0xffc1),representation);
    rejected_padding(DType::Int16,NumericLiteral{std::int64_t{-129}},representation);
    rejected_padding(DType::Int16,NumericLiteral{std::int64_t{128}},representation);
    rejected_padding(DType::UInt16,NumericLiteral{std::uint64_t{256}},representation);
    representation.valid_bits=24;representation.placement=BitPlacement::LeastSignificant;representation.encoding=StorageEncoding::PackedSigned24LE;
    typed_padding(DType::Int32,NumericLiteral{std::int64_t{-1}},UINT32_MAX,representation);
    representation.encoding=StorageEncoding::PackedSigned24BE;
    typed_padding(DType::Int32,NumericLiteral{std::int64_t{-1}},UINT32_MAX,representation);
}
void integer_padding_boundaries(){
    typed_padding(DType::Int64,NumericLiteral{INT64_MAX},std::uint64_t(INT64_MAX));
    rejected_padding(DType::Int8,NumericLiteral{std::int64_t{128}});
    rejected_padding(DType::UInt8,NumericLiteral{std::int64_t{-1}});
    rejected_padding(DType::Int64,NumericLiteral{9223372036854775808.0});
    rejected_padding(DType::UInt64,NumericLiteral{18446744073709551616.0});
    rejected_padding(DType::UInt32,NumericLiteral{1.5});
}
}
void* operator new(std::size_t bytes){++allocations;if(auto* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc();}
void operator delete(void* value) noexcept{std::free(value);}
void operator delete(void* value,std::size_t) noexcept{std::free(value);}
int main(){
    for(const auto& type:dtype_descriptors()){
        for(bool matrix:{false,true})exact_dtype_case(type.type,matrix);
        exact_dtype_case(type.type,true,true);
    }
    empty_and_bounds();second_axis_range();
    actual_refusal(TransformProcessingParameters{{size("mode",1),{"indices",NumericArray{{1}}}}},1);
    actual_refusal(TransformProcessingParameters{{size("mode",1),size("pad_after",2),size("padding",1)}},0);
    typed_padding(DType::UInt64,NumericLiteral{UINT64_MAX},UINT64_MAX);
    typed_padding(DType::UInt64,NumericLiteral{(std::uint64_t{1}<<53)+1},(std::uint64_t{1}<<53)+1);
    typed_padding(DType::Int64,NumericLiteral{INT64_MIN},std::bit_cast<std::uint64_t>(INT64_MIN));
    typed_padding(DType::Float64,NumericLiteral{-0.0},std::bit_cast<std::uint64_t>(-0.0));
    narrow_integer_padding();integer_padding_boundaries();
    std::puts("index extraction PASS");
}
