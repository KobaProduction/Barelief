// Observe the genuine BTrack mixed-precision seam; no algorithm substitutions.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#define private public
#include <OnsetDetectionFunction.h>
#undef private
namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class T> void write(const std::string& name,const std::vector<T>& values) {
    std::ofstream out(name,std::ios::binary);out.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));
    check(bool(out),"write genuine first hop");
}
std::vector<double> read(const char* path,std::size_t hop) {
    std::ifstream file(path,std::ios::binary);std::vector<float> input(hop);
    file.read(reinterpret_cast<char*>(input.data()),std::streamsize(hop*sizeof(float)));check(bool(file),"read first PCM hop");
    return {input.begin(),input.end()};
}
void capture(OnsetDetectionFunction& original,double value,const std::string& prefix) {
    std::vector<float> input,complex;input.reserve(2*std::size_t(original.frameSize));complex.reserve(input.capacity());
    for(int i=0;i<original.frameSize;++i){
        input.insert(input.end(),{original.fftIn[i].r,original.fftIn[i].i});
        complex.insert(complex.end(),{float(original.complexOut[i][0]),float(original.complexOut[i][1])});
    }
    write(prefix+".window.f64",original.window);write(prefix+".fft_input.f32",input);write(prefix+".complex.f32",complex);
    write(prefix+".norm.f64",original.magSpec);write(prefix+".phase.f64",original.phase);write(prefix+".odf.f64",std::vector<double>{value});
}
}
int main(int argc,char** argv) {
    try{
        check(argc==5,"usage: frame hop pcm.f32 prefix");const auto frame=std::stoi(argv[1]),hop=std::stoi(argv[2]);
        check(frame>=2 && frame<=16384 && hop>0 && hop<=frame,"bounded source geometry");auto pcm=read(argv[3],std::size_t(hop));
        OnsetDetectionFunction original(hop,frame,ComplexSpectralDifferenceHWR,HanningWindow);
        const double value=original.calculateOnsetDetectionFunctionSample(pcm.data());capture(original,value,argv[4]);return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
