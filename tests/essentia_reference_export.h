// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "essentia_reference_fixture.h"
#include <filesystem>
#include <fstream>
namespace essentia_reference {
inline void save(const std::filesystem::path& file,std::span<const float> values) {
    std::ofstream output(file,std::ios::binary);
    output.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size_bytes()));
    check(bool(output),"write graph evidence");
}
inline void export_metadata(const std::filesystem::path& path,bool multi,std::size_t samples) {
    std::ofstream metadata(path/"recipe.json");
    metadata<<"{\"revision\":\"b9fa6cb674ca43dfb94d28d293aeda441c6745db\",\"sample_rate\":44100,"
        "\"frame\":2048,\"primary_hop\":1024,\"global_hop\":512,\"seed\":17,\"session_samples\":"<<samples<<","
        "\"frame_tail\":\"CoverEnd\",\"time_origin\":\"session-relative seconds\",\"method\":\""
        <<(multi?"multifeature":"degara")<<"\"";
    if(multi)metadata<<",\"agreement_outputs\":{\"scores.f32\":{\"dtype\":\"Float32\",\"shape\":[5]},"
        "\"selected.u64\":{\"dtype\":\"UInt64\",\"shape\":[1]},\"pairwise.f32\":{\"dtype\":\"Float32\",\"shape\":[5,5]}}";
    metadata<<"}";
    check(bool(metadata),"write recipe metadata");
}
inline void export_agreement(const std::filesystem::path& path,const Observation& observed) {
    save(path/"scores.f32",observed.scores);save(path/"pairwise.f32",observed.pairwise);
    std::ofstream selected(path/"selected.u64",std::ios::binary);
    selected.write(reinterpret_cast<const char*>(&observed.selected),sizeof(observed.selected));
    check(bool(selected),"write selected agreement method");
}
inline void export_case(const char* root,EssentiaReferenceMethod method,bool silence,
    std::span<const float> pcm,const Observation& observed,const char* case_name=nullptr) {
    if(!root)return;
    const bool multi=method==EssentiaReferenceMethod::MultiFeature;
    const auto path=std::filesystem::path(root)/(case_name?case_name:(multi?(silence?"multi-silence":"multi-tones8"):"single-tones8"));
    check(std::filesystem::create_directories(path),"evidence case directory must be new; preserve prior exports");save(path/"pcm.f32",pcm);
    save(path/"ticks.f32",multi?std::span<const float>(observed.ticks):std::span<const float>(observed.candidates[0]));
    save(path/"intervals.f32",observed.intervals);save(path/"estimates.f32",observed.estimates);
    save(path/"bpm.f32",std::span(&observed.bpm,1));save(path/"confidence.f32",std::span(&observed.confidence,1));
    for(std::size_t i=0;i<(multi?5u:1u);++i){
        save(path/("curve"+std::to_string(i)+".f32"),observed.curves[i]);
        save(path/("candidate"+std::to_string(i)+".f32"),observed.candidates[i]);
    }
    if(multi)export_agreement(path,observed);
    export_metadata(path,multi,pcm.size());
}
}
