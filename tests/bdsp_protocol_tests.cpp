#include <bdsp_protocol/byte_stream.h>
#include <BDSP/transceivers.h>
#include <BDSP/memory_manager.h>
#include <array>
#include <cassert>
#include <iostream>
#include <vector>
#include <cstdlib>
using namespace bdsp_protocol;
using signal_contracts::Status;
struct CountingMemory : BDSP::core::IMemoryManager {
    size_t allocations=0;
    void *allocate_memory(size_t bytes) override {++allocations;return std::malloc(bytes);}
    void free_memory(void *pointer) override {std::free(pointer);}
};
struct PartialTransport : ByteTransport {
    std::vector<std::uint8_t> sent; std::size_t limit=3;
    TransportState state() const noexcept override{return TransportState::connected;}
    IoResult read(std::span<std::uint8_t>) noexcept override{return {Status::would_block,0};}
    IoResult write(std::span<const std::uint8_t> data) noexcept override {
        const auto n=std::min(limit,data.size());sent.insert(sent.end(),data.begin(),data.begin()+n);
        return {n?Status::ok:Status::would_block,n};
    }
};
int main() {
    std::vector<std::uint8_t> payload(1024);for(size_t i=0;i<payload.size();++i)payload[i]=i%256;
    std::vector<std::uint8_t> expected;BDSP::COBSZPETransceiver legacy;
    legacy.get_write_stream().set_stream_writer([](uint8_t b,void *p){static_cast<std::vector<uint8_t>*>(p)->push_back(b);},&expected);
    assert(legacy.send_data(13,payload.data(),payload.size())==BDSP::SEND_PACKET_OK);
    std::array<uint8_t,2200> raw{},out{};PacketStream stream(raw,out);
    int received=0;std::vector<uint8_t> received_payload;
    stream.set_packet_handler([](PacketView p,void *ctx){
        auto *pair=static_cast<std::pair<int*,std::vector<uint8_t>*>*>(ctx);++*pair->first;
        assert(p.id==13);pair->second->assign(p.payload.begin(),p.payload.end());
    },nullptr);
    std::pair context{&received,&received_payload};stream.set_packet_handler(stream.packet_handler(),&context);
    assert(stream.enqueue(13,payload)==Status::ok);
    assert(stream.enqueue(13,payload)==Status::overflow);
    payload.assign(payload.size(),99); // Enqueued bytes own the payload snapshot.
    PartialTransport transport;transport.limit=0;
    assert(stream.flush(transport).status==Status::would_block && stream.pending_bytes()==expected.size());
    transport.limit=3;
    while(stream.pending_bytes())assert(stream.flush(transport).status==Status::ok);
    assert(transport.sent==expected);
    for(auto byte:expected)assert(stream.feed(std::span(&byte,1))==Status::ok);
    assert(received==1 && received_payload[255]==255 && received_payload[256]==0);
    auto corrupted=expected;corrupted[20]^=1;assert(stream.feed(corrupted)==Status::malformed);
    assert(stream.feed(expected)==Status::ok && received==2);
    std::array<uint8_t,12> tiny{};PacketStream bounded(tiny,out);
    assert(bounded.feed(expected)==Status::overflow);
    std::array<uint8_t,1> short_out{88};auto result=encode_packet(13,payload,short_out);
    assert(result.status==Status::buffer_too_small && result.required_bytes>short_out.size() && short_out[0]==88);
    // Boundary sizes and COBS special patterns retain the upstream byte format.
    for(size_t size:{size_t(1),size_t(255),size_t(256),size_t(65535)}) {
        for(int pattern=0;pattern<3;++pattern) {
            std::vector<uint8_t> input(size);
            for(size_t i=0;i<size;++i)input[i]=pattern==0?0:pattern==1?255:i%256;
            expected.clear();assert(legacy.send_data(31,input.data(),input.size())==BDSP::SEND_PACKET_OK);
            std::vector<uint8_t> encoded(expected.size(),99);
            CountingMemory memory;BDSP::core::MemoryManager::set_new_manager(&memory);
            const auto actual=encode_packet(31,input,encoded);
            BDSP::core::MemoryManager::set_new_manager(nullptr);
            assert(actual && memory.allocations==0 && encoded==expected);
            std::vector<uint8_t> full_raw(65539),send_buffer(expected.size());PacketStream receiver(full_raw,send_buffer);
            size_t count=0;receiver.set_packet_handler([](PacketView packet,void *ctx){
                assert(packet.id==31);++*static_cast<size_t *>(ctx);
            },&count);
            const size_t split=expected.size()/2;
            assert(receiver.feed(std::span(expected).first(split))==Status::ok);
            assert(receiver.feed(std::span(expected).subspan(split))==Status::ok && count==1);
        }
    }
    std::cout<<"BDSP legacy bytes, arbitrary fragments, partial writes and overflow passed\n";
}
