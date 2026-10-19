#include <signal_contracts/data_frame.h>
#include <signal_contracts/numeric_policy.h>
#include <array>
#include <algorithm>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <source_location>
#include <string>
#include <limits>
using namespace signal_contracts;
static void require(bool yes,std::source_location at=std::source_location::current()) {
    if(!yes){std::fprintf(stderr,"FAIL %s:%u\n",at.file_name(),at.line());std::abort();}
}
static std::size_t allocations=0;
void* operator new(std::size_t n){++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
constexpr FrameLimits limits{8,1024,16384,4096,128,2048};
const std::array<std::uint32_t,3> channels{11,12,13};
NumericContract base(DType t,Rank r=Rank::Scalar) {
    NumericContract c;c.dtype=t;c.channel_order=std::span(channels).first(1);
    c.shape.rank=r;c.clock.domain="test-clock";
    if(r==Rank::Vector){c.semantic=Semantic::Numeric;c.shape.axes[0]=Axis::Declared;c.shape.extents[0]={1,3};}
    if(r==Rank::Matrix){c.semantic=Semantic::Numeric;c.shape.axes={Axis::Time,Axis::Band};c.shape.extents={ExtentBounds{1,2},ExtentBounds{1,3}};}
    return c;
}
template<class T> void primitive() {
    for(auto rank:{Rank::Scalar,Rank::Vector,Rank::Matrix})for(auto encoding:{StorageEncoding::Native,StorageEncoding::FixedLittleEndian,StorageEncoding::FixedBigEndian}) {
        auto c=base(dtype_of<T>(),rank);c.representation.encoding=encoding;
        alignas(16) std::array<std::byte,256> data{};
        PlaneLayout p{0,{4*sizeof(T),sizeof(T)}};
        MutableNumericView v{&c,data,std::span(&p,1),rank==Rank::Scalar?std::array<std::size_t,2>{1,1}:rank==Rank::Vector?std::array<std::size_t,2>{3,1}:std::array<std::size_t,2>{2,3},{},{}};
        require(validate_mutable(v,limits)==Status::ok);
        std::array<T,3> values{T{},std::numeric_limits<T>::lowest(),std::numeric_limits<T>::max()};
        alignas(16) std::array<std::byte,2048> own_payload{},own_meta{};std::array<BufferSlot,1> ps{},ms{};
        BufferPool payload(own_payload,ps,2048),metadata(own_meta,ms,2048);
        for(auto x:values){auto i=ElementIndex{0,rank==Rank::Scalar?0u:1u,rank==Rank::Matrix?2u:0u};require(store_element(v,i,x)==Status::ok);T y{};require((load_element(v.view(),i,y)==Status::ok && std::bit_cast<std::array<std::byte,sizeof(T)>>(x)==std::bit_cast<std::array<std::byte,sizeof(T)>>(y)));
            OwnedDataFrame owner;require(retain_frame({{},v.view()},limits,payload,metadata,owner)==Status::ok);
            auto copied=owner;auto moved=std::move(copied);require(!copied);
            T z{};require(load_element(std::get<NumericView>(moved.view().payload),i,z)==Status::ok && (std::bit_cast<std::array<std::byte,sizeof(T)>>(x)==std::bit_cast<std::array<std::byte,sizeof(T)>>(z)));
        }
    }
}
void pcm() {
    auto c=base(DType::Int32,Rank::Vector);c.semantic=Semantic::PCM;c.shape.axes[0]=Axis::Sample;c.clock.sample_rate=48000;c.representation.valid_bits=24;
    alignas(16) std::array<std::byte,64> bytes{};PlaneLayout p{0,{3,0}};
    MutableNumericView v{&c,bytes,std::span(&p,1),{3,1},{},{}};
    for(auto e:{StorageEncoding::PackedSigned24LE,StorageEncoding::PackedSigned24BE}) {
        c.representation.encoding=e;require(validate_mutable(v,limits)==Status::ok);
        for(auto x:{-8388608,0,8388607}){require(store_element(v,{0,0,0},std::int32_t(x))==Status::ok);std::int32_t y;require(load_element(v.view(),{0,0,0},y)==Status::ok && y==x);}
        auto saved=bytes;require(store_element(v,{0,0,0},std::int32_t{8388608})==Status::overflow && bytes==saved);
    }
    c.representation.encoding=StorageEncoding::FixedLittleEndian;p.stride_bytes[0]=4;
    for(auto placement:{BitPlacement::LeastSignificant,BitPlacement::MostSignificant}) {
        c.representation.placement=placement;require(store_element(v,{0,0,0},std::int32_t{-8388608})==Status::ok);
        std::int32_t x;require(load_element(v.view(),{0,0,0},x)==Status::ok && x==-8388608);
        bytes[placement==BitPlacement::LeastSignificant?3:0]=std::byte{1};
        require(load_element(v.view(),{0,0,0},x)==Status::malformed);
    }
    c=base(DType::UInt8,Rank::Vector);c.semantic=Semantic::PCM;c.shape.axes[0]=Axis::Sample;c.representation.normalization=Normalization::UnsignedUnit;p.stride_bytes[0]=1;
    auto padding=bytes[3];require(fill_silence(v,limits)==Status::ok);std::uint8_t x;require(load_element(v.view(),{0,0,0},x)==Status::ok && x==128);
    require(bytes[3]==padding); // padding remains untouched
}
void layout_and_values() {
    auto c=base(DType::UInt64,Rank::Vector);c.channel_order=channels;c.representation.layout=ChannelLayout::Interleaved;
    std::array<PlaneLayout,3> planes{{{0,{24,0}},{8,{24,0}},{16,{24,0}}}};
    alignas(16) std::array<std::byte,128> data{};MutableNumericView v{&c,data,planes,{3,1},{},{}};
    require(validate_mutable(v,limits)==Status::ok);
    auto exact=(std::uint64_t{1}<<53)+1;require(store_element(v,{2,2,0},exact)==Status::ok);std::uint64_t x=0;require(load_element(v.view(),{2,2,0},x)==Status::ok && x==exact);
    std::size_t required=777;require(required_storage(c,planes,required)==Status::ok && required==72);
    auto short_view=v;short_view.storage=std::span(data).first(71);require(validate_mutable(short_view,limits)!=Status::ok);
    planes[1].offset=0;require(validate_mutable(v,limits)!=Status::ok);planes[1].offset=8;
    planes[0].stride_bytes[0]=std::numeric_limits<std::size_t>::max();required=777;
    require(required_storage(c,planes,required)==Status::overflow && required==777);planes[0].stride_bytes[0]=24;
    c=base(DType::Float64);PlaneLayout p{};MutableNumericView f{&c,data,std::span(&p,1),{1,1},{},{}};
    require(store_element(f,{},std::numeric_limits<double>::infinity())==Status::ok);
    require(validate_mutable(f,limits)==Status::ok && validate_values(f.view(),ValueDomain::AnyIeee,limits)==Status::ok);
    require(validate_values(f.view(),ValueDomain::FiniteOnly,limits)!=Status::ok);
    auto nan_bits=std::uint64_t{0x7ff8123456789012ULL};double nan=std::bit_cast<double>(nan_bits);
    require(store_element(f,{},nan)==Status::ok && validate_mutable(f,limits)==Status::ok);double loaded=0;
    require(load_element(f.view(),{},loaded)==Status::ok && std::bit_cast<std::uint64_t>(loaded)==nan_bits);
    require(validate_values(f.view(),ValueDomain::FiniteOnly,limits)!=Status::ok);
    auto bad=f;bad.actual[0]=0;require(load_element(bad.view(),{},loaded)==Status::invalid_argument);
    c.representation.alignment=8;auto misaligned=f;misaligned.storage=std::span(data).subspan(1);require(validate_mutable(misaligned,limits)!=Status::ok);
}
void history_and_owner() {
    auto c=base(DType::UInt64,Rank::Matrix);c.semantic=Semantic::History;c.history_element=Semantic::MelEnergy;c.shape.extents[0]={0,2};c.quantity=Quantity::MelEnergy;
    alignas(16) std::array<std::byte,128> source{};PlaneLayout p{0,{24,8}};
    std::array<std::byte,2> masks{std::byte{0},std::byte{1}};
    MutableNumericView v{&c,source,std::span(&p,1),{2,3},{2,1,masks},{}};
    require(validate_mutable(v,limits)==Status::ok);require(store_element(v,{0,0,1},std::uint64_t{9007199254740993ULL})==Status::ok);
    NumericContract row_contract;PlaneLayout row_layout;NumericView row;
    require(history_row(v.view(),0,row,row_contract,std::span(&row_layout,1))==Status::ok);
    std::uint64_t value=0;require(load_element(row,{0,1,0},value)==Status::ok && value==9007199254740993ULL);
    require(history_row(v.view(),1,row,row_contract,std::span(&row_layout,1))==Status::ok);
    require(load_element(row,{0,1,0},value)==Status::ok && value==0);
    alignas(16) std::array<std::byte,2048> payload_bytes{},meta_bytes{};
    std::array<BufferSlot,2> payload_slots{},meta_slots{};
    BufferPool payload(payload_bytes,payload_slots,1024),metadata(meta_bytes,meta_slots,1024);
    DataFrameView frame{{1,2,3,4,5,6,-7,"stream",true,false},v.view()};OwnedDataFrame owned;
    require(retain_frame(frame,limits,payload,metadata,owned)==Status::ok);
    auto moved=std::move(owned);source.fill(std::byte{0});masks.fill(std::byte{0});
    auto retained=moved.view();require(retained.stamp.stream_id=="stream" && retained.stamp.timestamp_ticks==-7);
    auto n=std::get<NumericView>(retained.payload);require(n.contract->channel_order[0]==11 && n.contract->clock.domain=="test-clock");
    require(load_element(n,{0,0,1},value)==Status::ok && value==9007199254740993ULL);
    require(load_element(n,{0,1,1},value)==Status::ok && value==0);
    auto copy=moved;require(copy.view().stamp.stream_id=="stream");
    OwnedDataFrame second;require(retain_frame(frame,limits,payload,metadata,second)==Status::ok);
    auto before=moved.view();require(retain_frame(frame,limits,payload,metadata,moved)==Status::exhausted && moved.view().stamp.sequence==before.stamp.sequence);
    v.actual[0]=0;require(validate_mutable(v,limits)==Status::ok);
}
void structured() {
    StructuredContract c{"wled-audiosync",2,"LE",{44,44}};std::array<std::byte,44> bytes{};
    DataFrameView f{{},StructuredView{&c,bytes}};require(validate_view(f,limits)==Status::ok);
    auto bad=f;std::get<StructuredView>(bad.payload).bytes=std::span(bytes).first(43);require(validate_view(bad,limits)!=Status::ok);
    auto n=base(DType::UInt8,Rank::Vector);require(!same_contract(DataContract{c},DataContract{n}));
    alignas(16) std::array<std::byte,256> pbytes{},mbytes{};std::array<BufferSlot,1> ps{},ms{};
    BufferPool pp(pbytes,ps,256),mp(mbytes,ms,256);OwnedDataFrame owner;
    require(retain_frame(f,limits,pp,mp,owner)==Status::ok);bytes.fill(std::byte{0xff});
    auto view=std::get<StructuredView>(owner.view().payload);require(view.bytes[0]==std::byte{0} && view.contract->schema=="wled-audiosync");
    auto d=c;d.version=3;require(!same_contract(DataContract{c},DataContract{d}));
    NumericLiteral exact=std::uint64_t{18446744073709551615ULL};require(std::get<std::uint64_t>(exact)==UINT64_MAX);
}
void metadata_and_failures() {
    auto c=base(DType::Float64,Rank::Vector);c.shape.extents[0]={1,2};
    std::array<std::uint32_t,1> order{71};std::string clock="owned-clock",stream="owned-stream";
    c.channel_order=order;c.clock.domain=clock;c.coordinates[0].mode=CoordinateMode::ExplicitFloat64LE;c.coordinates[0].unit=NumericUnit::Hertz;
    alignas(16) std::array<std::byte,64> data{};std::array<std::byte,16> coords{};
    for(std::size_t i=0;i<2;++i){auto bits=std::bit_cast<std::uint64_t>(double(i)+.5);for(int j=0;j<8;++j)coords[i*8+j]=std::byte((bits>>(j*8))&255);}
    PlaneLayout plane{0,{8,0}};MutableNumericView v{&c,data,std::span(&plane,1),{2,1},{},{}};v.coordinates.axes[0]=coords;
    require(validate_mutable(v,limits)==Status::ok);
    alignas(16) std::array<std::byte,2048> storage{},meta{};std::array<BufferSlot,1> pslots{},mslots{};
    BufferPool payload(storage,pslots,2048),metadata(meta,mslots,2048);OwnedDataFrame owner;
    DataFrameView f{{1,2,3,4,5,6,7,stream},v.view()};const auto before=allocations;
    require(retain_frame(f,limits,payload,metadata,owner)==Status::ok && allocations==before);
    order[0]=99;clock.assign("destroy");stream.assign("destroy");coords.fill(std::byte{0});
    auto n=std::get<NumericView>(owner.view().payload);
    require(n.contract->channel_order[0]==71 && n.contract->clock.domain=="owned-clock" && owner.view().stamp.stream_id=="owned-stream");
    require(n.coordinates.axes[0][6]==std::byte{0xe0}); // IEEE .5 LE exact owned bits
    RetentionRequirements requirement{777,777,777};auto tiny=limits;tiny.max_metadata_bytes=1;
    require(validate_view(f,tiny)!=Status::ok && retention_requirements(f,tiny,requirement)!=Status::ok && requirement.payload_bytes==777);
    c.quantity=Quantity::FilledFrameCount;require(validate_contract(DataContract{c},limits)==Status::incompatible_contract);
    c=base(DType::UInt64);c.quantity=Quantity::FilledFrameCount;c.unit=NumericUnit::Frames;require(validate_contract(DataContract{c},limits)==Status::ok);
    PlaneLayout scalar{};MutableNumericView count{&c,data,std::span(&scalar,1),{1,1},{},{}};
    require(store_element(count,{},UINT64_MAX)==Status::ok);std::uint64_t exact=0;require(load_element(count.view(),{},exact)==Status::ok && exact==UINT64_MAX);
    c.representation.valid_bits=255;require(load_element(count.view(),{},exact)==Status::incompatible_contract && exact==UINT64_MAX);
    c=base(DType::UInt8,Rank::Vector);c.shape.extents[0]={1,64};plane={0,{1,0}};
    auto alias=v;alias.contract=&c;alias.planes=std::span(&plane,1);alias.actual={64,1};alias.coordinates={};alias.storage=storage;
    std::array<BufferSlot,1> other_slot{};BufferPool other(meta,other_slot,2048);OwnedDataFrame failed;
    // Pool storage aliases donor even though its slot is currently occupied: use a free new slot table.
    std::array<BufferSlot,1> alias_slots{};BufferPool alias_pool(storage,alias_slots,2048);
    require(retain_frame({{},alias.view()},limits,alias_pool,other,failed)==Status::invalid_argument && !failed && alias_pool.in_use()==0 && other.in_use()==0);
    auto unavailable=DataFrameView{{},StructuredView{}};StructuredContract packet{"custom",1,"bytes",{2,2}};
    unavailable.stamp.available=false;unavailable.payload=StructuredView{&packet,{}};
    require(validate_view(unavailable,limits)==Status::unavailable);
    require(retain_frame(unavailable,limits,alias_pool,other,failed)==Status::ok && validate_view(failed.view(),limits)==Status::unavailable);
    auto structured=std::get<StructuredView>(failed.view().payload);require(structured.contract->schema=="custom");
    alignas(16) std::array<std::byte,2048> rollback_payload{},rollback_meta{};
    std::array<BufferSlot,1> rp{},rm{};BufferPool rollback_pool(rollback_payload,rp,2048),full_meta(rollback_meta,rm,2048);
    BufferHandle occupied;require(full_meta.acquire(1,occupied)==Status::ok);OwnedDataFrame rejected;
    require(retain_frame(unavailable,limits,rollback_pool,full_meta,rejected)==Status::exhausted && rollback_pool.in_use()==0 && full_meta.in_use()==1 && !rejected);
    occupied.reset();alignas(16) std::array<std::byte,2049> unaligned{};BufferPool badmeta{std::span(unaligned).subspan(1),rm,2048};
    c=base(DType::UInt64);count.contract=&c;
    require(retain_frame({{},count.view()},limits,rollback_pool,badmeta,rejected)==Status::invalid_argument && rollback_pool.in_use()==0 && badmeta.in_use()==0);

}
void per_plane_history() {
    auto c=base(DType::UInt16,Rank::Matrix);c.channel_order=std::span(channels).first(2);c.semantic=Semantic::History;c.history_element=Semantic::MelEnergy;c.shape.extents[0]={0,2};
    std::array<PlaneLayout,2> planes{{{0,{8,2}},{20,{10,2}}}};std::array<std::byte,64> data{};
    MutableNumericView v{&c,data,planes,{2,3},{2,1,{}},{}};require(validate_mutable(v,limits)==Status::ok);
    require(store_element(v,{1,0,2},std::uint16_t{151})==Status::ok);
    NumericView out;NumericContract rc;std::array<PlaneLayout,2> rp{};
    require(history_row(v.view(),0,out,rc,rp)==Status::ok);std::uint16_t value=0;require(load_element(out,{1,2,0},value)==Status::ok && value==151);
    auto borrowed=v.view();require(history_row(borrowed,0,out,rc,planes)==Status::invalid_argument);
}
void literal_identity() {
    NumericLiteral a=std::bit_cast<float>(std::uint32_t{0x7fc01234});auto b=a;
    require(literal_equal(a,b));
    b=std::bit_cast<float>(std::uint32_t{0x7fc01235});require(!literal_equal(a,b));
    a=-0.0;b=0.0;require(!literal_equal(a,b));a=std::uint64_t{UINT64_MAX};b=a;require(literal_equal(a,b));
    b=std::int64_t{-1};require(!literal_equal(a,b));require(validate_literal(a)==Status::ok);
}
namespace history_integrity_repairs {
constexpr FrameLimits limits{8,4096,16384,4096,512,2048};
std::array<std::uint32_t,1> ids{4};
NumericContract history(Semantic e,Axis a) {NumericContract c;c.dtype=DType::UInt8;c.channel_order=ids;c.semantic=Semantic::History;c.history_element=e;c.shape.rank=Rank::Matrix;c.shape.axes={Axis::Time,a};c.shape.extents={ExtentBounds{0,2},ExtentBounds{1,3}};c.clock.domain="clock";return c;}
void semantic_row_projection(int& failed){auto expect=[&](bool okay,const char* name){if(!okay){std::printf("FAIL %s\n",name);++failed;}};
for(auto e:{Semantic::PCM,Semantic::Window,Semantic::Windowed,Semantic::SpectrumMagnitude,Semantic::MelEnergy,Semantic::Numeric})for(auto a:{Axis::Sample,Axis::Bin,Axis::Band,Axis::Declared}) {
 auto c=history(e,a);std::array<std::byte,6> bytes{};PlaneLayout p{0,{3,1}};NumericView v{&c,bytes,std::span(&p,1),{2,3},{2,0,{}},{}};
 bool expected=true; // Descriptive tags do not constrain primitive shape or axis labels.
 expect((validate_contract(DataContract{c},limits)==Status::ok)==expected,"semantic contract admission");
 if(expected){NumericView row;NumericContract rc;PlaneLayout rp;expect(history_row(v,0,row,rc,std::span(&rp,1))==Status::ok && validate_view({{},row},limits)==Status::ok,"valid semantic row projection");}
}
}
void configure_alias(int mode,NumericContract& c,NumericView& v,PlaneLayout& rp,NumericContract& rc,NumericView& row){
 if(mode==0)v.storage=std::as_bytes(std::span(&rp,1));
 if(mode==1)v.storage=std::as_bytes(std::span(&rc,1));
 if(mode==2)v.storage=std::as_bytes(std::span(&row,1));
 if(mode==3)c.clock.domain={reinterpret_cast<const char*>(&rp),sizeof(rp)};
 if(mode==4)v.ring.silence_mask=std::as_bytes(std::span(&rp,1)).first(2);
 if(mode==5){c.channel_order=std::span(&rc.clock.sample_rate,1);rc.clock.sample_rate=97;}
 if(mode==6){c.coordinates[0].mode=CoordinateMode::ExplicitFloat64LE;v.coordinates.axes[0]=std::as_bytes(std::span(&rp,1)).first(16);}
}
void alias_case(int mode,int& failed){auto expect=[&](bool okay,const char* name){if(!okay){std::printf("FAIL %s\n",name);++failed;}};
 auto c=history(Semantic::MelEnergy,Axis::Band);PlaneLayout p{0,{3,1}},rp{};NumericContract rc;NumericView row;std::array<std::byte,6> bytes{};
 NumericView v{&c,bytes,std::span(&p,1),{2,3},{2,0,{}},{}};
 configure_alias(mode,c,v,rp,rc,row);
 expect(validate_view({{},v},limits)==Status::ok,"alias fixture is structurally valid");
 auto oldrp=rp;auto oldrc=rc;auto oldrow=row;auto oldbytes=bytes;
 auto s=history_row(v,1,row,rc,std::span(&rp,1));
 expect(s==Status::invalid_argument,"writable output/donor alias refused");
 expect(std::memcmp(&rp,&oldrp,sizeof(rp))==0 && std::memcmp(&rc,&oldrc,sizeof(rc))==0 && std::memcmp(&row,&oldrow,sizeof(row))==0 && bytes==oldbytes,"alias refusal atomic");
}
void alias_refusal(int& failed){for(int mode=0;mode<7;++mode)alias_case(mode,failed);}
void run(){int failed=0;semantic_row_projection(failed);alias_refusal(failed);
std::printf("repair regression failures=%d\n",failed);require(failed==0);
}

}
static void select_silence_alias(int alias,MutableNumericView& view,NumericContract& contract,PlaneLayout& plane,
                                 std::array<std::uint32_t,1>& ids,char (&clock)[6]){
    if(alias==0)view.storage=std::as_writable_bytes(std::span(&view,1));
    if(alias==1)view.storage=std::as_writable_bytes(std::span(&contract,1));
    if(alias==2)view.storage=std::as_writable_bytes(std::span(&plane,1));
    if(alias==3)view.storage=std::as_writable_bytes(std::span(ids));
    if(alias==4)view.storage=std::as_writable_bytes(std::span(clock));
}
static void silence_alias_case(int alias){
    constexpr FrameLimits cap{8,4096,16384,4096,512,2048};
    std::array<std::uint32_t,1> ids{7};char clock[]="clock";
        NumericContract c;c.dtype=DType::UInt8;c.channel_order=ids;c.clock.domain=clock;
        PlaneLayout p{};std::array<std::byte,1> bytes{std::byte{99}};
        MutableNumericView v{&c,bytes,std::span(&p,1),{1,1},{},{}};
        select_silence_alias(alias,v,c,p,ids,clock);
        require(validate_mutable(v,cap)==Status::ok);
        const auto oldv=v;const auto oldc=c;const auto oldp=p;const auto oldids=ids;std::array<char,6> oldclock{};std::copy(std::begin(clock),std::end(clock),oldclock.begin());
        require(fill_silence(v,cap)==Status::invalid_argument);
        require(std::memcmp(&v,&oldv,sizeof(v))==0&&std::memcmp(&c,&oldc,sizeof(c))==0&&std::memcmp(&p,&oldp,sizeof(p))==0&&ids==oldids&&std::equal(std::begin(clock),std::end(clock),oldclock.begin()));
}
static void silence_alias_refusals(){for(int alias=0;alias<5;++alias)silence_alias_case(alias);
}
static void truncated_history_row_atomicity(){
    auto c=history_integrity_repairs::history(Semantic::MelEnergy,Axis::Band);
    std::array<std::byte,4> truncated{};PlaneLayout p{0,{3,1}},rp{99,{77,66}};NumericContract projected;NumericView row;
    NumericView v{&c,truncated,std::span(&p,1),{2,3},{2,0,{}},{}};
    const auto before=row;const auto oldplane=rp;const auto oldcontract=projected;
    require(history_row(v,1,row,projected,std::span(&rp,1))!=Status::ok);
    require(std::memcmp(&row,&before,sizeof(row))==0&&std::memcmp(&rp,&oldplane,sizeof(rp))==0&&std::memcmp(&projected,&oldcontract,sizeof(projected))==0);
}
static void primitive_roundtrips(){require(dtype_descriptors().size()==10);
    primitive<std::int8_t>();primitive<std::uint8_t>();primitive<std::int16_t>();primitive<std::uint16_t>();
    primitive<std::int32_t>();primitive<std::uint32_t>();primitive<std::int64_t>();primitive<std::uint64_t>();
    primitive<float>();primitive<double>();
}
int main(){silence_alias_refusals();truncated_history_row_atomicity();history_integrity_repairs::run();
    literal_identity();metadata_and_failures();per_plane_history();primitive_roundtrips();
    pcm();layout_and_values();history_and_owner();structured();std::puts("numeric foundation tests passed");}
