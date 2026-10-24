#include <offline/music_analysis.h>
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
QString signal_file(const QString& directory,const QString& name,float amplitude) {
    constexpr unsigned rate=48000;QByteArray bytes;bytes.append("RIFF");u32(bytes,36+4*rate);bytes.append("WAVEfmt ");u32(bytes,16);
    u16(bytes,3);u16(bytes,1);u32(bytes,rate);u32(bytes,4*rate);u16(bytes,4);u16(bytes,32);bytes.append("data");u32(bytes,4*rate);
    for(unsigned i=0;i<rate;++i){
        double value=0;for(unsigned tone=0;tone<7;++tone)value+=amplitude*std::sin(2*std::numbers::pi*(500+tone*400+40*double(i)/rate)*i/rate);
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
    assert(other.quiet_frames==0 && other.active_frames==t.active_frames && other.mean_spectrum==t.mean_spectrum && other.peak_spectrum==t.peak_spectrum);
    assert(t.quiet_mean_spectrum==t.mean_spectrum && t.quiet_peak_spectrum==t.peak_spectrum);
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
    assert(encode_music_scan(profile)==encode_music_scan(restored));
    assert(!decode_profile(encode_music_scan(scan.document),profile,error));
    assert(!decode_music_scan(encode_profile(restored),profile,error));
}
void malformed(const QString& path) {
    auto o=options();std::atomic_bool stop=false;auto scan=scan_music_files({path},o,stop);assert(scan.success);std::string diagnostic;
    assert(dsp_kernels::build_frequency_curve(o.compatibility,scan.document.profile.compositions,{},scan.document.profile,diagnostic));
    const auto original=scan.document.profile.gains;auto json=QJsonDocument::fromJson(encode_profile(scan.document)).object();
    auto function=json.value("function").toObject();function["maximum_hz"]=99999;json["function"]=function;QString error;
    assert(!decode_profile(QJsonDocument(json).toJson(),scan.document,error) && scan.document.profile.gains==original);
    json["version"]=5;assert(!decode_profile(QJsonDocument(json).toJson(),scan.document,error));
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
    retained_quiet_measurements(quiet);silence(zero);persistence(quiet,directory.path());malformed(quiet);compressed_formats();
    std::cout<<"music_scan_tests PASS\n";
}
