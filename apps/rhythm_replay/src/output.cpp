#include "support.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>

namespace rhythm_replay::detail {
namespace {
QJsonArray periodicity_summary(std::span<const TempogramProbe> probes,std::uint64_t frames) {
    QJsonArray result;
    for(const auto& probe:probes){
        require(probe.frames==frames && !probe.pending_sequence,"Periodicity frame counts differ or unmatched occupancy");
        require(probe.acf || probe.phase_frames==frames,"Fourier magnitude and phase frame counts differ");
        result.push_back(probe.summary());
    }
    return result;
}
}
void CsvFile::open(const QString& path,const char* header) {
    open_binary(path);
    require(std::fprintf(file_.get(),"%s\n",header)>0,"Cannot write replay CSV header");
}
void CsvFile::open_binary(const QString& path) {
#ifdef _WIN32
    file_.reset(_wfopen(path.toStdWString().c_str(),L"wb"));
#else
    file_.reset(std::fopen(QFile::encodeName(path).constData(),"wb"));
#endif
    require(bool(file_),"Cannot open replay CSV");
}
bool CsvFile::flush() noexcept {return file_ && std::fflush(file_.get())==0;}
void Observations::open(const QString& directory,unsigned frame_rate,bool reference_trace) {
    QDir dir(directory);
    require(!dir.exists() || dir.entryList(QDir::AllEntries|QDir::Hidden|QDir::System|QDir::NoDotAndDotDot).empty(),"Replay output directory must be empty");
    require(QDir().mkpath(directory),"Cannot create replay output directory");
    observations_.open(dir.filePath("observations.csv"),"stream,frame,t_obs,available_source_time,channel,flux,contrast,threshold,level,above_one,process_us,upstream_latency_s,pending_t_obs");
    events_.open(dir.filePath("events.csv"),"stream,frame,channel,t_obs,t_ready,strength,positive_mass");
    fused_.open(dir.filePath("fused_events.csv"),"stream,frame,channels,t_obs,t_ready,strength,positive_mass");
    features_.open(dir.filePath("features.csv"),"stream,frame,t_obs,feature,value");
    state_.open(dir.filePath("runtime.csv"),"frame,t_obs,port,available,value");
    tempogram_file_.open(dir.filePath("tempograms.csv"),"window,frame,t_obs,filled,bin,hz,magnitude");
    phase_file_.open(dir.filePath("fourier_phases.csv"),"window,frame,t_obs,filled,bin,hz,phase_rad");
    correlation_file_.open(dir.filePath("correlations.csv"),"window,frame,t_obs,filled,lag,seconds,coefficient");
    tempo_evidence_.open(dir.filePath("tempo_candidates.csv"),frame_rate);
    reference_.open(directory,reference_trace);
    prepare_probes();
}
void Observations::prepare_probes() {
    probes_[0].configure("full_float",15,2048);
    probes_[1].configure("compact_float",1007,96);
    probes_[2].configure("compact_bands_float",1007,96,false,4);
}
QJsonObject Probe::summary() const {
    return {{"stream",name},{"features",int(features)},{"channels",int(detector.output().size())},
        {"frames",double(frames)},{"peaks",double(peaks)},{"minimum",minimum},{"maximum",maximum},
        {"zero_values",double(zeros)},{"above_one_values",double(above_one)},{"upper_u8_values",double(upper_u8)},
        {"workspace_bytes",double(detector.workspace_bytes()+fusion.workspace_bytes())},
        {"fused_peaks",double(fused_peaks)},{"fusion_dropped",double(dropped_peaks)}};
}
QJsonObject Observations::finish() {
    check();
    require(observations_.flush() && events_.flush() && fused_.flush() && features_.flush() && state_.flush(),"Cannot flush replay CSV");
    require(tempogram_file_.flush(),"Cannot flush tempogram CSV");
    require(phase_file_.flush(),"Cannot flush Fourier phase CSV");
    require(correlation_file_.flush(),"Cannot flush correlation CSV");
    for(const auto& probe:probes_)require(probe.frames==frames() && probe.frames>0,"Replay streams have different or empty frame counts");
    QJsonArray streams;for(const auto& probe:probes_)streams.push_back(probe.summary());
    return {{"streams",streams},{"analysis_frames",double(frames())},{"reference",reference_.finish()},
        {"tempograms",periodicity_summary(tempograms_,frames())},{"correlations",periodicity_summary(correlations_,frames())},
        {"tempo_evidence",tempo_evidence_.finish(tempograms_[0].snapshots)},
        {"readiness_model","offline causal availability from current source packet; CPU and upstream latency reported separately"},
        {"end_of_file","no zero padding or invented final onset; incomplete peak remains unconfirmed"}};
}
}
