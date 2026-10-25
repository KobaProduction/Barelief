#include <offline/music_analysis.h>
#include <dsp_kernels/frequency_measurements.h>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <array>
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

namespace {
using namespace desktop_analysis::offline;
void u16(QByteArray& bytes,unsigned value){bytes.append(char(value));bytes.append(char(value>>8));}
void u32(QByteArray& bytes,unsigned value){u16(bytes,value);u16(bytes,value>>16);}
QString signal_file(const QString& directory,const QString& name,float amplitude,unsigned rate=48000,double first=500) {
    QByteArray bytes;bytes.append("RIFF");u32(bytes,36+4*rate);bytes.append("WAVEfmt ");u32(bytes,16);
    u16(bytes,3);u16(bytes,1);u32(bytes,rate);u32(bytes,4*rate);u16(bytes,4);u16(bytes,32);bytes.append("data");u32(bytes,4*rate);
    for(unsigned i=0;i<rate;++i){
        double value=0;for(unsigned tone=0;tone<7;++tone)value+=amplitude*std::sin(2*std::numbers::pi*(first+tone*200+10*double(i)/rate)*i/rate);
        u32(bytes,std::bit_cast<std::uint32_t>(float(value)));
    }
    const auto path=directory+"/"+name;QFile file(path);assert(file.open(QIODevice::WriteOnly));assert(file.write(bytes)==bytes.size());return path;
}
AnalysisOptions options(){AnalysisOptions o;o.compatibility.sample_rate=48000;o.compatibility.projection={dsp_kernels::SpectrumProjection::PeakAmplitude,true};o.preview_time_bins=16;o.preview_frequency_bins=32;return o;}
void retained_quiet_measurements(const QString& path) {
    auto o=options();std::atomic_bool stop=false;o.training.silence_rms=.01;
    const auto quiet=scan_music_files({path},o,stop);assert(quiet.success);
    const auto& t=quiet.document.profile.compositions.front();assert(t.active_frames && t.quiet_frames==t.active_frames && t.rms>0);
    assert(t.quiet_rms>0 && t.minimum_frame_rms>0 && t.maximum_frame_rms>=t.minimum_frame_rms);
    o.training.silence_rms=0;const auto no_threshold=scan_music_files({path},o,stop);assert(no_threshold.success);
    const auto& other=no_threshold.document.profile.compositions.front();
    assert(other.quiet_frames==t.quiet_frames && other.active_frames==t.active_frames && other.mean_spectrum==t.mean_spectrum && other.peak_spectrum==t.peak_spectrum);
    assert(t.quiet_mean_spectrum==t.mean_spectrum && t.quiet_peak_spectrum==t.peak_spectrum);
    auto classified=t;assert(dsp_kernels::classify_quiet_frames(classified,-160));assert(classified.quiet_frames==0);
    assert(dsp_kernels::classify_quiet_frames(classified,-140));assert(classified.quiet_frames==t.active_frames);
    for(std::size_t k=0;k<t.mean_spectrum.size();++k)assert(std::abs(classified.quiet_mean_spectrum[k]-t.mean_spectrum[k])<1e-12);
    assert(classified.mean_spectrum==t.mean_spectrum && classified.peak_spectrum==t.peak_spectrum);
    o.training.min_gain=std::numeric_limits<double>::quiet_NaN();o.training.max_gain=-1;
    assert(scan_music_files({path},o,stop).success);
}
void silence(const QString& path) {
    const auto o=options();std::atomic_bool stop=false;const auto scan=scan_music_files({path},o,stop);assert(scan.success);
    const auto& t=scan.document.profile.compositions.front();assert(t.active_frames==0 && t.digital_silence_frames==t.frames);
    dsp_kernels::FrequencyProfile curve;std::string error;
    assert(!dsp_kernels::build_frequency_curve(o.compatibility,scan.document.profile.compositions,{},curve,error));
    ProfileDocument restored;QString diagnostic;assert(decode_music_scan(encode_music_scan(scan.document),restored,diagnostic));
    assert(restored.profile.compositions.front().digital_silence_frames==t.frames);
}
void persistence(const QString& path,const QString& directory) {
    auto o=options();o.training.silence_rms=.1;std::atomic_bool stop=false;auto scan=scan_music_files({path},o,stop);assert(scan.success);
    QString error;assert(save_music_scan(directory+"/scan.json",scan.document,error));
    ProfileDocument restored;assert(load_music_scan(directory+"/scan.json",restored,error));
    assert(restored.profile.compositions.front().peak_spectrum==scan.document.profile.compositions.front().peak_spectrum);
    assert(restored.profile.compositions.front().quiet_mean_spectrum==scan.document.profile.compositions.front().quiet_mean_spectrum);
    std::string diagnostic;assert(dsp_kernels::build_frequency_curve(o.compatibility,restored.profile.compositions,{},restored.profile,diagnostic));
    restored.preserved["extension"]=42;assert(save_profile(directory+"/profile.json",restored,error));
    ProfileDocument profile;assert(load_profile(directory+"/profile.json",profile,error));assert(profile.profile.curve && profile.preserved.value("extension").toInt()==42);
    assert(profile.profile.gains==restored.profile.gains && profile.profile.curve->log_amplitude==restored.profile.curve->log_amplitude);
    assert(profile.profile.compositions.empty() && profile.previews.empty());
    assert(QFileInfo(directory+"/profile.json").size()<QFileInfo(directory+"/scan.json").size());
    assert(!decode_profile(encode_music_scan(scan.document),profile,error));
    assert(!decode_music_scan(encode_profile(restored),profile,error));
}
void malformed(const QString& path) {
    auto o=options();std::atomic_bool stop=false;auto scan=scan_music_files({path},o,stop);assert(scan.success);std::string diagnostic;
    assert(dsp_kernels::build_frequency_curve(o.compatibility,scan.document.profile.compositions,{},scan.document.profile,diagnostic));
    const auto original=scan.document.profile.gains;auto json=QJsonDocument::fromJson(encode_profile(scan.document)).object();
    auto function=json.value("function").toObject();function["maximum_hz"]=99999;json["function"]=function;QString error;
    assert(!decode_profile(QJsonDocument(json).toJson(),scan.document,error) && scan.document.profile.gains==original);
    json["version"]=6;assert(!decode_profile(QJsonDocument(json).toJson(),scan.document,error));
}
void native_and_fir(const QString& directory) {
    std::atomic_bool stop=false;auto o=options();
    const auto path=signal_file(directory,"high-44100.wav",.04f,44100,19900);
    const auto native=scan_music_files({path},o,stop);assert(native.success);
    const auto& n=native.document.profile.compositions.front();
    assert(n.source_sample_rate==44100 && n.sample_rate==44100 && n.samples==44100 && n.measured_max_hz==22050);
    const auto legacy=analyze_music_files({path},o,stop);assert(legacy.success && legacy.document.profile.compatibility.resampler_mode==dsp_kernels::ResamplerMode::Bypass);
    for(std::size_t k=0;k<legacy.document.profile.gains.size();++k)if(k*48000./4096>22050)assert(legacy.document.profile.gains[k]==1);
    o.resample_input=true;const auto filtered=scan_music_files({path},o,stop);assert(filtered.success);
    const auto& f=filtered.document.profile.compositions.front();
    assert(f.source_sample_rate==44100 && f.sample_rate==48000 && f.samples==48000 && f.measured_max_hz==17640);
    const auto band_peak=[](const auto& t){double peak=0;for(std::size_t k=0;k<t.peak_spectrum.size();++k)if(k*t.sample_rate/4096>20800)peak=std::max(peak,t.peak_spectrum[k]);return peak;};
    assert(band_peak(n)>.025 && band_peak(f)<.8*band_peak(n));
    const auto equal=signal_file(directory,"identity-48000.wav",.03f);const auto with_fir=scan_music_files({equal},o,stop);o.resample_input=false;
    const auto bypass=scan_music_files({equal},o,stop);assert(with_fir.success && bypass.success);
    assert(with_fir.document.profile.compositions.front().peak_spectrum==bypass.document.profile.compositions.front().peak_spectrum);
}
void legacy_scan_roundtrip(const QByteArray& bytes) {
    auto json=QJsonDocument::fromJson(bytes).object();auto tracks=json["compositions"].toArray();auto legacy=tracks[0].toObject();legacy.remove("rms_statistics");legacy.remove("analysis");tracks[0]=legacy;json["compositions"]=tracks;json["version"]=1;
    ProfileDocument loaded;QString error;
    assert(decode_music_scan(QJsonDocument(json).toJson(),loaded,error) && loaded.profile.compositions.front().rms_statistics_version==0);
    auto scanner=json["scanner"].toObject();scanner["silence_rms"]=.035;json["scanner"]=scanner;
    assert(decode_music_scan(QJsonDocument(json).toJson(),loaded,error));
    ProfileDocument legacy_copy;assert(decode_music_scan(encode_music_scan(loaded),legacy_copy,error) && legacy_copy.profile.training.silence_rms==.035);
    json["diagnostics"]=QJsonObject{{"quiet_db",-79}};assert(!decode_music_scan(QJsonDocument(json).toJson(),loaded,error));
}
void quiet_bands_roundtrip(const QString& directory) {
    std::atomic_bool stop=false;const auto path=signal_file(directory,"bands.wav",.02f);
    auto scanned=scan_music_files({path},options(),stop);assert(scanned.success);auto& t=scanned.document.profile.compositions.front();
    t.rms_bands.clear();t.active_frames=t.frames=102;t.digital_silence_frames=0;
    for(unsigned i=0;i<=dsp_kernels::rms_overflow_band;++i){
        dsp_kernels::CompositionStatistics::RmsBand b;b.index=i;b.frames=1;b.square_sum=std::pow(10.,(-201.+2*i)/10.);
        for(std::size_t k=0;k<t.mean_spectrum.size();++k){b.mean_spectrum.push_back(float(.001*(1+std::sin(double(k+i)))));b.peak_spectrum.push_back(2*b.mean_spectrum.back());}
        t.rms_bands.push_back(std::move(b));
    }
    assert(dsp_kernels::valid_rms_bands(t));const auto bytes=encode_music_scan(scanned.document);
    ProfileDocument loaded;QString error;assert(QFile::remove(path) && decode_music_scan(bytes,loaded,error));auto& restored=loaded.profile.compositions.front();
    assert(restored.rms_bands[70].mean_spectrum==t.rms_bands[70].mean_spectrum);
    assert(dsp_kernels::classify_quiet_frames(restored,-80) && restored.quiet_frames==61 && restored.quiet_rms>0);
    double mean=0;for(unsigned i=0;i<=60;++i)mean+=t.rms_bands[i].mean_spectrum[50];
    assert(std::abs(restored.quiet_mean_spectrum[50]-mean/61)<1e-15 && restored.quiet_peak_spectrum[50]>0);
    assert(bytes.size()<3*1024*1024);std::cout<<"102 occupied RMS groups: "<<bytes.size()<<" bytes per scan track\n";
    auto json=QJsonDocument::fromJson(bytes).object();json["version"]=3;
    const auto before=loaded.profile.compositions.front().quiet_frames;assert(!decode_music_scan(QJsonDocument(json).toJson(),loaded,error) && loaded.profile.compositions.front().quiet_frames==before);
    legacy_scan_roundtrip(bytes);
}
void rms_boundaries() {
    dsp_kernels::CompositionStatistics t;t.rms_statistics_version=1;t.mean_spectrum.resize(8);
    const std::array<float,8> spectrum{1,2,3,4,5,6,7,8};
    for(const auto rms:{.9e-6,1e-6,1.1e-6}){++t.active_frames;++t.frames;dsp_kernels::accumulate_rms_band(t,rms,spectrum);}
    assert(dsp_kernels::classify_quiet_frames(t,-120) && t.quiet_frames==2);
    assert(t.quiet_mean_spectrum[4]==5 && t.quiet_peak_spectrum[4]==5);
    assert(dsp_kernels::classify_quiet_frames(t,-122) && t.quiet_frames==0);
    assert(dsp_kernels::classify_quiet_frames(t,-118) && t.quiet_frames==3);
    assert(!dsp_kernels::classify_quiet_frames(t,-119) && t.quiet_frames==3);
}
void compressed_formats() {
    const auto directory=QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()+"/fixtures/music_scan/";
    const auto o=options();std::atomic_bool stop=false;
    for(const auto* suffix:{"flac","mp3"}){
        const auto result=scan_music_files({directory+"seven-changing-tones."+suffix},o,stop);
        assert(result.success && result.errors.empty());const auto& t=result.document.profile.compositions.front();
        assert(t.samples>=24000 && t.samples<30000 && t.rms>.01 && t.active_frames>0 && t.measured_max_hz==24000);
        assert(std::all_of(t.peak_spectrum.begin(),t.peak_spectrum.end(),[](double x){return std::isfinite(x) && x>=0;}));
    }
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir directory;assert(directory.isValid());
    const auto quiet=signal_file(directory.path(),"quiet.wav",1e-8f),zero=signal_file(directory.path(),"zero.wav",0);
    retained_quiet_measurements(quiet);silence(zero);persistence(quiet,directory.path());malformed(quiet);compressed_formats();native_and_fir(directory.path());quiet_bands_roundtrip(directory.path());rms_boundaries();
    std::cout<<"music_scan_tests PASS\n";
}
