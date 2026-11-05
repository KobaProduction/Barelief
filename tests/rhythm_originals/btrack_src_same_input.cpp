// Observe the genuine BTrack SRC callback, then replay its exact Float32 input.
#include <dsp_kernels/prepared_resampler.h>
#include <sinc_backend.h>
#include <samplerate.h>
#include <BTrack.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
std::vector<float> read(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);check(bool(file),"open input");
    const auto bytes=file.tellg();check(bytes>0 && std::size_t(bytes)%4==0,"Float32 input");
    std::vector<float> result(std::size_t(bytes)/4);file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()),bytes);check(bool(file),"read input");return result;
}
template<class T> void write(const std::string& path,const std::vector<T>& values,std::size_t count) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(count*sizeof(T)));
    check(bool(file),"write capture");
}
struct Capture {
    dsp_kernels::PreparedResampler prepared;
    std::vector<float> input,source,port;
    std::vector<std::uint64_t> frames,status;
    std::size_t width=0,capacity=0,step=0,calls=0;
    bool ok=true;
    void prepare(std::size_t hop,std::size_t count) {
        width=262144/hop;capacity=count;
        check(prepared.prepare_window({width,512,1},&desktop_resampling::sinc_backend_factory()),"prepare existing SRC window");
        input.resize(count*width);source.resize(count*512);port.resize(count*512);frames.resize(count);status.resize(count*4);
    }
    bool valid(const SRC_DATA& data,int converter,int channels) const noexcept {
        return calls<capacity && channels==1 && converter==SRC_SINC_BEST_QUALITY &&
            data.input_frames==long(width) && data.output_frames==512 && data.src_ratio==512./double(width);
    }
    void collect(const SRC_DATA& data,int code) {
        const auto values=std::span(data.data_in,width);std::size_t written=0;
        const bool passed=prepared.process_window(values,width,std::span(port).subspan(calls*512,512),512,written);
        std::copy(values.begin(),values.end(),input.begin()+std::ptrdiff_t(calls*width));
        std::copy_n(data.data_out,512,source.begin()+std::ptrdiff_t(calls*512));frames[calls]=step;
        status[4*calls]=std::uint64_t(code);status[4*calls+1]=std::uint64_t(passed);status[4*calls+2]=written;status[4*calls+3]=std::uint64_t(data.output_frames_gen);
        ok=ok && code==0 && passed && written==512 && data.output_frames_gen==512;++calls;
    }
    void save(const std::string& prefix) const {
        write(prefix+".input.f32",input,calls*width);write(prefix+".source.f32",source,calls*512);write(prefix+".port.f32",port,calls*512);
        write(prefix+".frames.u64",frames,calls);write(prefix+".status.u64",status,calls*4);
    }
};
Capture* active=nullptr;
}
extern "C" int __real_src_simple(SRC_DATA*,int,int);
extern "C" int __wrap_src_simple(SRC_DATA* data,int converter,int channels) {
    check(active && active->valid(*data,converter,channels),"exact genuine SRC parameters");
    const int status=__real_src_simple(data,converter,channels);active->collect(*data,status);return status;
}
namespace {
void replay(const std::vector<float>& pcm,std::size_t hop,BTrack& original,std::span<double> block,Capture& capture) {
    for(std::size_t at=0;at+hop<=pcm.size();at+=hop){
        capture.step=at/hop;std::copy_n(pcm.begin()+std::ptrdiff_t(at),hop,block.begin());original.processAudioFrame(block.data());
    }
}
int run(int argc,char** argv) {
    check(argc==5,"usage: probe frame hop pcm.f32 prefix");
    const auto frame=std::stoul(argv[1]),hop=std::stoul(argv[2]);check(hop && hop<=frame && frame<=16384,"bounded geometry");
    const auto pcm=read(argv[3]);Capture capture;capture.prepare(hop,pcm.size()/hop);active=&capture;
    BTrack original{int(hop),int(frame)};std::vector<double> block(hop);
    replay(pcm,hop,original,block,capture);active=nullptr;capture.save(argv[4]);
    std::printf("{\"calls\":%zu,\"input_frames\":%zu,\"output_frames\":512,\"ok\":%s}\n",capture.calls,capture.width,capture.ok?"true":"false");
    return capture.ok && capture.calls?0:1;
}
}

int main(int argc,char** argv) {
    try {return run(argc,argv);}
    catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
