#include <wled/numeric_wled_output_policy.h>
#include <array>
#include <stdexcept>
#include <cstdio>
using signal_contracts::Status;
static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Fixture {
    signal_contracts::StructuredContract contract{"wled-audio-sync",1,"audio-sync-44-le",{44,44}};
    wled_protocol::WledPacket source{};
    Fixture(){std::array<float,50> mel;mel.fill(30);require(wled_protocol::encode_mel(mel,100,2,1,99,source,{50,44}).status==wled_protocol::Status::Ok,"packet fixture");}
    signal_contracts::DataFrameView view(bool available=true) const {
        signal_contracts::FrameStamp stamp;stamp.available=available;
        return {stamp,signal_contracts::StructuredView{&contract,available?std::as_bytes(std::span(source)):std::span<const std::byte>{}}};
    }
};
static wled_protocol::DecodedPacket decoded(const wled_protocol::WledPacket& packet) {
    wled_protocol::DecodedPacket result{};require(wled_protocol::decode(packet,result,{0,44}).status==wled_protocol::Status::Ok,"valid emitted wire");return result;
}
int main(){try {
    Fixture fixture;wled_protocol::WledPacket output{};
    desktop::NumericWledOutputPolicy cadence;
    cadence.start(0);require(cadence.update(fixture.view(),0)==Status::ok,"accept packet");
    require(cadence.take_due_packet(16,output)==Status::would_block&&cadence.next_counter()==0,"cadence before17");
    require(cadence.take_due_packet(17,output)==Status::ok&&decoded(output).counter==0,"first send counter0");
    require(decoded(fixture.source).counter==99,"producer packet mutated");
    require(cadence.take_due_packet(17,output)==Status::would_block,"same timestamp burst");
    require(cadence.take_due_packet(34,output)==Status::ok&&decoded(output).counter==1,"second cadence");
    require(cadence.take_due_packet(1000,output)==Status::ok&&decoded(output).magnitude==0,"stale send silence");
    require(cadence.take_due_packet(1000,output)==Status::would_block,"late timer catchup burst");
    desktop::NumericWledOutputPolicy boundary(1);boundary.start(0);boundary.update(fixture.view(),0);
    require(boundary.take_due_packet(300,output)==Status::ok&&decoded(output).magnitude==2,"exact300 should remain fresh");
    auto invalid=fixture.source;invalid[0]^=1;auto bad=fixture.view();std::get<signal_contracts::StructuredView>(bad.payload).bytes=std::as_bytes(std::span(invalid));
    require(boundary.update(bad,300)==Status::invalid_argument,"bad header accepted");
    require(boundary.take_due_packet(301,output)==Status::ok&&decoded(output).magnitude==0,"invalid input renewed freshness");
    desktop::NumericWledOutputPolicy unavailable;unavailable.start(0);unavailable.update(fixture.view(),0);
    require(unavailable.update(fixture.view(false),1)==Status::unavailable,"unavailable input");
    require(unavailable.take_due_packet(17,output)==Status::ok&&decoded(output).magnitude==0,"unavailable retained nonzero payload");
    unavailable.note_send_result(false);require(!unavailable.running()&&unavailable.next_counter()==1,"failed attempt counter/stop");
    require(unavailable.take_due_packet(34,output)==Status::unavailable,"failure did not stop output");
    unavailable.start(50);require(unavailable.take_due_packet(67,output)==Status::ok&&decoded(output).counter==1,"start reset destination counter");
    desktop::NumericWledOutputPolicy rollover(1);rollover.start(0);
    for(unsigned i=0;i<257;++i){require(rollover.take_due_packet(i+1,output)==Status::ok,"rollover attempt");require(decoded(output).counter==std::uint8_t(i),"counter rollover");}
    desktop::NumericWledOutputPolicy rollback(1);rollback.start(20);rollback.update(fixture.view(),20);
    require(rollback.take_due_packet(21,output)==Status::ok,"initial monotonic send");
    require(rollback.take_due_packet(10,output)==Status::would_block,"clock rollback sent");
    require(rollback.take_due_packet(11,output)==Status::ok&&decoded(output).magnitude==0,"clock rollback retained stale payload");
    require(rollback.set_cadence(0)==Status::invalid_argument,"zero cadence accepted");
    return 0;
}catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}}
