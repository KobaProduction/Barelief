#include "native_export_io.h"
#include <iostream>
namespace {
std::vector<float> read_pcm(const char* path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    native_export::require(bool(input),"binary input open");
    const auto bytes=input.tellg();
    native_export::require(bytes>=0 && bytes%4==0,"Float32 input size");
    std::vector<float> pcm(std::size_t(bytes)/4);
    input.seekg(0);input.read(reinterpret_cast<char*>(pcm.data()),bytes);
    native_export::require(bool(input),"binary input read");
    return pcm;
}
native_export::Case arguments(char** argv) {
    native_export::Case result;
    result.frame=std::stoul(argv[2]);result.hop=std::stoul(argv[3]);result.rate=std::stoul(argv[4]);
    result.pcm=read_pcm(argv[5]);result.prefix=argv[6];result.window=std::stoul(argv[7]);
    native_export::require(result.frame>=2 && result.hop>0 && result.hop<=result.frame,"geometry");
    return result;
}
}
int main(int argc,char** argv) {
    try {
        native_export::require(argc==8,"library N hop Fs input output-prefix window-int");
        const auto data=arguments(argv);const std::string library=argv[1];
        if(library=="aubio_features")native_export::aubio_features(data);
        else throw std::runtime_error("unknown library");
        std::cout<<"{\"status\":\"ok\"}\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
