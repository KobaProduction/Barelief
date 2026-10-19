#include <signal_contracts/numeric_frame_gather.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <source_location>
#include <string>
#include <vector>
using namespace signal_contracts;
namespace allocation_probe {std::size_t count=0;}
void* operator new(std::size_t size){++allocation_probe::count;auto* p=std::malloc(size?size:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
int failures=0;
void expect(bool yes,std::source_location at=std::source_location::current()) {
    if(!yes){std::printf("FAIL %s:%u\n",at.file_name(),at.line());++failures;}
}
const NumericGatherLimits limits{{16,1<<22,1<<24,1<<18,128,1<<18},1<<16,1<<16,1<<18};
const std::array<std::uint32_t,2> channel_ids{11,77};
NumericContract history(std::size_t capacity,DType type=DType::UInt64) {
    NumericContract c;c.dtype=type;c.semantic=Semantic::History;c.history_element=Semantic::MelEnergy;c.quantity=Quantity::MelEnergy;
    c.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,capacity},ExtentBounds{4,4}}};c.channel_order=channel_ids;
    c.clock={"producer.clock",{1,1000},1000};
    c.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds,{0,1},{1,1000}};
    c.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz,{0,1},{1,1}};return c;
}
void f64(std::span<std::byte> bytes,std::size_t index,double value) {
    auto bits=std::bit_cast<std::uint64_t>(value);for(unsigned i=0;i<8;++i)bytes[index*8+i]=std::byte((bits>>(i*8))&255);
}
void run_history(std::size_t capacity,DType dtype,StorageEncoding encoding=StorageEncoding::FixedLittleEndian) {
    auto c=history(capacity,dtype);c.representation.encoding=encoding;
    if(encoding==StorageEncoding::PackedSigned24LE || encoding==StorageEncoding::PackedSigned24BE){c.history_element=Semantic::PCM;c.shape.axes[1]=Axis::Sample;c.representation.valid_bits=24;}
    NumericGatherRequirements requirements;NumericGatherRequest request{c,{},limits,true};expect(numeric_gather_requirements(request,requirements)==Status::ok);
    alignas(64) std::array<std::byte,4096> plan_bytes{};std::array<BufferSlot,2> slots{};BufferPool plans(plan_bytes,slots,2048);PreparedNumericGather plan;
    expect(prepare_numeric_gather(request,plans,plan)==Status::ok);if(!plan)return;
    auto copied=plan;auto moved=std::move(copied);expect(!copied);expect(same_contract(moved.output_contract(),plan.output_contract()));
    const auto width=[&]{std::size_t w=0;expect(storage_width(c,w)==Status::ok);return w;}();
    const std::size_t row_stride=4*width+8,plane_block=capacity*row_stride+8;
    const std::array<PlaneLayout,2> planes{{{0,{row_stride,width}},{plane_block,{row_stride+8,width}}}};
    std::size_t source_size=0;expect(required_storage(c,planes,source_size)==Status::ok);
    std::vector<std::byte> source_bytes(source_size),time(3*8),coordinates(4*8),mask(capacity);
    for(std::size_t i=0;i<3;++i)f64(time,i,10.0+double(i));
    for(std::size_t i=0;i<4;++i)f64(coordinates,i,70.0+double(i));
    MutableNumericView source{&c,source_bytes,planes,{3,4},{capacity,capacity-1,mask},{{time,coordinates}}};
    for(std::size_t p=0;p<2;++p)for(std::size_t row=0;row<3;++row)for(std::size_t col=0;col<4;++col)
        expect(store_bits(source,{p,row,col},dtype,std::uint64_t(32*p+8*row+col))==Status::ok);
    FrameStamp stamp{1,2,3,4,5,6,-123,"stream.exact",true,true};DataFrameView frame{stamp,source.view()};
    std::vector<std::byte> destination(requirements.payload_bytes,std::byte{0xa5}),metadata(requirements.output_metadata_bytes,std::byte{0xa6}),scratch(requirements.scratch_bytes);
    NumericGatherDestination target{{&plan.output_contract(),destination,plan.output_planes(),{4,1},{},{}},metadata};NumericGatherOutput out;
    const auto allocations=allocation_probe::count;
    expect(gather_numeric_frame(plan,frame,{},target,scratch,out)==Status::ok);
    expect(allocation_probe::count==allocations);
    if(out.payload_bytes){auto v=std::get<NumericView>(out.frame.payload);expect(v.storage.size()==requirements.payload_bytes);expect(v.storage.size()<source_bytes.size());
        for(std::size_t p=0;p<2;++p)for(std::size_t col=0;col<4;++col){std::uint64_t bits=0;expect(load_bits(v,{p,col,0},dtype,bits)==Status::ok);expect(bits==32*p+16+col);}
        expect(out.row.logical_row==2&&out.row.physical_row==1&&out.row.capacity==capacity&&out.row.filled_rows==3);
        expect(out.row.time_coordinate.size()==8&&std::memcmp(out.row.time_coordinate.data(),time.data()+16,8)==0);
        expect(out.frame.stamp.sequence==1&&out.frame.stamp.producer_epoch==6&&out.frame.stamp.timestamp_ticks==-123&&out.frame.stamp.stream_id=="stream.exact"&&out.frame.stamp.discontinuity);
        expect(v.coordinates.axes[0].data()!=coordinates.data());expect(std::memcmp(v.coordinates.axes[0].data(),coordinates.data(),32)==0);
        RetentionRequirements retained;expect(retention_requirements(out.frame,limits.frame,retained)==Status::ok);expect(retained.payload_bytes==requirements.payload_bytes);
        alignas(64) std::array<std::byte,1024> retained_payload{},retained_meta{};std::array<BufferSlot,1> ps{},ms{};BufferPool payload_pool(retained_payload,ps,1024),metadata_pool(retained_meta,ms,1024);OwnedDataFrame owner;
        expect(retain_frame(out.frame,limits.frame,payload_pool,metadata_pool,owner)==Status::ok);
        source_bytes.assign(source_bytes.size(),std::byte{0});coordinates.assign(32,std::byte{0});destination.assign(destination.size(),std::byte{0});metadata.assign(metadata.size(),std::byte{0});
        expect(owner&&owner.view().stamp.stream_id=="stream.exact");
    }
    // Selected mask follows logical row, not physical slot. Full sample backing can contain arbitrary stale values.
    mask[2]=std::byte{1};expect(gather_numeric_frame(plan,frame,{},target,scratch,out)==Status::ok);expect(out.row.silenced);
    const auto unchanged=destination;const auto unchanged_meta=metadata;const auto unchanged_out=out.payload_bytes;
    expect(gather_numeric_frame(plan,frame,{false,3},target,scratch,out)==Status::invalid_argument);
    expect(destination==unchanged&&metadata==unchanged_meta&&out.payload_bytes==unchanged_out);
    auto short_target=target;short_target.value.storage=std::span(destination).first(destination.size()-1);
    expect(gather_numeric_frame(plan,frame,{},short_target,scratch,out)==Status::buffer_too_small);expect(destination==unchanged&&metadata==unchanged_meta);
    expect(gather_numeric_frame(plan,frame,{},target,std::span(scratch).first(scratch.size()-1),out)==Status::buffer_too_small);
    auto aliases_stamp=frame;aliases_stamp.stamp.stream_id={reinterpret_cast<const char*>(scratch.data()),10};
    expect(gather_numeric_frame(plan,aliases_stamp,{},target,scratch,out)==Status::invalid_argument);
    expect(destination==unchanged&&metadata==unchanged_meta);
    auto alias=target;alias.value.storage=source_bytes;
    expect(gather_numeric_frame(plan,frame,{},alias,scratch,out)==Status::invalid_argument);
    source.actual[0]=0;source.coordinates.axes[0]={};frame.payload=source.view();expect(gather_numeric_frame(plan,frame,{},target,scratch,out)==Status::unavailable);
}
void projections_and_atomicity() {
    alignas(64) std::array<std::byte,4096> plan_bytes{};std::array<BufferSlot,2> slots{};BufferPool pool(plan_bytes,slots,2048);PreparedNumericGather plan;
    NumericContract c;
    {
        std::array<std::uint32_t,2> ids{11,77};std::array<std::uint32_t,1> selected{77};std::string clock="owned.clock";
        c=history(17);c.channel_order=ids;c.clock.domain=clock;
        expect(prepare_numeric_gather({c,selected,limits,true},pool,plan)==Status::ok);
    }
    expect(plan.input_contract().clock.domain=="owned.clock");expect(plan.input_contract().channel_order[0]==11);expect(plan.output_contract().channel_order.size()==1&&plan.output_contract().channel_order[0]==77);
    expect(plan.projected_input_contract().semantic==Semantic::History&&plan.projected_input_contract().channel_order.size()==1);
    auto prior=plan;expect(pool.in_use()==1);
    expect(prepare_numeric_gather({plan.input_contract(),{},limits,true},pool,plan)==Status::ok);expect(pool.in_use()==2);
    expect(prepare_numeric_gather({plan.input_contract(),{},limits,true},pool,plan)==Status::exhausted);expect(plan.output_contract().channel_order.size()==2);
    prior={};plan={};expect(pool.in_use()==0);
    c={};c.dtype=DType::UInt64;c.quantity=Quantity::FilledFrameCount;c.unit=NumericUnit::Frames;c.channel_order=std::span(channel_ids).first(1);
    expect(prepare_numeric_gather({c,{},limits},pool,plan)==Status::ok);
    const std::array<PlaneLayout,1> layouts{{{0,{0,0}}}};std::array<std::byte,8> input{};MutableNumericView v{&c,input,layouts,{1,1},{},{}};
    expect(store_bits(v,{},DType::UInt64,UINT64_MAX)==Status::ok);
    auto q=plan.requirements();std::vector<std::byte> dest(q.payload_bytes),meta(q.output_metadata_bytes),scratch(q.scratch_bytes);
    NumericGatherDestination target{{&plan.output_contract(),dest,plan.output_planes(),{1,1},{},{}},meta};NumericGatherOutput output;
    DataFrameView frame{{},v.view()};expect(gather_numeric_frame(plan,frame,{},target,scratch,output)==Status::ok);
    std::uint64_t bits=0;expect(load_bits(std::get<NumericView>(output.frame.payload),{},DType::UInt64,bits)==Status::ok&&bits==UINT64_MAX);
    // Writable descriptor object representation is a legal donor alias, not a fabricated T* in bytes.
    auto mutable_layouts=layouts;frame.payload=NumericView{&c,input,mutable_layouts,{1,1},{},{}};
    auto aliases_layout=target;aliases_layout.value.storage=std::as_writable_bytes(std::span(mutable_layouts));
    expect(gather_numeric_frame(plan,frame,{},aliases_layout,scratch,output)==Status::invalid_argument);expect(mutable_layouts[0].offset==0);
    plan={};c={};c.dtype=DType::UInt64;c.semantic=Semantic::Numeric;c.channel_order=channel_ids;
    c.shape={Rank::Matrix,{Axis::Declared,Axis::Declared},{ExtentBounds{2,2},ExtentBounds{3,3}}};c.representation.layout=ChannelLayout::Interleaved;
    c.representation.encoding=StorageEncoding::FixedBigEndian;
    const std::array<std::uint32_t,1> selected{77};expect(prepare_numeric_gather({c,selected,limits},pool,plan)==Status::ok);
    const std::array<PlaneLayout,2> matrix_planes{{{0,{64,16}},{8,{64,16}}}};std::array<std::byte,112> matrix{};
    MutableNumericView matrix_view{&c,matrix,matrix_planes,{2,3},{},{}};
    for(std::size_t p=0;p<2;++p)for(std::size_t r=0;r<2;++r)for(std::size_t col=0;col<3;++col)expect(store_bits(matrix_view,{p,r,col},DType::UInt64,(UINT64_C(1)<<54)+p*100+r*10+col)==Status::ok);
    q=plan.requirements();dest.resize(q.payload_bytes);meta.resize(q.output_metadata_bytes);scratch.resize(q.scratch_bytes);
    target={{&plan.output_contract(),dest,plan.output_planes(),{2,3},{},{}},meta};frame.payload=matrix_view.view();
    expect(gather_numeric_frame(plan,frame,{},target,scratch,output)==Status::ok);expect(output.payload_bytes==48);
    for(std::size_t r=0;r<2;++r)for(std::size_t col=0;col<3;++col){expect(load_bits(std::get<NumericView>(output.frame.payload),{0,r,col},DType::UInt64,bits)==Status::ok);expect(bits==(UINT64_C(1)<<54)+100+r*10+col);}
    plan={};c=history(17,DType::Int32);c.representation.valid_bits=24;c.representation.encoding=StorageEncoding::FixedLittleEndian;
    c.history_element=Semantic::PCM;c.shape.axes[1]=Axis::Sample;
    expect(prepare_numeric_gather({c,{},limits,true},pool,plan)==Status::ok);
    const std::array<PlaneLayout,2> history_planes{{{0,{16,4}},{272,{16,4}}}};std::array<std::byte,544> backing{};
    std::array<std::byte,16> time{};std::array<std::byte,32> frequencies{};std::array<std::byte,17> mask{};
    f64(time,0,1);f64(time,1,2);for(std::size_t i=0;i<4;++i)f64(frequencies,i,double(i));
    MutableNumericView history_view{&c,backing,history_planes,{2,4},{17,0,mask},{{time,frequencies}}};
    q=plan.requirements();dest.assign(q.payload_bytes,std::byte{0x55});meta.assign(q.output_metadata_bytes,std::byte{0x66});scratch.resize(q.scratch_bytes);
    target={{&plan.output_contract(),dest,plan.output_planes(),{4,1},{},{}},meta};frame.payload=history_view.view();
    auto old_dest=dest,old_meta=meta;
    backing[272+16+12+3]=std::byte{1}; // Last selected logical sample has noncanonical valid24 padding.
    expect(gather_numeric_frame(plan,frame,{},target,scratch,output)==Status::malformed);expect(dest==old_dest&&meta==old_meta);
    backing[303]=std::byte{0};
    f64(time,0,std::bit_cast<double>(UINT64_C(0x7ff8000000000001)));
    expect(gather_numeric_frame(plan,frame,{},target,scratch,output)==Status::ok); // Old unrelated time coordinate is not scanned.
    old_dest=dest;old_meta=meta;expect(gather_numeric_frame(plan,frame,{false,0},target,scratch,output)==Status::malformed);expect(dest==old_dest&&meta==old_meta);
    f64(frequencies,3,std::bit_cast<double>(UINT64_C(0x7ff0000000000000)));
    expect(gather_numeric_frame(plan,frame,{},target,scratch,output)==Status::malformed);expect(dest==old_dest&&meta==old_meta);
}
void prepared_requirements() {
    for(auto capacity:{17u,257u,65537u}){auto c=history(capacity);NumericGatherRequirements r;expect(numeric_gather_requirements({c,{},limits,true},r)==Status::ok);expect(r.payload_bytes==64);expect(r.output_metadata_bytes==32+8+1+128);}
    auto c=history(17);const std::array<std::uint32_t,1> selected{77};NumericGatherRequirements r;
    expect(numeric_gather_requirements({c,selected,limits,true},r)==Status::ok);expect(r.payload_bytes==32);
    const std::array<std::uint32_t,2> duplicate{77,77};expect(numeric_gather_requirements({c,duplicate,limits,true},r)==Status::invalid_argument);
    const std::array<std::uint32_t,1> unknown{999};expect(numeric_gather_requirements({c,unknown,limits,true},r)==Status::incompatible_contract);
    auto small=limits;small.max_output_payload_bytes=31;expect(numeric_gather_requirements({c,selected,small,true},r)==Status::exhausted);
}
void regular_history_coordinates() {
    auto c=history(17,DType::Float32);c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{7,1},{1,4}};
    c.coordinates[1]={};
    alignas(64) std::array<std::byte,2048> plan_bytes{};std::array<BufferSlot,1> slots{};BufferPool pool(plan_bytes,slots,2048);
    PreparedNumericGather plan;expect(prepare_numeric_gather({c,{},limits,true},pool,plan)==Status::ok);
    const std::array<PlaneLayout,2> planes{{{0,{16,4}},{272,{16,4}}}};std::array<std::byte,544> values{};
    NumericView view{&c,values,planes,{3,4},{17,16},{}};auto q=plan.requirements();
    std::vector<std::byte> payload(q.payload_bytes),metadata(q.output_metadata_bytes),scratch(q.scratch_bytes);
    NumericGatherDestination target{{&plan.output_contract(),payload,plan.output_planes(),{4,1},{},{}},metadata};NumericGatherOutput out;
    auto before=allocation_probe::count;expect(gather_numeric_frame(plan,{{},view},{},target,scratch,out)==Status::ok);expect(allocation_probe::count==before);
    std::array<std::byte,8> expected{};f64(expected,0,7.5);expect(out.row.time_coordinate.size()==8);
    expect(std::equal(expected.begin(),expected.end(),out.row.time_coordinate.begin()));
    expect(gather_numeric_frame(plan,{{},view},{false,0},target,scratch,out)==Status::ok);f64(expected,0,7.);
    expect(std::equal(expected.begin(),expected.end(),out.row.time_coordinate.begin()));
}
}
int main(){prepared_requirements();regular_history_coordinates();for(const auto& dtype:dtype_descriptors()){run_history(17,dtype.type);run_history(257,dtype.type,StorageEncoding::FixedBigEndian);}run_history(17,DType::Int32,StorageEncoding::PackedSigned24LE);run_history(17,DType::Int32,StorageEncoding::PackedSigned24BE);projections_and_atomicity();std::printf("numeric gather failures=%d\n",failures);return failures?1:0;}
