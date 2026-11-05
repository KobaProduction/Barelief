#include "essentia_tempo_export.h"
#include <cstdlib>
#include <iostream>
namespace {
essentia_tempo_export::Case read(char** argv) {
    essentia_tempo_export::Case c;auto& d=c.data;
    std::ifstream input(argv[2],std::ios::binary|std::ios::ate);
    native_export::require(bool(input),"open Float32 input");const auto bytes=input.tellg();
    native_export::require(bytes>=0 && bytes%4==0,"Float32 input size");
    d.pcm.resize(std::size_t(bytes)/4);input.seekg(0);input.read(reinterpret_cast<char*>(d.pcm.data()),bytes);
    native_export::require(bool(input),"read Float32 input");d.prefix=argv[3];
    d.rate=std::stoul(argv[4]);d.frame=std::stoul(argv[5]);d.hop=std::stoul(argv[6]);c.seed=std::stoul(argv[7]);
    return c;
}
}
namespace essentia_tempo_export {
float noise(void*) noexcept {return float(std::rand())/float(RAND_MAX);}
}
int main(int argc,char** argv) {
    try {
        native_export::require(argc==8,"method input prefix Fs frame hop seed");
        const auto c=read(argv);const std::string method=argv[1];std::srand(c.seed);
        if(method=="period")essentia_tempo_export::period(c);
        else if(method=="single" || method=="multi")essentia_tempo_export::pcm_beat(c,method=="multi");
        else if(method=="percival")essentia_tempo_export::percival(c);
        else if(method=="novelty")essentia_tempo_export::novelty(c);
        else throw std::runtime_error("unknown Essentia tempo family");
        std::cout<<"{\"status\":\"ok\"}\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
