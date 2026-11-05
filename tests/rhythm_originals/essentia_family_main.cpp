#include "essentia_family_export.h"
#include <cstdlib>
#include <iostream>
namespace {
std::vector<float> read(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    native_export::require(bool(f),"open input");const auto bytes=f.tellg();
    native_export::require(bytes>=0 && bytes%4==0,"Float32 input size");
    std::vector<float> values(std::size_t(bytes)/4);f.seekg(0);
    f.read(reinterpret_cast<char*>(values.data()),bytes);native_export::require(bool(f),"read input");return values;
}
essentia_family_export::Case arguments(char** argv) {
    essentia_family_export::Case c;c.input=read(argv[2]);c.prefix=argv[3];
    c.rate=std::stoul(argv[4]);c.rows=std::stoul(argv[5]);c.columns=std::stoul(argv[6]);c.seed=std::stoul(argv[7]);
    std::ifstream aux(std::string(argv[2])+".aux.f32");if(aux)c.aux=read(std::string(argv[2])+".aux.f32");return c;
}
}
namespace {
void export_case(const essentia_family_export::Case& c,const std::string& method) {
    if(method=="essentia_polar" || method=="pulse_train" || method=="band_emphasis" || method=="global_pcm")return essentia_family_export::diagnostics(c,method);
    if(method=="ibi" || method=="rubato" || method=="harmonic")return essentia_family_export::intervals(c,method);
    if(method=="summary_single" || method=="summary_multi" || method=="descriptors")return essentia_family_export::summary(c,method);
    if(method=="meter" || method=="beatogram" || method=="band_novelty" || method=="fixedbpm" || method=="rhythm_transform")return essentia_family_export::matrix(c,method);
    essentia_family_export::pcm(c,method);
}
}

int main(int argc,char** argv) {
    try {
        native_export::require(argc==8,"method input prefix Fs rows columns seed");
        const auto c=arguments(argv);const std::string method=argv[1];std::srand(c.seed);
        native_export::require(c.rate==44100,"family oracle packet explicitly supports Fs44100 only");
        export_case(c,method);std::cout<<"{\"status\":\"ok\"}\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
