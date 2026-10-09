#include <pipeline/numeric_projection.h>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <limits>
#include <cstring>
#include <bit>
using namespace signal_contracts;
namespace allocation_probe {std::size_t count=0;}
void* operator new(std::size_t size){++allocation_probe::count;auto* p=std::malloc(size?size:1);if(!p)throw std::bad_alloc();return p;}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace { int failures=0, checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::printf("FAIL %d: %s\n",__LINE__,#x);++failures;} } while(false)
constexpr FrameLimits limits{4,4096,32768,8192,128,4096};
const std::array<std::uint32_t,1> channels{31};
struct Pools {
    alignas(64) std::array<std::byte,8192> payload{},metadata{};
    std::array<BufferSlot,16> ps{},ms{};
    BufferPool p{payload,ps,512},m{metadata,ms,512};
};
std::shared_ptr<const pipeline::NumericDeliveryContext> context(const DataContract& contract,int node=4,std::uint16_t port=0) {
    auto result=std::make_shared<pipeline::NumericDeliveryContext>();
    result->endpoint={"numeric.graph",node,domain::PortDirection::Output,port};
    CHECK(domain::own_processing_contract(contract,limits,8192,result->contract)==Status::ok);return result;
}
template<class T> void scalar_value(T value,const char* text,bool exact) {
    NumericContract contract;contract.dtype=dtype_of<T>();contract.channel_order=channels;contract.clock={"clock",{1,1000},1000};
    alignas(64) std::array<std::byte,8> bytes{};const std::array<PlaneLayout,1> plane{{{0,{sizeof(T),sizeof(T)}}}};
    MutableNumericView writable{&contract,std::span(bytes).first(sizeof(T)),plane,{1,1},{},{}};CHECK(store_element(writable,{},value)==Status::ok);
    DataFrameView frame{{},writable.view()};auto ctx=context(contract,4,1);Pools pools;pipeline::DeliveredNumericFrame owner;
    const auto allocations=allocation_probe::count;
    CHECK(pipeline::retain_numeric_delivery(ctx,{3,7},frame,Status::ok,limits,pools.p,pools.m,owner)==Status::ok);
    CHECK(allocation_probe::count==allocations);
    bytes.fill(std::byte{0});contract.clock.domain={};
    auto retained=owner.frame.view();auto numeric=std::get<NumericView>(retained.payload);NumericLiteral literal;
    CHECK(pipeline::read_numeric_literal(numeric,{},literal)==Status::ok);
    CHECK(pipeline::exact_scalar_text(literal)==text);
    CHECK(numeric.contract->clock.domain=="clock");
    float projected=-7;bool loss=false;
    CHECK(pipeline::project_literal(literal,false,projected,loss)==(exact?Status::ok:Status::incompatible_contract));
    if(!exact)CHECK(projected==-7);
    CHECK(pipeline::project_literal(literal,true,projected,loss)==Status::ok);CHECK(loss==!exact);
    std::array<float,1> target{-8};pipeline::FloatProjectionReport report{42,13};
    CHECK(pipeline::project_numeric_float(numeric,0,{false,1},target,report)==(exact?Status::ok:Status::incompatible_contract));
    if(!exact){CHECK(target[0]==-8);CHECK(report.elements==42);}
    CHECK(pipeline::project_numeric_float(numeric,0,{true,1},target,report)==Status::ok);
    CHECK(report.elements==1 && report.inexact_elements==std::size_t(!exact));
    pipeline::NumericDeliveryMailbox<pipeline::DeliveredNumericFrame,2> mailbox;
    const auto noalloc=allocation_probe::count;CHECK(mailbox.push(owner));CHECK(mailbox.push(owner));CHECK(!mailbox.push(owner));
    CHECK(mailbox.dropped()==1);CHECK(mailbox.pop().has_value());mailbox.clear();CHECK(mailbox.size()==0);
    CHECK(allocation_probe::count==noalloc);
}
void all_dtypes() {
    scalar_value(std::int8_t{-128},"-128",true);scalar_value(std::uint8_t{255},"255",true);
    scalar_value(std::int16_t{-32768},"-32768",true);scalar_value(std::uint16_t{65535},"65535",true);
    scalar_value(std::numeric_limits<std::int32_t>::min(),"-2147483648",true);scalar_value(std::numeric_limits<std::uint32_t>::max(),"4294967295",false);
    scalar_value(std::numeric_limits<std::int64_t>::min(),"-9223372036854775808",true);scalar_value(std::numeric_limits<std::uint64_t>::max(),"18446744073709551615",false);
    scalar_value(float{0.5},"0.5",true);scalar_value(double{0.1},"0.10000000000000001",false);
    scalar_value(std::uint64_t{9007199254740993ULL},"9007199254740993",false);
    float x=9;bool loss=false;CHECK(pipeline::project_literal(std::numeric_limits<double>::infinity(),true,x,loss)==Status::malformed);CHECK(x==9);
    CHECK(pipeline::project_literal(std::numeric_limits<double>::max(),true,x,loss)==Status::malformed);CHECK(x==9);
}
void lifecycle() {
    pipeline::ScalarPresentation timed;
    FrameStamp timedStamp;timedStamp.producer_epoch=99;
    for(std::uint64_t i=0;i<140;++i){timedStamp.sequence=i;timedStamp.timestamp_ticks=std::int64_t(i*i);CHECK(timed.accept(timedStamp,std::uint64_t{7}));}
    const auto seconds=timed.relative_seconds(ClockContract{"source",{1,1000},1000});
    CHECK(seconds.size()==128 && seconds.front()==(144.-19321.)/1000. && seconds.back()==0);
    CHECK(timed.relative_seconds(ClockContract{}).empty());
    timedStamp.sequence=140;timedStamp.timestamp_ticks=19321;timed.accept(timedStamp,std::uint64_t{8});
    CHECK(timed.relative_seconds(ClockContract{"source",{1,1000},1000}).empty());
    timed.clear();timedStamp.sequence=0;timedStamp.timestamp_ticks=std::numeric_limits<std::int64_t>::max()-1;timed.accept(timedStamp,std::uint64_t{1});
    timedStamp.sequence=1;++timedStamp.timestamp_ticks;timed.accept(timedStamp,std::uint64_t{2});
    CHECK(timed.relative_seconds(ClockContract{"source",{1,1000},1000})==std::vector<double>({-.001,0}));
    pipeline::ScalarPresentation scalar;
    FrameStamp stamp; stamp.sequence=0; stamp.producer_epoch=0; stamp.discontinuity=true;
    CHECK(scalar.accept(stamp,std::uint64_t{9007199254740993ULL}));
    CHECK(scalar.size()==1);
    CHECK(!scalar.accept(stamp,std::uint64_t{9007199254740993ULL}));
    CHECK(scalar.size()==1);
    stamp.discontinuity=false;
    for(std::uint64_t i=1;i<140;++i){stamp.sequence=i;CHECK(scalar.accept(stamp,i));}
    CHECK(scalar.size()==128);
    stamp.sequence=143;CHECK(scalar.accept(stamp,std::uint64_t{143}));CHECK(scalar.size()==128);
    stamp.producer_epoch=1;stamp.sequence=0;CHECK(scalar.accept(stamp,std::uint64_t{0}));CHECK(scalar.size()==1);
    scalar.clear();CHECK(scalar.size()==0);CHECK(scalar.accept(stamp,std::uint64_t{0}));
    CHECK(scalar.latest_text()=="0");stamp.epoch=3;stamp.sequence=1;CHECK(scalar.accept(stamp,std::uint64_t{1}));
    stamp.discontinuity=false;stamp.epoch=4;stamp.sequence=2;CHECK(scalar.accept(stamp,std::uint64_t{2}));CHECK(scalar.size()==3);
    stamp.epoch=2;stamp.sequence=3;CHECK(!scalar.accept(stamp,std::uint64_t{3}));CHECK(scalar.latest_text()=="2");
    pipeline::ScalarPresentation independent;CHECK(independent.accept(stamp,std::uint64_t{77}));CHECK(independent.latest_text()=="77");CHECK(scalar.latest_text()=="2");
}
void history_delivery() {
    NumericContract history;history.dtype=DType::UInt64;history.semantic=Semantic::History;history.history_element=Semantic::MelEnergy;
    history.quantity=Quantity::MelEnergy;history.channel_order=channels;history.clock={"clock",{1,1000},1000};
    history.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,3},ExtentBounds{2,2}}};
    history.coordinates[1]={CoordinateMode::Regular,NumericUnit::Hertz,{70,1},{10,1}};
    auto ctx=context(history);auto row=history;row.semantic=history.history_element;row.history_element=Semantic::Numeric;
    row.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};row.coordinates={history.coordinates[1],AxisCoordinates{}};
    const std::array<PlaneLayout,1> planes{{{0,{8,8}}}};alignas(64) std::array<std::byte,16> bytes{};std::array<std::byte,8> time{};
    const auto time_bits=std::bit_cast<std::uint64_t>(1.25);for(unsigned i=0;i<8;++i)time[i]=std::byte((time_bits>>(8*i))&255);
    MutableNumericView source{&row,bytes,planes,{2,1},{},{}};Pools pools;
    pipeline::HistoryPresentation trail({3,2,56,{true,2}}),other({3,2,56,{true,2}});
    NumericGatherOutput gathered;gathered.row.history=true;gathered.row.capacity=3;gathered.row.time_coordinate=time;gathered.row.mask_present=true;
    gathered.frame={{},source.view()};gathered.frame.stamp.discontinuity=true;
    const std::array<std::byte,1> selected_mask{std::byte{0}};gathered.row.selected_mask=selected_mask;
    const auto make=[&](std::uint64_t ordinal,std::uint64_t count,std::uint64_t value,pipeline::DeliveredNumericHistoryRow& out) {
        CHECK(store_element(source,{0,0,0},value)==Status::ok);CHECK(store_element(source,{0,1,0},value+1)==Status::ok);
        gathered.frame.stamp.sequence=ordinal;gathered.frame.stamp.timestamp_ticks=std::int64_t(ordinal*10);gathered.row.filled_rows=count;gathered.row.logical_row=std::size_t(count-1);gathered.row.physical_row=std::size_t((ordinal-1)%3);
        const auto before=allocation_probe::count;
        CHECK(pipeline::retain_numeric_history_row(ctx,{7,2},pipeline::NumericHistoryEvent::Row,gathered,count,3,ordinal,limits,pools.p,pools.m,out)==Status::ok);
        CHECK(allocation_probe::count==before);
    };
    pipeline::DeliveredNumericHistoryRow event;make(1,1,9007199254740993ULL,event);
    time.fill(std::byte{0});bytes.fill(std::byte{0});
    CHECK(event.has_time_coordinate && event.time_coordinate[6]!=std::byte{0});CHECK(event.mask_present);
    CHECK(trail.accept(event)==Status::ok);CHECK(trail.size()==1 && trail.filled_count()==1 && trail.precision_loss());
    CHECK(trail.render_snapshot().size()==3 && trail.render_snapshot()[0]==std::vector<float>({0,0}) && trail.render_snapshot()[1]==std::vector<float>({0,0}));
    CHECK(trail.accept(event)==Status::would_block);CHECK(trail.size()==1);
    CHECK(other.accept(event)==Status::ok);gathered.frame.stamp.discontinuity=false;
    make(2,2,4,event);CHECK(trail.accept(event)==Status::ok);CHECK(trail.size()==2 && trail.precision_loss());
    make(3,3,6,event);CHECK(trail.accept(event)==Status::ok);make(4,3,8,event);CHECK(trail.accept(event)==Status::ok);
    auto snapshot=trail.render_snapshot();CHECK(snapshot.size()==3 && snapshot[0][0]==4 && snapshot[2][0]==8);CHECK(!trail.precision_loss());CHECK(other.size()==1);
    auto axes=trail.render_axes();CHECK(!axes.seconds && axes.ages==std::vector<double>({0,1,2}));
    CHECK(axes.column_axis==pipeline::HistoryColumnAxis::Hertz && axes.columns[0]==70 && axes.columns[1]==80);
    make(6,3,12,event);CHECK(trail.accept(event)==Status::ok);CHECK(trail.size()==1 && trail.filled_count()==3);
    pipeline::DeliveredNumericHistoryRow unavailable;unavailable.compact.context=ctx;unavailable.compact.delivery={7,2};unavailable.capacity=3;
    CHECK(trail.accept(unavailable)==Status::unavailable);CHECK(trail.size()==1 && !trail.available());
    unavailable.event=pipeline::NumericHistoryEvent::Reset;CHECK(trail.accept(unavailable)==Status::ok);CHECK(trail.size()==0);
    make(7,1,14,event);CHECK(trail.accept(event)==Status::ok);CHECK(trail.size()==1);
    make(8,2,16,event);event.compact.delivery={8,3};CHECK(trail.accept(event)==Status::ok);CHECK(trail.size()==2);
    pipeline::HistoryPresentation cold({3,2,56,{true,2}});CHECK(cold.accept(unavailable)==Status::ok);CHECK(cold.accept(event)==Status::ok);CHECK(cold.size()==1);
    pipeline::HistoryPresentation insufficient({3,2,55,{true,2}});CHECK(insufficient.accept(event)==Status::buffer_too_small);CHECK(insufficient.size()==0);
    event.compact.delivery.epoch=1;CHECK(trail.accept(event)==Status::would_block);CHECK(trail.size()==2);
    trail.clear();CHECK(trail.size()==0 && trail.filled_count()==0 && !trail.latest().compact.frame);
    // Full History matrices are never retained through the incremental callback API.
    NumericView full{&history,bytes,planes,{1,2},{3,0,{}},{}};pipeline::DeliveredNumericFrame refused;
    CHECK(pipeline::retain_numeric_delivery(ctx,{7,2},DataFrameView{{},full},Status::ok,limits,pools.p,pools.m,refused)==Status::incompatible_contract);
    CHECK(!refused.frame);
}
void packet_and_refusal() {
    std::string schema="wled.audio-sync";StructuredContract contract{schema,2,"LE",{44,44}};
    std::array<std::byte,44> packet{};packet[3]=std::byte{99};auto ctx=context(contract,8);Pools pools;
    pipeline::DeliveredNumericFrame owner;DataFrameView frame{{},StructuredView{&contract,packet}};
    const auto before=allocation_probe::count;
    CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},frame,Status::ok,limits,pools.p,pools.m,owner)==Status::ok);
    CHECK(allocation_probe::count==before);
    packet.fill(std::byte{0});schema.assign("changed");
    auto retained=std::get<StructuredView>(owner.frame.view().payload);
    CHECK(retained.contract->schema=="wled.audio-sync" && retained.contract->version==2 && retained.contract->encoding=="LE");
    CHECK(retained.bytes.size()==44 && retained.bytes[3]==std::byte{99});
    StructuredContract wrong{"wrong",1,"LE",{44,44}};frame.payload=StructuredView{&wrong,packet};
    CHECK(pipeline::retain_numeric_delivery(ctx,{2,1},frame,Status::ok,limits,pools.p,pools.m,owner)==Status::incompatible_contract);
    CHECK(owner.delivery.graph_revision==1);
    alignas(64) std::array<std::byte,512> payload{},metadata{};std::array<BufferSlot,1> ps{},ms{};
    BufferPool p(payload,ps,512),m(metadata,ms,512);frame=owner.frame.view();pipeline::DeliveredNumericFrame only;
    CHECK(pipeline::retain_numeric_delivery(ctx,{3,1},frame,Status::ok,limits,p,m,only)==Status::ok);
    CHECK(pipeline::retain_numeric_delivery(ctx,{4,1},frame,Status::ok,limits,p,m,only)==Status::exhausted);
    CHECK(only.delivery.graph_revision==3 && only.frame);
    pipeline::NumericDeliveryMailbox<pipeline::DeliveredNumericFrame,1> queue;CHECK(queue.push(std::move(only)));
    CHECK(p.in_use()==1 && m.in_use()==1);queue.clear();CHECK(p.in_use()==0 && m.in_use()==0);
}
void index_axes() {
    for(auto [axis,semantic,expected]:std::array{
        std::tuple{Axis::Band,Semantic::MelEnergy,pipeline::HistoryColumnAxis::Band},
        std::tuple{Axis::Bin,Semantic::SpectrumMagnitude,pipeline::HistoryColumnAxis::Bin},
        std::tuple{Axis::Sample,Semantic::PCM,pipeline::HistoryColumnAxis::Sample},
        std::tuple{Axis::Declared,Semantic::Numeric,pipeline::HistoryColumnAxis::Value}}) {
        NumericContract contract;contract.semantic=Semantic::History;contract.history_element=semantic;contract.channel_order=channels;
        contract.shape={Rank::Matrix,{Axis::Time,axis},{ExtentBounds{0,3},ExtentBounds{2,2}}};
        auto ctx=context(contract);pipeline::HistoryPresentation history({3,2,56,{true,2}});
        pipeline::DeliveredNumericHistoryRow event;event.compact.context=ctx;event.event=pipeline::NumericHistoryEvent::Empty;event.capacity=3;
        CHECK(history.accept(event)==Status::ok);const auto axes=history.render_axes();CHECK(!axes.seconds);
        CHECK(history.render_snapshot()==std::vector<std::vector<float>>({{0,0},{0,0},{0,0}}));
        CHECK(axes.column_axis==expected && axes.columns==std::vector<double>({0,1}));
    }
}
void pcm_typed_padding() {
    for(auto [dtype,width]:std::array{std::pair{DType::UInt8,8u},std::pair{DType::UInt16,12u},std::pair{DType::UInt64,64u}}) {
        NumericContract history;history.dtype=dtype;history.semantic=Semantic::History;history.history_element=Semantic::PCM;
        history.channel_order=channels;history.shape={Rank::Matrix,{Axis::Time,Axis::Sample},{ExtentBounds{0,3},ExtentBounds{1,1}}};
        history.representation.valid_bits=width;
        history.representation.normalization=Normalization::UnsignedUnit;
        pipeline::HistoryPresentation trail({3,1,28,{true,1}});
        pipeline::DeliveredNumericHistoryRow empty;empty.event=pipeline::NumericHistoryEvent::Empty;empty.capacity=3;empty.compact.context=context(history);
        CHECK(trail.accept(empty)==Status::ok);
        const float midpoint=float(std::uint64_t{1}<<(width-1));
        CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{midpoint},{midpoint},{midpoint}}));
    }
}
void padded_cold_snapshot() {
    NumericContract history;history.semantic=Semantic::History;history.history_element=Semantic::Numeric;history.channel_order=channels;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,5},ExtentBounds{2,2}}};
    history.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    const auto ctx=context(history);Pools pools;
    const std::array<float,10> values{91,92,93,94,95,96,1,2,3,4};
    const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};std::array<std::byte,40> times{};
    for(auto [row,value]:std::array{std::pair{3,12.},std::pair{4,12.5}}) {
        const auto bits=std::bit_cast<std::uint64_t>(value);
        for(unsigned b=0;b<8;++b)times[row*8+b]=std::byte((bits>>(8*b))&255);
    }
    NumericView view{&history,std::as_bytes(std::span(values)),planes,{5,2},{5,0},{{times,{}}}};
    FrameStamp stamp;stamp.sequence=10000;stamp.producer_epoch=3;
    pipeline::DeliveredNumericFrame snapshot{ctx,{2,1},{},Status::ok};
    CHECK(retain_frame({stamp,view},limits,pools.p,pools.m,snapshot.frame)==Status::ok);
    pipeline::HistoryPresentation trail({8,2,136,{true,2}});
    CHECK(trail.accept_snapshot(snapshot,10000,2)==Status::ok);
    CHECK(trail.size()==2 && trail.filled_count()==2 && trail.capacity()==5);
    CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{91,92},{93,94},{95,96},{1,2},{3,4}}));
    CHECK(!trail.render_axes().seconds && trail.render_axes().ages==std::vector<double>({0,1,2,3,4}));
    CHECK(trail.accept_snapshot(snapshot,10000,6)==Status::malformed);
    CHECK(trail.render_snapshot()[4]==std::vector<float>({3,4}));
}
void history_time_resynchronization() {
    for(auto mode:{CoordinateMode::None,CoordinateMode::Regular,CoordinateMode::ExplicitFloat64LE}) {
        NumericContract history;history.semantic=Semantic::History;history.history_element=Semantic::Numeric;
        history.channel_order=channels;history.clock={"pcm.clock",{1,48000},48000};
        history.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,3},ExtentBounds{2,2}}};
        if(mode!=CoordinateMode::None)history.coordinates[0]={mode,NumericUnit::Seconds,{100,1},{1,4}};
        auto ctx=context(history);Pools pools;
        const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
        const std::array<float,6> values{3,4,1,2,2,3}; // wrapped physical storage
        std::array<std::byte,24> times{};
        auto encode=[](double value,std::span<std::byte> bytes){auto bits=std::bit_cast<std::uint64_t>(value);for(unsigned b=0;b<8;++b)bytes[b]=std::byte((bits>>(8*b))&255);};
        encode(7.,std::span(times).subspan(0,8));encode(8.,std::span(times).subspan(8,8));encode(10.,std::span(times).subspan(16,8));
        CoordinateView coordinates;if(mode==CoordinateMode::ExplicitFloat64LE)coordinates.axes[0]=times;
        NumericView matrix{&history,std::as_bytes(std::span(values)),planes,{3,2},{3,1},coordinates};
        FrameStamp stamp;stamp.sequence=10;stamp.timestamp_ticks=999999;stamp.producer_epoch=1;
        pipeline::DeliveredNumericFrame snapshot{ctx,{1,1},{},Status::ok};
        CHECK(retain_frame({stamp,matrix},limits,pools.p,pools.m,snapshot.frame)==Status::ok);
        pipeline::HistoryPresentation trail({3,2,56,{true,2}});CHECK(trail.accept_snapshot(snapshot,10,3)==Status::ok);
        auto axes=trail.render_axes();CHECK(axes.seconds==(mode!=CoordinateMode::None));
        CHECK(axes.ages==(mode==CoordinateMode::ExplicitFloat64LE?std::vector<double>{0,2,3}:
            mode==CoordinateMode::Regular?std::vector<double>{0,.25,.5}:std::vector<double>{0,1,2}));
        auto row=history;row.semantic=Semantic::Numeric;row.history_element=Semantic::Numeric;
        row.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};row.coordinates={};
        const std::array<PlaneLayout,1> row_planes{{{0,{4,0}}}};const std::array<float,2> next{4,5};
        pipeline::DeliveredNumericHistoryRow event;event.event=pipeline::NumericHistoryEvent::Row;
        event.capacity=event.filled_count=3;event.accepted_row_ordinal=11;
        // Regular compact coordinate repeats at the latest logical row once
        // full. Presentation must use row ordinals for rolling age, not origin.
        if(mode!=CoordinateMode::None){event.has_time_coordinate=true;encode(mode==CoordinateMode::Regular?100.5:11.5,event.time_coordinate);}
        stamp.sequence=11;stamp.timestamp_ticks+=123;
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,NumericView{&row,std::as_bytes(std::span(next)),row_planes,{2,1},{},{}}},Status::ok,limits,pools.p,pools.m,event.compact,true)==Status::ok);
        CHECK(trail.accept(event)==Status::ok);axes=trail.render_axes();CHECK(axes.seconds==(mode!=CoordinateMode::None));
        const auto expected=mode==CoordinateMode::ExplicitFloat64LE?std::vector<double>{0,1.5,3.5}:
            mode==CoordinateMode::Regular?std::vector<double>{0,.25,.5}:std::vector<double>{0,1,2};
        CHECK(axes.ages==expected);
        const std::array<float,6> replacement{2,3,3,4,4,5};matrix.storage=std::as_bytes(std::span(replacement));matrix.ring.oldest=0;
        encode(8.,std::span(times).subspan(0,8));encode(10.,std::span(times).subspan(8,8));encode(11.5,std::span(times).subspan(16,8));
        pipeline::DeliveredNumericFrame resync{ctx,{1,1},{},Status::ok};
        CHECK(retain_frame({stamp,matrix},limits,pools.p,pools.m,resync.frame)==Status::ok);
        CHECK(trail.accept_snapshot(resync,11,3)==Status::ok);CHECK(trail.render_axes().ages==expected);
        CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{2,3},{3,4},{4,5}}));
    }
}
}
int main(){lifecycle();all_dtypes();history_delivery();packet_and_refusal();index_axes();history_time_resynchronization();padded_cold_snapshot();pcm_typed_padding();std::printf("numeric presentation: %d checks, %d failures\n",checks,failures);return failures?1:0;}
