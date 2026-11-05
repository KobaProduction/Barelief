// Prepared source Sinc replay on sealed Float32 SRC_DATA input windows.
#include <dsp_kernels/prepared_resampler.h>
#include <sinc_backend.h>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
std::vector<float> read(const char* path,std::size_t width) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);check(bool(file),"open SRCinput windows");const auto bytes=file.tellg();
    check(bytes>0 && std::size_t(bytes)%(width*sizeof(float))==0,"complete SRC input windows");std::vector<float> input(std::size_t(bytes)/4);
    file.seekg(0);file.read(reinterpret_cast<char*>(input.data()),bytes);check(bool(file),"read SRC input");return input;
}
template<class T> void write(const std::string& path,const std::vector<T>& values) {
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));check(bool(out),"write SRC output");
}
void replay(std::span<const float> rows,std::size_t width,const std::string& prefix) {
    dsp_kernels::ResamplingWindowConfig config{width,512,1};config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    dsp_kernels::PreparedResampler operation;dsp_kernels::ResamplerResources bound;const auto* factory=&desktop_resampling::sinc_backend_factory();
    check(decltype(operation)::window_resource_requirements(config,factory,bound) && operation.prepare_window(config,factory) &&
        operation.resources().owned_bytes<=bound.owned_bytes,"source SRC query/prepare");
    std::vector<float> output((rows.size()/width)*512);std::size_t written=0;
    for(std::size_t i=0;i<rows.size()/width;++i)
        check(operation.process_window(rows.subspan(i*width,width),width,std::span(output).subspan(i*512,512),512,written) && written==512,"source SRC window process");
    write(prefix+".f32",output);write(prefix+".workspace.u64",std::vector<std::uint64_t>{bound.owned_bytes,operation.resources().owned_bytes,1});
}
}
int main(int argc,char** argv) {
    try{
        check(argc==4,"usage: input-width captured-input.f32 prefix");const auto width=std::stoul(argv[1]);check(width && width<=131072,"bounded SRC width");
        const auto input=read(argv[2],width);replay(input,width,argv[3]);std::printf("{\"rows\":%zu,\"status\":\"ok\"}\n",input.size()/width);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
