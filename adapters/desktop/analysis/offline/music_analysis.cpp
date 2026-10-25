#include "music_analysis.h"
#include "audio_decoder.h"
#include <dsp_kernels/prepared_resampler.h>
#include <dsp_kernels/sample_cadence.h>
#include <dsp_kernels/frequency_measurements.h>
#include <signal_contracts/spectral_calibration.h>
#include <speex_backend.h>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <exception>
#include <utility>

namespace desktop_analysis::offline {
using namespace dsp_kernels;
QStringList scan_audio_files(const QString& folder,bool recursive) {
    QStringList out;QDirIterator it(folder,{"*.wav","*.flac","*.mp3"},QDir::Files,recursive?QDirIterator::Subdirectories:QDirIterator::NoIteratorFlags);
    while(it.hasNext()&&out.size()<4096)out.append(it.next());out.sort(Qt::CaseSensitive);return out;
}
namespace {
double preview_band_gain(const std::vector<float>& gains,std::size_t band,std::size_t bands){
    const auto first=band*gains.size()/bands,last=(band+1)*gains.size()/bands;
    double gain=0;for(auto index=first;index<last;++index)gain+=gains[index];return gain/double(last-first);
}
void apply_preview_band(std::vector<float>& values,std::size_t rows,std::size_t bands,std::size_t band,double gain){
    for(std::size_t row=0;row<rows;++row)values[row*bands+band]*=float(gain);
}
}
std::vector<float> SpectrogramPreview::normalized(const std::vector<float>& gains)const {
    if(!rows || !bands || rows>2048 || bands>2048 || values.size()!=rows*bands)return {};
    if(gains.empty())return values;
    if(gains.size()<bands || !std::all_of(gains.begin(),gains.end(),signal_contracts::valid_profile_gain))return {};
    auto result=values;
    for(std::size_t band=0;band<bands;++band)apply_preview_band(result,rows,bands,band,preview_band_gain(gains,band,bands));
    return result;
}
namespace {
class TrackAnalysis {
    const AnalysisOptions& options_;
    ProfileCompatibility frontend_;
    const std::atomic_bool& cancel_;
    CompositionStatistics& statistics_;
    SpectrogramPreview* preview_;
    QString& error_;
    AudioDecoder decoder_;
    PreparedResampler resampler_;
    PreparedRfft fft_;
    PreparedSpectrumProjection projection_;
    SampleCadence cadence_;
    std::vector<float> weights_, window_, windowed_, raw_, spectrum_, decoded_, converted_;
    std::size_t hop_=0, filled_=0;
    std::uint64_t written_=0, decoded_count_=0, expected_=0, discard_=0;
    double square_sum_=0, presence_floor_=0;
    bool draining_=false;
    static constexpr std::size_t chunk=4096;
    const ProfileCompatibility& frontend() const {return frontend_;}
    bool fail(const char* message) {error_=QString::fromUtf8(message);return false;}
    bool prepare_decoder(const QString& path) {
        const auto& c=frontend();
        if(!decoder_.open(path,error_,c.channel_policy,c.channel_index))return false;
        configure_track_frontend();statistics_.source=path.toStdString();
        expected_=std::uint64_t(std::ceil(double(decoder_.length())*c.sample_rate/decoder_.sample_rate()));
        return resampler_.prepare({c.resampler_mode,decoder_.sample_rate(),std::uint32_t(c.sample_rate),1,c.resampler_quality},
                                   &desktop_resampling::speex_backend_factory()) || fail("Cannot prepare the declared resampler");
    }
    void configure_track_frontend() {
        const auto& c=frontend();
        statistics_.source_sample_rate=decoder_.sample_rate();
        if(!options_.resample_input){frontend_.sample_rate=decoder_.sample_rate();frontend_.resampler_mode=ResamplerMode::Bypass;}
        statistics_.sample_rate=c.sample_rate;statistics_.rms_statistics_version=1;
        statistics_.measured_max_hz=std::min(c.sample_rate,double(decoder_.sample_rate()))/2;
        if(options_.resample_input && decoder_.sample_rate()!=c.sample_rate)
            statistics_.measured_max_hz=.40*std::min(c.sample_rate,double(decoder_.sample_rate()));
    }
    bool prepare_spectrum() {
        const auto& c=frontend();const auto n=c.fft_size,bins=n/2+1;
        weights_.resize(n);window_.resize(n);windowed_.resize(n);raw_.resize(2*bins);spectrum_.resize(bins);
        dsp_kernels::window(weights_,c.window);
        if(!fft_.prepare(n) || !projection_.prepare(n,c.sample_rate,profile_window_scaling(c),c.projection))return fail("Spectrum preparation failed");
        prepare_statistics(bins);
        decoded_.resize(chunk);converted_.resize(PreparedResampler::maximum_output(chunk,decoder_.sample_rate(),std::uint32_t(c.sample_rate)));
        const bool amplitude=c.projection.representation==SpectrumProjection::Magnitude || c.projection.representation==SpectrumProjection::PeakAmplitude;
        presence_floor_=c.reference*std::pow(10.,c.floor_db/(amplitude?20.:10.));
        return true;
    }
    void prepare_statistics(std::size_t bins) {
        statistics_.mean_spectrum.assign(bins,0);statistics_.peak_spectrum.assign(bins,0);statistics_.frequency_presence.assign(bins,0);
        statistics_.quiet_mean_spectrum.assign(bins,0);statistics_.quiet_peak_spectrum.assign(bins,0);
    }
    bool prepare_cadence() {
        const auto& c=frontend();const auto& training=options_.training;
        if(training.interval_numerator){
            if(!cadence_.prepare(std::uint32_t(c.sample_rate),training.interval_numerator,training.interval_denominator))return fail("Invalid analysis cadence");
        }else cadence_.base=training.hop;
        hop_=cadence_.next();
        if(cadence_.base+(cadence_.fraction?1:0)>c.fft_size)return fail("Native sample rate requires a shorter interval or larger FFT");
        filled_=c.fft_size-hop_;
        const auto delay=resampler_.resources().fixed_delay_input_samples;
        if(decoder_.sample_rate()!=std::uint32_t(c.sample_rate))
            discard_=(std::uint64_t(delay)*std::uint32_t(c.sample_rate)+decoder_.sample_rate()-1)/decoder_.sample_rate();
        return true;
    }
    bool prepare_preview(const QString& path) {
        if(!preview_)return true;
        if(!expected_)return fail("Cannot determine track duration for bounded full-track preview");
        preview_->source=path;preview_->rows=std::min(options_.preview_time_bins,std::size_t(std::max<std::uint64_t>(1,expected_/cadence_.base+1)));
        preview_->bands=std::min(frontend().fft_size/2+1,options_.preview_frequency_bins);
        preview_->sample_rate=frontend().sample_rate;preview_->bin_hz=frontend().sample_rate/frontend().fft_size;
        preview_->values.assign(preview_->rows*preview_->bands,0);preview_->counts.assign(preview_->rows,0);
        prepare_preview_coordinates();
        return true;
    }
    void prepare_preview_coordinates() {
        preview_->time_seconds.resize(preview_->rows);preview_->frequency_hz.resize(preview_->bands);
        const auto duration=double(expected_)/frontend().sample_rate;
        for(std::size_t row=0;row<preview_->rows;++row)preview_->time_seconds[row]=(double(row)+.5)*duration/double(preview_->rows);
        const auto bins=frontend().fft_size/2+1;
        for(std::size_t band=0;band<preview_->bands;++band){
            const auto first=band*bins/preview_->bands,last=(band+1)*bins/preview_->bands;
            preview_->frequency_hz[band]=.5*double(first+last-1)*preview_->bin_hz;
        }
    }
    double window_rms() const {
        double energy=0;for(float value:window_)energy+=double(value)*value;
        const auto real=std::min<std::uint64_t>(written_,window_.size());
        return real?std::sqrt(energy/double(real)):0;
    }
    void accumulate_quiet(double rms) {
        ++statistics_.quiet_frames;statistics_.quiet_rms+=rms*rms;
        for(std::size_t bin=0;bin<spectrum_.size();++bin){
            statistics_.quiet_mean_spectrum[bin]+=spectrum_[bin];
            statistics_.quiet_peak_spectrum[bin]=std::max(statistics_.quiet_peak_spectrum[bin],double(spectrum_[bin]));
        }
    }
    void accumulate_statistics() {
        ++statistics_.frames;
        const auto rms=window_rms();
        statistics_.maximum_frame_rms=std::max(statistics_.maximum_frame_rms,rms);
        if(!rms){++statistics_.digital_silence_frames;return;}
        if(!statistics_.active_frames || rms<statistics_.minimum_frame_rms)statistics_.minimum_frame_rms=rms;
        accumulate_rms_band(statistics_,rms,spectrum_);
        if(rms<=options_.training.silence_rms)accumulate_quiet(rms);
        ++statistics_.active_frames;
        accumulate_spectrum();
    }
    void accumulate_spectrum() {
        for(std::size_t bin=0;bin<spectrum_.size();++bin){
            statistics_.mean_spectrum[bin]+=spectrum_[bin];
            statistics_.peak_spectrum[bin]=std::max(statistics_.peak_spectrum[bin],double(spectrum_[bin]));
            statistics_.frequency_presence[bin]+=spectrum_[bin]>presence_floor_?1.:0.;
        }
    }
    void accumulate_preview() {
        if(!preview_)return;
        const auto center=written_>window_.size()/2?written_-window_.size()/2:0;
        const auto row=std::min(preview_->rows-1,std::size_t(double(center)*preview_->rows/double(expected_)));
        ++preview_->counts[row];
        for(std::size_t band=0;band<preview_->bands;++band){
            const auto first=band*spectrum_.size()/preview_->bands,last=(band+1)*spectrum_.size()/preview_->bands;
            double sum=0;for(auto bin=first;bin<last;++bin)sum+=spectrum_[bin];
            preview_->values[row*preview_->bands+band]+=float(sum/double(last-first));
        }
    }
    bool process_window() {
        for(std::size_t i=0;i<window_.size();++i)windowed_[i]=window_[i]*weights_[i];
        if(!fft_.process(windowed_,raw_) || !projection_.process(raw_,spectrum_))return fail("Spectrum calculation failed");
        accumulate_statistics();accumulate_preview();
        hop_=cadence_.next();
        std::move(window_.begin()+hop_,window_.end(),window_.begin());filled_=window_.size()-hop_;
        return true;
    }
    bool accept_sample(float value) {
        if(discard_){--discard_;return true;}
        if(draining_ && written_>=expected_)return true;
        if(!std::isfinite(value))return fail("Nonfinite decoded PCM");
        ++written_;square_sum_+=double(value)*value;window_[filled_++]=value;
        return filled_!=window_.size() || process_window();
    }
    bool consume(std::size_t count) {
        std::size_t produced=0;
        if(!resampler_.process(decoded_,count,count,converted_,converted_.size(),produced))return fail("File resampling failed");
        for(std::size_t i=0;i<produced;++i)if(!accept_sample(converted_[i]))return false;
        return true;
    }
    bool read_stream() {
        while(!cancel_.load(std::memory_order_relaxed)){
            std::size_t count=0;if(!decoder_.read(decoded_,count,error_))return false;
            if(!count)return true;
            if(count>UINT64_MAX-decoded_count_)return fail("Decoded sample count overflow");
            decoded_count_+=count;if(!consume(count))return false;
        }
        return fail("Cancelled");
    }
    bool drain() {
        const auto in=decoder_.sample_rate(),out=std::uint32_t(frontend().sample_rate);
        if(decoded_count_>(UINT64_MAX-(in-1))/out)return fail("Resampled length overflow");
        expected_=(decoded_count_*out+in-1)/in;draining_=true;
        if(in==out || written_>=expected_)return true;
        std::fill(decoded_.begin(),decoded_.end(),0.f);
        std::size_t remaining=resampler_.resources().fixed_delay_input_samples+(in+out-1)/out+1;
        while(written_<expected_ && remaining){const auto count=std::min(remaining,chunk);if(!consume(count))return false;remaining-=count;}
        return written_==expected_ || fail("Incomplete resampler drain");
    }
    void finish_statistics() {
        statistics_.samples=written_;statistics_.rms=written_?std::sqrt(square_sum_/double(written_)):0;
        if(!statistics_.active_frames)return;
        for(auto& value:statistics_.mean_spectrum)value/=double(statistics_.active_frames);
        for(auto& value:statistics_.frequency_presence)value/=double(statistics_.active_frames);
        if(statistics_.quiet_frames){
            statistics_.quiet_rms=std::sqrt(statistics_.quiet_rms/double(statistics_.quiet_frames));
            for(auto& value:statistics_.quiet_mean_spectrum)value/=double(statistics_.quiet_frames);
        }
    }
    void finish_preview() {
        if(!preview_)return;
        preview_->duration_seconds=double(written_)/frontend().sample_rate;
        for(std::size_t row=0;row<preview_->rows;++row)if(preview_->counts[row])
            for(std::size_t band=0;band<preview_->bands;++band)preview_->values[row*preview_->bands+band]/=float(preview_->counts[row]);
    }
public:
    TrackAnalysis(const AnalysisOptions& options,const std::atomic_bool& cancel,CompositionStatistics& stats,SpectrogramPreview* preview,QString& error):
        options_(options),frontend_(options.compatibility),cancel_(cancel),statistics_(stats),preview_(preview),error_(error){}
    bool run(const QString& path) {
        if(!prepare_decoder(path) || !prepare_spectrum() || !prepare_cadence() || !prepare_preview(path))return false;
        if(!read_stream() || !drain() || cancel_.load())return false;
        // Only complete cadence deadlines contribute, exactly as the live Window.
        // EOF padding is solely a resampler drain and never creates an extra STFT frame.
        finish_statistics();finish_preview();return true;
    }
};
bool analyze_track(const QString& path,const AnalysisOptions& options,const std::atomic_bool& cancel,
                   CompositionStatistics& stats,SpectrogramPreview* preview,QString& error) {
    return TrackAnalysis(options,cancel,stats,preview,error).run(path);
}
bool valid_capture_settings(const AnalysisOptions& options,std::string& error) {
    auto training=options.training;
    // Native cadence can only be validated after opening the track. Validate
    // common settings here; TrackAnalysis checks the actual sample clock.
    if(!options.resample_input && training.interval_numerator){training.interval_numerator=0;training.hop=1;}
    return valid_profile_settings(options.compatibility,training,error);
}

}
PreviewResult analyze_music_preview(const QString& path,const AnalysisOptions& opt,const std::atomic_bool& cancel) {
    PreviewResult result;result.cancelled=cancel.load();if(result.cancelled)return result;
    result.preview.source=path;std::string validation;
    if(!valid_capture_settings(opt,validation) || opt.preview_time_bins<1 || opt.preview_time_bins>2048 || opt.preview_frequency_bins<1 || opt.preview_frequency_bins>2048 ||
       std::floor(opt.compatibility.sample_rate)!=opt.compatibility.sample_rate){result.error="Invalid preview options";return result;}
    CompositionStatistics statistics;
    try{result.success=analyze_track(path,opt,cancel,statistics,&result.preview,result.error);}
    catch(const std::exception& e){result.error=QString::fromUtf8(e.what());}
    result.cancelled=cancel.load();if(result.cancelled)result.success=false;return result;
}
namespace {
AnalysisOptions frontend_options(const AnalysisOptions& input) {
    auto result=input;
    if(!result.resample_input)result.compatibility.resampler_mode=ResamplerMode::Bypass;
    return result;
}
AnalysisOptions measurement_options(const AnalysisOptions& input) {
    auto result=frontend_options(input);result.training.gain_mode=ProfileGainMode::MeanEqualization;
    result.training.min_gain=1;result.training.max_gain=1;result.training.smoothing_radius=0;
    result.training.silence_rms=.0001;
    return result;
}
bool valid_analysis_options(const QStringList& files,const AnalysisOptions& options,QString& error) {
    std::string validation;
    if(files.empty() || files.size()>4096 || !valid_capture_settings(options,validation) || options.workers<1 || options.workers>50 || options.preview_time_bins<1 || options.preview_time_bins>2048 || options.preview_frequency_bins<1 || options.preview_frequency_bins>2048){
        error=validation.empty()?QStringLiteral("Invalid analysis options"):QString::fromStdString(validation);return false;
    }
    const auto rows=options.preview_time_bins,bands=std::min(options.preview_frequency_bins,options.compatibility.fft_size/2+1);
    const auto bytes=std::uint64_t(files.size())*(rows*bands*sizeof(float)+(rows+bands)*sizeof(double)+rows*sizeof(std::uint64_t));
    if(bytes>512ull*1024*1024){error="Preview memory exceeds 512 MiB; reduce preview resolution or the file list";return false;}
    const auto bins=options.compatibility.fft_size/2+1;
    const auto statistics_bytes=5*bins*sizeof(double)+(rms_overflow_band+1)*(sizeof(CompositionStatistics::RmsBand)+2*bins*sizeof(float));
    if(std::uint64_t(files.size())*statistics_bytes>512ull*1024*1024){error="Spectral statistics exceed 512 MiB; reduce FFT size or the file list";return false;}
    return true;
}
class AnalysisBatch {
    const QStringList& files_;
    const AnalysisOptions& options_;
    const std::atomic_bool& cancel_;
    const AnalysisServices::Progress& progress_;
    std::vector<SpectrogramPreview> previews_;
    std::vector<CompositionStatistics> statistics_;
    std::vector<QString> errors_;
    std::atomic_size_t next_{0},done_{0};
    std::mutex callback_mutex_;
    void analyze(std::size_t index) {
        try{analyze_track(files_[qsizetype(index)],options_,cancel_,statistics_[index],&previews_[index],errors_[index]);}
        catch(const std::exception& error){errors_[index]=QString::fromUtf8(error.what());}
        std::lock_guard lock(callback_mutex_);const auto completed=done_.fetch_add(1)+1;
        if(progress_)progress_(completed,std::size_t(files_.size()));
    }
    void work() {
        while(!cancel_.load()){
            const auto index=next_.fetch_add(1);if(index>=std::size_t(files_.size()))return;
            analyze(index);
        }
    }
    std::vector<CompositionStatistics> collect(AnalysisResult& result) {
        std::vector<CompositionStatistics> valid;
        for(std::size_t i=0;i<statistics_.size();++i){
            if(!errors_[i].isEmpty()){result.errors.append(files_[qsizetype(i)]+": "+errors_[i]);continue;}
            result.audio_seconds+=double(statistics_[i].samples)/measurement_rate(statistics_[i],options_.compatibility.sample_rate);
            valid.push_back(std::move(statistics_[i]));result.document.previews.push_back(std::move(previews_[i]));
        }
        return valid;
    }
    void store_summary(AnalysisResult& result,const std::vector<CompositionStatistics>& valid) const {
        const auto& t=options_.training;
        result.document.preserved["scanner"]=QJsonObject{{"resample_input",options_.resample_input},{"hop",double(t.hop)},{"interval_numerator",double(t.interval_numerator)},{"interval_denominator",double(t.interval_denominator)}};
        result.document.preserved["preview_settings"]=QJsonObject{{"time_cells",double(options_.preview_time_bins)},{"frequency_cells",double(options_.preview_frequency_bins)}};
        if(!result.document.previews.empty())result.preview=result.document.previews.front();
        QJsonArray failures;for(const auto& error:std::as_const(result.errors))failures.append(error);
        const auto active=std::count_if(valid.begin(),valid.end(),[](const auto& track){return track.active_frames>0;});
        result.document.preserved["analysis_run"]=QJsonObject{{"requested_count",int(files_.size())},{"processed_count",int(valid.size())},{"active_composition_count",int(active)},
            {"failed_count",int(result.errors.size())},{"audio_seconds",result.audio_seconds},{"errors",failures}};
    }
public:
    AnalysisBatch(const QStringList& files,const AnalysisOptions& options,const std::atomic_bool& cancel,const AnalysisServices::Progress& progress):
        files_(files),options_(options),cancel_(cancel),progress_(progress),previews_(std::size_t(files.size())),statistics_(std::size_t(files.size())),errors_(std::size_t(files.size())){}
    void run() {
        std::vector<std::jthread> workers;
        for(unsigned i=0;i<std::min<unsigned>(options_.workers,unsigned(files_.size()));++i)workers.emplace_back([this]{work();});
        for(auto& thread:workers)thread.join();
    }
    void finish(AnalysisResult& result) {
        const auto valid=collect(result);std::string validation;
        result.success=build_frequency_profile(options_.compatibility,options_.training,valid,result.document.profile,validation);
        if(result.success)store_summary(result,valid);
        else result.errors.append(QString::fromStdString(validation));
    }
    void finish_scan(AnalysisResult& result) {
        auto valid=collect(result);auto& p=result.document.profile;
        p.compatibility=options_.compatibility;p.training=options_.training;
        if(!options_.resample_input && !valid.empty())p.compatibility.sample_rate=valid.front().sample_rate;
        p.training.gain_mode=ProfileGainMode::MeanEqualization;p.training.min_gain=1;p.training.max_gain=1;
        p.gains.assign(p.compatibility.fft_size/2+1,1);p.mean_spectrum.assign(p.gains.size(),0);
        p.compositions=std::move(valid);result.success=!p.compositions.empty();
        if(result.success)store_summary(result,p.compositions);
    }
};
void run_scan_batch(const QStringList& files,const AnalysisOptions& options,const std::atomic_bool& cancel,
    const AnalysisServices::Progress& progress,AnalysisResult& result,bool measurements_only=true) {
    QElapsedTimer timer;timer.start();AnalysisBatch batch(files,options,cancel,progress);batch.run();
    result.cancelled=cancel.load();result.wall_seconds=timer.nsecsElapsed()/1e9;
    if(!result.cancelled){
        if(measurements_only)batch.finish_scan(result);else batch.finish(result);
    }
}
}
AnalysisResult analyze_music_files(const QStringList& files,const AnalysisOptions& requested,const std::atomic_bool& cancel,const AnalysisServices::Progress& progress) {
    AnalysisResult result;result.cancelled=cancel.load();if(result.cancelled)return result;
    const auto options=frontend_options(requested);
    QString error;
    if(!valid_analysis_options(files,options,error)){result.errors.append(error);return result;}
    run_scan_batch(files,options,cancel,progress,result,false);
    return result;
}
AnalysisResult scan_music_files(const QStringList& files,const AnalysisOptions& requested,const std::atomic_bool& cancel,const AnalysisServices::Progress& progress) {
    AnalysisResult result;result.cancelled=cancel.load();if(result.cancelled)return result;
    const auto options=measurement_options(requested);
    QString error;
    if(!valid_analysis_options(files,options,error)){result.errors.append(error);return result;}
    run_scan_batch(files,options,cancel,progress,result);
    return result;
}
}
