#include <offline/curve_analysis.h>
#include <offline/profile_json.h>
#include <dsp_kernels/frequency_measurements.h>
#include <core_app/music_analysis_dialog.h>
#include <barelief/numeric_series_view.h>
#include <barelief/ui_text.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <future>

namespace {
using namespace dsp_kernels;
using namespace desktop_analysis::offline;
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
FrequencyProfile measurements() {
    FrequencyProfile p;p.compatibility.sample_rate=32768;p.compatibility.fft_size=4096;
    p.compatibility.projection={SpectrumProjection::PeakAmplitude,true};p.compatibility.resampler_mode=ResamplerMode::Bypass;
    p.training.min_gain=p.training.max_gain=1;p.gains.assign(2049,1);p.mean_spectrum.assign(2049,0);
    for(const double rate:{32768.,44100.,48000.}){
        CompositionStatistics t;t.source="synthetic-"+std::to_string(int(rate))+".wav";
        t.source_sample_rate=t.sample_rate=rate;t.measured_max_hz=rate/2;t.frames=t.active_frames=100;t.samples=std::uint64_t(rate);t.rms=.05;
        t.mean_spectrum.resize(2049);t.peak_spectrum.resize(2049);t.quiet_mean_spectrum.assign(2049,0);t.quiet_peak_spectrum.assign(2049,0);
        const auto multiplier=double(1u<<p.compositions.size());
        for(std::size_t k=0;k<2049;++k){t.peak_spectrum[k]=multiplier*(.1+k*rate/4096/1e6);t.mean_spectrum[k]=t.peak_spectrum[k]/4;}
        t.rms_statistics_version=1;CompositionStatistics::RmsBand b;b.index=87;b.frames=100;b.square_sum=.25;
        b.mean_spectrum.assign(t.mean_spectrum.begin(),t.mean_spectrum.end());b.peak_spectrum.assign(t.peak_spectrum.begin(),t.peak_spectrum.end());
        t.rms_bands.push_back(std::move(b));p.compositions.push_back(std::move(t));
    }
    return p;
}
void native_axis_and_coverage() {
    const auto source=measurements();FrequencyMeasurements data;std::string error;FrequencyCurveOptions options;
    check(combine_frequency_measurements(source.compatibility,source.compositions,options,data,error),"combine native grids");
    check(data.hz.back()==24000 && data.hz.size()>2049,"32768 Hz placeholder must not truncate a 48000 Hz track");
    for(std::size_t k=0;k<data.hz.size();++k){
        const auto hz=data.hz[k];double count=0,sum=0;unsigned multiplier=1;
        for(const auto& t:source.compositions){if(hz<=t.sample_rate/2){++count;sum+=multiplier*(.1+hz/1e6);}multiplier*=2;}
        check(data.contributors[k]==count,"coverage follows native Nyquist");
        check(std::abs(data.maxima[k]-sum/count)<1e-12,"equal track weighting in physical Hz");
    }
    auto different=source.compatibility;different.sample_rate=48000;FrequencyMeasurements other;
    check(combine_frequency_measurements(different,source.compositions,options,other,error),"combine with another target placeholder");
    check(other.hz==data.hz && other.maxima==data.maxima,"target grid is absent from native aggregation");
}
void cache_and_representation() {
    const auto source=measurements();const auto corpus=curve_corpus(source);FrequencyCurveOptions options;
    auto first=std::make_shared<CurveAnalysis>(analyze_frequency_curves(corpus,options));
    check(first->error.empty() && first->polynomial.curve && first->piecewise.curve,"both approximations calculated");
    options.max_gain=2;auto limited=std::make_shared<CurveAnalysis>(analyze_frequency_curves(corpus,options,first));
    check(limited->measurements==first->measurements,"gain changes reuse combined measurements");
    check(limited->polynomial.curve->coefficients==first->polynomial.curve->coefficients,"gain changes do not refit polynomial");
    options.polynomial_degree=3;auto degree=analyze_frequency_curves(corpus,options,limited);
    check(degree.measurements==limited->measurements,"degree changes reuse measurements");
    check(degree.piecewise.curve->knot_hz==limited->piecewise.curve->knot_hz,"degree changes preserve independent piecewise fit");
    options.aggregation=PeakAggregation::UpperPercentile;auto percentile=analyze_frequency_curves(corpus,options,limited);
    check(percentile.error.empty() && percentile.measurements!=limited->measurements,"aggregation changes rebuild only derived statistics");
    check(corpus->tracks.front().rms_bands.empty(),"curve job does not copy RMS archive data");
}
void portable_function_storage(const QString& directory) {
    ProfileDocument raw;raw.profile=measurements();QString error;
    const auto scan=directory+"/measurements.brscan";check(save_music_scan(scan,raw,error),qPrintable(error));
    ProfileDocument restored;check(load_music_scan(scan,restored,error),qPrintable(error));
    auto result=analyze_frequency_curves(curve_corpus(restored.profile),{});check(result.error.empty(),result.error.c_str());
    ProfileDocument ready;ready.profile=result.selected();const auto path=directory+"/function.brprofile";
    check(save_profile(path,ready,error) && load_profile(path,restored,error),qPrintable(error));
    check(restored.profile.compositions.empty() && restored.previews.empty(),"ready profile has no track archives");
    check(restored.profile.curve->measurement_hz.back()==24000,"saved function keeps frequencies above placeholder Nyquist");
    const auto object=QJsonDocument::fromJson(encode_profile(ready)).object();
    check(object.value("version").toInt()==6 && !object.contains("gains"),"new function does not persist a target grid cache");
    for(const double rate:{32768.,44100.,48000.})for(const std::size_t fft:{512,2048,4096,8192}){
        auto target=ready.profile.compatibility;target.sample_rate=rate;target.fft_size=fft;FrequencyProfile compiled;
        std::string diagnostic;check(resample_frequency_profile(restored.profile,target,compiled,diagnostic),diagnostic.c_str());
        for(std::size_t k=0;k<compiled.gains.size();++k)
            check(compiled.gains[k]==float(frequency_curve_gain(ready.profile,k*rate/fft)),"target coefficients equal function evaluation");
    }
}
void malformed_axes_are_atomic() {
    const auto source=measurements();const auto result=analyze_frequency_curves(curve_corpus(source),{});
    ProfileDocument ready;ready.profile=result.selected();auto object=QJsonDocument::fromJson(encode_profile(ready)).object();
    auto function=object.value("function").toObject();function["edge_hz"]=0;object["function"]=function;
    const auto before=ready.profile.gains;QString error;
    check(!decode_profile(QJsonDocument(object).toJson(),ready,error) && ready.profile.gains==before,"invalid function rejected atomically");
    auto broken=result.selected();broken.curve->measurement_hz[2]=broken.curve->measurement_hz[1];std::string diagnostic;
    check(!update_frequency_curve_gains(broken,diagnostic),"invalid axis rejected before interpolation");
    broken=result.selected();broken.compatibility.reference=17;
    const auto old=broken.gains;check(update_frequency_curve_gains(broken,diagnostic) && broken.gains==old,"display reference does not change new physical gain function");
}
void legacy_function_semantics() {
    auto raw=measurements();raw.compatibility.sample_rate=48000;
    auto calculation=analyze_frequency_curves(curve_corpus(raw),{});ProfileDocument legacy;legacy.profile=calculation.selected();
    auto& p=legacy.profile;auto& function=*p.curve;function.measured_spectrum=p.peak_spectrum;
    function.measurement_hz.clear();function.mean_amplitude.clear();function.contributors.clear();function.edge_hz=0;
    p.compatibility.reference=.25;std::string validation;check(update_frequency_curve_gains(p,validation),validation.c_str());
    const auto bytes=encode_profile(legacy);const auto root=QJsonDocument::fromJson(bytes).object();
    check(root.value("version").toInt()==4 && root.value("function").toObject().value("version").toInt()==1,"authentic legacy function encoding");
    ProfileDocument restored;QString error;check(decode_profile(bytes,restored,error),qPrintable(error));
    check(restored.profile.curve->measurement_hz.empty() && restored.profile.compatibility.reference==.25,"legacy grid and reference retained");
    check(restored.profile.gains==p.gains,"legacy coefficients preserved");
    for(double hz=0;hz<25000;hz+=137)
        check(frequency_curve_gain(restored.profile,hz)==frequency_curve_gain(p,hz),"legacy function retained at arbitrary physical frequencies");
}
bool wait_ready(core_app::MusicAnalysisDialog& dialog) {
    QElapsedTimer deadline;deadline.start();
    while(deadline.elapsed()<10000){QApplication::processEvents();
        if(dialog.findChild<QPushButton*>("musicSaveProfile")->isEnabled())return true;
        QThread::msleep(1);
    }
    return false;
}
struct ReleaseFit {
    std::atomic_bool& released;
    ~ReleaseFit(){released=true;}
};
void stale_fit_is_discarded(const QString& directory) {
    ProfileDocument raw;raw.profile=measurements();AnalysisServices services;
    services.load_scan=[raw](const QString&,ProfileDocument& out,QString&){out=raw;return true;};services.save=save_profile;
    std::promise<void> entered;auto running=entered.get_future();std::atomic_bool released=false;std::atomic_uint calls=0;
    services.curves=[&](auto source,const auto& settings,auto previous){
        if(calls.fetch_add(1)==0){entered.set_value();while(!released.load())QThread::msleep(1);}
        return analyze_frequency_curves(source,settings,previous);
    };
    core_app::MusicAnalysisDialog dialog({}, {},std::move(services));ReleaseFit release{released};QString error;
    check(dialog.load_scan_from("synthetic-snapshot",error),qPrintable(error));QElapsedTimer deadline;deadline.start();
    while(running.wait_for(std::chrono::seconds(0))!=std::future_status::ready && deadline.elapsed()<5000){QApplication::processEvents();QThread::msleep(1);}
    check(running.wait_for(std::chrono::seconds(0))==std::future_status::ready,"first fit confirmed in flight");
    dialog.findChild<QComboBox*>("musicAggregation")->setCurrentIndex(1);
    dialog.findChild<QDoubleSpinBox*>("musicMaximumGain")->setValue(2);released=true;
    check(wait_ready(dialog) && calls.load()>=2,"superseded worker completes before latest worker is published");
    const auto path=directory+"/latest.brprofile";check(dialog.save_profile_to(path,error),qPrintable(error));
    ProfileDocument saved;check(load_profile(path,saved,error),qPrintable(error));
    check(saved.profile.curve->options.aggregation==PeakAggregation::GlobalMaximum && saved.profile.training.max_gain==2,"old worker cannot publish stale settings");
}
void automatic_ui(const QString& directory) {
    ProfileDocument raw;raw.profile=measurements();QString error;const auto scan=directory+"/ui.brscan";
    check(save_music_scan(scan,raw,error),qPrintable(error));
    AnalysisServices services;services.load_scan=load_music_scan;services.save=save_profile;services.save_scan=save_music_scan;
    unsigned preview_reads=0;services.preview=[&](const auto&,const auto&,const auto&){++preview_reads;return PreviewResult{};};
    core_app::MusicAnalysisDialog dialog({}, {},std::move(services));dialog.resize(1600,900);dialog.show();
    check(dialog.load_scan_from(scan,error) && wait_ready(dialog),"loaded scan automatically builds curve");
    auto* aggregation=dialog.findChild<QComboBox*>("musicAggregation");aggregation->setCurrentIndex(1);aggregation->setCurrentIndex(2);aggregation->setCurrentIndex(0);
    check(wait_ready(dialog),"latest interactive settings complete automatically");
    const auto output=directory+"/ui.brprofile";check(dialog.save_profile_to(output,error),qPrintable(error));
    ProfileDocument saved;check(load_profile(output,saved,error),qPrintable(error));
    check(saved.profile.curve->options.aggregation==PeakAggregation::MeanTrackMaxima,"stale calculation never overwrites latest choice");
    check(preview_reads==0,"selecting a track never silently rereads audio");
    auto* view=dynamic_cast<NumericSeriesView*>(dialog.findChild<QWidget*>("musicMeanCurve"));check(view,"shared numeric plot");
    check(view->presentation_model()->axes().value("xTicks").toList().size()>=5,"readable frequency divisions");
    check(view->presentation_model()->axes().value("yTicks").toList().size()>=3,"intermediate vertical divisions");
    check(!dialog.findChild<QPushButton*>("musicBuildCurve"),"manual build action removed");
    check(dialog.findChild<QComboBox*>("musicProfileTarget")->isVisible(),"application card remains visible inside analysis workspace");
    dialog.resize(1024,700);QApplication::processEvents();
    if(dialog.width()!=1024 || dialog.height()!=700){
        std::cerr<<"window="<<dialog.width()<<'x'<<dialog.height()<<" minimum="<<dialog.minimumSizeHint().width()<<'x'<<dialog.minimumSizeHint().height()<<'\n';
        const auto widgets=dialog.findChildren<QWidget*>();
        for(auto* widget:widgets)if(widget->minimumSizeHint().width()>500 || widget->minimumSizeHint().height()>650)
            std::cerr<<widget->metaObject()->className()<<' '<<qPrintable(widget->objectName())<<" min="<<widget->minimumSizeHint().width()<<'x'<<widget->minimumSizeHint().height()<<'\n';
    }
    check(dialog.width()==1024 && dialog.height()==700,"workflow fits smaller desktop window");
    auto* scroll=dialog.findChild<QScrollArea*>();auto* apply=dialog.findChild<QPushButton*>("musicApplyProfile");
    scroll->ensureWidgetVisible(apply);QApplication::processEvents();
    check(scroll->viewport()->rect().contains(apply->mapTo(scroll->viewport(),apply->rect().center())),"application action remains reachable by scrolling");
}
bool wait_target_summary(core_app::MusicAnalysisDialog& dialog,const QString& text){
    QElapsedTimer deadline;deadline.start();
    while(deadline.elapsed()<3000){QApplication::processEvents();
        if(dialog.findChild<QLabel*>("musicApplicationSummary")->text().contains(text))return true;
        QThread::msleep(2);
    }
    return false;
}
void target_grid_updates(){
    ProfileDocument raw;raw.profile=measurements();AnalysisServices services;
    services.load_scan=[raw](const QString&,ProfileDocument& out,QString&){out=raw;return true;};
    std::vector<core_app::FrequencyProfileTarget> targets(2);
    for(std::size_t i=0;i<targets.size();++i){
        targets[i].coefficients=10+int(i);targets[i].arithmetic=20+int(i);targets[i].label=QString::number(i);
        targets[i].route.graph_id="fixture";targets[i].options.compatibility=raw.profile.compatibility;
    }
    unsigned fits=0;services.curves=[&](auto source,const auto& settings,auto previous){++fits;return analyze_frequency_curves(source,settings,previous);};
    std::vector<float> applied;
    core_app::MusicAnalysisDialog dialog([&]{return targets;},[&](const auto& selected,const auto& profile,QString&){
        check(selected.coefficients==11,"graph updates preserve the explicitly selected route");
        FrequencyProfile compiled;std::string error;check(resample_frequency_profile(profile,selected.options.compatibility,compiled,error),error.c_str());
        applied=compiled.gains;return true;
    },std::move(services));QString error;
    check(dialog.load_scan_from("snapshot",error) && wait_ready(dialog),"target grid scenario loads measurements");
    auto* choice=dialog.findChild<QComboBox*>("musicProfileTarget");choice->setCurrentIndex(1);
    targets[1].options.compatibility.sample_rate=44100;targets[1].options.compatibility.fft_size=2048;
    check(wait_target_summary(dialog,"44100") && choice->currentIndex()==1,"grid watcher refreshes the selected target without switching routes");
    targets[1].options.compatibility.sample_rate=48000;targets[1].options.compatibility.fft_size=8192;
    dialog.findChild<QPushButton*>("musicApplyProfile")->click();
    check(applied.size()==4097 && fits==1,"Apply reads current graph geometry immediately and uses the same function without refitting");
    targets.pop_back();
    check(wait_target_summary(dialog,"No declared") && choice->currentIndex()==-1,"a removed selected route cannot silently select another route");
    check(!dialog.findChild<QPushButton*>("musicApplyProfile")->isEnabled(),"removed route disables application");
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);ui::Language::instance().set("en");QTemporaryDir directory;
    try{
        native_axis_and_coverage();cache_and_representation();portable_function_storage(directory.path());malformed_axes_are_atomic();legacy_function_semantics();
        automatic_ui(directory.path());stale_fit_is_discarded(directory.path());target_grid_updates();
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    std::cout<<"music_workflow_tests PASS\n";
}
