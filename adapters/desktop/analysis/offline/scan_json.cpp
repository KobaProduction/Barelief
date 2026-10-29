#include "profile_codec.h"
#include "music_archive.h"
#include "archive_zip.h"
#include <QFile>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <dsp_kernels/frequency_measurements.h>

namespace desktop_analysis::offline {
namespace {
constexpr qint64 limit=1024ll*1024*1024;
ProfileDocument measurement_document(const ProfileDocument& input) {
    auto result=input;auto& p=result.profile;p.curve.reset();
    p.training.gain_mode=dsp_kernels::ProfileGainMode::MeanEqualization;p.training.min_gain=1;p.training.max_gain=1;
    p.gains.assign(p.compatibility.fft_size/2+1,1);p.mean_spectrum.assign(p.gains.size(),0);
    p.peak_spectrum.clear();p.frequency_presence.clear();return result;
}
QJsonObject scanner_settings(const QJsonObject& training,QJsonObject scanner,bool grouped) {
    for(const auto* key:{"hop","interval_numerator","interval_denominator"})if(!scanner.contains(key))scanner[key]=training.value(key);
    if(grouped)scanner.remove("silence_rms");
    else if(!scanner.contains("silence_rms"))scanner["silence_rms"]=training.value("silence_rms");
    scanner["silence_policy"]="exclude_exact_zero_only";return scanner;
}
QJsonObject codec_training(const QJsonObject& placeholder,const QJsonObject& scanner,const QJsonObject& diagnostics) {
    auto training=placeholder.value("training").toObject();
    for(const auto* key:{"hop","interval_numerator","interval_denominator"})training[key]=scanner.value(key);
    if(scanner.contains("silence_rms"))training["silence_rms"]=scanner.value("silence_rms");
    if(diagnostics.contains("quiet_db"))training["silence_rms"]=std::pow(10.,diagnostics.value("quiet_db").toDouble()/20.);
    return training;
}
bool restore_codec_grid(QJsonObject& object,const QJsonObject& placeholder,QString& error) {
    if(object.value("version").toInt()!=3)return true;
    auto metadata=object.value("metadata").toObject();
    if(metadata.value("grid").toObject().value("kind").toString()!="per_track_rfft_hz"){error="Invalid scan frequency grid";return false;}
    auto grid=placeholder.value("metadata").toObject().value("grid").toObject();
    grid["step_hz"]=metadata.value("sample_rate").toDouble()/metadata.value("fft_size").toDouble();
    metadata["grid"]=grid;object["metadata"]=metadata;return true;
}
void restore_codec_payload(QJsonObject& object,const QJsonObject& placeholder,const QJsonObject& scanner) {
    object["training"]=codec_training(placeholder,scanner,object.value("diagnostics").toObject());
    object["gains"]=placeholder.value("gains");object["mean_spectrum"]=placeholder.value("mean_spectrum");
    object["version"]=5;object["kind"]="barelief.frequency_profile";
}
bool restore_codec_fields(QJsonObject& object,QString& error) {
    const auto scanner=object.value("scanner").toObject();
    if(scanner.value("silence_policy").toString()!="exclude_exact_zero_only"){error="Unsupported scan silence policy";return false;}
    ProfileDocument defaults;defaults.profile.compatibility.fft_size=std::size_t(object.value("metadata").toObject().value("fft_size").toInt());
    if(defaults.profile.compatibility.fft_size<512 || defaults.profile.compatibility.fft_size>dsp_kernels::max_fft_samples){error="Invalid scan FFT size";return false;}
    const auto placeholder=QJsonDocument::fromJson(encode_profile(measurement_document(defaults))).object();
    if(!restore_codec_grid(object,placeholder,error))return false;
    restore_codec_payload(object,placeholder,scanner);
    return true;
}
bool complete_measurements(const ProfileDocument& document) {
    const auto bins=document.profile.compatibility.fft_size/2+1;
    return !document.profile.compositions.empty() && std::ranges::all_of(document.profile.compositions,[bins](const auto& t){
        return t.mean_spectrum.size()==bins && t.peak_spectrum.size()==bins &&
            ((t.rms_statistics_version==1 && dsp_kernels::valid_rms_bands(t)) ||
             (t.quiet_mean_spectrum.size()==bins && t.quiet_peak_spectrum.size()==bins));
    });
}
bool valid_scan_header(const QJsonObject& object,QString& error) {
    const auto version=object.value("version").toDouble();
    if(object.value("kind").toString()!="barelief.music_scan" || (version!=1 && version!=2 && version!=3)){error="Unsupported scan document";return false;}
    return true;
}
bool parse_scan(const QByteArray& bytes,QJsonObject& object,QString& error) {
    if(bytes.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    QJsonParseError parse;object=QJsonDocument::fromJson(bytes,&parse).object();
    if(parse.error!=QJsonParseError::NoError){error="Malformed scan JSON";return false;}
    return valid_scan_header(object,error);
}
bool save_scan_json(const QString& path,const ProfileDocument& input,QString& error) {
    const auto bytes=encode_music_scan(input);ProfileDocument check;
    if(!decode_music_scan(bytes,check,error))return false;
    QSaveFile file(path);file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()){error=file.errorString();return false;}
    return true;
}
}
QJsonObject encode_scan_object(const ProfileDocument& input,ProfileArraySink* sink) {
    auto object=encode_profile_object(input,sink,false);
    const auto grouped=std::ranges::all_of(input.profile.compositions,[](const auto& t){return t.rms_statistics_version==1;});
    object["scanner"]=scanner_settings(object.value("training").toObject(),object.value("scanner").toObject(),grouped);
    for(const auto* key:{"training","gains","mean_spectrum","peak_spectrum","frequency_presence","function"})object.remove(key);
    object["kind"]="barelief.music_scan";object["version"]=3;
    auto metadata=object.value("metadata").toObject(),grid=metadata.value("grid").toObject();
    grid["kind"]="per_track_rfft_hz";grid.remove("step_hz");metadata["grid"]=grid;object["metadata"]=metadata;
    return object;
}
QByteArray encode_music_scan(const ProfileDocument& input){return QJsonDocument(encode_scan_object(input)).toJson(QJsonDocument::Compact);}
bool decode_scan_object(QJsonObject object,ProfileDocument& output,QString& error,ProfileArraySource* source) {
    if(!valid_scan_header(object,error))return false;
    const auto original=object;
    if(!restore_codec_fields(object,error))return false;
    ProfileDocument candidate;
    ProfileReadDiagnostic diagnostic;
    if(!decode_profile_object(object,candidate,error,diagnostic,source))return false;
    if(!complete_measurements(candidate)){error="Scan requires complete per-track measurements";return false;}
    candidate.preserved=original;output=std::move(candidate);return true;
}
bool decode_music_scan(const QByteArray& bytes,ProfileDocument& output,QString& error) {
    QJsonObject object;if(!parse_scan(bytes,object,error))return false;
    return decode_scan_object(object,output,error);
}
namespace {
struct ScanCollectionReader {
    QFile file;
    StoredZipReader zip;
    QJsonArray listed;
    QJsonArray indexed;
    MusicScanCollectionInfo summary;
    QByteArray manifest_digest,index_digest;
    bool cached=false;
    explicit ScanCollectionReader(const QString& path):file(path),zip(file){}
    bool open(QString& error){
        if(!file.open(QIODevice::ReadOnly) || !zip.open(error,true)){
            if(error.isEmpty())error=file.errorString();
            return false;
        }
        QByteArray main_bytes,index_bytes;
        if(!zip.read("manifest.json",music_manifest_limit,main_bytes,error) ||
           !zip.read("pack-index.json",music_manifest_limit,index_bytes,error))return false;
        const auto main=QJsonDocument::fromJson(main_bytes).object();
        const auto index=QJsonDocument::fromJson(index_bytes).object();
        if(main.value("kind").toString()!="barelief.music_scan_collection" ||
           index.value("kind").toString()!="barelief.music_scan_pack" ||
           main.value("version").toInt()!=1 || index.value("version").toInt()!=1 ||
           !index.value("completed").toBool() ||
           !main.value("parts").isArray() || !index.value("parts").isArray()){
            error="Unsupported .brscan collection archive";return false;
        }
        listed=main.value("parts").toArray();indexed=index.value("parts").toArray();
        cached=zip.entries().contains("summary.json");
        manifest_digest=QCryptographicHash::hash(main_bytes,QCryptographicHash::Sha256).toHex();
        index_digest=QCryptographicHash::hash(index_bytes,QCryptographicHash::Sha256).toHex();
        if(listed.empty() || listed.size()!=indexed.size() ||
           std::size_t(zip.entries().size())!=std::size_t(listed.size())+2+std::size_t(cached)){
            error="Incomplete or inconsistent .brscan collection";return false;
        }
        const auto whole=[](const QJsonObject& obj,const char* key,std::size_t& value){
            const auto v=obj.value(key);
            if(!v.isDouble() || v.toDouble()<0 || v.toDouble()>10000000 ||
               std::floor(v.toDouble())!=v.toDouble())return false;
            value=std::size_t(v.toDouble());return true;
        };
        if(!whole(main,"requested_count",summary.requested_count) ||
           !whole(main,"processed_count",summary.processed_count) ||
           !whole(main,"failed_count",summary.failed_count) ||
           summary.processed_count+summary.failed_count!=summary.requested_count){
            error="Invalid .brscan corpus measurement counts";return false;
        }
        summary.part_count=std::size_t(listed.size());
        std::size_t requested=0,processed=0,failed=0;
        for(qsizetype i=0;i<listed.size();++i){
            const auto stat=listed[i].toObject(),entry=indexed[i].toObject();
            const auto name=QStringLiteral("music-scan-part-%1.brscan").arg(i+1,6,10,QLatin1Char('0'));
            if(stat.value("name").toString()!=name || entry.value("name").toString()!=name ||
               entry.value("sha256").toString().size()!=64){
                error="Unexpected scan part identity";return false;
            }
            std::size_t n=0,p=0,f=0;
            if(!whole(stat,"requested_count",n)||!whole(stat,"processed_count",p)||
               !whole(stat,"failed_count",f)||n!=p+f ||
               entry.value("requested_count").toDouble()!=double(n) ||
               entry.value("processed_count").toDouble()!=double(p) ||
               entry.value("failed_count").toDouble()!=double(f)){
                error="Inconsistent scan part metadata";return false;
            }
            requested+=n;processed+=p;failed+=f;
        }
        if(requested!=summary.requested_count||processed!=summary.processed_count||
           failed!=summary.failed_count){error="Scan collection counters mismatch";return false;}
        return true;
    }
    bool cached_summary(QJsonObject& object,QString& error) {
        if(!cached || !zip.entries().contains("summary.json"))return false;
        QByteArray bytes;
        if(!zip.read("summary.json",music_manifest_limit,bytes,error))return false;
        QJsonParseError parse;
        const auto parsed=QJsonDocument::fromJson(bytes,&parse);
        if(parse.error!=QJsonParseError::NoError || !parsed.isObject()){
            error="Malformed cached scan summary";return false;
        }
        object=parsed.object();
        if(object.value("manifest_sha256").toString().toLatin1()!=manifest_digest ||
           object.value("index_sha256").toString().toLatin1()!=index_digest){
            error="Cached summary does not match the scan parts";return false;
        }
        return true;
    }
    bool part(qsizetype index,ProfileDocument& output,QString& error){
        const auto item=indexed[index].toObject();
        const auto name=item.value("name").toString();
        const auto size=item.value("size").toDouble(-1);
        if(!std::isfinite(size)||size<0||size>music_archive_limit||std::floor(size)!=size){
            error="Oversized scan collection part";return false;
        }
        QTemporaryFile tmp(QDir::tempPath()+"/barelief-scan-XXXXXX");
        if(!tmp.open() || !zip.extract_to("parts/"+name,tmp,music_archive_limit,error))return false;
        if(tmp.size()!=qint64(size)){error="Scan collection part size mismatch";return false;}
        tmp.seek(0);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if(!digest.addData(&tmp) ||
           QString::fromLatin1(digest.result().toHex())!=item.value("sha256").toString()){
            error="Scan collection part SHA-256 mismatch";return false;
        }
        tmp.flush();tmp.close();
        if(!load_music_scan(tmp.fileName(),output,error))return false;
        const auto counts=output.preserved.value("analysis_run").toObject();
        if(counts.value("processed_count").toInt(-1)!=item.value("processed_count").toInt(-2) ||
           counts.value("failed_count").toInt(-1)!=item.value("failed_count").toInt(-2) ||
           counts.value("requested_count").toInt(-1)!=item.value("requested_count").toInt(-2)){
            error="Decoded scan part count mismatch";return false;
        }
        return true;
    }
};
}
bool inspect_music_scan(const QString& path,MusicScanCollectionInfo& summary,QString& error) {
    ScanCollectionReader reader(path);
    if(reader.open(error)){summary=reader.summary;return true;}
    // Existing classic .brscan files remain accepted by the same public API.
    const auto collection_error=error;error.clear();
    if(!QFileInfo(path).isFile() || QFileInfo(path).size()>music_archive_limit){
        error=collection_error;return false;
    }
    ProfileDocument document;
    ProfileReadDiagnostic diagnostic;
    if(!load_music_archive(path,MusicArchiveKind::Scan,document,error,diagnostic))return false;
    summary.part_count=1;
    summary.processed_count=document.profile.compositions.size();
    const auto run=document.preserved.value("analysis_run").toObject();
    summary.requested_count=std::size_t(run.value("requested_count").toInt(int(summary.processed_count)));
    summary.failed_count=std::size_t(run.value("failed_count").toInt());
    return true;
}
bool visit_music_scan_parts(const QString& path,const MusicScanPartVisitor& visitor,
                            MusicScanCollectionInfo& summary,QString& error) {
    if(!visitor){error="Missing scan part consumer";return false;}
    ScanCollectionReader reader(path);
    if(reader.open(error)){
        summary=reader.summary;
        for(qsizetype index=0;index<reader.listed.size();++index){
            ProfileDocument part;
            if(!reader.part(index,part,error))return false;
            if(!visitor(std::move(part),std::size_t(index))){
                error="Scan part consumption cancelled";return false;
            }
        }
        return true;
    }
    const auto collection_error=error;error.clear();
    if(!QFileInfo(path).isFile() || QFileInfo(path).size()>music_archive_limit){
        error=collection_error;return false;
    }
    ProfileDocument document;
    ProfileReadDiagnostic diagnostic;
    if(!load_music_archive(path,MusicArchiveKind::Scan,document,error,diagnostic))return false;
    const auto n=document.profile.compositions.size();
    summary={};
    summary.part_count=1;summary.processed_count=n;
    const auto run=document.preserved.value("analysis_run").toObject();
    summary.requested_count=std::size_t(run.value("requested_count").toInt(int(n)));
    summary.failed_count=std::size_t(run.value("failed_count").toInt());
    if(!visitor(std::move(document),0)){error="Scan part consumption cancelled";return false;}
    return true;
}

bool save_music_scan(const QString& path,const ProfileDocument& input,QString& error) {
    ProfileDocument frontend;frontend.profile.compatibility=input.profile.compatibility;frontend.profile.training=input.profile.training;
    if(!valid_profile_storage(measurement_document(frontend),error) || !valid_scan_measurements(input,error))return false;
    if(!complete_measurements(input)){error="Scan requires complete per-track measurements";return false;}
    if(path.endsWith(".json",Qt::CaseInsensitive))return save_scan_json(path,input,error);
    QTemporaryDir staging(QDir::tempPath()+"/barelief-scan-save-XXXXXX");
    if(!staging.isValid()){error="Cannot prepare temporary scan storage";return false;}
    const auto part_name=QStringLiteral("music-scan-part-000001.brscan");
    if(!save_music_archive(staging.filePath(part_name),input,MusicArchiveKind::Scan,error))
        return false;
    const auto run=input.preserved.value("analysis_run").toObject();
    const auto processed=int(input.profile.compositions.size());
    const auto failed=run.value("failed_count").toInt(0);
    const auto requested=run.value("requested_count").toInt(processed+failed);
    if(failed<0 || requested!=processed+failed){
        error="Invalid complete scan counters";return false;
    }
    const QJsonObject entry{
        {"name",part_name},{"requested_count",requested},
        {"processed_count",processed},{"failed_count",failed}
    };
    const QJsonArray parts{entry};
    const QJsonObject manifest{
        {"kind","barelief.music_scan_collection"},{"version",1},
        {"requested_count",requested},{"processed_count",processed},
        {"failed_count",failed},{"fft_size",int(input.profile.compatibility.fft_size)},
        {"parts",parts}
    };
    return save_music_scan_collection(QDir(staging.path()),path,manifest,parts,error);
}
namespace {
void add_spectrum_bin(MusicScanSummaryAccumulator& sum,
                      const dsp_kernels::CompositionStatistics& track,
                      double rate,double max_hz,std::size_t k) {
    constexpr double analysis_rate=48000.;
    const auto fft=sum.fft_size;
    const double hz=analysis_rate*double(k)/double(fft);
    if(hz>max_hz)return;
    const double source_bin=hz*double(fft)/rate;
    if(source_bin>double(fft/2))return;
    const auto a=std::size_t(source_bin),b=std::min(a+1,fft/2);
    const auto weight=source_bin-double(a);
    const auto interpolate=[&](const auto& values){return values[a]*(1-weight)+values[b]*weight;};
    sum.peak[k]+=interpolate(track.peak_spectrum);
    sum.mean[k]+=interpolate(track.mean_spectrum);
    if(track.frequency_presence.size()==fft/2+1)
        sum.presence[k]+=interpolate(track.frequency_presence);
    ++sum.contributors[k];
}
bool add_spectrum_track(MusicScanSummaryAccumulator& sum,
                        const dsp_kernels::CompositionStatistics& track,
                        double fallback_rate,QString& error) {
    const auto bins=sum.fft_size/2+1;
    if(track.peak_spectrum.size()!=bins || track.mean_spectrum.size()!=bins){
        error="Incomplete spectrum in scan summary";return false;
    }
    const auto rate=track.sample_rate>0?track.sample_rate:fallback_rate;
    if(!std::isfinite(rate)||rate<=0){error="Invalid track sample rate";return false;}
    const auto max_hz=track.measured_max_hz>0?std::min(track.measured_max_hz,rate/2.):rate/2.;
    for(std::size_t k=0;k<bins;++k)add_spectrum_bin(sum,track,rate,max_hz,k);
    ++sum.tracks;
    return true;
}
}
bool MusicScanSummaryAccumulator::add(const ProfileDocument& part,QString& error) {
    const auto n=part.profile.compatibility.fft_size;
    if(n<512 || n>16384 || (n&(n-1)) || (fft_size && fft_size!=n)){
        error="Cached summary requires one supported FFT size";return false;
    }
    const auto bins=n/2+1;
    if(peak.empty()){
        fft_size=n;
        peak.assign(bins,0);mean.assign(bins,0);presence.assign(bins,0);
        contributors.assign(bins,0);
    }
    for(const auto& track:part.profile.compositions)
        if(!add_spectrum_track(*this,track,part.profile.compatibility.sample_rate,error))return false;
    return true;
}
QJsonObject MusicScanSummaryAccumulator::result()const {
    QJsonArray peaks,means,presences,counts;
    for(std::size_t k=0;k<peak.size();++k){
        const auto denominator=contributors[k];
        peaks.append(denominator?double(peak[k]/denominator):0.);
        means.append(denominator?double(mean[k]/denominator):0.);
        presences.append(denominator?double(presence[k]/denominator):0.);
        counts.append(double(denominator));
    }
    return QJsonObject{{"kind","barelief.music_scan_summary"},{"version",1},
        {"fft_size",int(fft_size)},{"sample_rate_hz",48000},
        {"processed_count",double(tracks)},
        {"peak_spectrum",peaks},{"mean_spectrum",means},
        {"frequency_presence",presences},{"contributors",counts}};
}
// The corpus has a bounded per-bin profile even when individual tracks
// number in the hundreds of thousands. The underlying individual tracks
// remain available through visit_music_scan_parts().
bool load_music_scan_collection_summary(const QString& path,ProfileDocument& output,
                                        MusicScanCollectionInfo& summary,QString& error) {
    constexpr double target_fs=48000.;
    std::size_t target_fft=0,bins=0;
    std::vector<long double> peak_sum,temporal_sum,presence_sum;
    std::vector<std::size_t> contributors;
    bool initialized=false;
    std::size_t contributing_tracks=0;
    auto visitor=[&](ProfileDocument&& part,std::size_t)->bool {
        const auto n=part.profile.compatibility.fft_size;
        if(!initialized) {
            if(n<512 || n>16384 || (n&(n-1))){error="Unsupported FFT size in scan";return false;}
            target_fft=n;bins=n/2+1;
            peak_sum.assign(bins,0);temporal_sum.assign(bins,0);presence_sum.assign(bins,0);
            contributors.assign(bins,0);
            output.profile.compatibility=part.profile.compatibility;
            output.profile.training=part.profile.training;
            initialized=true;
        }
        if(n!=target_fft){error="Mixed FFT sizes cannot form one common profile";return false;}
        for(const auto& t:part.profile.compositions){
            if(t.peak_spectrum.size()!=bins || t.mean_spectrum.size()!=bins){
                error="Incomplete component spectrum";return false;
            }
            const auto rate=t.sample_rate>0?t.sample_rate:part.profile.compatibility.sample_rate;
            if(!std::isfinite(rate)||rate<=0)return false;
            const auto max_hz=t.measured_max_hz>0?std::min(t.measured_max_hz,rate/2.):rate/2.;
            for(std::size_t k=0;k<bins;++k){
                const double hz=target_fs*double(k)/double(target_fft);
                if(hz>max_hz)continue;
                const auto source_bin=hz*double(n)/rate;
                if(source_bin>double(n/2))continue;
                const auto a=std::size_t(source_bin),b=std::min(a+1,n/2);
                const auto weight=source_bin-double(a);
                const auto interpolate=[&](const std::vector<double>& values){
                    return values[a]*(1-weight)+values[b]*weight;
                };
                peak_sum[k]+=interpolate(t.peak_spectrum);
                temporal_sum[k]+=interpolate(t.mean_spectrum);
                if(t.frequency_presence.size()==bins)
                    presence_sum[k]+=interpolate(t.frequency_presence);
                ++contributors[k];
            }
            ++contributing_tracks;
        }
        return true;
    };
    if(!visit_music_scan_parts(path,visitor,summary,error))return false;
    if(!initialized || contributing_tracks!=summary.processed_count){
        error="Streamed scan composition count mismatch";return false;
    }
    auto& p=output.profile;
    p.compatibility.sample_rate=target_fs;
    p.mean_spectrum.assign(bins,0);
    p.peak_spectrum.assign(bins,0);
    p.frequency_presence.assign(bins,0);
    p.gains.assign(bins,1);
    for(std::size_t k=0;k<bins;++k){
        if(!contributors[k])continue;
        p.mean_spectrum[k]=double(temporal_sum[k]/contributors[k]);
        p.peak_spectrum[k]=double(peak_sum[k]/contributors[k]);
        p.frequency_presence[k]=double(presence_sum[k]/contributors[k]);
    }
    QJsonArray counts;
    for(auto n:contributors)counts.append(double(n));
    output.preserved["scan_collection"]=QJsonObject{
        {"format","brscan ZIP64 collection"},
        {"source",path},
        {"requested_count",double(summary.requested_count)},
        {"processed_count",double(summary.processed_count)},
        {"failed_count",double(summary.failed_count)},
        {"part_count",double(summary.part_count)},
        {"grid_sample_rate_hz",target_fs},
        {"interpolation","amplitude-linearly-in-hertz"},
        {"positive_bin_contributors",counts},
        {"track_statistics","lazy in one part at a time: visit_music_scan_parts"}
    };
    return true;
}

namespace {
bool read_cached_array(const QJsonObject& object,const char* name,
                       std::vector<double>& values,std::size_t bins,QString& error) {
    const auto items=object.value(name).toArray();
    if(items.size()!=qsizetype(bins)){error=QStringLiteral("Invalid cached scan %1 size").arg(name);return false;}
    values.clear();values.reserve(bins);
    for(const auto& item:items){
        if(!item.isDouble() || !std::isfinite(item.toDouble())){
            error=QStringLiteral("Non-finite cached scan %1").arg(name);return false;
        }
        values.push_back(item.toDouble());
    }
    return true;
}
bool valid_cached_header(const QJsonObject& cache,
                         const MusicScanCollectionInfo& summary,QString& error) {
    if(cache.value("kind").toString()!="barelief.music_scan_summary" ||
       cache.value("version").toInt()!=1 ||
       cache.value("fft_size").toInt()<512 || cache.value("fft_size").toInt()>16384 ||
       (cache.value("fft_size").toInt()&(cache.value("fft_size").toInt()-1)) ||
       cache.value("sample_rate_hz").toDouble()!=48000. ||
       cache.value("processed_count").toDouble()!=double(summary.processed_count)){
        error="Cached scan summary has incompatible metadata";return false;
    }
    return true;
}
bool restore_cached_arrays(const QJsonObject& cache,ProfileDocument& output,
                           QJsonArray& contributor_array,
                           const MusicScanCollectionInfo& summary,QString& error) {
    auto& profile=output.profile;
    const auto bins=profile.compatibility.fft_size/2+1;
    if(!read_cached_array(cache,"peak_spectrum",profile.peak_spectrum,bins,error) ||
       !read_cached_array(cache,"mean_spectrum",profile.mean_spectrum,bins,error) ||
       !read_cached_array(cache,"frequency_presence",profile.frequency_presence,bins,error))return false;
    std::vector<double> counts;
    if(!read_cached_array(cache,"contributors",counts,bins,error))return false;
    for(const auto value:counts){
        if(value<0 || value>double(summary.processed_count) || std::floor(value)!=value){
            error="Invalid cached contributor count";return false;
        }
        contributor_array.append(value);
    }
    profile.gains.assign(bins,1);
    return true;
}
QJsonObject cached_collection_metadata(const QString& path,
                                       const MusicScanCollectionInfo& summary,
                                       const QJsonArray& contributors) {
    return QJsonObject{
        {"format","brscan ZIP64 collection"},{"source",path},
        {"requested_count",double(summary.requested_count)},
        {"processed_count",double(summary.processed_count)},
        {"failed_count",double(summary.failed_count)},
        {"part_count",double(summary.part_count)},
        {"grid_sample_rate_hz",48000.},
        {"interpolation","amplitude-linearly-in-hertz"},
        {"positive_bin_contributors",contributors},
        {"track_statistics","lazy in one part at a time: visit_music_scan_parts"}
    };
}
bool load_cached_scan_collection(const QString& path,ProfileDocument& output,
                                 QString& error) {
    ScanCollectionReader reader(path);
    if(!reader.open(error) || !reader.cached){
        if(error.isEmpty())error="Missing cached scan summary";
        return false;
    }
    QJsonObject cache;
    if(!reader.cached_summary(cache,error) ||
       !valid_cached_header(cache,reader.summary,error))return false;
    ProfileDocument first;
    if(!reader.part(0,first,error))return false;
    if(first.profile.compatibility.fft_size!=std::size_t(cache.value("fft_size").toInt())){
        error="Cached scan FFT size differs from stored measurements";return false;
    }
    ProfileDocument candidate;
    candidate.profile.compatibility=first.profile.compatibility;
    candidate.profile.training=first.profile.training;
    candidate.profile.compatibility.sample_rate=48000.;
    QJsonArray contributors;
    if(!restore_cached_arrays(cache,candidate,contributors,reader.summary,error))return false;
    candidate.preserved["scan_collection"]=cached_collection_metadata(path,reader.summary,contributors);
    output=std::move(candidate);
    return true;
}
}
bool save_music_scan_part(const QString& path,const ProfileDocument& document,QString& error) {
    // An inner storage block; never exposed as a complete user-visible scan.
    return save_music_archive(path,document,MusicArchiveKind::Scan,error);
}
bool load_music_scan(const QString& path,ProfileDocument& output,QString& error) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)){error=file.errorString();return false;}
    if(file.peek(4)==QByteArray("PK\x03\x04",4)){
        // The user-visible .brscan format is unified; the envelope may contain
        // one old scan or many parts. Never allocate all part measurements.
        StoredZipReader probe(file);
        if(!probe.open(error,true))return false;
        if(probe.entries().contains("pack-index.json")){
            file.close();
            MusicScanCollectionInfo info;
            if(!inspect_music_scan(path,info,error))return false;
            if(info.part_count==1){
                return visit_music_scan_parts(path,[&](ProfileDocument&& part,std::size_t){
                    output=std::move(part);return true;
                },info,error);
            }
            if(probe.entries().contains("summary.json"))
                return load_cached_scan_collection(path,output,error);
            return load_music_scan_collection_summary(path,output,info,error);
        }
        file.close();
        ProfileReadDiagnostic diagnostic;
        return load_music_archive(path,MusicArchiveKind::Scan,output,error,diagnostic);
    }
    if(file.size()>limit){error="Scan exceeds 1 GiB limit";return false;}
    const auto bytes=file.read(limit+1);
    if(file.error()!=QFileDevice::NoError){error=file.errorString();return false;}
    return decode_music_scan(bytes,output,error);
}
}
