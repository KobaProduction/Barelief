#include <offline/music_analysis.h>
#include <dsp_kernels/pcm_resampler.h>
#include <dsp_kernels/sample_cadence.h>
#include <offline/audio_decoder.h>
#include <QCoreApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void u16(QByteArray& out,unsigned x){out.append(char(x));out.append(char(x>>8));}
void u32(QByteArray& out,unsigned x){u16(out,x);u16(out,x>>16);}
void wav(const QString& path,unsigned frames,bool silence=false,unsigned rate=32768,bool terminal_impulse=false) {
    QByteArray bytes;bytes.append("RIFF");u32(bytes,36+frames*2);bytes.append("WAVEfmt ");u32(bytes,16);u16(bytes,1);u16(bytes,1);u32(bytes,rate);u32(bytes,rate*2);u16(bytes,2);u16(bytes,16);bytes.append("data");u32(bytes,frames*2);
    for(unsigned i=0;i<frames;++i){double v=0;for(unsigned tone=0;tone<7;++tone)v+=.035*std::sin(2*std::numbers::pi*(256+tone*128)*i/rate);auto sample=std::int16_t(terminal_impulse?(i+1==frames?32767:0):(silence?0:std::lround(v*32767)));u16(bytes,std::uint16_t(sample));}
    QFile file(path);require(file.open(QIODevice::WriteOnly),"WAV create failed");require(file.write(bytes)==bytes.size(),"WAV write failed");
}
void mel_coordinates_oracle(){
    dsp_kernels::MelFilterBank bank;require(bank.prepare(16,32000,4),"prepare shared Mel");
    std::array<float,16> input;std::array<double,16> grid;
    for(std::size_t i=0;i<input.size();++i){input[i]=float(i+1);grid[i]=double(i)*1000;}
    std::array<float,4> regular,explicit_grid;
    require(bank.process(input,regular) && bank.prepare(16,32000,4,grid) &&
        bank.process(input,explicit_grid),"Mel coordinate paths");
    require(regular==explicit_grid,"same prepared weights for implicit and explicit canonical coordinates");
    for(auto& hz:grid)hz*=.9;
    require(bank.prepare(16,32000,4,grid) && bank.process(input,explicit_grid),"explicit noncanonical grid accepted");
    for(std::size_t band=0;band<4;++band){
        const auto edge=[](std::size_t i){return 700.*(std::pow(1.+16000./700.,double(i)/5)-1.);};
        double sum=0;const double left=edge(band),center=edge(band+1),right=edge(band+2);
        for(std::size_t i=0;i<input.size();++i){const double hz=grid[i];if(hz>left && hz<right)sum+=input[i]*(hz<center?(hz-left)/(center-left):(right-hz)/(right-center));}
        require(std::abs(explicit_grid[band]-sum)<1e-4,"independent continuous-Hertz triangular weighting oracle");
    }
}
void cadence_oracle(){
    for(const auto rate:{32768u,44100u}){
        dsp_kernels::SampleCadence cadence;require(cadence.prepare(rate,1,120),"Prepare rational cadence");
        std::uint64_t total=0;
        for(std::uint64_t i=1;i<=1200;++i){total+=cadence.next();require(total==i*rate/120,"rational deadlines equal integer oracle");}
    }
    dsp_kernels::ProfileCompatibility frontend;frontend.fft_size=512;
    const auto scaling=dsp_kernels::profile_window_scaling(frontend);
    require(scaling.samples==512 && scaling.sum==dsp_kernels::window_sum(512,frontend.window),"declared window C scaling");
    require(scaling.sum_squares==dsp_kernels::window_square_sum(512,frontend.window),"declared window E scaling");
}
void compensation_curve_oracle() {
    using namespace dsp_kernels;
    const std::array<double,6> peaks{0,.5,.1,.25,0,1.2};
    std::vector<float> gains;
    require(build_frequency_peak_compensation_curve(peaks,1e-6,10,gains),
            "Per-bin peak compensation must prepare");
    require(gains==std::vector<float>({1,2,10,4,1,1}),
            "Each bin is independently normalized: .5->2, .1->10, .25->4");
    PreparedFrequencyNormalizer normalizer;
    require(normalizer.prepare(gains),"Prepared peak normalizer");
    const std::vector<float> input{0,.5f,.1f,.25f,0,1.2f};std::vector<float> output(input.size());
    require(normalizer.process(input,output),"Apply peak normalization");
    require(std::abs(output[1]-1)<1e-6 && std::abs(output[2]-1)<1e-6 &&
            std::abs(output[3]-1)<1e-6 && output[5]==input[5],
            "Measured maxima are normalized to unity without attenuating values above one");
    require(build_frequency_peak_compensation_curve(peaks,1e-6,5,gains) &&
            gains[1]==2 && gains[2]==5 && gains[3]==4,"Maximum boost clips only required gains");
    const auto unchanged=gains;
    auto broken=peaks;broken[2]=std::numeric_limits<double>::quiet_NaN();
    require(!build_frequency_peak_compensation_curve(broken,0,5,gains) && gains==unchanged,
            "Nonfinite measurements must preserve destination");
    require(!build_frequency_peak_compensation_curve(peaks,0,.5,gains) && gains==unchanged,
            "Invalid maximum amplification must be rejected");
    const std::array<double,4> all_silent{};
    require(build_frequency_peak_compensation_curve(all_silent,1e-6,5,gains) &&
            std::all_of(gains.begin(),gains.end(),[](float v){return v==1;}),
            "Silent peak bins are not amplified");
    ProfileCompatibility c;c.fft_size=512;
    ProfileTraining training;training.gain_mode=ProfileGainMode::PeakNormalization;
    training.hop=256;training.smoothing_radius=0;training.max_gain=10;
    CompositionStatistics track;track.source="peak-one";track.frames=track.active_frames=2;
    track.mean_spectrum.assign(257,.05);track.peak_spectrum.assign(257,.5);
    track.peak_spectrum[1]=.5;track.peak_spectrum[2]=.1;
    auto second=track;second.source="peak-two";second.peak_spectrum[1]=.25;
    second.peak_spectrum[2]=.2;second.mean_spectrum[1]=.01;
    FrequencyProfile profile;std::string error;
    require(build_frequency_profile(c,training,std::array{track,second},profile,error),
            "Measured per-bin peak profile must build");
    require(profile.peak_spectrum[1]==.5 && profile.peak_spectrum[2]==.2 &&
            profile.gains[1]==2 && profile.gains[2]==5,
            "Global maximum across tracks, not average, drives independent gains");
    desktop_analysis::offline::ProfileDocument original;original.profile=profile;
    desktop_analysis::offline::ProfileDocument restored;QString diagnostic;
    require(desktop_analysis::offline::decode_profile(
            desktop_analysis::offline::encode_profile(original),restored,diagnostic) &&
            restored.profile.training.gain_mode==ProfileGainMode::PeakNormalization &&
            restored.profile.peak_spectrum==profile.peak_spectrum &&
            restored.profile.compositions[1].peak_spectrum==second.peak_spectrum,
            "Persisted peak mode and per-track peaks must round-trip");
    training.gain_mode=ProfileGainMode::MeanEqualization;
    track.mean_spectrum.assign(257,1);track.mean_spectrum[1]=10;track.peak_spectrum.clear();
    FrequencyProfile mean_profile;
    require(build_frequency_profile(c,training,std::span(&track,1),mean_profile,error) &&
            mean_profile.gains[1]<1 && mean_profile.peak_spectrum.empty(),
            "Legacy mean equalization remains independent and may attenuate");
}
void profile_mismatch_reasons(){
    using namespace dsp_kernels;
    FrequencyProfile profile;const auto bins=profile.compatibility.fft_size/2+1;
    profile.gains.assign(bins,1);profile.mean_spectrum.assign(bins,1);
    auto expected=profile.compatibility;expected.sample_rate=44100;
    const auto reasons=profile_mismatches(profile,expected);
    require(std::find(reasons.begin(),reasons.end(),ProfileMismatchReason::SampleRate)!=reasons.end(),"typed Fs mismatch reason");
    expected=profile.compatibility;expected.reference=2;expected.floor_db=-90;
    require(profile_mismatches(profile,expected,ProfileMatchScope::SignalOnly).empty(),"signal-only scope preserves profile training calibration");
    require(profile_mismatches(profile,expected).size()==2,"exact scope reports reference and floor reasons");
    profile.gains[0]=0;
    const auto malformed=profile_mismatches(profile,profile.compatibility);
    require(std::find(malformed.begin(),malformed.end(),ProfileMismatchReason::InvalidCoefficient)!=malformed.end(),"malformed coefficient has typed reason");
}
void preview_coordinate_oracle(const desktop_analysis::offline::SpectrogramPreview& image,std::size_t fft_bins){
    require(image.time_seconds.size()==image.rows && image.frequency_hz.size()==image.bands,"preview coordinate extents");
    for(std::size_t row=0;row<image.rows;++row)require(std::abs(image.time_seconds[row]-(double(row)+.5)*image.duration_seconds/double(image.rows))<1e-12,"exact temporal bucket centers");
    for(std::size_t band=0;band<image.bands;++band){
        const auto first=band*fft_bins/image.bands,last=(band+1)*fft_bins/image.bands;
        require(image.frequency_hz[band]==double(first+last-1)*.5*image.bin_hz,"mean FFT-bin frequency bucket center");
    }
}
void invalid_preview_axes_are_atomic(const desktop_analysis::offline::ProfileDocument& original){
    using namespace desktop_analysis::offline;
    const auto source=QJsonDocument::fromJson(encode_profile(original)).object();
    auto malformed=source;auto tracks=source.value("compositions").toArray();
    auto track=tracks[0].toObject(),preview=track.value("preview").toObject();
    preview.remove("time_seconds");track["preview"]=preview;tracks[0]=track;malformed["compositions"]=tracks;
    ProfileDocument output=original;QString error;
    require(!decode_profile(QJsonDocument(malformed).toJson(QJsonDocument::Compact),output,error),"missing preview coordinates rejected");
    require(output.previews[0].time_seconds==original.previews[0].time_seconds && output.profile.gains==original.profile.gains,"invalid coordinate load leaves output unchanged");
}
void malformed_preview_normalization(){
    desktop_analysis::offline::SpectrogramPreview image;image.rows=2;image.bands=2;image.values={1};
    require(image.normalized({1,1}).empty(),"short preview payload refused before normalization");
    image.values={1,2,3,4};require(image.normalized({1}).empty(),"normalization refuses more bands than gain bins");
    image.rows=SIZE_MAX;image.bands=2;require(image.normalized({1,1}).empty(),"overflowing preview dimensions refused");
}
void channel_selection_oracle(const QString& path){
    QByteArray bytes;bytes.append("RIFF");u32(bytes,44);bytes.append("WAVEfmt ");u32(bytes,16);
    u16(bytes,1);u16(bytes,2);u32(bytes,32768);u32(bytes,32768*4);u16(bytes,4);u16(bytes,16);bytes.append("data");u32(bytes,8);
    u16(bytes,16384);u16(bytes,std::uint16_t(-16384));u16(bytes,8192);u16(bytes,24576);
    QFile file(path);require(file.open(QIODevice::WriteOnly),"stereo fixture");file.write(bytes);file.close();
    desktop_analysis::offline::AudioDecoder decoder;QString error;std::array<float,2> output;std::size_t count=0;
    require(decoder.open(path,error,dsp_kernels::ChannelPolicy::Mean),"explicit mean decoder");
    require(decoder.read(output,count,error) && count==2 && output[0]==0 && output[1]==.5f,"arithmetic mean channel oracle");
    require(decoder.open(path,error,dsp_kernels::ChannelPolicy::Select,1),"explicit selected decoder");
    require(decoder.read(output,count,error) && count==2 && output[0]==-.5f && output[1]==.75f,"selected channel oracle");
}

}
static void budget_and_cancel(const QTemporaryDir& temp,auto& options,std::atomic_bool& cancel) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        AnalysisOptions over_budget;over_budget.preview_time_bins=2048;over_budget.preview_frequency_bins=2048;QStringList many;for(unsigned i=0;i<4096;++i)many.append(temp.filePath("short.wav"));
        require(!analyze_music_files(many,over_budget,cancel).success,"Oversized preview allocation rejected before processing");
        cancel=true;auto stopped=analyze_music_files({temp.filePath("long.wav")},options,cancel);require(stopped.cancelled&&!stopped.success,"Cancellation ignored");
}

static void frontend_modes(const QTemporaryDir& temp,std::atomic_bool& cancel,QString& json_error) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        // Exercise the same frontend choices as the live graph, including EOF
        // drain and a reloaded profile/preview without a second library scan.
        for(const auto mode:{ResamplerMode::ExistingFir,ResamplerMode::CompactFir,ResamplerMode::SpeexDsp,ResamplerMode::IirExperimental,ResamplerMode::LinearDiagnostic}){
            AnalysisOptions selected;selected.workers=1;selected.compatibility.sample_rate=37000;selected.compatibility.resampler_mode=mode;selected.compatibility.resampler_quality=7;
            selected.compatibility.fft_size=512;selected.training.hop=128;selected.preview_time_bins=16;selected.preview_frequency_bins=32;
            const auto path=temp.filePath("frontend.wav");wav(path,4801,false,48000);
            auto scan=analyze_music_files({path,path},selected,cancel);
            if(!scan.success)std::cerr<<scan.errors.join('\n').toStdString()<<'\n';
            require(scan.success && scan.document.profile.compatibility.resampler_mode==mode && scan.document.profile.compatibility.resampler_quality==7,"scanner honors graph frontend mode and quality");
            require(scan.document.profile.compositions[0].samples==(4801ull*37000+47999)/48000,"frontend drains to exact content length");
            selected.workers=2;auto threaded=analyze_music_files({path,path},selected,cancel);
            require(threaded.success && threaded.document.profile.mean_spectrum==scan.document.profile.mean_spectrum,"frontend results are deterministic across scan workers");
            ProfileDocument restored;require(decode_profile(encode_profile(scan.document),restored,json_error),"frontend profile roundtrip");
            const auto preview=analyze_music_preview(path,selected,cancel);
            require(preview.success && preview.preview.values==scan.document.previews[0].values && restored.previews[0].values==preview.preview.values,"rescanned and stored previews use identical frontend");
            if(mode==ResamplerMode::SpeexDsp){
                for(const auto count:{1u,97u,4097u}){wav(path,count,false,48000,true);const auto tail=analyze_music_preview(path,selected,cancel);
                    require(tail.success && std::abs(tail.preview.duration_seconds-double((std::uint64_t(count)*37000+47999)/48000)/37000)<1e-12,"Speex terminal impulse drains to exact length");}
            }
        }
}

static void large_fft_scans(const QTemporaryDir& temp,auto& options,std::atomic_bool& cancel) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        options.training.interval_numerator=1;options.training.interval_denominator=120;
        for(const std::size_t fft:{8192,16384}){
            AnalysisOptions large;large.compatibility.fft_size=fft;large.training.hop=fft/4;large.workers=50;large.preview_time_bins=16;large.preview_frequency_bins=32;
            auto scan=analyze_music_files({temp.filePath("short.wav")},large,cancel);
            require(scan.success && scan.document.profile.gains.size()==fft/2+1 && scan.document.profile.compositions[0].mean_spectrum.size()==fft/2+1,"8192/16384 FFT scans retain every coefficient");
            require(scan.document.profile.mean_spectrum[fft*256/32768]>1e-5 && scan.document.previews[0].bands==32,"Large FFT resolves synthetic tone independently of bounded display resolution");
        }
}

static double terminal_impulse_sample(std::uint64_t i,std::uint64_t count,std::uint64_t rate,std::uint64_t target,std::uint64_t discard,const auto& g,const auto& w) {
double value=0;
                    if(rate==target)value=i+1==count?32767./32768.:0.;
                    else{const auto position=(i+discard)*rate;const auto newest=(position+target-1)/target;
                        if(newest>=count-1 && newest-(count-1)<g.taps){const auto k=std::size_t(newest-(count-1));
                            const auto back=(newest*target-position)*g.phases;const auto phase=back/target;
                            const double blend=double(back%target)/target;
                            value=((1.-blend)*w[phase*g.taps+k]+blend*w[(phase+1)*g.taps+k])*32767./32768.;}}
return value;
}
static void terminal_tail_scans(const QTemporaryDir& temp,auto& options,std::atomic_bool& cancel) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        channel_selection_oracle(temp.filePath("channels.wav"));
        options.training.interval_numerator=0;options.training.hop=1;
        // Short tracks, decoder chunk boundaries, equal-rate and 8x upsampling:
        // an indexed terminal-impulse oracle checks both the retained tail and
        // the absence of extra silence beyond ceil(N*Fs_out/Fs_in).
        for(const auto rates:{std::pair{48000u,32768u},std::pair{48000u,32000u},std::pair{44100u,32768u},
                              std::pair{44100u,32000u},std::pair{32768u,32768u},std::pair{8000u,64000u}})
            for(const auto count:{1u,3u,97u,4095u,4096u,4097u}){
                const auto [rate,target]=rates;const auto path=temp.filePath("terminal.wav");wav(path,count,false,rate,true);
                options.compatibility.sample_rate=target;auto sampled=analyze_music_files({path},options,cancel);
                require(sampled.success,"Terminal impulse analysis failed");
                const auto expected=(std::uint64_t(count)*target+rate-1)/rate;
                const auto& stats=sampled.document.profile.compositions[0];require(stats.samples==expected,"Terminal impulse EOF count is not exact");
                PcmResampler filter;require(filter.prepare(rate,target,1),"Tail oracle prepare failed");
                const auto g=filter.filter_geometry();const auto w=filter.coefficients();
                const auto discard=(std::uint64_t(g.delay)*target+rate-1)/rate;double squares=0;
                for(std::uint64_t i=0;i<expected;++i){const auto value=terminal_impulse_sample(i,count,rate,target,discard,g,w);
                    squares+=value*value;
                }
                require(std::abs(stats.rms-std::sqrt(squares/expected))<2e-7,"Terminal impulse tail differs from indexed oracle");
            }
}

static void resampled_eof(const QTemporaryDir& temp,auto& options,std::atomic_bool& cancel) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        // EOF regression: previous fixed63-input flush lost the final output.
        for(const auto rate:{48000u,44100u})for(const auto target:{32768u,32000u}){
            const auto path=temp.filePath(QString("resample-%1.wav").arg(rate));wav(path,rate,false,rate);
            options.compatibility.sample_rate=target;auto sampled=analyze_music_files({path},options,cancel);
            require(sampled.success,"Resampled WAV failed");require(sampled.document.profile.compositions[0].samples==target,"Resampled EOF length is not exact");
            require(sampled.preview.duration_seconds==1.,"Resampled preview duration is not one second");
        }
}

static void library_transfer(const QTemporaryDir& temp,const auto& single,QString& json_error,auto& audited) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        // A library-sized export fixture contains no decoded music and must not
        // invoke the scanner. This measures JSON/file transfer separately.
        auto library=single.document;library.preserved.remove("analysis_run");library.profile.compatibility.fft_size=4096;library.profile.training.gain_mode=ProfileGainMode::MeanEqualization;library.profile.gains.assign(2049,1);library.profile.mean_spectrum.assign(2049,1);library.profile.peak_spectrum.clear();library.profile.frequency_presence.assign(2049,1);library.profile.compositions.clear();library.previews.clear();
        std::uint32_t rng=17;
        for(unsigned i=0;i<643;++i){CompositionStatistics track;track.source="synthetic-export-fixture/"+std::string(160,'x')+"/track-"+std::to_string(i)+".wav";track.frames=track.active_frames=10;track.samples=48000;track.rms=.1;
            track.mean_spectrum.resize(2049);for(auto& value:track.mean_spectrum){rng=rng*1664525u+1013904223u;value=double(rng)/4294967296.;}
            track.frequency_presence.assign(2049,.5);
            SpectrogramPreview image;image.source=QString::fromStdString(track.source);image.sample_rate=library.profile.compatibility.sample_rate;image.bin_hz=image.sample_rate/4096;image.rows=2;image.bands=16;image.duration_seconds=double(track.samples)/image.sample_rate;image.values.assign(32,.125f);
            image.time_seconds={image.duration_seconds*.25,image.duration_seconds*.75};
            for(std::size_t band=0;band<image.bands;++band){const auto first=band*2049/image.bands,last=(band+1)*2049/image.bands;image.frequency_hz.push_back(.5*double(first+last-1)*image.bin_hz);}
            library.previews.push_back(std::move(image));library.profile.compositions.push_back(std::move(track));}
        QElapsedTimer export_timer;export_timer.start();const auto export_path=temp.filePath("synthetic-library-profile.json");require(save_profile(export_path,library,json_error),"Library-sized coefficient export");const auto save_ms=export_timer.nsecsElapsed()/1e6;require(QFile(export_path).size()>1024*1024,"Per-track export must exceed the obsolete 1 MiB cap");
        export_timer.restart();require(load_profile(export_path,audited,json_error) && audited.profile.compositions.size()==643 && audited.profile.gains==library.profile.gains && audited.profile.compositions.back().mean_spectrum==library.profile.compositions.back().mean_spectrum && audited.previews.back().values==library.previews.back().values,"Library-sized coefficient import without scanning");
        std::cout<<"Synthetic JSON transfer: 643 records, "<<QFile(export_path).size()<<" bytes; save="<<save_ms<<" ms; load="<<export_timer.nsecsElapsed()/1e6<<" ms\n";
}

static void normalized_preview(const auto& single) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        auto normalized=single.preview.normalized(std::vector<float>(257,2));require(normalized.size()==single.preview.values.size(),"Normalized preview invalid");
        for(std::size_t i=0;i<normalized.size();++i)require(normalized[i]==single.preview.values[i]*2,"Normalized preview gain is incorrect");
}

static void worker_determinism(const QTemporaryDir& temp,const auto& single,auto& options,std::atomic_bool& cancel,QString& json_error,auto& audited) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        options.workers=4;auto parallel=analyze_music_files({temp.filePath("short.wav"),temp.filePath("long.wav"),temp.filePath("silence.wav")},options,cancel);
        require(parallel.success&&encode_profile(single.document)==encode_profile(parallel.document),"Worker count affects exported profile");
        const auto summary=single.document.preserved.value("analysis_run").toObject();require(summary.value("requested_count").toInt()==3 && summary.value("processed_count").toInt()==3 && summary.value("active_composition_count").toInt()==2 && summary.value("failed_count").toInt()==0,"Batch counts distinguish silent files from decoding failures");
        auto partial=analyze_music_files({temp.filePath("short.wav"),temp.filePath("silence.wav"),temp.filePath("missing.wav")},options,cancel);
        require(partial.success && partial.document.profile.compositions.size()==2 && partial.document.preserved["analysis_run"].toObject()["failed_count"].toInt()==1,"Partial failure remains explicit in exported audit");
        require(decode_profile(encode_profile(partial.document),audited,json_error) && audited.preserved["analysis_run"]==partial.document.preserved["analysis_run"],"Save/load preserves the run audit without rescanning audio");
}

static void compatibility_matching(const auto& c,const auto& loaded) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        const auto matches=[&](const ProfileCompatibility& expected,ProfileMatchScope scope=ProfileMatchScope::Exact){return profile_mismatches(loaded.profile,expected,scope).empty();};
        require(matches(c),"Decoded profile does not match its declared frontend");
        auto wrong=c;wrong.window=WindowFunction::Hamming;require(!matches(wrong),"Wrong window accepted");
        wrong=c;wrong.sample_rate=32000;require(!matches(wrong),"Wrong Fs accepted");
        wrong=c;wrong.reference=2;require(!matches(wrong),"Wrong reference accepted");
        wrong=c;wrong.floor_db=-100;require(!matches(wrong),"Wrong floor accepted");
        auto training_only=c;training_only.reference=2;training_only.floor_db=-100;
        require(matches(training_only,ProfileMatchScope::SignalOnly),"Signal-only matching must ignore profile-owned training levels");
        training_only.window=WindowFunction::Hamming;
        require(!matches(training_only,ProfileMatchScope::SignalOnly),"Signal-only matching must still check the analysis window");
        wrong=c;wrong.resampler_mode=ResamplerMode::IirExperimental;require(!matches(wrong),"Profile after a different resampler accepted");
        wrong=c;wrong.resampler_design_version=1;require(!matches(wrong),"Old frontend design accepted");
}

static void normalizer_validation(const auto& sentinel) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        PreparedFrequencyNormalizer normalizer;require(normalizer.prepare_manual(257,2),"Manual normalizer preparation failed");std::vector<float> input(257,3),out(257,9);
        require(normalizer.process(input,out)&&out[0]==6,"Manual gain failed");input[1]=NAN;require(!normalizer.process(input,out)&&out[0]==6,"Failure mutated normalizer output");
        input[1]=3;require(normalizer.prepare_manual(257,0)&&normalizer.process(input,out)&&out[0]==0,"Zero manual gain rejected");
        require(normalizer.prepare(sentinel.gains),"Profile normalizer preparation failed");
}

static void profile_serialization(const auto& c,const auto& doc,const auto& encoded,auto& loaded,QString& json_error) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        for(const auto kind:amplitude_spectrum_projections())for(const bool compensate:{false,true}){
            auto candidate=doc;candidate.profile.compatibility.projection={kind,compensate};ProfileDocument roundtrip;
            require(decode_profile(encode_profile(candidate),roundtrip,json_error) && roundtrip.profile.compatibility.projection==candidate.profile.compatibility.projection,"representation and explicit compensation survive profile export/import");
        }
        require(encode_profile(loaded)==encoded,"Profile canonical export is not reproducible");require(loaded.profile.gains==doc.profile.gains && loaded.profile.mean_spectrum==doc.profile.mean_spectrum && loaded.profile.compositions[0].mean_spectrum==doc.profile.compositions[0].mean_spectrum,"Binary reload changed gains or per-track Float64 spectra");
        auto binary=QJsonDocument::fromJson(encoded).object();require(binary["gains"].toObject()["encoding"].toString()=="f32le-qcompress-base64" && binary["mean_spectrum"].toObject()["encoding"].toString()=="f64le-qcompress-base64","Profile arrays use typed binary storage");
        auto damaged=binary;auto packed=damaged["gains"].toObject();packed["data"]="!!!!";damaged["gains"]=packed;
        require(!decode_profile(QJsonDocument(damaged).toJson(),loaded,json_error) && loaded.profile.gains==doc.profile.gains,"Corrupt Base64 rejected atomically");
        auto expanded=qCompress(QByteArray(qsizetype(doc.profile.gains.size()*8),char(0)));const auto expected_bytes=doc.profile.gains.size()*4;
        for(unsigned j=0;j<4;++j)expanded[int(j)]=char(expected_bytes>>(8*(3-j)));
        damaged=binary;packed=damaged["gains"].toObject();packed["data"]=QString::fromLatin1(expanded.toBase64());damaged["gains"]=packed;
        require(!decode_profile(QJsonDocument(damaged).toJson(),loaded,json_error),"Compressed stream cannot exceed its declared output budget");
        damaged=binary;packed=damaged["mean_spectrum"].toObject();packed["count"]=1;damaged["mean_spectrum"]=packed;require(!decode_profile(QJsonDocument(damaged).toJson(),loaded,json_error),"Wrong binary shape rejected");
        require(loaded.preserved["future_hint"].toObject()["keep"].toInt()==42,"Top-level unknown field lost");
        require(loaded.preserved["metadata"].toObject()["future_metadata"].toString()=="yes","Nested unknown field lost");
        auto bad=QJsonDocument::fromJson(encoded).object();bad["version"]=4;require(!decode_profile(QJsonDocument(bad).toJson(),loaded,json_error),"Future version accepted");require(loaded.profile.gains==doc.profile.gains,"Future rejection not atomic");
        require(!decode_profile("{malformed",loaded,json_error),"Malformed JSON accepted");
}

static auto weighting_profile(const auto& c,const auto& t) {
using namespace dsp_kernels;using namespace desktop_analysis::offline;
        CompositionStatistics a,b,quiet;a.frames=a.active_frames=10;a.rms=.1;a.mean_spectrum.assign(257,2);
        b=a;b.frames=b.active_frames=1000;b.mean_spectrum.assign(257,8);quiet=a;quiet.active_frames=0;quiet.mean_spectrum.assign(257,0);
        FrequencyProfile p;std::string error;require(build_frequency_profile(c,t,std::array{a,b,quiet},p,error),"Profile build failed");
        require(std::abs(p.mean_spectrum[40]-5)<1e-12,"Long tracks bias equal composition weighting");
        require(p.gains[0]==1 && p.gains[1]==1 &&
                std::all_of(p.gains.begin(),p.gains.end(),[](float gain){return gain>=1;}),
                "Scanned frequency profile must never attenuate the source spectrum");
        FrequencyProfile sentinel=p;require(!build_frequency_profile(c,t,std::array{quiet},p,error),"All silence accepted");require(p.gains==sentinel.gains,"Rejected build mutated profile");
        ProfileDocument doc;doc.profile=sentinel;doc.preserved["future_hint"]=QJsonObject{{"keep",42}};doc.preserved["metadata"]=QJsonObject{{"future_metadata","yes"}};
        doc.preserved["training"]=QJsonObject{{"future_training","yes"}};doc.preserved["compositions"]=QJsonArray{QJsonObject{{"future_track","yes"}}};
return doc;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);using namespace dsp_kernels;using namespace desktop_analysis::offline;
    try {
        cadence_oracle();mel_coordinates_oracle();compensation_curve_oracle();profile_mismatch_reasons();malformed_preview_normalization();
        ProfileCompatibility c;c.fft_size=512;ProfileTraining t;t.hop=256;t.smoothing_radius=2;
        auto doc=weighting_profile(c,t);
        auto encoded=encode_profile(doc);ProfileDocument loaded;QString json_error;require(decode_profile(encoded,loaded,json_error),"Export reload failed");
        profile_serialization(c,doc,encoded,loaded,json_error);
        compatibility_matching(c,loaded);
        normalizer_validation(doc.profile);
        QTemporaryDir temp;require(temp.isValid(),"Temp directory failed");wav(temp.filePath("short.wav"),32768);wav(temp.filePath("long.wav"),65536);wav(temp.filePath("silence.wav"),32768,true);
        require(save_profile(temp.filePath("profile.json"),doc,json_error),"Atomic profile save failed");require(load_profile(temp.filePath("profile.json"),loaded,json_error),"Profile file load failed");
        AnalysisOptions options;options.compatibility=c;options.training=t;options.workers=1;options.preview_time_bins=32;std::atomic_bool cancel=false;
        auto single=analyze_music_files({temp.filePath("short.wav"),temp.filePath("long.wav"),temp.filePath("silence.wav")},options,cancel);
        require(single.success,"Streaming WAV batch failed");require(single.document.profile.compositions[2].active_frames==0,"Silence not excluded");require(single.preview.values.size()<=32*257,"Preview is not bounded");
        require(std::abs(single.preview.duration_seconds-1)<1e-6,"Full-track preview lost duration");
        preview_coordinate_oracle(single.preview,single.document.profile.compatibility.fft_size/2+1);
        invalid_preview_axes_are_atomic(single.document);
        require(single.document.previews.size()==3 && single.document.previews[1].duration_seconds==2,"Every scanned track receives a bounded full-track preview");
        ProfileDocument cached;require(decode_profile(encode_profile(single.document),cached,json_error) && cached.previews.size()==3 && cached.previews[1].values==single.document.previews[1].values && cached.previews[1].time_seconds==single.document.previews[1].time_seconds && cached.previews[1].frequency_hz==single.document.previews[1].frequency_hz && cached.profile.compositions[1].mean_spectrum==single.document.profile.compositions[1].mean_spectrum,"Per-track previews and full-resolution statistics survive binary save/load exactly");
        ProfileDocument audited;worker_determinism(temp,single,options,cancel,json_error,audited);
        library_transfer(temp,single,json_error,audited);
        normalized_preview(single);
        resampled_eof(temp,options,cancel);
        terminal_tail_scans(temp,options,cancel);
        large_fft_scans(temp,options,cancel);
        frontend_modes(temp,cancel,json_error);
        budget_and_cancel(temp,options,cancel);
        std::cout<<"music_profile_tests PASS; wall="<<single.wall_seconds<<" s, audio="<<single.audio_seconds<<" s; threads deterministic\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
