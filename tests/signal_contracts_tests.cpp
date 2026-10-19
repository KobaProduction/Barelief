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

namespace {
void descriptor_and_scalar_contracts() {
    assert(frame_descriptor(FrameType::PCM)->stereo && !frame_descriptor(FrameType::Mel)->stereo);
    assert(!frame_descriptor(static_cast<FrameType>(-1)) && !frame_descriptor(static_cast<FrameType>(99)));
    FrameView wrong_type;wrong_type.type=static_cast<FrameType>(99);
    wrong_type.metadata.available=false;
    assert(validate(wrong_type)==Status::invalid_argument);
    FrameView scalar;std::array<float,1> zero{};
    scalar.type=FrameType::Scalar;scalar.metadata.sample_rate=48000;scalar.metadata.sequence=1;scalar.values=zero;
    assert(validate(scalar)==Status::ok);
    scalar.left=zero;assert(validate(scalar)==Status::invalid_argument);scalar.left={};
    scalar.metadata.bands=1;assert(validate(scalar)==Status::invalid_argument);scalar.metadata.bands=0;
    zero[0]=std::numeric_limits<float>::infinity();assert(validate(scalar)==Status::invalid_argument);
    for(const auto& descriptor:frame_descriptors())
        assert(frame_descriptor(descriptor.type)==&descriptor && data_shape(descriptor.type)==descriptor.shape && type_id(descriptor.type)==descriptor.id);
}
void mel_contracts() {
    std::array<float,6> data{1,2,3,4,5,6};
    FrameView frame;frame.type=FrameType::Mel;frame.metadata.sample_rate=48000;
    frame.metadata.sequence=1;frame.metadata.bands=3;frame.values=std::span(data).first(3);
    assert(validate(frame)==Status::ok);
    frame.history.frames=1;assert(validate(frame)==Status::invalid_argument);frame.history={};
    auto mel=contract(frame),pcm=mel;pcm.type=FrameType::PCM;
    assert(!compatible(mel,pcm));pcm=mel;pcm.sample_rate=44100;assert(!compatible(mel,pcm));
    frame.values=std::span(data);assert(validate(frame)==Status::invalid_argument);
}
FrameView history_frame(std::array<float,6>& data) {
    FrameView frame;frame.type=FrameType::History;
    frame.metadata.sample_rate=48000;frame.metadata.sequence=1;
    frame.metadata.bands=3;frame.metadata.history_capacity=2;
    frame.history={data,3,2,2,1};
    return frame;
}
void history_ring_contracts() {
    std::array<float,6> data{1,2,3,4,5,6};
    auto frame=history_frame(data);
    assert(validate(frame)==Status::ok);
    assert(frame.history.row(0)[0]==4 && frame.history.row(1)[0]==1);
    std::array<float,3> newest_zero{};
    const auto original_contract=contract(frame);
    frame.history.newest_override=newest_zero;
    assert(validate(frame)==Status::ok && contract(frame)==original_contract);
    assert(frame.history.row(0)[0]==4 && frame.history.row(1)[0]==0 && data[0]==1);
    frame.history.newest_override=std::span(newest_zero).first(1);
    assert(validate(frame)==Status::invalid_argument);
    frame.history.newest_override=newest_zero;newest_zero[1]=std::numeric_limits<float>::quiet_NaN();
    assert(validate(frame)==Status::invalid_argument);
}
void unavailable_history_contracts() {
    std::array<float,6> data{1,2,3,4,5,6};
    auto frame=history_frame(data);
    frame.history.oldest=2;assert(validate(frame)==Status::invalid_argument);
    frame.history={};frame.metadata.available=false;frame.metadata.sequence=0;
    assert(validate(frame)==Status::unavailable);
    frame.values=std::span(data).first(1);assert(validate(frame)==Status::invalid_argument);
}
void pool_reference_counting() {
    std::array<std::byte,32> storage{};std::array<BufferSlot,2> slots{};
    BufferPool pool(storage,slots,16);BufferHandle first,second;
    assert(pool.acquire(17,first)==Status::buffer_too_small);
    assert(pool.acquire(16,first)==Status::ok);first.bytes()[0]=std::byte{42};
    auto retained=first;first.reset();assert(retained.bytes()[0]==std::byte{42});
    assert(pool.acquire(8,second)==Status::ok);BufferHandle blocked;
    assert(pool.acquire(1,blocked)==Status::exhausted);
    retained.reset();assert(pool.acquire(1,blocked)==Status::ok);
    assert(pool.in_use()==2);second.reset();blocked.reset();assert(pool.in_use()==0);
}
void retained_history_owns_source() {
    alignas(float) std::array<std::byte,256> storage{};std::array<BufferSlot,2> slots{};
    BufferPool pool(storage,slots,128);RetainedFrame queued;
    {
        std::string id="transient-id";
        std::array<float,6> original{1,2,3,4,5,6};
        FrameView source=history_frame(original);source.metadata.sequence=42;
        source.metadata.history_capacity=3;
        std::array<float,3> override_row{};
        source.metadata.stream_id=id;source.history.newest_override=override_row;
        assert(retain(source,pool,queued)==Status::ok);
        original.fill(-99);override_row.fill(-99);id.assign(id.size(),'x');
    }
    assert(queued.view.metadata.stream_id=="transient-id" && queued.view.history.row(0)[0]==4);
    assert(queued.view.history.row(1)[0]==0 && validate(queued.view)==Status::ok);
    assert(queued.view.history.newest_override.empty());
    auto copy=queued;queued={};
    assert(copy.view.history.row(0)[2]==6 && copy.view.metadata.sequence==42);
    assert(pool.in_use()==1);copy={};assert(pool.in_use()==0);
}
void malformed_history_never_reaches_retain() {
    alignas(float) std::array<std::byte,256> storage{};std::array<BufferSlot,2> slots{};
    BufferPool pool(storage,slots,128);RetainedFrame queued;
    std::array<float,6> data{1,2,3,4,5,6};std::array<float,3> zero{};
    FrameView malformed;malformed.type=FrameType::Mel;malformed.metadata.sequence=1;
    malformed.metadata.sample_rate=48000;malformed.values=std::span(data).first(3);
    for(auto member:{&HistoryView::frames,&HistoryView::bands,&HistoryView::capacity,&HistoryView::oldest}) {
        malformed.history={};malformed.history.*member=1;
        assert(validate(malformed)==Status::invalid_argument);
        assert(retain(malformed,pool,queued)==Status::invalid_argument && !queued.buffer);
        assert(pool.in_use()==0);
    }
    malformed.history={};malformed.history.newest_override=zero;
    assert(validate(malformed)==Status::invalid_argument);
    assert(retain(malformed,pool,queued)==Status::invalid_argument && !queued.buffer);
    malformed.metadata.available=false;malformed.metadata.sequence=0;malformed.values={};
    assert(validate(malformed)==Status::invalid_argument);
    FrameView empty_marker;empty_marker.metadata.available=false;
    assert(retain(empty_marker,pool,queued)==Status::ok && !queued.view.metadata.available);
    assert(validate(queued.view)==Status::unavailable);
}
void opaque_packet_contract() {
    std::array<std::uint8_t,44> opaque{};FrameView packet;
    packet.type=FrameType::WledPacket;packet.packet=opaque;packet.metadata.sequence=1;
    packet.metadata.sample_rate=48000;
    assert(validate(packet)==Status::ok);
    packet.packet=std::span(opaque).first(43);assert(validate(packet)==Status::invalid_argument);
}
void zero_history_rows_survive_retention() {
    alignas(float) std::array<std::byte,256> storage{};std::array<BufferSlot,2> slots{};
    BufferPool pool(storage,slots,128);RetainedFrame queued;
    std::array<float,6> values{1,2,3,4,5,6};std::array<float,2> zero{};
    std::array<std::uint8_t,4> mask{0,1,0,1};
    FrameView frame;frame.type=FrameType::History;frame.metadata.sample_rate=48000;
    frame.metadata.sequence=1;frame.metadata.bands=2;frame.metadata.history_capacity=4;
    frame.history={values,2,3,3,1,{},mask,zero,1};
    assert(validate(frame)==Status::ok && frame.history.row(0)[0]==0 && frame.history.row(1)[0]==5 && frame.history.row(2)[0]==0);
    assert(retain(frame,pool,queued)==Status::ok);
    assert(queued.view.history.row(0)[0]==0 && queued.view.history.row(1)[0]==5 && queued.view.history.zero_rows.empty());
    queued={};frame.history.zero_oldest=4;assert(validate(frame)==Status::invalid_argument);
    frame.history.zero_oldest=1;mask[0]=2;assert(validate(frame)==Status::invalid_argument);mask[0]=0;
    zero[0]=1;assert(validate(frame)==Status::invalid_argument);zero[0]=0;
    frame.type=FrameType::Mel;assert(validate(frame)==Status::invalid_argument);
}
}
int main() {
    descriptor_and_scalar_contracts();
    mel_contracts();
    history_ring_contracts();
    unavailable_history_contracts();
    pool_reference_counting();
    retained_history_owns_source();
    malformed_history_never_reaches_retain();
    opaque_packet_contract();
    zero_history_rows_survive_retention();
    std::cout<<"semantic contracts and bounded ownership passed\n";
}
