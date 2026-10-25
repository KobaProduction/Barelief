#include <offline/music_analysis.h>
#include <offline/archive_zip.h>
#include <offline/archive_blocks.h>
#include <dsp_kernels/frequency_measurements.h>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace desktop_analysis::offline;
using Entries=QMap<QString,QByteArray>;
QByteArray read_file(const QString& path){QFile f(path);assert(f.open(QIODevice::ReadOnly));return f.readAll();}
void write_file(const QString& path,QByteArrayView bytes){QSaveFile f(path);assert(f.open(QIODevice::WriteOnly));assert(f.write(bytes.data(),bytes.size())==bytes.size());assert(f.commit());}
Entries read_zip(const QString& path) {
    QFile file(path);assert(file.open(QIODevice::ReadOnly));StoredZipReader zip(file);QString error;assert(zip.open(error));Entries result;
    for(const auto& name:zip.entries())assert(zip.read(name,music_archive_limit,result[name],error));
    return result;
}
void write_zip(const QString& path,const Entries& entries) {
    QSaveFile file(path);assert(file.open(QIODevice::WriteOnly));StoredZipWriter zip(file);QString error;assert(zip.open(error));
    for(auto i=entries.begin();i!=entries.end();++i)assert(zip.add(i.key(),i.value(),error));
    assert(zip.finish(error));assert(file.commit());
}
QJsonObject manifest(const Entries& entries){return QJsonDocument::fromJson(entries.value("manifest.json")).object();}
void set_manifest(Entries& entries,const QJsonObject& root){entries["manifest.json"]=QJsonDocument(root).toJson(QJsonDocument::Compact);}
ProfileDocument measurements() {
    AnalysisOptions o;o.compatibility.sample_rate=48000;o.compatibility.projection={dsp_kernels::SpectrumProjection::PeakAmplitude,true};
    o.preview_time_bins=16;o.preview_frequency_bins=32;o.workers=1;std::atomic_bool stop=false;
    const auto audio=QFileInfo(QString::fromUtf8(__FILE__)).absolutePath()+"/fixtures/music_scan/seven-changing-tones.flac";
    auto result=scan_music_files({audio},o,stop);assert(result.success);return std::move(result.document);
}
void add_groups(ProfileDocument& d) {
    auto& t=d.profile.compositions.front();t.rms_bands.clear();t.frames=t.active_frames=102;t.digital_silence_frames=0;
    for(unsigned i=0;i<=dsp_kernels::rms_overflow_band;++i){
        dsp_kernels::CompositionStatistics::RmsBand b;b.index=i;b.frames=1;b.square_sum=std::pow(10.,(-201.+2*i)/10.);
        for(std::size_t k=0;k<t.mean_spectrum.size();++k){b.mean_spectrum.push_back(float(.001*(1+std::sin(double(k+i)))));b.peak_spectrum.push_back(2*b.mean_spectrum.back());}
        t.rms_bands.push_back(std::move(b));
    }
    d.archive_preserved["outer_extension"]=QJsonObject{{"value",17}};
    d.preserved["extension"]=42;
    d.preserved["compositions"]=QJsonArray{QJsonObject{{"mean_spectrum",QJsonObject{{"array_extension","keep"}}}}};
}
void same_measurements(const ProfileDocument& a,const ProfileDocument& b) {
    const auto& x=a.profile.compositions.front();const auto& y=b.profile.compositions.front();
    assert(x.mean_spectrum==y.mean_spectrum && x.peak_spectrum==y.peak_spectrum && x.frequency_presence==y.frequency_presence);
    assert(x.source_sample_rate==y.source_sample_rate && x.sample_rate==y.sample_rate && x.measured_max_hz==y.measured_max_hz);
    assert(x.rms_bands.size()==y.rms_bands.size() && x.rms==y.rms && x.frames==y.frames && x.active_frames==y.active_frames);
    for(std::size_t i=0;i<x.rms_bands.size();++i){
        const auto& p=x.rms_bands[i];const auto& q=y.rms_bands[i];
        assert(p.index==q.index && p.frames==q.frames && p.square_sum==q.square_sum && p.mean_spectrum==q.mean_spectrum && p.peak_spectrum==q.peak_spectrum);
    }
    assert(a.previews.front().values==b.previews.front().values && a.previews.front().time_seconds==b.previews.front().time_seconds && a.previews.front().frequency_hz==b.previews.front().frequency_hz);
}
void scan_roundtrip(const QString& path,const ProfileDocument& original) {
    QString error;assert(save_music_scan(path,original,error));assert(read_file(path).startsWith(QByteArray("PK\x03\x04",4)));
    ProfileDocument loaded;const auto loaded_ok=load_music_scan(path,loaded,error);if(!loaded_ok)std::cerr<<error.toStdString()<<'\n';assert(loaded_ok);same_measurements(original,loaded);
    auto& track=loaded.profile.compositions.front();assert(dsp_kernels::classify_quiet_frames(track,-120));assert(track.quiet_frames==41);
    assert(dsp_kernels::classify_quiet_frames(track,-80) && track.quiet_frames==61);
    const auto files=read_zip(path);const auto root=manifest(files);assert(root.value("compression").toString()=="blosc2-zstd5-shuffle-v1");
    assert(files.size()==11 && root.value("blocks").toArray().size()==10);
    const auto matrix=files.value("arrays/tracks/0/rms/mean_spectrum.b2");assert(!matrix.isEmpty());
    assert(root.value("document").toObject().value("compositions").toArray()[0].toObject().value("mean_spectrum").toObject().value("array_extension").toString()=="keep");
    std::cout<<"Scan JSON="<<encode_music_scan(original).size()<<" bytes, archive="<<read_file(path).size()<<" bytes\n";
}
void extensions_roundtrip(const QString& path,const ProfileDocument& original) {
    auto files=read_zip(path);auto root=manifest(files);auto blocks=root.value("blocks").toArray();
    auto block=blocks[0].toObject();block["block_extension"]="keep";blocks[0]=block;root["blocks"]=blocks;set_manifest(files,root);write_zip(path,files);
    ProfileDocument loaded;QString error;assert(load_music_scan(path,loaded,error));
    assert(loaded.preserved.value("extension").toInt()==42 && loaded.archive_preserved.value("outer_extension").toObject().value("value").toInt()==17);
    assert(save_music_scan(path,loaded,error));root=manifest(read_zip(path));
    assert(root.value("blocks").toArray()[0].toObject().value("block_extension").toString()=="keep");same_measurements(original,loaded);
}
void rejected_scan(const QString& path,const ProfileDocument& original) {
    auto loaded=original;const auto before=encode_music_scan(loaded);QString error;
    assert(!load_music_scan(path,loaded,error) && !error.isEmpty());assert(encode_music_scan(loaded)==before);
}
void bad_manifests(const QString& path,const Entries& valid,const ProfileDocument& original) {
    for(const auto* field:{"version","kind","content","compression"}){
        auto files=valid;auto root=manifest(files);root[field]=QStringLiteral("unsupported");set_manifest(files,root);write_zip(path,files);rejected_scan(path,original);
    }
    auto files=valid;files.remove("manifest.json");write_zip(path,files);rejected_scan(path,original);
    files=valid;files["manifest.json"]="{";write_zip(path,files);rejected_scan(path,original);
    files=valid;files["extra.bin"]="unknown";write_zip(path,files);rejected_scan(path,original);
    files=valid;files.remove("arrays/tracks/0/rms/mean_spectrum.b2");write_zip(path,files);rejected_scan(path,original);
}
void bad_descriptors(const QString& path,const Entries& valid,const ProfileDocument& original) {
    for(const auto* field:{"dtype","bytes","sha256","shape","entry"}){
        auto files=valid;auto root=manifest(files);auto blocks=root.value("blocks").toArray();auto block=blocks[0].toObject();
        block[field]=field==QStringLiteral("shape")?QJsonValue(QJsonArray{1073741824.,2049}):QJsonValue(QStringLiteral("invalid"));
        blocks[0]=block;root["blocks"]=blocks;set_manifest(files,root);write_zip(path,files);rejected_scan(path,original);
    }
    auto files=valid;auto root=manifest(files);auto blocks=root.value("blocks").toArray();blocks.append(blocks[0]);root["blocks"]=blocks;
    set_manifest(files,root);write_zip(path,files);rejected_scan(path,original);
}
void bad_slice(const QString& path,const Entries& valid,const ProfileDocument& original) {
    auto files=valid;auto root=manifest(files);auto document=root.value("document").toObject();auto tracks=document.value("compositions").toArray();
    auto track=tracks[0].toObject();auto reference=track.value("mean_spectrum").toObject();reference["offset"]=1e9;track["mean_spectrum"]=reference;
    tracks[0]=track;document["compositions"]=tracks;root["document"]=document;set_manifest(files,root);write_zip(path,files);rejected_scan(path,original);
}
void damaged_payloads(const QString& path,const Entries& valid,const ProfileDocument& original) {
    const QString entry="arrays/tracks/0/rms/mean_spectrum.b2";
    for(int offset:{0,3,16,21,22,24,31,40}){
        auto files=valid;files[entry][offset]=char(static_cast<unsigned char>(files[entry][offset])^0x7f);
        write_zip(path,files);rejected_scan(path,original);
    }
    auto files=valid;files[entry].chop(1);write_zip(path,files);rejected_scan(path,original);
    files=valid;files[entry]+=QByteArray(1,'\0');write_zip(path,files);rejected_scan(path,original);
}
void damaged_zip(const QString& path,const Entries& valid,const ProfileDocument& original) {
    write_zip(path,valid);const auto bytes=read_file(path);
    write_file(path,QByteArrayView(bytes).first(bytes.size()-13));rejected_scan(path,original);
    auto bad=bytes;bad.replace("peak_spectrum","mean_spectrum");write_file(path,bad);rejected_scan(path,original);
    bad=bytes;bad.replace("arrays/","../out/");write_file(path,bad);rejected_scan(path,original);
    bad=bytes;const auto name=valid.firstKey().toUtf8();const auto offset=30+name.size();assert(offset<bad.size());bad[offset]=char(static_cast<unsigned char>(bad[offset])^1);
    write_file(path,bad);rejected_scan(path,original);
}
void atomic_save(const QString& path,const ProfileDocument& original) {
    QString error;assert(save_music_scan(path,original,error));const auto bytes=read_file(path);
    auto invalid=original;invalid.profile.compositions.front().rms_bands[0].mean_spectrum[0]=std::numeric_limits<float>::quiet_NaN();
    assert(!save_music_scan(path,invalid,error) && read_file(path)==bytes);
}
void legacy_json(const QString& directory,const ProfileDocument& original) {
    const auto path=directory+"/legacy.json";QString error;assert(save_music_scan(path,original,error));assert(read_file(path).startsWith('{'));
    ProfileDocument restored;assert(load_music_scan(path,restored,error));same_measurements(original,restored);
    assert(save_music_scan(directory+"/legacy-converted.brscan",restored,error));
    assert(load_music_scan(directory+"/legacy-converted.brscan",restored,error));same_measurements(original,restored);
}
void scan_without_normalizer(const QString& path,const ProfileDocument& original) {
    auto raw=original;raw.profile.gains.clear();raw.profile.mean_spectrum.clear();raw.profile.training.min_gain=std::numeric_limits<float>::quiet_NaN();
    QString error;assert(save_music_scan(path,raw,error));ProfileDocument loaded;assert(load_music_scan(path,loaded,error));same_measurements(original,loaded);
}
template<class T> void constant_blocks() {
    for(const auto value:{T(0),T(-0.),T(1),std::numeric_limits<T>::denorm_min()}){
        const auto raw=array_detail::bytes(std::vector<T>(2049,value));const auto packed=compress_music_block(raw,sizeof(T));QByteArray restored;
        assert(!packed.isEmpty() && decompress_music_block(packed,sizeof(T),raw.size(),restored) && restored==raw);
    }
}
void profile_roundtrip(const QString& path,const ProfileDocument& measurements) {
    auto original=measurements;std::string diagnostic;assert(dsp_kernels::build_frequency_curve(original.profile.compatibility,original.profile.compositions,{},original.profile,diagnostic));
    QString error;assert(save_profile(path,original,error));ProfileDocument loaded;assert(load_profile(path,loaded,error));
    assert(loaded.profile.curve && loaded.profile.compositions.empty() && loaded.previews.empty());
    assert(loaded.profile.gains==original.profile.gains && loaded.profile.curve->log_amplitude==original.profile.curve->log_amplitude);
    for(const auto rate:{32768.,44100.,48000.})for(const auto fft:{512u,2048u,8192u}){
        auto target=original.profile.compatibility;target.sample_rate=rate;target.fft_size=fft;dsp_kernels::FrequencyProfile a,b;
        assert(dsp_kernels::resample_frequency_profile(original.profile,target,a,diagnostic) && dsp_kernels::resample_frequency_profile(loaded.profile,target,b,diagnostic));assert(a.gains==b.gains);
    }
    assert(!load_music_scan(path,loaded,error));
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir temporary;assert(temporary.isValid());constant_blocks<float>();constant_blocks<double>();auto original=measurements();add_groups(original);
    const auto scan=temporary.filePath("измерения.brscan"),bad=temporary.filePath("bad.brscan");scan_roundtrip(scan,original);extensions_roundtrip(scan,original);
    const auto valid=read_zip(scan);bad_manifests(bad,valid,original);bad_descriptors(bad,valid,original);bad_slice(bad,valid,original);
    damaged_payloads(bad,valid,original);damaged_zip(bad,valid,original);atomic_save(scan,original);legacy_json(temporary.path(),original);
    scan_without_normalizer(temporary.filePath("raw.brscan"),original);
    profile_roundtrip(temporary.filePath("профиль.brprofile"),original);
    const auto evidence=qEnvironmentVariable("BARELIEF_ARCHIVE_EVIDENCE");QString error;
    if(!evidence.isEmpty()){assert(save_music_scan(evidence+"/sample.brscan",original,error));profile_roundtrip(evidence+"/sample.brprofile",original);}
    std::cout<<"music_archive_tests PASS\n";
}
