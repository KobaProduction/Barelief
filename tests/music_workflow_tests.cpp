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
#include <QJsonArray>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <future>
#include <QJsonObject>
#include <QPainterPath>

namespace {
using namespace dsp_kernels;
using namespace desktop_analysis::offline;
void check(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
std::vector<double> stress_coordinates(unsigned tracks,bool diverse) {
    std::vector<double> hz;hz.reserve(std::size_t(tracks)*2049);
    const double rates[]{32768,44100,48000};
    for(unsigned t=0;t<tracks;++t)for(unsigned k=0;k<=2048;++k)
        hz.push_back(k*(diverse?44000.+t:rates[t%3])/4096);
    std::sort(hz.begin(),hz.end());hz.erase(std::unique(hz.begin(),hz.end()),hz.end());return hz;
}
std::vector<NumericSeriesView::Series> stress_series(const std::vector<double>& hz) {
    std::vector<NumericSeriesView::Series> result(3);
    for(unsigned s=0;s<result.size();++s){
        result[s].name=QString::number(s);result[s].values.reserve(hz.size());
        for(const auto x:hz)result[s].values.push_back(std::exp(-x/5000)*(1+.2*std::sin(x*(s+1))));
    }
    return result;
}
void stress_update(NumericSeriesView& view,const std::vector<double>& hz,const std::vector<NumericSeriesView::Series>& series,ValueDisplayScale scale) {
    const auto transaction=view.batch_update();
    view.set_scale(scale,true,-120,20);view.set_titles("Hz","dB");view.set_series(hz,series);
}
QJsonObject plot_stress(unsigned tracks,bool diverse,double view_width) {
    const auto hz=stress_coordinates(tracks,diverse);const auto series=stress_series(hz);
    NumericSeriesView view;view.resize(1000,600);ValueDisplayScale scale;scale.mode=ValueDisplayScale::Mode::Decibels;
    view.set_scale(scale,true,-120,20);view.set_series(hz,series);view.set_horizontal_view(.45*(1-view_width),view_width);
    unsigned publications=0;QObject::connect(view.presentation_model(),&QuickPlotModel::changed,[&]{++publications;});
    std::array<double,3> timings{};
    for(auto& elapsed:timings){
        QElapsedTimer clock;clock.start();
        stress_update(view,hz,series,scale);
        elapsed=clock.nsecsElapsed()/1e6;
    }
    std::sort(timings.begin(),timings.end());
    return {{"tracks",int(tracks)},{"diverse_rates",diverse},{"points",qint64(hz.size())},{"viewport_width",view_width},
        {"median_ms",timings[1]},{"publications_per_operation",publications/3.}};
}
void plot_benchmark() {
    QJsonArray rows;
    for(const auto diverse:{false,true})for(const auto tracks:{100u,1000u})for(const auto width:{1.,.01})
        rows.append(plot_stress(tracks,diverse,width));
    std::cout<<QJsonDocument(rows).toJson(QJsonDocument::Compact).constData()<<'\n';
}
class PlotProbe final:public NumericSeriesView {
public:
    using NumericSeriesView::series_path;
};
void plot_batching_and_range() {
    PlotProbe view;unsigned publications=0;
    QObject::connect(view.presentation_model(),&QuickPlotModel::changed,[&]{++publications;});
    ValueDisplayScale scale;scale.mode=ValueDisplayScale::Mode::Decibels;scale.floor=-300;
    {
        const auto outer=view.batch_update();
        view.set_bands({{1,2,true}});stress_update(view,{0,1,2},{{"one",{.001,.1,1}}},scale);
        check(publications==0,"nested transactions do not publish intermediate rasters");
    }
    check(publications==1 && view.y_range().second==10,"one final raster includes current automatic range");
    view.set_horizontal_view(.4,.1);check(view.y_range().second==10,"navigation preserves the global range");
    scale.reference=.01;view.set_scale(scale,true,0,0);
    check(view.y_range().second==50,"scale changes invalidate cached automatic range");
    view.set_series({0,1},{{"new",{.001,.01}}});check(view.y_range().second==10,"data changes invalidate cached range");
    view.set_scale(scale,false,-180,20);check(view.y_range().first==-180,"manual range remains synchronous");
}
void plot_visible_geometry() {
    PlotProbe view;ValueDisplayScale scale;scale.mode=ValueDisplayScale::Mode::Linear;
    view.set_scale(scale,false,0,1);std::vector<double> hz(10001);NumericSeriesView::Series series;
    for(std::size_t i=0;i<hz.size();++i)hz[i]=double(i);
    series.values.assign(hz.size(),.5);view.set_series(hz,{series});
    auto path=view.series_path(series,{100,100},4000.2,4010.8);
    check(path.elementCount()==12,"visible range retains exactly one neighbor on either side");
    check(path.elementAt(0).x<0 && path.elementAt(11).x>100,"boundary crossing segments are retained");
    series.values[4005]=std::numeric_limits<double>::quiet_NaN();path=view.series_path(series,{100,100},4000.2,4010.8);
    unsigned moves=0;for(int i=0;i<path.elementCount();++i)moves+=path.elementAt(i).isMoveTo();
    check(moves==2,"nonfinite samples remain gaps after clipping");
    check(view.series_path(series,{100,100},1,1).isEmpty(),"empty viewport produces no geometry");
    view.set_series({0,.1,1,10,100,1000},{{"log",{.5,.5,.5,.5,.5,.5}}});view.set_logarithmic_x(true);
    path=view.series_path({"log",{.5,.5,.5,.5,.5,.5}},{100,100},.5,1.5);
    check(path.elementCount()==3 && path.elementAt(0).x==-50 && path.elementAt(2).x==150,"log viewport clips in physical coordinates and excludes DC");
}
void plot_pixel_extrema() {
    PlotProbe view;ValueDisplayScale scale;scale.mode=ValueDisplayScale::Mode::Linear;view.set_scale(scale,false,0,1);
    std::vector<double> hz(10001);NumericSeriesView::Series series;series.values.assign(hz.size(),.5);
    for(std::size_t i=0;i<hz.size();++i)hz[i]=double(i)/10000;
    series.values[1210]=0;series.values[1211]=1;view.set_series(hz,{series});
    const auto path=view.series_path(series,{32,100},0,1);
    check(path.elementCount()<=4*33,"dense geometry is bounded by pixel columns");
    int minimum=-1,maximum=-1;
    for(int i=0;i<path.elementCount();++i){
        const auto p=path.elementAt(i);if(p.y==100)minimum=i;if(p.y==0)maximum=i;
    }
    check(minimum>=0 && maximum>minimum,"both local extrema survive in original order within the same pixel");
    check(path.elementAt(0).x==0 && path.elementAt(path.elementCount()-1).x==32,"first and last samples survive reduction");
    view.presentation_model()->hover(.1211,.5);
    check(view.presentation_model()->state().value("title").toString().endsWith(": 1"),"hover reads full source samples rather than reduced geometry");
    series.values[1210]=std::numeric_limits<double>::quiet_NaN();
    const auto gapped=view.series_path(series,{32,100},0,1);unsigned moves=0;
    for(int i=0;i<gapped.elementCount();++i)moves+=gapped.elementAt(i).isMoveTo();
    check(moves==2,"a gap inside a dense pixel bucket never becomes a connecting line");
}
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
double log_oracle(double hz){
    const auto x=2*(hz-100)/23900-1;
    return -3+.2*x+.15*x*x+.3*x*x*x;
}
FrequencyMeasurements density_measurements(bool clustered){
    FrequencyMeasurements data;data.edge_hz=100;
    for(double hz=0;hz<=24000;hz+=100)data.hz.push_back(hz);
    if(clustered)for(double hz=5001;hz<6000;hz+=1)data.hz.push_back(hz);
    std::sort(data.hz.begin(),data.hz.end());data.hz.erase(std::unique(data.hz.begin(),data.hz.end()),data.hz.end());
    for(const auto hz:data.hz){data.maxima.push_back(std::exp(log_oracle(hz)));data.mean_amplitude.push_back(data.maxima.back()/4);data.contributors.push_back(1);}
    return data;
}
void fit_density_invariance(){
    const auto calibration=measurements().compatibility;FrequencyCurveOptions options;options.polynomial_degree=2;
    for(const auto method:{CurveApproximation::Polynomial,CurveApproximation::PiecewiseSmooth}){
        options.approximation=method;FrequencyProfile uniform,clustered;std::string error;
        check(fit_frequency_measurements(calibration,density_measurements(false),options,uniform,error),error.c_str());
        check(fit_frequency_measurements(calibration,density_measurements(true),options,clustered,error),error.c_str());
        check(std::abs(uniform.curve->fit_rmse_db-clustered.curve->fit_rmse_db)<.002,"error metric is independent of a dense frequency cluster");
        for(double hz=137;hz<23900;hz+=173){
            const auto a=frequency_curve_amplitude(uniform,hz),b=frequency_curve_amplitude(clustered,hz);
            check(std::abs(20*std::log10(a/b))<.005,"dense sampling does not bias the approximation between source points");
            const auto x=2*(hz-100)/23900-1;
            const auto expected=method==CurveApproximation::Polynomial?-3+.38*x+.15*x*x:log_oracle(hz);
            check(std::abs(20/std::log(10.)*(std::log(b)-expected))<.02,"fit agrees with an independent analytic oracle");
        }
        if(method==CurveApproximation::Polynomial)
            check(std::abs(uniform.curve->fit_rmse_db-20/std::log(10.)*.12/std::sqrt(7.))<.001,"physical-Hz polynomial RMS matches its analytic integral");
    }
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
void readable_track_list(core_app::MusicAnalysisDialog& dialog){
    auto* tracks=dialog.findChild<QListWidget*>("musicResultFiles");
    check(tracks->item(0)->data(Qt::UserRole).toString().contains("32768") && tracks->item(0)->data(Qt::UserRole).toString().contains("1.0 s"),"track list exposes native rates and duration while retaining source identity");
    auto* workspace=dialog.findChild<QSplitter*>("musicWorkspace");const auto sizes=workspace->sizes();
    workspace->setSizes({650,500,300});
    QApplication::processEvents();
    check(tracks->visualItemRect(tracks->item(0)).height()>=3*tracks->fontMetrics().height(),"track details occupy separate readable lines instead of a clipped horizontal row");
    workspace->setSizes(sizes);QApplication::processEvents();
    const auto* failed=tracks->item(tracks->count()-1);
    check(tracks->count()==4 && !(failed->flags()&Qt::ItemIsEnabled) && failed->toolTip().contains("missing.wav"),"failed files remain visible as diagnostics and cannot enter the corpus");
}
void localized_result_status(core_app::MusicAnalysisDialog& dialog){
    dialog.findChild<QPushButton*>("musicNewScan")->click();dialog.findChild<QPushButton*>("musicShowResults")->click();
    ui::Language::instance().set("ru");ui::Language::instance().set("en");
    check(dialog.findChild<QLabel*>("musicStatus")->text()==ui::text("Showing completed scan results"),"the current result status changes language along with the workspace");
}
void automatic_ui(const QString& directory) {
    ProfileDocument raw;raw.profile=measurements();QString error;const auto scan=directory+"/ui.brscan";
    raw.preserved["analysis_run"]=QJsonObject{{"requested_count",4},{"processed_count",3},{"failed_count",1},{"errors",QJsonArray{"missing.wav: cannot open"}}};
    check(save_music_scan(scan,raw,error),qPrintable(error));
    AnalysisServices services;services.load_scan=load_music_scan;services.save=save_profile;services.save_scan=save_music_scan;
    unsigned preview_reads=0;services.preview=[&](const auto&,const auto&,const auto&){++preview_reads;return PreviewResult{};};
    core_app::MusicAnalysisDialog dialog({}, {},std::move(services));dialog.resize(1600,900);dialog.show();
    check(dialog.load_scan_from(scan,error) && wait_ready(dialog),"loaded scan automatically builds curve");
    auto* storage=dialog.findChild<QLabel*>("musicStorageInfo");
    check(storage->isVisible() && storage->text().contains("ui.brscan") && storage->toolTip().contains("Blosc2/Zstd5"),"actual saved-file information is available without archive settings");
    readable_track_list(dialog);
    localized_result_status(dialog);
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
void cached_selection(core_app::MusicAnalysisDialog& dialog,const unsigned& fits){
    auto* choice=dialog.findChild<QComboBox*>("musicApproximation");
    for(const auto method:{0,1}){
        choice->setCurrentIndex(method);
        check(dialog.findChild<QPushButton*>("musicSaveProfile")->isEnabled() && fits==1,"selecting a cached approximation is immediate and launches no worker");
    }
}
void retained_application(core_app::MusicAnalysisDialog& dialog,const QLabel& application_state){
    dialog.findChild<QPushButton*>("musicNewScan")->click();dialog.findChild<QPushButton*>("musicShowResults")->click();
    check(application_state.text().contains("Profile applied"),"opening new scan settings preserves the application state of unchanged results");
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
    cached_selection(dialog,fits);
    auto* choice=dialog.findChild<QComboBox*>("musicProfileTarget");choice->setCurrentIndex(1);
    targets[1].options.compatibility.sample_rate=44100;targets[1].options.compatibility.fft_size=2048;
    check(wait_target_summary(dialog,"44100") && choice->currentIndex()==1,"grid watcher refreshes the selected target without switching routes");
    targets[1].options.compatibility.sample_rate=48000;targets[1].options.compatibility.fft_size=8192;
    dialog.findChild<QPushButton*>("musicApplyProfile")->click();
    check(applied.size()==4097 && fits==1,"Apply reads current graph geometry immediately and uses the same function without refitting");
    auto* application_state=dialog.findChild<QLabel*>("musicApplicationState");
    check(application_state->text().contains("Profile applied"),"successful application is visible on the application card");
    retained_application(dialog,*application_state);
    targets[1].options.compatibility.sample_rate=32768;
    check(wait_target_summary(dialog,"32768") && application_state->text().contains("Apply again"),"graph grid changes explicitly require applying the profile again");
    check(!dialog.findChild<QLabel*>("musicStatus")->text().contains("Profile applied to the scheme"),"stale application success is removed after a grid change");
    targets.pop_back();
    check(wait_target_summary(dialog,"No declared") && choice->currentIndex()==-1,"a removed selected route cannot silently select another route");
    check(!dialog.findChild<QPushButton*>("musicApplyProfile")->isEnabled(),"removed route disables application");
}
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);ui::Language::instance().set("en");QTemporaryDir directory;
    if(app.arguments().contains("--plot-benchmark")){plot_benchmark();return 0;}
    try{
        plot_batching_and_range();plot_visible_geometry();plot_pixel_extrema();
        native_axis_and_coverage();cache_and_representation();fit_density_invariance();portable_function_storage(directory.path());malformed_axes_are_atomic();legacy_function_semantics();
        automatic_ui(directory.path());stale_fit_is_discarded(directory.path());target_grid_updates();
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    std::cout<<"music_workflow_tests PASS\n";
}
