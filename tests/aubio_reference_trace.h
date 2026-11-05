// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/runtime.h>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace aubio_trace {
struct Trace {
    std::vector<float> magnitude,phase,onset,tempo;
    std::vector<std::uint64_t> onset_sample,beat_sample,tatum;
    std::size_t frame=0;
    explicit Trace(std::size_t frames):magnitude(frames*513),phase(frames*513),onset(frames*3),tempo(frames*4),
        onset_sample(frames),beat_sample(frames),tatum(frames){}
    template<class T> static void scalar(const signal_contracts::NumericView& value,T& output) {
        if(signal_contracts::load_element(value,{0,0,0},output)!=signal_contracts::Status::ok)std::abort();
    }
    void row(const signal_contracts::NumericView& value,std::vector<float>& values) {
        for(std::size_t i=0;i<513;++i)
            if(signal_contracts::load_element(value,{0,i,0},values[(frame-1)*513+i])!=signal_contracts::Status::ok)std::abort();
    }
    void capture(domain::NodeId node,domain::PortId port,const signal_contracts::NumericView& value) {
        if(node==12)row(value,magnitude);if(node==13)row(value,phase);
        onset_event(node,port,value);
        if(node==33)tracking(port,value);
    }
    void onset_event(domain::NodeId node,domain::PortId port,const signal_contracts::NumericView& value) {
        if(node==21)scalar(value,onset[(frame-1)*3]);
        if(node==22 && port==2)scalar(value,onset[(frame-1)*3+1]);
        if(node==24 && port==0)scalar(value,onset[(frame-1)*3+2]);
        if(node==24 && port==1)scalar(value,onset_sample[frame-1]);
    }
    void tracking(domain::PortId port,const signal_contracts::NumericView& value) {
        if(port==2)scalar(value,tempo[(frame-1)*4]);
        if(port==4)scalar(value,tempo[(frame-1)*4+1]);
        if(port==5)scalar(value,tempo[(frame-1)*4+2]);
        if(port==3)scalar(value,tempo[(frame-1)*4+3]);
        beat_event(port,value);
    }
    void beat_event(domain::PortId port,const signal_contracts::NumericView& value) {
        if(port==6){std::int64_t v=0;scalar(value,v);beat_sample[frame-1]=std::uint64_t(v);}
        if(port==8){std::uint32_t v=0;scalar(value,v);tatum[frame-1]=v;}
    }
    template<class T> static void save(const std::filesystem::path& path,const std::vector<T>& values) {
        std::ofstream file(path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()*sizeof(T)));
        if(!file)std::abort();
    }
    void export_values(const char* path,std::span<const float> pcm) const {
        const std::filesystem::path root(path);if(!std::filesystem::create_directory(root))std::abort();
        save(root/"norm.f32",magnitude);save(root/"phase.f32",phase);save(root/"onsets.f32",onset);save(root/"tempo.f32",tempo);
        save(root/"onset_last.u64",onset_sample);save(root/"tempo_last.u64",beat_sample);save(root/"tatum.u64",tatum);
        const std::vector<float> copy(pcm.begin(),pcm.end());save(root/"pcm.f32",copy);
    }
};
}
