#include <wled_protocol/wled_packet.h>
#include <device_protocols/wled_packet.h>
#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>

namespace {
std::size_t allocations = 0;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(3); }
}
auto literal(std::string_view hex) {
    wled_protocol::WledPacket result{};
    require(hex.size() == result.size()*2, "invalid literal fixture");
    auto digit = [](char c) { return c <= '9' ? c-'0' : c-'a'+10; };
    for (std::size_t i=0; i<result.size(); ++i) result[i]=std::uint8_t(digit(hex[2*i])*16+digit(hex[2*i+1]));
    return result;
}
void put_float(wled_protocol::WledPacket& bytes, std::size_t offset, float value) {
    const auto bits=std::bit_cast<std::uint32_t>(value);
    for (unsigned i=0;i<4;++i) bytes[offset+i]=std::uint8_t(bits>>(8*i));
}
}
void* operator new(std::size_t size) {
    ++allocations;
    if (void* p=std::malloc(size ? size : 1)) return p;
    std::abort();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main() {
    using namespace wled_protocol;
    constexpr Limits limits{80,44};
    const float nan=std::numeric_limits<float>::quiet_NaN(), inf=std::numeric_limits<float>::infinity();
    // Literal fixtures from git-show HEAD make_packet (not the new encoder).
    const auto zero=literal("3030303032000000000000000000000000000000000000000000000000000000000000000000000000000000");
    const auto zero255=literal("3030303032000000000000000000000000ff0000000000000000000000000000000000000000000000000000");
    const auto mixed=literal("303030303200000000601f4200601f4201ff007fff00ff000000000000000000000000000000403f00b0b346");
    const auto sanitized=literal("303030303200000000007f4100007f41010700ff000000000000000000000000000000000000000000000000");
    const auto tail=literal("303030303200000000007f4200007f4200003f3f3f3f3f3f3f3f3f3f3f3f3f3f3f3f00000000000000000000");
    const auto half=literal("30303030320000000000ff420000ff4200017f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f00000000803f0000dc43");
    WledPacket output{};
    const auto baseline=allocations;
    auto success = [](Result result) {
        require(result.status==Status::Ok && result.used==44 && result.required==44, "codec success size/status");
    };
    success(silence(0,output,limits)); require(output==zero, "HEAD silence golden");
    success(silence(255,output,limits)); require(output==zero255, "HEAD counter255 silence golden");
    const std::array bins{0.f,.5f,1.f,-1.f,2.f,nan,inf};
    success(encode_bins(bins,23000,.75f,255,output,limits)); require(output==mixed, "HEAD mixed golden");
    success(encode_bins(std::array{nan,100.f,-1.f},inf,-1,7,output,limits)); require(output==sanitized, "HEAD sanitation golden");
    std::array<float,20> long_bins{}; long_bins.fill(.25f); long_bins[16]=1;long_bins[19]=nan;
    require(device_protocols::wled_packet(long_bins,-1,nan,0)==tail, "legacy ignores bin tail");
    std::array<float,16> halves{};halves.fill(.5f);
    success(encode_bins(halves,440,1,1,output,limits));require(output==half,"HEAD strict peak threshold/truncation golden");
    // Separately captured from the unaccepted pre-extraction finite Mel draft.
    constexpr std::array<std::string_view,5> mel_hex{
        "30303030320000000000ff420000ff4200177f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f00000000003f00007a44",
        "30303030320000000058fc420058fc4201173f3f3f42505c697884919daab9bfbfbf00000000003f00007a44",
        "3030303032000000ff9ffa42ff9ffa4201171b273441505c697884919daab9c5d2e100000000003f00007a44",
        "3030303032000000ff9ffa42ff9ffa4201171b283441505c697884919daab9c5d2e100000000003f00007a44",
        "3030303032000000fe9ffa42fe9ffa4201171b273440505c697784909daab8c5d2e100000000003f00007a44"};
    constexpr std::array<std::size_t,5> sizes{1,3,16,50,80};
    std::array<float,80> mel{};
    for(std::size_t k=0;k<sizes.size();++k) {
        const auto n=sizes[k];for(std::size_t i=0;i<n;++i) mel[i]=float(i+1)/float(n+1);
        const auto values=std::span<const float>(mel).first(n);
        success(encode_mel(values,1000,.5f,1,23,output,limits));
        require(output==literal(mel_hex[k]), "finite interpolation/mapping golden");
        require(device_protocols::encode_wled(values,1000,.5f,1,23)==output, "legacy Mel wrapper parity");
    }
    success(encode_mel(std::array{.1f,.2f,.3f},440,.3f,0,7,output,limits));
    require(output==literal("3030303032000000000000000000000000070000000000000000000000000000000000009a99993e0000dc43"),"gain0 preserves metadata");
    success(encode_mel(std::array{.1f,.2f,.3f},440,.3f,100,7,output,limits));
    require(output==literal("303030303200000000007f4300007f430107ffffffffffffffffffffffffffffffff00009a99993e0000dc43"),"gain100 golden");
    require(device_protocols::encode_wled({},0,0,1,255)==zero255,"legacy empty Mel valid silence");
    const auto legacy_silent_meta=literal("3030303032000000000000000000000000070000000000000000000000000000000000009a99993e0000dc43");
    const auto legacy_saturated=literal("303030303200000000007f4300007f430107ffffffffffffffffffffffffffffffff00009a99993e0000dc43");
    const auto legacy_nonfinite_mel=literal("303030303200000001806541018065410007000000000000000000000000004c4c4c00009a99993e0000dc43");
    require(device_protocols::encode_wled({},440,.3f,1,7)==legacy_silent_meta,"legacy empty preserves signal metadata");
    for(float gain:{-1.f,nan,inf})
        require(device_protocols::encode_wled(std::array{.1f,.2f,.3f},440,.3f,gain,7)==legacy_silent_meta,"legacy gain sanitation golden");
    require(device_protocols::encode_wled(std::array{.1f,.2f,.3f},440,.3f,101,7)==legacy_saturated,"legacy arbitrary gain golden");
    for(float middle:{nan,inf})
        require(device_protocols::encode_wled(std::array{.1f,middle,.3f},440,.3f,1,7)==legacy_nonfinite_mel,"legacy nonfinite interpolation golden");
    auto safe_overflow=zero;safe_overflow[17]=7;
    require(device_protocols::encode_wled(std::array{std::numeric_limits<float>::max()},440,.3f,100,7)==safe_overflow,"legacy numerical overflow returns valid silence");
    auto reject = [&](Result result, Status expected, const WledPacket& before) {
        require(result.status==expected && result.used==0 && result.required==44 && output==before,"atomic codec failure/status");
    };
    output.fill(0xa5);const auto sentinel=output;
    reject(silence(0,std::span(output).first(43),limits),Status::BufferTooSmall,sentinel);
    reject(silence(0,output,{80,43}),Status::LimitExceeded,sentinel);
    reject(encode_bins(long_bins,0,0,0,output,limits),Status::InvalidSize,sentinel);
    reject(encode_mel({},0,0,1,0,output,limits),Status::InvalidArgument,sentinel);
    reject(encode_mel(mel,0,0,1,0,output,{79,44}),Status::LimitExceeded,sentinel);
    reject(encode_mel(std::array{nan},0,0,1,0,output,limits),Status::NonFiniteInput,sentinel);
    reject(encode_mel(std::array{1.f},inf,0,1,0,output,limits),Status::NonFiniteInput,sentinel);
    reject(encode_mel(std::array{1.f},0,0,nan,0,output,limits),Status::NonFiniteInput,sentinel);
    reject(encode_mel(std::array{1.f},0,-1,1,0,output,limits),Status::InvalidArgument,sentinel);
    for(float gain:{-1.f,101.f}) reject(encode_mel(std::array{1.f},0,0,gain,0,output,limits),Status::InvalidArgument,sentinel);
    reject(encode_mel(std::array{std::numeric_limits<float>::max()},0,0,100,0,output,limits),Status::NonFiniteResult,sentinel);
    std::array<std::uint8_t,48> larger{};larger.fill(0x5a);success(silence(0,larger,limits));
    require(larger[44]==0x5a && larger[47]==0x5a,"encode suffix preserved");
    std::array<float,16> overlapping{};overlapping.fill(.5f);
    auto overlapping_bytes=std::span(reinterpret_cast<std::uint8_t*>(overlapping.data()),sizeof(overlapping));
    success(encode_bins(overlapping,440,1,1,overlapping_bytes,limits));
    require(std::equal(half.begin(),half.end(),overlapping_bytes.begin()),"input/destination overlap uses staging");
    DecodedPacket decoded{};success(decode(mixed,decoded,limits));
    require(decoded.counter==255 && decoded.sample_peak==1 && decoded.peak_hz==23000 && decoded.magnitude==.75f && decoded.bins[1]==127,"read-only decoded fields");
    auto foreign=mixed; foreign[6]=7;foreign[7]=8;foreign[34]=0x34;foreign[35]=0x12;
    put_float(foreign,8,1024);put_float(foreign,12,-256);put_float(foreign,36,-1);put_float(foreign,40,-2);foreign[16]=255;
    success(decode(foreign,decoded,limits));
    require(decoded.sample_raw==1024 && decoded.sample_smoothed==-256 && decoded.magnitude==-1 && decoded.peak_hz==-2 && decoded.sample_peak==255 && decoded.pressure[0]==7 && decoded.zero_crossing_count==0x1234,"foreign finite values/raw peak/reserved bytes preserved");
    const auto foreign_before=foreign;
    require(write_counter(foreign,0,limits)==Status::Ok,"counter patch foreign valid packet");
    for(std::size_t i=0;i<44;++i) require(foreign[i]==(i==17?0:foreign_before[i]),"patch must only change counter");
    foreign[16]=2;success(decode(foreign,decoded,limits));require(decoded.sample_peak==2,"peak2 supported");
    const auto baddecode = [&](std::span<const std::uint8_t> bytes, Limits lim, Status expected) {
        decoded.sample_raw=123;decoded.counter=91;decoded.bins.fill(42);
        const auto result=decode(bytes,decoded,lim);
        require(result.status==expected && result.used==0 && result.required==44 && decoded.sample_raw==123 && decoded.counter==91 && decoded.bins[0]==42,"decode failure commits no partial fields");
        require(validate_packet(bytes,lim)==expected,"validate agrees decode");
    };
    baddecode(std::span(mixed).first(43),limits,Status::InvalidSize);
    baddecode(larger,limits,Status::InvalidSize);
    baddecode(mixed,{80,43},Status::LimitExceeded);
    WledPacket invalid{};baddecode(invalid,limits,Status::InvalidHeader);
    invalid=mixed;invalid[5]=1;baddecode(invalid,limits,Status::InvalidHeader);
    for(const auto offset:{8u,12u,36u,40u}) {
        invalid=mixed;put_float(invalid,offset,nan);baddecode(invalid,limits,Status::NonFiniteInput);
        invalid=mixed;put_float(invalid,offset,inf);baddecode(invalid,limits,Status::NonFiniteInput);
        const auto unchanged=invalid;
        require(write_counter(invalid,1,limits)==Status::NonFiniteInput && invalid==unchanged,"invalid packet patch atomic");
    }
    require(validate_packet(zero,limits)==Status::Ok,"zero signal is valid packet");
    require(mixed==literal("303030303200000000601f4200601f4201ff007fff00ff000000000000000000000000000000403f00b0b346"),"decode mutated input");
    require(allocations==baseline,"codec processing allocates");
    std::cout << "wled_protocol HEAD goldens, bounded codecs and counter tests passed\n";
}
