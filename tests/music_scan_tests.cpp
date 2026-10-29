#include <offline/music_analysis.h>
#include <offline/music_archive.h>
#include <dsp_kernels/frequency_measurements.h>
#include <QCoreApplication>
#include <QDir>
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
void large_folder_enumeration(const QString& directory) {
    const auto folder=directory+"/many-tracks";
    assert(QDir().mkpath(folder+"/nested"));
    constexpr int count=4121;
    for(int i=0;i<count;++i){
        const auto parent=i%2?folder:folder+"/nested";
        const auto path=parent+QString("/track-%1.mp3").arg(i,5,10,QLatin1Char('0'));
        QFile file(path);assert(file.open(QIODevice::WriteOnly));file.close();
    }
    const auto all=scan_audio_files(folder,true);
    assert(all.size()==count);
    assert(std::is_sorted(all.cbegin(),all.cend()));
    const auto direct=scan_audio_files(folder,false);
    assert(direct.size()==count/2);
    assert(scan_audio_files(folder+"/missing",true).empty());
}
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
void multi_part_archive_scan(const QString& sample_path,const QString& directory) {
    std::atomic_bool initial_stop=false;
    const auto seed=scan_music_files({sample_path},options(),initial_stop);
    assert(seed.success);
    const auto base=seed.document.profile.compositions.front();
    QStringList paths;
    for(int i=0;i<7;++i)paths.append(QStringLiteral("source_%1.mp3").arg(i));
    std::atomic_bool stop=false;
    std::vector<std::size_t> progress_calls;
    int invocations=0;
    const auto scan=[&](const QStringList& batch,const AnalysisOptions&,const std::atomic_bool&,
                        const AnalysisServices::Progress& progress)->AnalysisResult {
        ++invocations;
        AnalysisResult result;
        result.success=true;result.document=seed.document;
        result.document.previews.clear();
        result.document.profile.compositions.clear();
        // Deliberately attach previews inconsistent with the actual duration.
        // Bulk scientific scans must preserve measured spectra and omit these
        // expendable derivatives instead of failing the whole corpus.
        SpectrogramPreview bad_preview;
        bad_preview.rows=1;bad_preview.bands=1;
        bad_preview.duration_seconds=-1;
        result.document.previews.assign(std::size_t(batch.size()),bad_preview);
        for(const auto& path:batch){
            auto composition=base;
            composition.source=path.toStdString();
            result.document.profile.compositions.push_back(std::move(composition));
        }
        result.document.preserved["analysis_run"]=QJsonObject{
            {"requested_count",int(batch.size())},
            {"processed_count",int(batch.size())},
            {"failed_count",0},
            {"audio_seconds",1.},
            {"errors",QJsonArray{}}
        };
        result.document.preserved["scanner"]=QJsonObject{
            {"resample_input",false},{"hop",double(options().training.hop)},
            {"interval_numerator",double(options().training.interval_numerator)},
            {"interval_denominator",double(options().training.interval_denominator)}
        };
        if(progress)progress(std::size_t(batch.size()),std::size_t(batch.size()));
        return result;
    };
    const decltype(AnalysisServices::save_scan) save=[](const QString& path,const ProfileDocument& doc,QString& error) {
        return save_music_scan(path,doc,error);
    };
    const auto output=directory+"/parts";
    assert(QDir().mkpath(output));
    // Simulate abandoned data left by a previous hard crash.
    const auto stale=output+"/.barelief-scan-incomplete-obsolete";
    assert(QDir().mkpath(stale));
    QFile orphan(stale+"/orphan.tmp");
    assert(orphan.open(QIODevice::WriteOnly));
    orphan.write("garbage");orphan.close();

    auto result=scan_music_in_parts(paths,options(),stop,
        [&](std::size_t n,std::size_t total){assert(total==7);progress_calls.push_back(n);},
        scan,save,output,3);
    assert(result.success && result.saved_part_count==3 && result.requested_track_count==7);
    assert(result.errors.empty() && invocations==3 && progress_calls==std::vector<std::size_t>({3,6,7}));
    assert(!QFileInfo::exists(stale));
    const auto finalized=result.batch_directory;
    assert(QFileInfo(finalized).isFile() && finalized.endsWith(".brscan"));
    MusicScanCollectionInfo info;QString diagnostic;
    assert(inspect_music_scan(finalized,info,diagnostic));
    assert(info.requested_count==7 && info.processed_count==7 &&
           info.failed_count==0 && info.part_count==3);
    std::size_t verified=0;
    assert(visit_music_scan_parts(finalized,[&](ProfileDocument&& loaded,std::size_t index){
        assert(index==verified);
        assert(std::all_of(loaded.previews.begin(),loaded.previews.end(),
            [](const auto& preview){return preview.rows==0 && preview.values.empty();}));
        assert(loaded.profile.compositions.size()==(index==2?1:3));
        assert(loaded.profile.compositions.front().source==paths[index*3].toStdString());
        ++verified;return true;
    },info,diagnostic));
    assert(verified==3);
    ProfileDocument corpus_summary;
    assert(load_music_scan(finalized,corpus_summary,diagnostic));
    assert(corpus_summary.profile.compositions.empty());
    const auto measured=corpus_summary.preserved.value("scan_collection").toObject();
    assert(measured.value("processed_count").toInt()==7 &&
           measured.value("part_count").toInt()==3 &&
           corpus_summary.profile.peak_spectrum.size()==2049);
    MusicScanSummaryAccumulator reference;
    assert(visit_music_scan_parts(finalized,[&](ProfileDocument&& chunk,std::size_t){
        return reference.add(chunk,diagnostic);
    },info,diagnostic));
    const auto cached=reference.result();
    for(int bin=0;bin<2049;++bin){
        assert(std::abs(corpus_summary.profile.peak_spectrum[bin]-
            cached.value("peak_spectrum").toArray()[bin].toDouble())<1e-12);
        assert(std::abs(corpus_summary.profile.mean_spectrum[bin]-
            cached.value("mean_spectrum").toArray()[bin].toDouble())<1e-12);
    }
    const auto raw_parts=directory+"/legacy-check-parts";
    assert(QDir().mkpath(raw_parts));
    QJsonArray raw_descriptors;
    assert(visit_music_scan_parts(finalized,[&](ProfileDocument&& chunk,std::size_t index){
        const auto name=QStringLiteral("music-scan-part-%1.brscan")
            .arg(index+1,6,10,QLatin1Char('0'));
        if(!save_music_scan_part(raw_parts+"/"+name,chunk,diagnostic))return false;
        const auto measured=int(chunk.profile.compositions.size());
        raw_descriptors.append(QJsonObject{{"name",name},{"requested_count",measured},
                                           {"processed_count",measured},{"failed_count",0}});
        return true;
    },info,diagnostic));
    const QJsonObject raw_manifest{{"kind","barelief.music_scan_collection"},
        {"version",1},{"requested_count",7},{"processed_count",7},
        {"failed_count",0},{"fft_size",4096},{"parts",raw_descriptors}};
    const auto without_cache=directory+"/no-cache.brscan";
    assert(save_music_scan_collection(QDir(raw_parts),without_cache,
                                      raw_manifest,raw_descriptors,diagnostic));
    ProfileDocument uncached;
    assert(load_music_scan(without_cache,uncached,diagnostic));
    for(int bin=0;bin<2049;++bin){
        assert(std::abs(uncached.profile.peak_spectrum[bin]-
                        corpus_summary.profile.peak_spectrum[bin])<1e-12);
        assert(std::abs(uncached.profile.mean_spectrum[bin]-
                        corpus_summary.profile.mean_spectrum[bin])<1e-12);
    }
    assert(QDir(output).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden).empty());
    // Re-running performs fresh analysis; old output is not used or overwritten.
    progress_calls.clear();
    const auto repeated=scan_music_in_parts(paths,options(),stop,
        [&](std::size_t n,std::size_t total){assert(total==7);progress_calls.push_back(n);},
        scan,save,output,3);
    assert(repeated.success && repeated.saved_part_count==3 && invocations==6);
    assert(repeated.batch_directory!=finalized && progress_calls==std::vector<std::size_t>({3,6,7}));
    assert(QFileInfo(finalized).isFile() && QFileInfo(repeated.batch_directory).isFile());
    // A fatal error mid-run must leave no partial parts or committed collection.
    const auto fail_dir=directory+"/failure";
    assert(QDir().mkpath(fail_dir));
    int calls=0;
    const decltype(AnalysisServices::scan_files) broken=
        [&](const QStringList& batch,const AnalysisOptions& opts,const std::atomic_bool& stopped,
            const AnalysisServices::Progress& update)->AnalysisResult{
            ++calls;
            if(calls==2){AnalysisResult failure;failure.errors.append("Decoder fatal");return failure;}
            return scan(batch,opts,stopped,update);
        };
    const auto failed=scan_music_in_parts(paths,options(),stop,{},broken,save,fail_dir,3);
    assert(!failed.success && failed.saved_part_count==0 && !failed.errors.empty());
    assert(QDir(fail_dir).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden).empty());
    // Cancellation cleans temporary output as well.
    const auto cancel_dir=directory+"/cancelled";
    assert(QDir().mkpath(cancel_dir));
    std::atomic_bool aborted=false;
    const auto cancelled=scan_music_in_parts(paths,options(),aborted,
        [&](std::size_t n,std::size_t){if(n==3)aborted.store(true);},
        scan,save,cancel_dir,3);
    assert(cancelled.cancelled && !cancelled.success && cancelled.saved_part_count==0);
    assert(QDir(cancel_dir).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden).empty());

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
    json["version"]=7;assert(!decode_profile(QJsonDocument(json).toJson(),scan.document,error));
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
void native_cadence_uses_track_clock(const QString& directory) {
    auto o=options();o.compatibility.fft_size=512;o.compatibility.sample_rate=48000;
    o.training.interval_numerator=1;o.training.interval_denominator=64;std::atomic_bool stop=false;
    const auto path=signal_file(directory,"native-clock-32768.wav",.03f,32768);
    const auto first=scan_music_files({path},o,stop);assert(first.success);
    assert(first.document.profile.compatibility.sample_rate==32768);
    o.compatibility.sample_rate=96000;const auto second=scan_music_files({path},o,stop);assert(second.success);
    assert(first.document.profile.compositions[0].peak_spectrum==second.document.profile.compositions[0].peak_spectrum);
    QString error;const auto stored=directory+"/native-clock.brscan";assert(save_music_scan(stored,second.document,error));
    ProfileDocument restored;assert(load_music_scan(stored,restored,error) && restored.profile.compatibility.sample_rate==32768);
    o.resample_input=true;assert(!scan_music_files({path},o,stop).success);
}
void legacy_scan_roundtrip(const QByteArray& bytes) {
    auto json=QJsonDocument::fromJson(bytes).object();auto tracks=json["compositions"].toArray();auto legacy=tracks[0].toObject();legacy.remove("rms_statistics");legacy.remove("analysis");tracks[0]=legacy;json["compositions"]=tracks;json["version"]=1;
    auto metadata=json["metadata"].toObject();auto grid=metadata["grid"].toObject();
    grid["kind"]="rfft_hz_dc_nyquist";grid["step_hz"]=metadata["sample_rate"].toDouble()/metadata["fft_size"].toDouble();metadata["grid"]=grid;json["metadata"]=metadata;
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
    auto json=QJsonDocument::fromJson(bytes).object();json["version"]=4;
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
void corrupted_audio_does_not_invalidate_scan_archive(const QString& directory) {
    std::atomic_bool cancelled=false;
    const auto good=signal_file(directory,"good-for-batch.wav",.04f);
    const auto bad=directory+"/truncated.mp3";
    QFile file(bad);assert(file.open(QIODevice::WriteOnly));
    assert(file.write("ID3\\x04\\0\\0\\0\\0\\0\\0",10)==10);file.close();
    const auto batch=scan_music_files({good,bad},options(),cancelled);
    assert(batch.success && batch.document.profile.compositions.size()==1);
    assert(batch.errors.size()==1 && batch.document.previews.empty());
    assert(batch.document.profile.compositions.front().source==good.toStdString());
    QString error;
    const auto archive=directory+"/valid-measurements-with-errors.brscan";
    assert(save_music_scan(archive,batch.document,error));
    ProfileDocument restored;
    assert(load_music_scan(archive,restored,error));
    assert(restored.profile.compositions.size()==1);
    assert(std::all_of(restored.previews.begin(),restored.previews.end(),
                       [](const auto& image){return image.rows==0;}));
    const auto run=restored.preserved.value("analysis_run").toObject();
    assert(run.value("requested_count").toInt()==2);
    assert(run.value("processed_count").toInt()==1);
    assert(run.value("failed_count").toInt()==1);
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
void optional_real_fma_part_23_smoke(const QString& scratch) {
    const auto corpus=qEnvironmentVariable("BARELIEF_FMA_SMOKE_DIR");
    if(corpus.isEmpty())return;
    const auto all=scan_audio_files(corpus,true);
    assert(all.size()>2944);
    const auto portion=all.mid(2816,128);
    std::atomic_bool stop=false;
    auto o=options();o.workers=4;o.compatibility.sample_rate=44100;
    o.training.interval_numerator=1;o.training.interval_denominator=120;
    const auto output=scratch+"/real-fma-part23";
    assert(QDir().mkpath(output));
    const auto result=scan_music_in_parts(portion,o,stop,{},
        scan_music_files,save_music_scan,output,128);
    assert(result.success && result.saved_part_count==1);
    ProfileDocument loaded;QString error;
    MusicScanCollectionInfo info;
    assert(visit_music_scan_parts(result.batch_directory,
        [&](ProfileDocument&& part,std::size_t){loaded=std::move(part);return true;},
        info,error));
    assert(info.part_count==1);
    assert(result.errors.size()==128-loaded.profile.compositions.size());
    assert(std::all_of(loaded.previews.begin(),loaded.previews.end(),
        [](const auto& image){return image.rows==0;}));
    std::cout<<"FMA_REAL_PART23 requested=128 succeeded="
        <<loaded.profile.compositions.size()<<" failed="<<result.errors.size()<<"\n";
    // Previously failed very short MP3 files must never invalidate a
    // surviving valid track or crash the scan collection transaction.
    const QStringList mixed{
        corpus+"/001/001485.mp3",
        corpus+"/001/001486.mp3",
        corpus+"/002/002624.mp3",
        corpus+"/003/003284.mp3"
    };
    const auto mixed_directory=scratch+"/mixed-real-mp3";
    assert(QDir().mkpath(mixed_directory));
    const auto mixed_result=scan_music_in_parts(mixed,o,stop,{},
        scan_music_files,save_music_scan,mixed_directory,4);
    assert(mixed_result.success && mixed_result.saved_part_count==1);
    ProfileDocument mixed_loaded;
    assert(visit_music_scan_parts(mixed_result.batch_directory,
        [&](ProfileDocument&& part,std::size_t){mixed_loaded=std::move(part);return true;},
        info,error));
    const auto& preserved=mixed_loaded.preserved.value("analysis_run").toObject();
    assert(preserved.value("requested_count").toInt()==4);
    assert(preserved.value("processed_count").toInt()+preserved.value("failed_count").toInt()==4);
    assert(mixed_loaded.profile.compositions.size()>=1);
    std::cout<<"FMA_SHORT_MP3_SMOKE processed="
        <<preserved.value("processed_count").toInt()<<" failed="
        <<preserved.value("failed_count").toInt()<<"\n";
}
void optional_real_small_collection_cycle() {
    const auto corpus=qEnvironmentVariable("BARELIEF_E2E_FMA_DIR");
    const auto output=qEnvironmentVariable("BARELIEF_E2E_OUTPUT_DIR");
    if(corpus.isEmpty() || output.isEmpty())return;
    assert(QDir(output).exists());
    QStringList paths;
    for(const auto* file:{"000002.mp3","000003.mp3","000005.mp3","000010.mp3",
                          "000020.mp3","000026.mp3","000030.mp3","000046.mp3"}){
        const auto path=corpus+"/000/"+file;
        assert(QFileInfo(path).isFile());paths.append(path);
    }
    for(const auto* file:{"001/001485.mp3","001/001486.mp3","002/002624.mp3"}){
        const auto path=corpus+"/"+file;
        assert(QFileInfo(path).isFile());paths.append(path);
    }
    const auto damaged=output+"/deliberately-invalid.mp3";
    {QFile file(damaged);assert(file.open(QIODevice::WriteOnly));
        assert(file.write("invalid MP3",11)==11);}
    paths.append(damaged);
    auto opt=options();opt.workers=2;opt.compatibility.sample_rate=44100;
    const auto alternate=qEnvironmentVariableIntValue("BARELIEF_E2E_FFT");
    if(alternate)opt.compatibility.fft_size=std::size_t(alternate);
    const auto bins=opt.compatibility.fft_size/2+1;
    opt.training.interval_numerator=1;opt.training.interval_denominator=120;
    std::atomic_bool cancel=false;
    const auto result=scan_music_in_parts(paths,opt,cancel,{},scan_music_files,
                                          save_music_scan_part,output,4);
    assert(result.success && !result.cancelled && result.saved_part_count==3);
    assert(QFileInfo(result.batch_directory).isFile());
    MusicScanCollectionInfo info;QString error;
    assert(inspect_music_scan(result.batch_directory,info,error));
    assert(info.part_count==3 && info.requested_count==12);
    assert(info.processed_count+info.failed_count==12 && info.failed_count>=1);
    std::size_t chunks=0,tracks=0;
    assert(visit_music_scan_parts(result.batch_directory,
       [&](ProfileDocument&& part,std::size_t index){
           assert(index==chunks && part.profile.compositions.size()<=4);
           for(const auto& t:part.profile.compositions){
               assert(t.peak_spectrum.size()==bins && t.mean_spectrum.size()==bins);
               assert(dsp_kernels::valid_rms_bands(t));
               assert(std::all_of(t.peak_spectrum.begin(),t.peak_spectrum.end(),
                   [](double value){return std::isfinite(value)&&value>=0;}));
           }
           tracks+=part.profile.compositions.size();++chunks;
           return true;
       },info,error));
    assert(chunks==3 && tracks==info.processed_count);
    ProfileDocument restored;
    assert(load_music_scan(result.batch_directory,restored,error));
    assert(restored.profile.compositions.empty());
    const auto& peak=restored.profile.peak_spectrum;
    assert(peak.size()==bins && *std::max_element(peak.begin(),peak.end())>0);
    const auto stat=restored.preserved.value("scan_collection").toObject();
    assert(std::size_t(stat.value("processed_count").toDouble())==tracks);
    assert(stat.value("positive_bin_contributors").toArray().size()==bins);
    assert(QDir(output).entryList({".barelief-scan-incomplete-*"},QDir::Dirs|QDir::Hidden).empty());
    std::cout<<"FMA_REAL_E2E_PASS file="<<result.batch_directory.toStdString()
             <<" requested=12 measured="<<tracks<<" failed="<<info.failed_count
             <<" parts="<<chunks<<" cache_bins="<<peak.size()<<'\n';
}
void optional_unified_native_brscan_check() {
    const auto path=qEnvironmentVariable("BARELIEF_UNIFIED_NATIVE_BRSCAN");
    if(path.isEmpty())return;
    MusicScanCollectionInfo meta;
    QString error;
    assert(inspect_music_scan(path,meta,error));
    assert(meta.part_count==833 && meta.requested_count==106574 &&
           meta.processed_count==106412 && meta.failed_count==162);
    std::size_t visited=0;
    const auto complete=visit_music_scan_parts(path,[&](ProfileDocument&& part,std::size_t index){
        assert(index==0);
        assert(part.profile.compositions.size()==128);
        ++visited;
        return false; // deliberate bounded-read cancellation
    },meta,error);
    assert(!complete && visited==1 && error.contains("cancelled"));
    std::cout<<"NATIVE_ZIP64_FMA_FIRST_PART_OK; total_tracks="
             <<meta.processed_count<<" parts="<<meta.part_count<<'\n';
}

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir directory;assert(directory.isValid());large_folder_enumeration(directory.path());
    const auto quiet=signal_file(directory.path(),"quiet.wav",1e-8f),zero=signal_file(directory.path(),"zero.wav",0);
    retained_quiet_measurements(quiet);multi_part_archive_scan(quiet,directory.path());silence(zero);persistence(quiet,directory.path());malformed(quiet);compressed_formats();native_and_fir(directory.path());native_cadence_uses_track_clock(directory.path());quiet_bands_roundtrip(directory.path());corrupted_audio_does_not_invalidate_scan_archive(directory.path());optional_real_fma_part_23_smoke(directory.path());optional_real_small_collection_cycle();optional_unified_native_brscan_check();rms_boundaries();
    std::cout<<"music_scan_tests PASS\n";
}
