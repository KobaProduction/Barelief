#include <signal_contracts/frame.h>
// Qt's default slots keyword expands away. Public portable headers must retain
// the caller-supplied pool capacity when included after QObject as well.
#define slots
#include <signal_contracts/buffer_pool.h>
#undef slots
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <string>
using namespace signal_contracts;
int main() {
    assert(frame_descriptor(FrameType::PCM)->stereo && !frame_descriptor(FrameType::Mel)->stereo);
    assert(!frame_descriptor(static_cast<FrameType>(-1)) && !frame_descriptor(static_cast<FrameType>(99)));
    FrameView wrong_type; wrong_type.type=static_cast<FrameType>(99);
    wrong_type.metadata.available=false;
    assert(validate(wrong_type)==Status::invalid_argument);
    FrameView scalar;std::array<float,1> zero{};
    scalar.type=FrameType::Scalar;scalar.metadata.sample_rate=48000;scalar.metadata.sequence=1;scalar.values=zero;
    assert(validate(scalar)==Status::ok);
    scalar.left=zero;assert(validate(scalar)==Status::invalid_argument);scalar.left={};
    scalar.metadata.bands=1;assert(validate(scalar)==Status::invalid_argument);scalar.metadata.bands=0;
    zero[0]=std::numeric_limits<float>::infinity();assert(validate(scalar)==Status::invalid_argument);zero[0]=0;
    for(const auto& descriptor:frame_descriptors()) {
        assert(frame_descriptor(descriptor.type)==&descriptor && data_shape(descriptor.type)==descriptor.shape && type_id(descriptor.type)==descriptor.id);
    }
    std::array<float,6> data{1,2,3,4,5,6};
    FrameView f; f.type=FrameType::Mel; f.metadata.sample_rate=48000;
    f.metadata.sequence=1; f.metadata.bands=3; f.values=std::span(data).first(3);
    assert(validate(f)==Status::ok);
    f.history.frames=1; assert(validate(f)==Status::invalid_argument); f.history={};
    auto c=contract(f), pcm=c; pcm.type=FrameType::PCM;
    assert(!compatible(c,pcm)); pcm=c; pcm.sample_rate=44100; assert(!compatible(c,pcm));
    f.values=std::span(data); assert(validate(f)==Status::invalid_argument);
    f.values={}; f.type=FrameType::History; f.metadata.history_capacity=2;
    f.history={data,3,2,2,1}; assert(validate(f)==Status::ok);
    assert(f.history.row(0)[0]==4 && f.history.row(1)[0]==1);
    std::array<float,3> newest_zero{};
    const auto history_contract=contract(f);
    f.history.newest_override=newest_zero;
    assert(validate(f)==Status::ok && contract(f)==history_contract);
    assert(f.history.row(0)[0]==4 && f.history.row(1)[0]==0 && data[0]==1);
    f.history.newest_override=std::span(newest_zero).first(1);
    assert(validate(f)==Status::invalid_argument);
    f.history.newest_override=newest_zero;newest_zero[1]=std::numeric_limits<float>::quiet_NaN();
    assert(validate(f)==Status::invalid_argument);newest_zero[1]=0;
    f.history.newest_override={};
    f.history.oldest=2; assert(validate(f)==Status::invalid_argument);
    f.history={}; f.metadata.available=false; f.metadata.sequence=0;
    assert(validate(f)==Status::unavailable);
    f.values=std::span(data).first(1); assert(validate(f)==Status::invalid_argument);
    std::array<std::byte,32> storage{}; std::array<BufferSlot,2> slots{};
    BufferPool pool(storage,slots,16); BufferHandle a,b;
    assert(pool.acquire(17,a)==Status::buffer_too_small);
    assert(pool.acquire(16,a)==Status::ok); a.bytes()[0]=std::byte{42};
    auto retained=a; a.reset(); assert(retained.bytes()[0]==std::byte{42});
    assert(pool.acquire(8,b)==Status::ok); BufferHandle blocked;
    assert(pool.acquire(1,blocked)==Status::exhausted);
    retained.reset(); assert(pool.acquire(1,blocked)==Status::ok);
    assert(pool.in_use()==2); b.reset(); blocked.reset(); assert(pool.in_use()==0);
    alignas(float) std::array<std::byte,256> retained_storage{};std::array<BufferSlot,2> retained_slots{};
    BufferPool retained_pool(retained_storage,retained_slots,128);
    RetainedFrame queued;
    {
        std::string id="transient-id";
        std::array<float,6> original{1,2,3,4,5,6};
        FrameView source;source.type=FrameType::History;source.metadata.sample_rate=48000;
        source.metadata.sequence=42;source.metadata.bands=3;source.metadata.history_capacity=3;
        std::array<float,3> override_row{};
        source.metadata.stream_id=id;source.history={original,3,2,2,1};
        source.history.newest_override=override_row;
        assert(retain(source,retained_pool,queued)==Status::ok);
        original.fill(-99);override_row.fill(-99);id.assign(id.size(),'x');
    }
    assert(queued.view.metadata.stream_id=="transient-id" && queued.view.history.row(0)[0]==4);
    assert(queued.view.history.row(1)[0]==0 && validate(queued.view)==Status::ok);
    assert(queued.view.history.newest_override.empty());
    auto copy=queued;queued={};
    assert(copy.view.history.row(0)[2]==6 && copy.view.metadata.sequence==42);
    assert(retained_pool.in_use()==1);
    copy={};assert(retained_pool.in_use()==0);
    // Regression: a non-History frame must never traverse a malformed history ring.
    FrameView malformed;malformed.type=FrameType::Mel;malformed.metadata.sequence=1;
    malformed.metadata.sample_rate=48000;malformed.values=std::span(data).first(3);
    for(auto member:{&HistoryView::frames,&HistoryView::bands,&HistoryView::capacity,&HistoryView::oldest}) {
        malformed.history={};malformed.history.*member=1;
        assert(validate(malformed)==Status::invalid_argument);
        assert(retain(malformed,retained_pool,queued)==Status::invalid_argument && !queued.buffer);
        assert(retained_pool.in_use()==0);
    }
    malformed.history={};malformed.history.newest_override=newest_zero;
    assert(validate(malformed)==Status::invalid_argument);
    assert(retain(malformed,retained_pool,queued)==Status::invalid_argument && !queued.buffer);
    malformed.metadata.available=false;malformed.metadata.sequence=0;malformed.values={};
    assert(validate(malformed)==Status::invalid_argument);
    FrameView empty_marker;empty_marker.metadata.available=false;
    assert(retain(empty_marker,retained_pool,queued)==Status::ok && !queued.view.metadata.available);
    assert(validate(queued.view)==Status::unavailable);
    // Generic packet contracts describe opaque geometry, not AudioSync wire fields.
    std::array<std::uint8_t,44> opaque{};FrameView packet;
    packet.type=FrameType::WledPacket;packet.packet=opaque;packet.metadata.sequence=1;
    packet.metadata.sample_rate=48000;
    assert(validate(packet)==Status::ok);
    packet.packet=std::span(opaque).first(43);assert(validate(packet)==Status::invalid_argument);
    std::array<float,6> mask_values{1,2,3,4,5,6};std::array<float,2> mask_zero{};
    std::array<std::uint8_t,4> mask{0,1,0,1};
    FrameView masked;masked.type=FrameType::History;masked.metadata.sample_rate=48000;
    masked.metadata.sequence=1;masked.metadata.bands=2;masked.metadata.history_capacity=4;
    masked.history={mask_values,2,3,3,1,{},mask,mask_zero,1};
    assert(validate(masked)==Status::ok && masked.history.row(0)[0]==0 && masked.history.row(1)[0]==5 && masked.history.row(2)[0]==0);
    assert(retain(masked,retained_pool,queued)==Status::ok);
    assert(queued.view.history.row(0)[0]==0 && queued.view.history.row(1)[0]==5 && queued.view.history.zero_rows.empty());
    queued={};masked.history.zero_oldest=4;assert(validate(masked)==Status::invalid_argument);
    masked.history.zero_oldest=1;mask[0]=2;assert(validate(masked)==Status::invalid_argument);mask[0]=0;
    mask_zero[0]=1;assert(validate(masked)==Status::invalid_argument);mask_zero[0]=0;
    masked.type=FrameType::Mel;assert(validate(masked)==Status::invalid_argument);
    std::cout<<"semantic contracts and bounded ownership passed\n";
}
