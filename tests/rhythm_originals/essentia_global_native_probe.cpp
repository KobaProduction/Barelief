// SPDX-License-Identifier: AGPL-3.0-or-later
// Calls the real pinned standard/streaming algorithms. The explicit provisioned
// mode sets public cold output capacity before network creation. No detector
// replacement, private-access macro, or processing-time buffer growth lives here.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "algorithmfactory.h"
#include "algorithm.h"
#include "essentia.h"
#include "network.h"
#include "vectorinput.h"
#include "vectoroutput.h"

namespace {
namespace e=essentia;
using Values=std::vector<e::Real>;
static_assert(sizeof(e::Real)==4,"the pinned native cache uses Float32");
Values tones(std::size_t size) {
    Values result(size);
    for(std::size_t i=0;i<size;++i)
        for(unsigned tone=0;tone<8;++tone)
            result[i]+=e::Real(.02*std::sin(6.283185307179586*(83.+tone*tone*137.)*double(i)/44100.+.15*std::sin(double(i)/44100.+tone)));
    return result;
}
void save(const std::string& path,const Values& values) {
    if(std::ifstream(path.c_str()).good())throw std::runtime_error("refusing to overwrite probe output");
    std::ofstream output(path.c_str(),std::ios::binary);
    output.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(e::Real)));
    if(!output)throw std::runtime_error("cannot write probe output");
}
void require_finite(const Values& values) {
    if(!std::all_of(values.begin(),values.end(),[](e::Real value){return std::isfinite(value);}))
        throw std::runtime_error("PROBE_DATA_ERROR: nonfinite native output");
}
void compute(const std::string& method,const Values& pcm,int frame,int hop,Values& output) {
    std::unique_ptr<e::standard::Algorithm> algorithm(e::standard::AlgorithmFactory::create("OnsetDetectionGlobal"));
    algorithm->configure("method",method,"sampleRate",44100.f,"frameSize",frame,"hopSize",hop);
    algorithm->input("signal").set(pcm);algorithm->output("onsetDetections").set(output);
    algorithm->compute();require_finite(output);
}
int bounds(const std::string& prefix) {
    const auto pcm=tones(4096+17);Values output;
    std::puts("NATIVE phase=beat_emphasis_begin input_samples=4113 frame=2048 hop=512");std::fflush(stdout);
    compute("beat_emphasis",pcm,2048,512,output);save(prefix+".f32",output);
    std::printf("NATIVE phase=beat_emphasis_complete count=%zu\n",output.size());
    return output.empty()?1:0;
}
int reuse(const std::string& method,const std::string& prefix) {
    const auto pcm=tones(4096+17);Values empty;
    compute(method,pcm,2048,512,empty);
    if(empty.empty())throw std::runtime_error("native baseline unexpectedly empty");
    Values prefilled(empty.size(),17.f);compute(method,pcm,2048,512,prefilled);
    save(prefix+".empty.f32",empty);save(prefix+".prefilled.f32",prefilled);
    const bool same=empty==prefilled;
    std::printf("REUSE method=%s fresh_instances=1 empty_count=%zu prefilled_count=%zu equal=%d\n",
        method.c_str(),empty.size(),prefilled.size(),int(same));
    return 0;
}
std::size_t reference_frames(const Values& pcm,std::size_t expected,const std::string& prefix) {
    Values reference;compute("infogain",pcm,16,1,reference);
    std::printf("FRAMING frame=16 hop=1 input_samples=%zu expected=%zu actual=%zu\n",pcm.size(),expected,reference.size());
    std::fflush(stdout);
    if(reference.size()!=expected)throw std::runtime_error("PROBE_SETUP_ERROR: shortened framing did not produce requested token count");
    save(prefix+".reference.f32",reference);
    return reference.size();
}
void provision_output(e::streaming::Algorithm& detector,std::size_t count) {
    auto& output=detector.output("onsetDetections");auto info=output.bufferInfo();
    if(count>std::size_t(std::numeric_limits<int>::max()))throw std::runtime_error("PROBE_SETUP_ERROR: buffer capacity overflow");
    info.size=int(count);output.setBufferInfo(info);
    if(output.bufferInfo().size<int(count))throw std::runtime_error("PROBE_SETUP_ERROR: public buffer override not applied");
    std::printf("PROVISION phase=before_network capacity=%d contiguous=%d\n",output.bufferInfo().size,output.bufferInfo().maxContiguousElements);
}
int publish_stream(const std::string& prefix,std::size_t count,const Values& output) {
    save(prefix+".f32",output);
    std::printf("BURST phase=complete expected=%zu actual=%zu\n",count,output.size());
    return output.size()==count?0:1;
}
int stream(std::size_t expected,const std::string& prefix,bool provision=false) {
    if(expected<327679 || expected>327681)throw std::runtime_error("PROBE_SETUP_ERROR: unsupported boundary count");
    const auto pcm=tones(expected+15);const auto count=reference_frames(pcm,expected,prefix);Values output;
    e::streaming::VectorInput<e::Real> input(&pcm);
    std::unique_ptr<e::streaming::Algorithm> detector(e::streaming::AlgorithmFactory::create("OnsetDetectionGlobal"));
    e::streaming::VectorOutput<e::Real> sink(&output);
    detector->configure("method","infogain","sampleRate",44100.f,"frameSize",16,"hopSize",1);
    if(provision)provision_output(*detector,count);
    input.output("data") >> detector->input("signal");detector->output("onsetDetections") >> sink.input("data");
    e::scheduler::Network network(&input,false);
    std::printf("BURST phase=begin odf_count=%zu pinned_capacity=327680 collector=VectorOutput\n",count);std::fflush(stdout);
    network.run();return publish_stream(prefix,count,output);
}
int dispatch(int argc,char** argv) {
    if(argc<3 || argc>4)throw std::runtime_error("PROBE_SETUP_ERROR: mode prefix [token_count] required");
    const std::string mode=argv[1],prefix=argv[2];
    if(mode=="bounds")return bounds(prefix);
    if(mode=="reuse-info")return reuse("infogain",prefix);
    if(mode=="reuse-emphasis")return reuse("beat_emphasis",prefix);
    if((mode=="stream" || mode=="stream-provisioned") && argc==4)
        return stream(std::size_t(std::stoull(argv[3])),prefix,mode=="stream-provisioned");
    throw std::runtime_error("PROBE_SETUP_ERROR: unknown native probe mode");
}
}
int main(int argc,char** argv) {
    try {
        e::init();const int result=dispatch(argc,argv);e::shutdown();return result;
    }catch(const std::exception& error){std::fprintf(stderr,"NATIVE_EXCEPTION: %s\n",error.what());e::shutdown();return 2;}
}
