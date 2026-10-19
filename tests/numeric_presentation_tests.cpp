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
NumericContract scalar_contract(DType dtype) {
    NumericContract contract;
    contract.dtype=dtype;
    contract.channel_order=channels;
    contract.clock={"clock",{1,1000},1000};
    return contract;
}
template<class T> class ScalarDeliveryScenario {
    NumericContract contract=scalar_contract(dtype_of<T>());
    alignas(64) std::array<std::byte,8> bytes{};
    const std::array<PlaneLayout,1> planes{{{0,{sizeof(T),sizeof(T)}}}};
    Pools pools;
    std::shared_ptr<const pipeline::NumericDeliveryContext> ctx=context(contract,4,1);
    pipeline::DeliveredNumericFrame owner;
    NumericLiteral literal;
    using Mailbox=pipeline::NumericDeliveryMailbox<pipeline::DeliveredNumericFrame,2>;
    MutableNumericView writable() {
        return {&contract,std::span(bytes).first(sizeof(T)),planes,{1,1},{},{}};
    }
    NumericView retained() const {return std::get<NumericView>(owner.frame.view().payload);}
    void write(T value) {
        auto view=writable();
        CHECK(store_element(view,{},value)==Status::ok);
    }
    void retain(T value) {
        write(value);
        const DataFrameView frame{{},writable().view()};
        const auto allocations=allocation_probe::count;
        CHECK(pipeline::retain_numeric_delivery(ctx,{3,7},frame,Status::ok,limits,pools.p,pools.m,owner)==Status::ok);
        CHECK(allocation_probe::count==allocations);
    }
    void detach_source() {
        bytes.fill(std::byte{0});
        contract.clock.domain={};
        CHECK(pipeline::read_numeric_literal(retained(),{},literal)==Status::ok);
    }
    void retained_identity(const char* text) const {
        CHECK(pipeline::exact_scalar_text(literal)==text);
        CHECK(retained().contract->clock.domain=="clock");
    }
    void strict_literal(bool exact) const {
        float projected=-7;
        bool loss=false;
        CHECK(pipeline::project_literal(literal,false,projected,loss)==(exact?Status::ok:Status::incompatible_contract));
        if(!exact)CHECK(projected==-7);
    }
    void lossy_literal(bool exact) const {
        float projected=-7;
        bool loss=false;
        CHECK(pipeline::project_literal(literal,true,projected,loss)==Status::ok);
        CHECK(loss==!exact);
    }
    static void refused_projection(const std::array<float,1>& target,const pipeline::FloatProjectionReport& report) {
        CHECK(target[0]==-8);
        CHECK(report.elements==42 && report.inexact_elements==13);
    }
    void strict_projection(bool exact) const {
        std::array<float,1> target{-8};
        pipeline::FloatProjectionReport report{42,13};
        CHECK(pipeline::project_numeric_float(retained(),0,{false,1},target,report)==(exact?Status::ok:Status::incompatible_contract));
        if(!exact)refused_projection(target,report);
    }
    void lossy_projection(bool exact) const {
        std::array<float,1> target{-8};
        pipeline::FloatProjectionReport report{42,13};
        CHECK(pipeline::project_numeric_float(retained(),0,{true,1},target,report)==Status::ok);
        CHECK(report.elements==1 && report.inexact_elements==std::size_t(!exact));
    }
    void fill_mailbox(Mailbox& mailbox) const {
        CHECK(mailbox.push(owner));
        CHECK(mailbox.push(owner));
        CHECK(!mailbox.push(owner));
    }
    static void drain_mailbox(Mailbox& mailbox) {
        CHECK(mailbox.dropped()==1);
        CHECK(mailbox.pop().has_value());
        mailbox.clear();
        CHECK(mailbox.size()==0);
    }
    void mailbox_noalloc() const {
        Mailbox mailbox;
        const auto before=allocation_probe::count;
        fill_mailbox(mailbox);
        drain_mailbox(mailbox);
        CHECK(allocation_probe::count==before);
    }
public:
    void run(T value,const char* text,bool exact) {
        retain(value);
        detach_source();
        retained_identity(text);
        strict_literal(exact);
        lossy_literal(exact);
        strict_projection(exact);
        lossy_projection(exact);
        mailbox_noalloc();
    }
};
template<class T> void scalar_value(T value,const char* text,bool exact) {
    ScalarDeliveryScenario<T> scenario;
    scenario.run(value,text,exact);
}
void signed_dtypes() {
    scalar_value(std::int8_t{-128},"-128",true);
    scalar_value(std::int16_t{-32768},"-32768",true);
    scalar_value(std::numeric_limits<std::int32_t>::min(),"-2147483648",true);
    scalar_value(std::numeric_limits<std::int64_t>::min(),"-9223372036854775808",true);
}
void unsigned_dtypes() {
    scalar_value(std::uint8_t{255},"255",true);
    scalar_value(std::uint16_t{65535},"65535",true);
    scalar_value(std::numeric_limits<std::uint32_t>::max(),"4294967295",false);
    scalar_value(std::numeric_limits<std::uint64_t>::max(),"18446744073709551615",false);
    scalar_value(std::uint64_t{9007199254740993ULL},"9007199254740993",false);
}
void unrepresentable_literal(double value) {
    float projected=9;
    bool loss=false;
    CHECK(pipeline::project_literal(value,true,projected,loss)==Status::malformed);
    CHECK(projected==9);
}
void floating_dtypes() {
    scalar_value(float{0.5},"0.5",true);
    scalar_value(double{0.1},"0.10000000000000001",false);
    unrepresentable_literal(std::numeric_limits<double>::infinity());
    unrepresentable_literal(std::numeric_limits<double>::max());
}
void all_dtypes() {
    signed_dtypes();
    unsigned_dtypes();
    floating_dtypes();
}
void append_quadratic_times(pipeline::ScalarPresentation& timed,FrameStamp& stamp) {
    stamp.producer_epoch=99;
    for(std::uint64_t i=0;i<140;++i) {
        stamp.sequence=i;
        stamp.timestamp_ticks=std::int64_t(i*i);
        CHECK(timed.accept(stamp,std::uint64_t{7}));
    }
}
void verify_source_clock(const pipeline::ScalarPresentation& timed) {
    const auto seconds=timed.relative_seconds(ClockContract{"source",{1,1000},1000});
    CHECK(seconds.size()==128 && seconds.front()==(144.-19321.)/1000. && seconds.back()==0);
    CHECK(timed.relative_seconds(ClockContract{}).empty());
}
void repeated_time_has_no_seconds(pipeline::ScalarPresentation& timed,FrameStamp& stamp) {
    stamp.sequence=140;
    stamp.timestamp_ticks=19321;
    timed.accept(stamp,std::uint64_t{8});
    CHECK(timed.relative_seconds(ClockContract{"source",{1,1000},1000}).empty());
}
void large_timestamp_delta(pipeline::ScalarPresentation& timed,FrameStamp& stamp) {
    timed.clear();
    stamp.sequence=0;
    stamp.timestamp_ticks=std::numeric_limits<std::int64_t>::max()-1;
    timed.accept(stamp,std::uint64_t{1});
    stamp.sequence=1;
    ++stamp.timestamp_ticks;
    timed.accept(stamp,std::uint64_t{2});
    CHECK(timed.relative_seconds(ClockContract{"source",{1,1000},1000})==std::vector<double>({-.001,0}));
}
void scalar_time_lifecycle() {
    pipeline::ScalarPresentation timed;
    FrameStamp stamp;
    append_quadratic_times(timed,stamp);
    verify_source_clock(timed);
    repeated_time_has_no_seconds(timed,stamp);
    large_timestamp_delta(timed,stamp);
}
class ScalarRingScenario {
    pipeline::ScalarPresentation scalar;
    FrameStamp stamp;
    void first_frame() {
        stamp.discontinuity=true;
        CHECK(scalar.accept(stamp,std::uint64_t{9007199254740993ULL}));
        CHECK(scalar.size()==1);
    }
    void duplicate_frame() {
        CHECK(!scalar.accept(stamp,std::uint64_t{9007199254740993ULL}));
        CHECK(scalar.size()==1);
    }
    void fill_and_wrap() {
        stamp.discontinuity=false;
        for(std::uint64_t i=1;i<140;++i) {
            stamp.sequence=i;
            CHECK(scalar.accept(stamp,i));
        }
        CHECK(scalar.size()==128);
    }
    void sparse_frame() {
        stamp.sequence=143;
        CHECK(scalar.accept(stamp,std::uint64_t{143}));
        CHECK(scalar.size()==128);
    }
    void new_producer() {
        stamp.producer_epoch=1;
        stamp.sequence=0;
        CHECK(scalar.accept(stamp,std::uint64_t{0}));
        CHECK(scalar.size()==1);
    }
    void explicit_clear() {
        scalar.clear();
        CHECK(scalar.size()==0);
        CHECK(scalar.accept(stamp,std::uint64_t{0}));
    }
    void advance_delivery() {
        CHECK(scalar.latest_text()=="0");
        stamp.epoch=3;
        stamp.sequence=1;
        CHECK(scalar.accept(stamp,std::uint64_t{1}));
    }
    void compatible_delivery() {
        stamp.discontinuity=false;
        stamp.epoch=4;
        stamp.sequence=2;
        CHECK(scalar.accept(stamp,std::uint64_t{2}));
        CHECK(scalar.size()==3);
    }
    void stale_delivery() {
        stamp.epoch=2;
        stamp.sequence=3;
        CHECK(!scalar.accept(stamp,std::uint64_t{3}));
        CHECK(scalar.latest_text()=="2");
    }
    void independent_instance() const {
        pipeline::ScalarPresentation independent;
        CHECK(independent.accept(stamp,std::uint64_t{77}));
        CHECK(independent.latest_text()=="77");
        CHECK(scalar.latest_text()=="2");
    }
public:
    void run() {
        first_frame();
        duplicate_frame();
        fill_and_wrap();
        sparse_frame();
        new_producer();
        explicit_clear();
        advance_delivery();
        compatible_delivery();
        stale_delivery();
        independent_instance();
    }
};
void lifecycle() {
    scalar_time_lifecycle();
    ScalarRingScenario scenario;
    scenario.run();
}
NumericContract compact_history_fixture() {
    NumericContract history;
    history.dtype=DType::UInt64;
    history.semantic=Semantic::History;
    history.history_element=Semantic::MelEnergy;
    history.quantity=Quantity::MelEnergy;
    history.channel_order=channels;
    history.clock={"clock",{1,1000},1000};
    history.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,3},ExtentBounds{2,2}}};
    history.coordinates[1]={CoordinateMode::Regular,NumericUnit::Hertz,{70,1},{10,1}};
    return history;
}
NumericContract compact_vector_fixture(const NumericContract& history) {
    auto row=history;
    row.semantic=history.history_element;
    row.history_element=Semantic::Numeric;
    row.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};
    row.coordinates={history.coordinates[1],AxisCoordinates{}};
    return row;
}
class HistoryDeliveryScenario {
    NumericContract history=compact_history_fixture(),row=compact_vector_fixture(history);
    const std::array<PlaneLayout,1> planes{{{0,{8,8}}}};
    alignas(64) std::array<std::byte,16> bytes{};
    std::array<std::byte,8> time{};
    const std::array<std::byte,1> selected_mask{std::byte{0}};
    MutableNumericView source{&row,bytes,planes,{2,1},{},{}};
    Pools pools;
    std::shared_ptr<const pipeline::NumericDeliveryContext> ctx=context(history);
    pipeline::HistoryPresentation trail{{3,2,56,{true,2}}},other{{3,2,56,{true,2}}};
    NumericGatherOutput gathered;
    pipeline::DeliveredNumericHistoryRow event,unavailable;
    void initialize() {
        const auto bits=std::bit_cast<std::uint64_t>(1.25);
        for(unsigned i=0;i<8;++i)time[i]=std::byte((bits>>(8*i))&255);
        gathered.row.history=true;
        gathered.row.capacity=3;
        gathered.row.time_coordinate=time;
        gathered.row.mask_present=true;
        gathered.row.selected_mask=selected_mask;
        gathered.frame={{},source.view()};
        gathered.frame.stamp.discontinuity=true;
        unavailable.compact.context=ctx;
        unavailable.compact.delivery={7,2};
        unavailable.capacity=3;
    }
    void write_values(std::uint64_t value) {
        CHECK(store_element(source,{0,0,0},value)==Status::ok);
        CHECK(store_element(source,{0,1,0},value+1)==Status::ok);
    }
    void retain_event(std::uint64_t ordinal,std::uint64_t count) {
        const auto before=allocation_probe::count;
        CHECK(pipeline::retain_numeric_history_row(ctx,{7,2},pipeline::NumericHistoryEvent::Row,
            gathered,count,3,ordinal,limits,pools.p,pools.m,event)==Status::ok);
        CHECK(allocation_probe::count==before);
    }
    void make(std::uint64_t ordinal,std::uint64_t count,std::uint64_t value) {
        write_values(value);
        gathered.frame.stamp.sequence=ordinal;
        gathered.frame.stamp.timestamp_ticks=std::int64_t(ordinal*10);
        gathered.row.filled_rows=count;
        gathered.row.logical_row=std::size_t(count-1);
        gathered.row.physical_row=std::size_t((ordinal-1)%3);
        retain_event(ordinal,count);
    }
    void detached_event() {
        make(1,1,9007199254740993ULL);
        time.fill(std::byte{0});
        bytes.fill(std::byte{0});
        CHECK(event.has_time_coordinate && event.time_coordinate[6]!=std::byte{0});
        CHECK(event.mask_present);
    }
    void first_acceptance() {
        CHECK(trail.accept(event)==Status::ok);
        CHECK(trail.size()==1 && trail.filled_count()==1 && trail.precision_loss());
        CHECK(trail.render_snapshot().size()==3 && trail.render_snapshot()[0]==std::vector<float>({0,0})
            && trail.render_snapshot()[1]==std::vector<float>({0,0}));
    }
    void duplicate() {
        CHECK(trail.accept(event)==Status::would_block);
        CHECK(trail.size()==1);
    }
    void independent_history() {
        CHECK(other.accept(event)==Status::ok);
        gathered.frame.stamp.discontinuity=false;
    }
    void second_row() {
        make(2,2,4);
        CHECK(trail.accept(event)==Status::ok);
        CHECK(trail.size()==2 && trail.precision_loss());
    }
    void wrap_rows() {
        make(3,3,6);
        CHECK(trail.accept(event)==Status::ok);
        make(4,3,8);
        CHECK(trail.accept(event)==Status::ok);
    }
    void verify_wrapped_rows() const {
        const auto snapshot=trail.render_snapshot();
        CHECK(snapshot.size()==3 && snapshot[0][0]==4 && snapshot[2][0]==8);
        CHECK(!trail.precision_loss());
        CHECK(other.size()==1);
    }
    void verify_axes() const {
        const auto axes=trail.render_axes();
        CHECK(!axes.seconds && axes.ages==std::vector<double>({0,1,2}));
        CHECK(axes.column_coordinates && axes.column_unit==NumericUnit::Hertz && axes.columns==std::vector<double>({70,80}));
    }
    void gap_resets_rows() {
        make(6,3,12);
        CHECK(trail.accept(event)==Status::ok);
        CHECK(trail.size()==1 && trail.filled_count()==3);
    }
    void unavailable_preserves_rows() {
        CHECK(trail.accept(unavailable)==Status::unavailable);
        CHECK(trail.size()==1 && !trail.available());
    }
    void reset_clears_rows() {
        unavailable.event=pipeline::NumericHistoryEvent::Reset;
        CHECK(trail.accept(unavailable)==Status::ok);
        CHECK(trail.size()==0);
    }
    void restart_rows() {
        make(7,1,14);
        CHECK(trail.accept(event)==Status::ok);
        CHECK(trail.size()==1);
    }
    void advance_delivery() {
        make(8,2,16);
        event.compact.delivery={8,3};
        CHECK(trail.accept(event)==Status::ok);
        CHECK(trail.size()==2);
    }
    void cold_delivery() const {
        pipeline::HistoryPresentation cold({3,2,56,{true,2}});
        CHECK(cold.accept(unavailable)==Status::ok);
        CHECK(cold.accept(event)==Status::ok);
        CHECK(cold.size()==1);
    }
    void resource_refusal() const {
        pipeline::HistoryPresentation insufficient({3,2,55,{true,2}});
        CHECK(insufficient.accept(event)==Status::buffer_too_small);
        CHECK(insufficient.size()==0);
    }
    void stale_delivery() {
        event.compact.delivery.epoch=1;
        CHECK(trail.accept(event)==Status::would_block);
        CHECK(trail.size()==2);
    }
    void clear_releases_history() {
        trail.clear();
        CHECK(trail.size()==0 && trail.filled_count()==0 && !trail.latest().compact.frame);
    }
    void matrix_refused_incrementally() {
        const NumericView full{&history,bytes,planes,{1,2},{3,0,{}},{}};
        pipeline::DeliveredNumericFrame refused;
        CHECK(pipeline::retain_numeric_delivery(ctx,{7,2},DataFrameView{{},full},Status::ok,
            limits,pools.p,pools.m,refused)==Status::incompatible_contract);
        CHECK(!refused.frame);
    }
    void retained_ring() {
        detached_event();
        first_acceptance();
        duplicate();
        independent_history();
        second_row();
        wrap_rows();
        verify_wrapped_rows();
        verify_axes();
    }
    void reset_and_fences() {
        gap_resets_rows();
        unavailable_preserves_rows();
        reset_clears_rows();
        restart_rows();
        advance_delivery();
        cold_delivery();
        resource_refusal();
        stale_delivery();
    }
public:
    void run() {
        initialize();
        retained_ring();
        reset_and_fences();
        clear_releases_history();
        matrix_refused_incrementally();
    }
};
void history_delivery() {
    HistoryDeliveryScenario scenario;
    scenario.run();
}
class StructuredDeliveryScenario {
    std::string schema="wled.audio-sync";
    StructuredContract contract{schema,2,"LE",{44,44}};
    std::array<std::byte,44> packet{};
    Pools pools;
    std::shared_ptr<const pipeline::NumericDeliveryContext> ctx=context(contract,8);
    alignas(64) std::array<std::byte,512> payload{},metadata{};
    std::array<BufferSlot,1> payload_slots{},metadata_slots{};
    BufferPool p{payload,payload_slots,512},m{metadata,metadata_slots,512};
    pipeline::DeliveredNumericFrame owner,only;
    void retain_source() {
        packet[3]=std::byte{99};
        const DataFrameView frame{{},StructuredView{&contract,packet}};
        const auto before=allocation_probe::count;
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},frame,Status::ok,limits,pools.p,pools.m,owner)==Status::ok);
        CHECK(allocation_probe::count==before);
    }
    void detach_source() {
        packet.fill(std::byte{0});
        schema.assign("changed");
        const auto retained=std::get<StructuredView>(owner.frame.view().payload);
        CHECK(retained.contract->schema=="wled.audio-sync" && retained.contract->version==2 && retained.contract->encoding=="LE");
        CHECK(retained.bytes.size()==44 && retained.bytes[3]==std::byte{99});
    }
    void wrong_contract_refused() {
        const StructuredContract wrong{"wrong",1,"LE",{44,44}};
        const DataFrameView frame{{},StructuredView{&wrong,packet}};
        CHECK(pipeline::retain_numeric_delivery(ctx,{2,1},frame,Status::ok,limits,pools.p,pools.m,owner)==Status::incompatible_contract);
        CHECK(owner.delivery.graph_revision==1);
    }
    void exhaust_pool() {
        const auto frame=owner.frame.view();
        CHECK(pipeline::retain_numeric_delivery(ctx,{3,1},frame,Status::ok,limits,p,m,only)==Status::ok);
        CHECK(pipeline::retain_numeric_delivery(ctx,{4,1},frame,Status::ok,limits,p,m,only)==Status::exhausted);
        CHECK(only.delivery.graph_revision==3 && only.frame);
    }
    void queue_release() {
        pipeline::NumericDeliveryMailbox<pipeline::DeliveredNumericFrame,1> queue;
        CHECK(queue.push(std::move(only)));
        CHECK(p.in_use()==1 && m.in_use()==1);
        queue.clear();
        CHECK(p.in_use()==0 && m.in_use()==0);
    }
public:
    void run() {
        retain_source();
        detach_source();
        wrong_contract_refused();
        exhaust_pool();
        queue_release();
    }
};
void packet_and_refusal() {
    StructuredDeliveryScenario scenario;
    scenario.run();
}
void accept_empty_history(const NumericContract& contract,pipeline::HistoryPresentation& history,std::size_t capacity) {
    pipeline::DeliveredNumericHistoryRow event;
    event.compact.context=context(contract);
    event.event=pipeline::NumericHistoryEvent::Empty;
    event.capacity=capacity;
    CHECK(history.accept(event)==Status::ok);
}
void verify_index_axes(const pipeline::HistoryPresentation& history) {
    const auto axes=history.render_axes();
    CHECK(!axes.seconds);
    CHECK(history.render_snapshot()==std::vector<std::vector<float>>({{0,0},{0,0},{0,0}}));
    CHECK(!axes.column_coordinates && axes.column_unit==NumericUnit::Unspecified && axes.columns==std::vector<double>({0,1}));
}
void index_axis_case(Axis axis,Semantic semantic) {
    NumericContract contract;
    contract.semantic=Semantic::History;
    contract.history_element=semantic;
    contract.channel_order=channels;
    contract.shape={Rank::Matrix,{Axis::Time,axis},{ExtentBounds{0,3},ExtentBounds{2,2}}};
    pipeline::HistoryPresentation history({3,2,56,{true,2}});
    accept_empty_history(contract,history,3);
    verify_index_axes(history);
}
void index_axes() {
    for(auto [axis,semantic]:std::array{
        std::pair{Axis::Band,Semantic::MelEnergy},
        std::pair{Axis::Bin,Semantic::SpectrumMagnitude},
        std::pair{Axis::Sample,Semantic::PCM},
        std::pair{Axis::Declared,Semantic::Numeric}}) {
        index_axis_case(axis,semantic);
    }
}
NumericContract unsigned_pcm_history(DType dtype,unsigned width) {
    NumericContract history;
    history.dtype=dtype;history.semantic=Semantic::History;history.history_element=Semantic::PCM;
    history.channel_order=channels;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Sample},{ExtentBounds{0,3},ExtentBounds{1,1}}};
    history.representation.valid_bits=width;
    history.representation.normalization=Normalization::UnsignedUnit;
    return history;
}
void unsigned_pcm_padding_case(DType dtype,unsigned width) {
    const auto contract=unsigned_pcm_history(dtype,width);
    pipeline::HistoryPresentation history({3,1,28,{true,1}});
    accept_empty_history(contract,history,3);
    const float midpoint=float(std::uint64_t{1}<<(width-1));
    CHECK(history.render_snapshot()==std::vector<std::vector<float>>({{midpoint},{midpoint},{midpoint}}));
}
void pcm_typed_padding() {
    for(const auto [dtype,width]:std::array{std::pair{DType::UInt8,8u},std::pair{DType::UInt16,12u},std::pair{DType::UInt64,64u}})
        unsigned_pcm_padding_case(dtype,width);
}
NumericContract resize_history_contract(std::size_t capacity);
NumericContract padded_history_contract() {
    auto history=resize_history_contract(5);
    history.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    return history;
}
class PaddedSnapshotScenario {
    NumericContract history=padded_history_contract();
    Pools pools;
    const std::array<float,10> values{91,92,93,94,95,96,1,2,3,4};
    const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
    std::array<std::byte,40> times{};
    pipeline::DeliveredNumericFrame snapshot{context(history),{2,1},{},Status::ok};
    pipeline::HistoryPresentation trail{{8,2,136,{true,2}}};
    void initialize_coordinates() {
        for(const auto [row,value]:std::array{std::pair{3,12.},std::pair{4,12.5}}) {
            const auto bits=std::bit_cast<std::uint64_t>(value);
            for(unsigned b=0;b<8;++b)times[row*8+b]=std::byte((bits>>(8*b))&255);
        }
    }
    void retain_snapshot() {
        const NumericView view{&history,std::as_bytes(std::span(values)),planes,{5,2},{5,0},{{times,{}}}};
        FrameStamp stamp;
        stamp.sequence=10000;
        stamp.producer_epoch=3;
        CHECK(retain_frame({stamp,view},limits,pools.p,pools.m,snapshot.frame)==Status::ok);
    }
    void accept_snapshot() {
        CHECK(trail.accept_snapshot(snapshot,10000,2)==Status::ok);
        CHECK(trail.size()==2 && trail.filled_count()==2 && trail.capacity()==5);
    }
    void verify_padding() const {
        CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{91,92},{93,94},{95,96},{1,2},{3,4}}));
        CHECK(!trail.render_axes().seconds && trail.render_axes().ages==std::vector<double>({0,1,2,3,4}));
    }
    void refuse_invalid_fill() {
        CHECK(trail.accept_snapshot(snapshot,10000,6)==Status::malformed);
        CHECK(trail.render_snapshot()[4]==std::vector<float>({3,4}));
    }
public:
    void run() {
        initialize_coordinates();
        retain_snapshot();
        accept_snapshot();
        verify_padding();
        refuse_invalid_fill();
    }
};
void padded_cold_snapshot() {
    PaddedSnapshotScenario scenario;
    scenario.run();
}
NumericContract resize_history_contract(std::size_t capacity) {
    NumericContract history;
    history.semantic=Semantic::History;
    history.history_element=Semantic::Numeric;
    history.channel_order=channels;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,capacity},ExtentBounds{2,2}}};
    return history;
}
struct SnapshotResizeScenario {
    Pools pools;
    pipeline::HistoryPresentation trail{{8,2,136,{true,2}}};
    void accept_full_snapshot() {
        const auto history=resize_history_contract(5);
        const std::array<float,10> values{91,92,93,94,95,96,1,2,3,4};
        const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
        const NumericView view{&history,std::as_bytes(std::span(values)),planes,{5,2},{5,0},{}};
        pipeline::DeliveredNumericFrame snapshot{context(history),{2,1},{},Status::ok};
        CHECK(retain_frame({FrameStamp{},view},limits,pools.p,pools.m,snapshot.frame)==Status::ok);
        CHECK(trail.accept_snapshot(snapshot,10,2)==Status::ok);
    }
    void verify_snapshot() const {
        CHECK(trail.size()==2 && trail.filled_count()==2 && trail.capacity()==5);
        CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{91,92},{93,94},{95,96},{1,2},{3,4}}));
    }
    void shrink_while_unavailable() {
        pipeline::DeliveredNumericHistoryRow event;
        event.compact.context=context(resize_history_contract(3));
        event.compact.delivery={2,1};
        event.capacity=3;
        event.event=pipeline::NumericHistoryEvent::Unavailable;
        CHECK(trail.accept(event)==Status::unavailable);
    }
    void verify_retained_suffix() const {
        CHECK(!trail.available() && trail.size()==2 && trail.filled_count()==2 && trail.capacity()==3);
        CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{0,0},{1,2},{3,4}}));
    }
};
pipeline::HistoryAxes declared_column_axes(const AxisCoordinates& coordinates) {
    auto history=resize_history_contract(3);
    history.coordinates[1]=coordinates;
    pipeline::HistoryPresentation trail({3,2,56,{true,2}});
    pipeline::DeliveredNumericHistoryRow event;
    event.compact.context=context(history);
    event.event=pipeline::NumericHistoryEvent::Empty;
    event.capacity=3;
    CHECK(trail.accept(event)==Status::ok);
    return trail.render_axes();
}
void generic_column_coordinates() {
    for(const auto unit:{NumericUnit::Radians,NumericUnit::Values,NumericUnit::Unspecified}) {
        const auto axes=declared_column_axes({CoordinateMode::Regular,unit,{-3,1},{2,1}});
        CHECK(axes.column_coordinates && axes.column_unit==unit);
        CHECK(axes.columns==std::vector<double>({-3,-1}));
    }
}
void snapshot_unavailable_capacity_change() {
    SnapshotResizeScenario scenario;
    scenario.accept_full_snapshot();
    scenario.verify_snapshot();
    scenario.shrink_while_unavailable();
    scenario.verify_retained_suffix();
}
void encode_test_coordinate(double value,std::span<std::byte> bytes) {
    const auto bits=std::bit_cast<std::uint64_t>(value);
    for(unsigned b=0;b<8;++b)bytes[b]=std::byte((bits>>(8*b))&255);
}
class HistoryResynchronizationScenario {
    CoordinateMode mode;
    NumericContract history;
    Pools pools;
    const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
    const std::array<float,6> values{3,4,1,2,2,3};
    const std::array<float,6> replacement{2,3,3,4,4,5};
    std::array<std::byte,24> times{};
    std::shared_ptr<const pipeline::NumericDeliveryContext> ctx;
    NumericView matrix;
    FrameStamp stamp;
    pipeline::HistoryPresentation trail{{3,2,56,{true,2}}};
    void initialize_history() {
        history=resize_history_contract(3);
        history.clock={"pcm.clock",{1,48000},48000};
        if(mode!=CoordinateMode::None)history.coordinates[0]={mode,NumericUnit::Seconds,{100,1},{1,4}};
        ctx=context(history);
        encode_test_coordinate(7.,std::span(times).subspan(0,8));
        encode_test_coordinate(8.,std::span(times).subspan(8,8));
        encode_test_coordinate(10.,std::span(times).subspan(16,8));
        CoordinateView coordinates;
        if(mode==CoordinateMode::ExplicitFloat64LE)coordinates.axes[0]=times;
        matrix={&history,std::as_bytes(std::span(values)),planes,{3,2},{3,1},coordinates};
    }
    void accept_snapshot() {
        stamp.sequence=10;
        stamp.timestamp_ticks=999999;
        stamp.producer_epoch=1;
        pipeline::DeliveredNumericFrame snapshot{ctx,{1,1},{},Status::ok};
        CHECK(retain_frame({stamp,matrix},limits,pools.p,pools.m,snapshot.frame)==Status::ok);
        CHECK(trail.accept_snapshot(snapshot,10,3)==Status::ok);
    }
    void verify_initial_ages() const {
        const auto axes=trail.render_axes();
        CHECK(axes.seconds==(mode!=CoordinateMode::None));
        CHECK(axes.ages==(mode==CoordinateMode::ExplicitFloat64LE?std::vector<double>{0,2,3}:
            mode==CoordinateMode::Regular?std::vector<double>{0,.25,.5}:std::vector<double>{0,1,2}));
    }
    NumericContract compact_row_contract() const {
        auto row=history;
        row.semantic=Semantic::Numeric;
        row.history_element=Semantic::Numeric;
        row.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};
        row.coordinates={};
        return row;
    }
    void retain_compact_row(pipeline::DeliveredNumericHistoryRow& event) {
        const auto row=compact_row_contract();
        const std::array<PlaneLayout,1> row_planes{{{0,{4,0}}}};
        const std::array<float,2> next{4,5};
        const NumericView view{&row,std::as_bytes(std::span(next)),row_planes,{2,1},{},{}};
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,view},Status::ok,limits,pools.p,pools.m,event.compact,true)==Status::ok);
    }
    void accept_compact_row() {
        pipeline::DeliveredNumericHistoryRow event;
        event.event=pipeline::NumericHistoryEvent::Row;
        event.capacity=event.filled_count=3;
        event.accepted_row_ordinal=11;
        // A regular rolling coordinate repeats; ordinals retain the row spacing.
        if(mode!=CoordinateMode::None) {
            event.has_time_coordinate=true;
            encode_test_coordinate(mode==CoordinateMode::Regular?100.5:11.5,event.time_coordinate);
        }
        stamp.sequence=11;
        stamp.timestamp_ticks+=123;
        retain_compact_row(event);
        CHECK(trail.accept(event)==Status::ok);
    }
    std::vector<double> expected_ages() const {
        if(mode==CoordinateMode::ExplicitFloat64LE)return {0,1.5,3.5};
        if(mode==CoordinateMode::Regular)return {0,.25,.5};
        return {0,1,2};
    }
    void verify_compact_ages() const {
        const auto axes=trail.render_axes();
        CHECK(axes.seconds==(mode!=CoordinateMode::None));
        CHECK(axes.ages==expected_ages());
    }
    void prepare_resync() {
        matrix.storage=std::as_bytes(std::span(replacement));
        matrix.ring.oldest=0;
        encode_test_coordinate(8.,std::span(times).subspan(0,8));
        encode_test_coordinate(10.,std::span(times).subspan(8,8));
        encode_test_coordinate(11.5,std::span(times).subspan(16,8));
    }
    void accept_resync() {
        pipeline::DeliveredNumericFrame snapshot{ctx,{1,1},{},Status::ok};
        CHECK(retain_frame({stamp,matrix},limits,pools.p,pools.m,snapshot.frame)==Status::ok);
        CHECK(trail.accept_snapshot(snapshot,11,3)==Status::ok);
    }
    void verify_resync() const {
        CHECK(trail.render_axes().ages==expected_ages());
        CHECK(trail.render_snapshot()==std::vector<std::vector<float>>({{2,3},{3,4},{4,5}}));
    }
public:
    explicit HistoryResynchronizationScenario(CoordinateMode value):mode(value){}
    void run() {
        initialize_history();
        accept_snapshot();
        verify_initial_ages();
        accept_compact_row();
        verify_compact_ages();
        prepare_resync();
        accept_resync();
        verify_resync();
    }
};
void history_time_resynchronization() {
    for(const auto mode:{CoordinateMode::None,CoordinateMode::Regular,CoordinateMode::ExplicitFloat64LE}) {
        HistoryResynchronizationScenario scenario(mode);
        scenario.run();
    }
}
}
int main(){lifecycle();all_dtypes();history_delivery();packet_and_refusal();index_axes();history_time_resynchronization();padded_cold_snapshot();pcm_typed_padding();snapshot_unavailable_capacity_change();generic_column_coordinates();std::printf("numeric presentation: %d checks, %d failures\n",checks,failures);return failures?1:0;}
