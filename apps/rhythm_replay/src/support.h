#pragma once
#include <rhythm_replay/replay.h>
#include <dataflow_runtime/runtime.h>
#include <dsp_kernels/onsets.h>
#include <dsp_kernels/onset_fusion.h>
#include <dsp_kernels/tempo_evidence.h>
#include <QJsonObject>
#include <QJsonArray>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace rhythm_replay::detail {
inline void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
struct FileCloser {void operator()(std::FILE* file) const noexcept {std::fclose(file);}};
class CsvFile {
public:
    void open(const QString& path,const char* header);
    void open_binary(const QString& path);
    bool flush() noexcept;
    std::FILE* get() const noexcept {return file_.get();}
private:
    std::unique_ptr<std::FILE,FileCloser> file_;
};
struct Probe {
    const char* name=nullptr;
    domain::NodeId node=0;
    unsigned features=0, channels=0;
    bool quantized=false;
    dsp_kernels::PreparedOnsets detector;
    dsp_kernels::PreparedOnsetFusion fusion;
    std::array<float,dsp_kernels::max_onset_features> floats{};
    std::array<std::uint8_t,dsp_kernels::max_onset_features> bytes{};
    std::uint64_t frames=0,peaks=0,zeros=0,above_one=0,upper_u8=0;
    std::uint64_t fused_peaks=0,dropped_peaks=0;
    double minimum=0,maximum=0;
    void configure(const char*,domain::NodeId,unsigned,bool=false,unsigned=0);
    void account(unsigned index) noexcept;
    bool read_element(const signal_contracts::NumericView&,unsigned index) noexcept;
    bool read(const signal_contracts::NumericView&) noexcept;
    bool process(double observed,double available) noexcept;
    void prepare();
    QJsonObject summary() const;
};
struct TempogramProbe {
    domain::NodeId history=0;
    std::uint64_t capacity=0,filled=0,frames=0,snapshots=0;
    std::uint64_t count_sequence=0,pending_sequence=0,phase_sequence=0,phase_frames=0;
    double observed=0,phase_observed=0;
    std::array<float,2049> magnitude{};
    std::array<float,2049> phase{};
    std::array<double,2049> hz{};
    bool acf=false;
    bool receive(const dataflow_runtime::NumericPortFrameView&,std::FILE*,std::FILE* phases=nullptr) noexcept;
    bool accept_count(const dataflow_runtime::NumericPortFrameView&,std::FILE*,std::FILE*) noexcept;
    bool accept_phase(const dataflow_runtime::NumericPortFrameView&,std::FILE*,std::FILE*) noexcept;
    bool accept_magnitude(const dataflow_runtime::NumericPortFrameView&,std::FILE*,std::FILE*) noexcept;
    bool capture(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool accepts(const signal_contracts::NumericView&) const noexcept;
    bool capture_value(const signal_contracts::NumericView&,std::size_t) noexcept;
    bool flush(std::FILE*,std::FILE*) noexcept;
    QJsonObject summary() const;
};
class TempoEvidenceRecorder {
public:
    void open(const QString&,unsigned frame_rate);
    bool receive(const dataflow_runtime::NumericPortFrameView&,const std::array<TempogramProbe,2>&,const std::array<TempogramProbe,2>&) noexcept;
    QJsonObject finish(std::uint64_t expected);
private:
    bool write(std::uint64_t,double,const std::array<TempogramProbe,2>&) noexcept;
    bool write_candidate(const dsp_kernels::rhythm::TempoEvidenceCandidate&,std::size_t,bool,
                         std::uint64_t,double,const std::array<TempogramProbe,2>&) noexcept;
    bool read_matrix(const signal_contracts::NumericView&) noexcept;
    bool begin_frame(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool read_scalar(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool complete(const std::array<TempogramProbe,2>&,const std::array<TempogramProbe,2>&) noexcept;
    CsvFile file_;
    dsp_kernels::rhythm::TempoEvidenceOutput output_{};
    std::uint64_t snapshots_=0,rows_=0,frames_=0,sequence_=0;
    double observed_=0;
};
struct ReferenceEventDiagnostic {
    bool seen=false;
    domain::NodeId node=0;
    domain::PortId port=0;
    domain::PortDirection direction=domain::PortDirection::Output;
    signal_contracts::Status status=signal_contracts::Status::ok;
    signal_contracts::DType dtype=signal_contracts::DType::Float32;
    signal_contracts::Rank rank=signal_contracts::Rank::Scalar;
    std::array<std::size_t,2> actual{};
    signal_contracts::FrameStamp stamp;
    signal_contracts::Ratio tick;
    std::uint32_t sample_rate=0;
    std::size_t domain_bytes=0,stream_bytes=0;
    std::array<char,193> domain_hex{},stream_hex{};
    std::uint64_t events=0;
};
class ReferenceRecorder {
public:
    void open(const QString&,bool trace);
    bool receive(const dataflow_runtime::NumericPortFrameView&,double available) noexcept;
    const char* failure() const noexcept {return failure_message_.data();}
    QJsonObject finish();
private:
    struct Method {domain::NodeId node;const char* name;std::uint64_t frames=0;};
    bool write_method(Method&,const dataflow_runtime::NumericPortFrameView&,double) noexcept;
    bool write_values(const signal_contracts::NumericView&,std::FILE*,bool last_row) noexcept;
    bool write_trace(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool accept_pcm(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool reject(const char* reason) noexcept {reason_=reason;return false;}
    void observe_progression(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool diagnose(const dataflow_runtime::NumericPortFrameView&) noexcept;
    void write_diagnostic(std::FILE*,const char*,const ReferenceEventDiagnostic&) const noexcept;
    QJsonArray methods_summary();
    QJsonArray trace_summary();
    std::array<Method,6> methods_{{{1320,"spectral_flux"},{1330,"complex_difference"},
        {1340,"phase_deviation"},{1350,"high_frequency_content"},{1304,"mel_flux"},{1360,"max_filter_flux"}}};
    CsvFile envelopes_;
    CsvFile diagnostic_;
    std::array<ReferenceEventDiagnostic,3> progression_;
    std::array<char,512> failure_message_{};
    const char* reason_="unspecified",*active_stage_="none";
    std::uint64_t active_method_frames_=0;
    bool diagnosed_=false;
    std::array<CsvFile,5> trace_;
    std::array<std::uint64_t,5> stage_frames_{};
    std::array<float,4096> scratch_{};
    std::uint64_t pcm_samples_=0,spectral_frames_=0;
    std::int64_t pcm_begin_=0;
    bool tracing_=false;
};
class Observations {
public:
    void open(const QString& directory,unsigned frame_rate,bool reference_trace=false);
    void available_at(double time) noexcept {available_=time;}
    static void receive(void*,const dataflow_runtime::NumericPortFrameView&) noexcept;
    void check() const;
    QJsonObject finish();
    std::uint64_t frames() const noexcept {return probes_[1].frames;}
private:
    void prepare_probes();
    void receive(const dataflow_runtime::NumericPortFrameView&) noexcept;
    void write_available(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool accept_status(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool write_probes(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool write_probe(Probe&,const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool write_channels(Probe&,double observed,double available,double process_us,double latency) noexcept;
    bool write_features(const Probe&,double observed) noexcept;
    bool write_fused(Probe&) noexcept;
    bool write_state(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool write_tempograms(const dataflow_runtime::NumericPortFrameView&) noexcept;
    std::array<Probe,3> probes_{};
    std::array<TempogramProbe,2> tempograms_{{{1200,512},{1210,4096}}};
    std::array<TempogramProbe,2> correlations_{{
        {.history=1200,.capacity=512,.acf=true},{.history=1210,.capacity=4096,.acf=true}}};
    CsvFile observations_,events_,fused_,features_,state_,tempogram_file_,correlation_file_,phase_file_;
    TempoEvidenceRecorder tempo_evidence_;
    ReferenceRecorder reference_;
    double available_=0;
    const char* error_=nullptr;
    domain::NodeId failed_node_=-1;
    domain::PortId failed_port_=0;
    signal_contracts::Status failed_status_=signal_contracts::Status::ok;
};
}
