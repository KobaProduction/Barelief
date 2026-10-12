// Retained SDK helper behavior through the canonical numeric API.
// Transport/replay fences live in embedded_numeric_session_tests.
#include <embedded_support/synthetic_pipeline.h>
#include <dsp_kernels/dsp.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

using signal_contracts::Status;
namespace es=embedded_support;
namespace sc=signal_contracts;
namespace nd=bdsp_protocol::numeric_data;
namespace {int failures=0;bool tracking=false;std::size_t allocations=0;}
void* operator new(std::size_t n){if(tracking)++allocations;auto* p=std::malloc(n?n:1);if(!p)std::abort();return p;}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
namespace {
#define CHECK(x) do{if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
struct Capture {
    sc::Semantic semantic=sc::Semantic::PCM;
    std::size_t channels=1,elements=256,frames=0;
    std::uint64_t sequence=0;
    bool positive=false,changed=false;float previous=0;
};
void receive(void* user,const nd::Message& message) {
    auto& c=*static_cast<Capture*>(user);++c.frames;
    CHECK(message.kind==nd::Kind::Frame && message.stamp.available);
    CHECK(message.endpoint.direction==nd::Direction::Input);
    CHECK(message.stamp.sequence>c.sequence);c.sequence=message.stamp.sequence;
    const auto* contract=std::get_if<sc::NumericContract>(&message.contract);
    CHECK(contract);if(!contract)return;
    CHECK(contract->semantic==c.semantic && contract->channel_order.size()==c.channels);
    CHECK(contract->dtype==sc::DType::Float32 && contract->representation.encoding==sc::StorageEncoding::FixedLittleEndian);
    const auto frame=message.frame_view();CHECK(sc::validate_view(frame,{2,8192,32768,2048,64,256})==Status::ok);
    const auto& numeric=std::get<sc::NumericView>(frame.payload);
    CHECK(numeric.actual[0]==c.elements);
    for(std::size_t channel=0;channel<c.channels;++channel)for(std::size_t element=0;element<c.elements;++element) {
        float value=0;CHECK(sc::load_element(numeric,{channel,element,0},value)==Status::ok);
        CHECK(std::isfinite(value));c.positive|=value>0;
        if(!channel && !element){if(c.frames>1)c.changed|=value!=c.previous;c.previous=value;}
    }
}
void run(bool spectral,std::uint32_t fft) {
    es::Profile profile;profile.spectral=spectral;profile.fft_size=fft;
    // Helper implementation oracle; the whole ordinary320KiB composition is separate.
    profile.memory_bytes=2*1024*1024;
    es::SyntheticPipeline pipeline;
    const auto prepared=pipeline.prepare(profile,"sdk-helper");
    CHECK(prepared==Status::ok);if(prepared!=Status::ok)return;
    CHECK(es::admit(profile,10,pipeline.memory_requirements()).status==Status::ok);
    CHECK(pipeline.output_binding().stream=="sdk-helper");
    Capture capture;capture.semantic=spectral?sc::Semantic::MelEnergy:sc::Semantic::PCM;
    capture.channels=1;capture.elements=spectral?8:256;
    allocations=0;tracking=true;
    for(unsigned tick=0;tick<64;++tick)CHECK(pipeline.tick(receive,&capture)==Status::ok);
    tracking=false;CHECK(allocations==0 && capture.frames>0 && capture.positive && capture.changed);
    if(!spectral)CHECK(capture.frames==64);
    // Failed cold replacement keeps the previous prepared helper usable.
    auto insufficient=profile;insufficient.memory_bytes=64*1024;
    CHECK(pipeline.prepare(insufficient,"rejected")==Status::exhausted);
    const auto frames=capture.frames;CHECK(pipeline.tick(receive,&capture)==Status::ok);
    if(!spectral)CHECK(capture.frames==frames+1);
    CHECK(pipeline.output_binding().stream=="sdk-helper");
}
}
int main() {
    es::SyntheticPipeline empty;CHECK(empty.tick(nullptr,nullptr)==Status::unavailable);
    run(false,512);
    for(auto fft:{512u,1024u,2048u,4096u}) {
        if(fft<=dsp_kernels::max_fft_samples)run(true,fft);
        else {es::Profile p;p.spectral=true;p.fft_size=fft;CHECK(es::admit(p,3).status==Status::invalid_argument);}
    }
    std::printf("embedded canonical SDK helper: %d failures\n",failures);return failures?1:0;
}
