#include <core_app/music_analysis_dialog.h>
#include "music_frequency_model.h"
#include <core_app/scheme_editor/numeric_node_operations.h>
#include <optional>
#include <utility>
#include <core_app/spectrum_projection_ui.h>
#include <core_app/resampler_presentation.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_series_view.h>
#include <core_app/ui/collapsible_section.h>
#include <offline/curve_analysis.h>
#include <QScrollArea>
#include <QGroupBox>
#include <barelief/numeric_image_view.h>
#include <barelief/numeric_plot_limits.h>
#include <QWheelEvent>
#include <QMouseEvent>
#include <barelief/ui_text.h>
#include <offline/music_analysis.h>
#include <dsp_kernels/frequency_measurements.h>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QStandardItemModel>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QAbstractItemModel>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <thread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>

namespace core_app {
using namespace desktop_analysis::offline;
namespace {
class TrackNameDelegate final:public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QString displayText(const QVariant& value,const QLocale&)const override {
        return QFileInfo(value.toString()).fileName();
    }
};
bool same_target_route(const FrequencyProfileTarget& a,const FrequencyProfileTarget& b){
    return a.coefficients==b.coefficients && a.arithmetic==b.arithmetic && a.projection==b.projection &&
        a.route.graph_id==b.route.graph_id && a.route.frame_source==b.route.frame_source &&
        a.route.coefficient_output==b.route.coefficient_output && a.route.coefficient_input==b.route.coefficient_input &&
        a.route.frame_output==b.route.frame_output && a.route.frame_input==b.route.frame_input;
}
bool preview_geometry_valid(const SpectrogramPreview& preview){
    return presentation::NumericPlotLimits::accepts(preview.rows,preview.bands) && preview.time_seconds.size()==preview.rows && preview.frequency_hz.size()==preview.bands && preview.values.size()==preview.rows*preview.bands;
}
class Curve final:public NumericSeriesView {
public:
    std::vector<double> values,hz;
    std::vector<NumericSeriesView::Series> comparisons;
    double fs=0,reference=1,display_floor=-120,display_ceiling=20,limit=100;
    bool gain=false,magnitude=false,automatic=true;
    explicit Curve(QWidget* parent):NumericSeriesView(parent){}
    void update(){
        ValueDisplayScale scale;scale.mode=ValueDisplayScale::Mode::Decibels;
        scale.factor=magnitude?20:10;scale.reference=gain?1:reference;scale.floor=-300;
        set_scale(scale,!gain&&automatic,gain?minimum_gain_db():display_floor,gain?std::max(3.,scale.factor*std::log10(limit)+3):display_ceiling);
        auto x=hz;
        if(x.size()!=values.size()){
            x.resize(values.size());
            for(std::size_t i=0;i<x.size();++i)x[i]=double(i)*fs/(2*double(std::max(std::size_t{1},x.size()-1)));
        }
        std::vector<NumericSeriesView::Series> series{{ui::text(gain?"Gain, dB":"Measurements"),values,false}};
        series.insert(series.end(),comparisons.begin(),comparisons.end());
        set_titles(ui::text("Frequency, Hz"),ui::text(gain?"Gain, dB":"Spectrum, dB"));
        set_series(std::move(x),std::move(series));
    }
    double minimum_gain_db()const {
        auto minimum=1.;for(const auto value:values)if(value>0)minimum=std::min(minimum,value);
        return (magnitude?20:10)*std::log10(minimum)-3;
    }
};
class Spectrogram final:public QWidget {
public:
    SpectrogramPreview preview;std::vector<float> data;double reference=1,floor_db=-120;bool magnitude=false,image_dirty=true;
    explicit Spectrogram(QWidget* parent):QWidget(parent){auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);image=new NumericImageView(this);layout->addWidget(image);setMinimumHeight(220);}
    void update(){
        if(!preview_geometry_valid(preview) || data.size()!=preview.rows*preview.bands){image->set_matrix({});return;}
        const auto rows=preview_rows();
        if(!image->set_axis_metadata(preview_axes())){image->set_matrix({});return;}
        image->set_y_range(false,floor_db,0,0);image->set_matrix(rows);image_dirty=false;
    }
private:
    SpectrogramAxisMetadata preview_axes()const {
        SpectrogramAxisMetadata axes;axes.age_axis=false;axes.time_coordinates=true;axes.column_coordinates=true;
        axes.time_title_key="Elapsed time, s";axes.column_title_key="Frequency, Hz (Z)";
        axes.value_scale.mode=ValueDisplayScale::Mode::Decibels;axes.value_scale.factor=magnitude?20:10;axes.value_scale.reference=reference;axes.value_scale.floor=floor_db;
        axes.ages=preview.time_seconds;axes.columns=preview.frequency_hz;
        return axes;
    }
    std::vector<std::vector<float>> preview_rows()const {
        std::vector<std::vector<float>> rows(preview.rows,std::vector<float>(preview.bands));
        for(std::size_t row=0;row<preview.rows;++row)std::copy_n(data.begin()+row*preview.bands,preview.bands,rows[row].begin());
        return rows;
    }
    NumericImageView* image;
};
QPushButton* button(const char* title,const char* name,QWidget* parent){auto* b=new QPushButton(parent);ui::bind(b,"text",title);b->setObjectName(name);return b;}
QLabel* label(const char* title,QWidget* parent){auto* l=new QLabel(parent);l->setWordWrap(true);ui::bind(l,"text",title);return l;}
QString cadence_caption(const dsp_kernels::ProfileTraining& training){
    if(!training.interval_numerator)return ui::text("Fixed hop: %1 samples").arg(training.hop);
    return ui::text("Interval: %1/%2 s (fractional sample steps)").arg(qulonglong(training.interval_numerator)).arg(qulonglong(training.interval_denominator));
}
}
namespace { QString profile_read_issue_text(const ProfileReadDiagnostic&); }

struct MusicAnalysisDialog::State {
    QVBoxLayout *root=nullptr,*layout=nullptr;
    QHBoxLayout* header=nullptr;
    QFormLayout *left=nullptr,*right=nullptr;
    QWidget *setup_page=nullptr,*results_page=nullptr,*apply_page=nullptr;
    QFormLayout* curve_form=nullptr;
    QComboBox *aggregation=nullptr,*approximation=nullptr;
    QSpinBox *degree=nullptr,*segments=nullptr;
    QDoubleSpinBox* percentile=nullptr;
    QPushButton *build_curve=nullptr,*save_scan=nullptr,*load_scan=nullptr,*curve_step=nullptr,*apply_step=nullptr;
    QLabel *fit_quality=nullptr,*application_summary=nullptr;
    bool curve_ready=false;
    bool restoring_controls=false;
    std::shared_ptr<const CurveCorpus> corpus;
    std::shared_ptr<const CurveAnalysis> curve_analysis;
    std::future<CurveAnalysis> curve_future;
    QTimer* curve_debounce=nullptr;
    QTimer* curve_poll=nullptr;
    std::uint64_t curve_revision=0,launched_revision=0;
    bool curve_pending=false;
    QComboBox* resample_mode=nullptr;
    QDoubleSpinBox* analysis_rate=nullptr;
    QFormLayout* advanced_form=nullptr;
    QFormLayout* fit_form=nullptr;
    QVBoxLayout* controls_layout=nullptr;
    QCheckBox *auto_axes=nullptr,*log_frequency=nullptr,*corrected_spectrum=nullptr;
    QDoubleSpinBox *axis_minimum=nullptr,*axis_maximum=nullptr;
    QTimer* target_watch=nullptr;
    QFormLayout* quiet_form=nullptr;
    QComboBox* track_units=nullptr;
    QComboBox* channel_policy=nullptr;
    QSpinBox* channel_index=nullptr;
    const dsp_kernels::FrequencyProfile& active_profile()const {
        return curve_analysis && curve_ready?curve_analysis->selected():document.profile;
    }
    Curve* fitted=nullptr;
    Curve* track_peak=nullptr;
    QCheckBox* quiet_track=nullptr;
    QCheckBox* fir=nullptr;
    QComboBox* quiet_level=nullptr;
    Curve* target_gains=nullptr;
    QTabWidget* tabs=nullptr;
    AnalysisServices services;
    std::optional<ProfileReadDiagnostic> profile_load_diagnostic;
    AnalysisOptions baseline_options;
    MusicAnalysisDialog* owner;QListWidget *files,*result_files;QComboBox *rate,*size,*window,*spectrum,*gain_mode;QSpinBox *workers,*hop,*smooth,*preview_rows,*preview_bands;QDoubleSpinBox *minimum,*maximum,*reference,*floor;
    QPushButton *run,*cancel,*save,*load,*add,*folder,*clear,*apply,*reload_targets,*new_scan,*show_results;QLabel *status,*average,*file_count,*result_summary,*preview_title,*dc,*mode_title,*scan_summary,*memory;QStackedWidget* pages;QProgressBar* progress;QTableWidget *statistics;QTableView* frequencies;MusicFrequencyModel* frequency_model;Curve *mean,*gains,*track_mean;Spectrogram* image;QCheckBox* normalized,*compensate;QTimer* timer;
    QComboBox* target;MusicAnalysisDialog::Targets read_targets;MusicAnalysisDialog::Apply apply_profile;std::vector<FrequencyProfileTarget> targets;
    QComboBox* cadence;QLabel* cadence_interval;
    QLabel* frontend_status;
    std::uint64_t interval_numerator=1,interval_denominator=120;
    ProfileDocument document;SpectrogramPreview preview;bool has_profile=false,applied_status=false;
    std::shared_ptr<std::atomic_bool> cancelled=std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_size_t> completed=std::make_shared<std::atomic_size_t>(0);
    std::future<AnalysisResult> future;
    std::future<PreviewResult> preview_future;
    std::shared_ptr<std::atomic_bool> preview_cancel=std::make_shared<std::atomic_bool>(false);
    QTimer* preview_timer=nullptr;QString preview_requested,profile_path;bool preview_pending=false;
    void update_preview(){
        const auto& c=document.profile.compatibility;
        image->preview=preview;image->reference=reference->value();image->floor_db=auto_axes->isChecked()?-120:axis_minimum->value();
        image->magnitude=c.projection.representation==dsp_kernels::SpectrumProjection::Magnitude || c.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude;
        image->data=corrected_preview();image->image_dirty=true;image->update();
        preview_title->setText(preview_pending || preview_future.valid()?ui::text("Building preview: %1").arg(preview_requested):
            preview.rows?ui::text("Preview: %1 (%2 s)").arg(preview.source).arg(preview.duration_seconds,0,'f',2):ui::text("Select a track to view its spectrogram"));
    }
    std::vector<float> corrected_preview() const {
        if(!preview_geometry_valid(preview) || !normalized->isChecked())return preview.values;
        auto frontend=document.profile.compatibility;frontend.sample_rate=preview.sample_rate;
        dsp_kernels::FrequencyProfile adapted;std::string error;
        if(!curve_ready || !dsp_kernels::resample_frequency_profile(active_profile(),frontend,adapted,error))return {};
        return preview.normalized(adapted.gains);
    }
    void stop_preview(){
        preview_cancel->store(true);preview_pending=false;preview_requested.clear();preview_timer->stop();
        if(preview_future.valid()){preview_future.wait();preview_future.get();}
    }
    void launch_preview(){
        if(!services.preview){preview_pending=false;return;}
        preview_pending=false;preview_cancel=std::make_shared<std::atomic_bool>(false);
        AnalysisOptions opt;opt.compatibility=document.profile.compatibility;opt.training=document.profile.training;
        opt.resample_input=baseline_options.resample_input;
        const auto path=preview_requested;auto stop=preview_cancel;
        preview_future=std::async(std::launch::async,[path,opt,stop,service=services.preview]{return service(path,opt,*stop);});
    }
    bool restore_cached_preview(const QString& path){
        for(const auto& cached:document.previews)if(cached.source==path && cached.rows){
            stop_preview();preview=cached;update_preview();return true;
        }
        return false;
    }
    void request_preview(const QString& path){
        if(!has_profile || future.valid() || mode!=Mode::Results || path.isEmpty())return;
        show_track_mean(path);
        if(restore_cached_preview(path))return;
        preview={};update_preview();preview_title->setText(ui::text("No saved spectrogram for this track. A new scan is required."));
    }
    void accept_preview_result(PreviewResult result){
            if(!preview_pending){
                if(result.success)preview=std::move(result.preview);
                else if(!result.cancelled)preview_title->setText(ui::text("Preview failed: %1").arg(result.error));
                if(result.success)update_preview();
            }
    }
    void poll_preview(){
        if(preview_future.valid() && preview_future.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            accept_preview_result(preview_future.get());
        }
        if(!preview_future.valid()){
            if(preview_pending){launch_preview();update_preview();}
            else preview_timer->stop();
        }
    }
    enum class Mode{Setup,Scanning,Results,Application};Mode mode=Mode::Setup;
    explicit State(MusicAnalysisDialog* dialog):owner(dialog){
        baseline_options.training.gain_mode=dsp_kernels::ProfileGainMode::PeakNormalization;
        baseline_options.compatibility.projection.representation=dsp_kernels::SpectrumProjection::PeakAmplitude;
        baseline_options.compatibility.sample_rate=48000;
    }
    void set_mode(Mode value){
        mode=value;const bool busy=mode==Mode::Scanning,results=mode==Mode::Results || mode==Mode::Application;
        show_mode_header(busy,results);
        update_action_availability(busy,results);update_target_availability(results);
        for(QWidget* widget:std::initializer_list<QWidget*>{rate,size,window,spectrum,compensate,workers,hop,fir,resample_mode,channel_policy,files,preview_rows,preview_bands})widget->setEnabled(!busy&&!results);
        channel_index->setEnabled(!busy&&!results&&channel_policy->currentIndex()==1);
        update_curve_availability(busy);
        update_cadence_controls();update_frontend_summary();
        if(mode==Mode::Results && result_files->currentItem())request_preview(result_files->currentItem()->text());
    }
    void update_curve_availability(bool busy){
        const bool measurements=has_profile&&bool(corpus);
        for(QWidget* widget:std::initializer_list<QWidget*>{maximum,floor,aggregation,approximation,degree,segments,percentile})widget->setEnabled(!busy&&measurements);
        quiet_level->setEnabled(!busy&&has_rms_groups());
        fit_form->setRowVisible(degree,approximation->currentIndex()==0);fit_form->setRowVisible(segments,approximation->currentIndex()==1);
        fit_form->setRowVisible(percentile,aggregation->currentIndex()==2);
        percentile->setEnabled(!busy&&measurements&&aggregation->currentIndex()==2);
    }
    void show_mode_header(bool busy,bool results){
        pages->setCurrentIndex(results?1:0);
        mode_title->setText(ui::text(results?"Analysis and normalization":busy?"Scanning":"Music scan"));
    }
    void update_action_availability(bool busy,bool results){
        for(auto* b:{add,folder,clear})b->setEnabled(!busy);run->setEnabled(!busy&&!results);
        load->setEnabled(!busy);save->setEnabled(results&&has_profile);cancel->setEnabled(busy);
        new_scan->setVisible(results);show_results->setVisible(!results&&has_profile);show_results->setEnabled(!busy);
        target->setEnabled(!busy);reload_targets->setEnabled(!busy);
        save->setEnabled(results&&has_profile&&curve_ready);save_scan->setEnabled(!busy&&can_save_scan());
        load_scan->setEnabled(!busy);curve_step->setEnabled(!busy&&has_profile);apply_step->setEnabled(!busy&&curve_ready);
    }
    void update_target_availability(bool results){
        auto* items=qobject_cast<QStandardItemModel*>(target->model());
        bool selected_compatible=false;
        for(std::size_t i=0;i<targets.size();++i){
            const auto reason=results&&has_profile&&curve_ready?frequency_profile_incompatibility(targets[i],active_profile()):QString{};
            if(items)if(auto* item=items->item(int(i))){item->setEnabled(reason.isEmpty());item->setToolTip(reason);}
            if(int(i)==target->currentIndex()){selected_compatible=reason.isEmpty();target->setToolTip(reason);}
        }
        apply->setEnabled(results&&has_profile&&curve_ready&&bool(apply_profile)&&selected_compatible&&target->currentIndex()>=0);
        update_application_summary();
    }
    void update_memory(){
        memory->clear();memory->hide();
    }
    void update_frontend_summary(){
        const auto frontend=options().compatibility;
        const auto channel=frontend.channel_policy==dsp_kernels::ChannelPolicy::Mean?ui::text("Mean channels"):ui::text("Selected channel %1").arg(frontend.channel_index);
        frontend_status->setText(fir->isChecked()?ui::text("Declared frontend: %1; %2 (quality %3)").arg(channel,ui::text(presentation::resampler_caption(frontend.resampler_mode))).arg(frontend.resampler_quality):ui::text("%1; native sample rate per track, no PCM resampling").arg(channel));
    }
    void show_track_mean(const QString& path){
        track_mean->values.clear();track_peak->values.clear();
        for(const auto& track:document.profile.compositions)if(QString::fromStdString(track.source)==path){
            track_mean->fs=track_peak->fs=dsp_kernels::measurement_rate(track,document.profile.compatibility.sample_rate);
            track_mean->values=quiet_track->isChecked()?track.quiet_mean_spectrum:track.mean_spectrum;
            track_peak->values=quiet_track->isChecked()?track.quiet_peak_spectrum:track.peak_spectrum;break;
        }
        convert_track_units();
        update_track_plot(track_mean);update_track_plot(track_peak);
    }
    void convert_track_units(){
        const auto& c=document.profile.compatibility;
        const bool calibrated=c.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude && c.projection.compensate;
        track_units->setEnabled(calibrated);
        if(!calibrated || track_units->currentIndex()==0)return;
        const auto sum=dsp_kernels::profile_window_scaling(c).sum;
        for(auto* plot:{track_mean,track_peak})for(std::size_t i=0;i<plot->values.size();++i)
            plot->values[i]*=sum/(i==0 || i+1==plot->values.size()?1:2);
    }
    void update_track_plot(Curve* plot){
        const auto& c=document.profile.compatibility;apply_display_settings(plot);
        plot->magnitude=c.projection.representation==dsp_kernels::SpectrumProjection::Magnitude || c.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude;plot->update();
    }
    void apply_display_settings(Curve* plot){
        plot->reference=reference->value();plot->automatic=auto_axes->isChecked();
        plot->display_floor=axis_minimum->value();plot->display_ceiling=axis_maximum->value();
    }
    void update_file_count(){file_count->setText(ui::text("Files in list: %1").arg(files->count()));update_memory();}
    void collect_cadence(dsp_kernels::ProfileTraining& training)const {
        training.interval_numerator=cadence->currentIndex()==1?1:0;
        training.interval_denominator=std::uint64_t(analysis_rate->value());
        training.hop=std::size_t(hop->value());
    }
    void update_cadence_controls(){
        const bool editable=mode==Mode::Setup,rational=cadence->currentIndex()==1;
        cadence->setEnabled(editable);hop->setEnabled(editable&&!rational);
        analysis_rate->setEnabled(editable&&rational);
        dsp_kernels::ProfileTraining training;collect_cadence(training);
        cadence_interval->setText(cadence_caption(training));
    }
    AnalysisOptions options()const {
        auto o=baseline_options;
        collect_frontend(o.compatibility);collect_cadence(o.training);o.resample_input=fir->isChecked();
        o.workers=unsigned(workers->value());o.preview_time_bins=std::size_t(preview_rows->value());o.preview_frequency_bins=std::size_t(preview_bands->value());return o;
    }
    void collect_frontend(dsp_kernels::ProfileCompatibility& frontend)const {
        frontend.sample_rate=fir->isChecked()?rate->currentText().toDouble():48000;frontend.fft_size=size->currentText().toUInt();
        frontend.resampler_mode=fir->isChecked()?baseline_resampler():dsp_kernels::ResamplerMode::Bypass;
        frontend.window=dsp_kernels::WindowFunction(window->currentIndex());
        frontend.channel_policy=channel_policy->currentIndex()==0?dsp_kernels::ChannelPolicy::Mean:dsp_kernels::ChannelPolicy::Select;
        frontend.channel_index=std::uint32_t(channel_index->value());frontend.coherent_gain=0;frontend.power_gain=0;
        frontend.projection={dsp_kernels::SpectrumProjection::PeakAmplitude,true};frontend.reference=1;
    }
    void collect_training(dsp_kernels::ProfileTraining& training)const {
        training.gain_mode=gain_mode->currentIndex()==1?dsp_kernels::ProfileGainMode::PeakNormalization:dsp_kernels::ProfileGainMode::MeanEqualization;
        training.smoothing_radius=std::size_t(smooth->value());
        training.min_gain=training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization?1.:minimum->value();
        training.max_gain=maximum->value();
    }
    void set_busy(bool busy){if(busy)profile_load_diagnostic.reset();set_mode(busy?Mode::Scanning:has_profile?Mode::Results:Mode::Setup);}
    void refresh() {
        refresh_result_files();refresh_scan_summary();refresh_curves();update_preview();refresh_statistics();refresh_frequency_statistics();refresh_result_summary();
    }
    void refresh_result_files(){
        QHash<QString,Qt::CheckState> checks;
        for(int i=0;i<result_files->count();++i)checks.insert(result_files->item(i)->text(),result_files->item(i)->checkState());
        {QSignalBlocker block(result_files);const auto selected=result_files->currentItem()?result_files->currentItem()->text():preview.source;result_files->clear();
            fill_result_files(checks);
            const auto match=result_files->findItems(selected,Qt::MatchExactly);if(!match.empty())result_files->setCurrentItem(match.front());else if(result_files->count())result_files->setCurrentRow(0);}
    }
    void fill_result_files(const QHash<QString,Qt::CheckState>& checks){
        for(const auto& track:document.profile.compositions){
            auto* item=new QListWidgetItem(QString::fromStdString(track.source),result_files);
            item->setFlags(item->flags()|Qt::ItemIsUserCheckable);item->setCheckState(checks.value(item->text(),Qt::Checked));
            item->setToolTip(item->text()+"\n"+ui::text("Include track in corpus calculation"));
        }
    }
    void refresh_scan_summary(){
        const auto& p=document.profile;
        if(has_profile && p.compositions.empty()){
            scan_summary->setText(p.curve?ui::text("Ready profile: %1–%2 Hz. Load a scan to change curve construction.").arg(p.curve->minimum_hz).arg(p.curve->maximum_hz):
                ui::text("Legacy profile: %1 Hz, FFT %2; exact frontend required").arg(p.compatibility.sample_rate).arg(p.compatibility.fft_size));return;
        }
        if(has_profile)scan_summary->setText(ui::text("Scan: %1 tracks; native track coordinates; FFT %2; %3; %4").arg(p.compositions.size()).arg(p.compatibility.fft_size).arg(window->itemText(int(p.compatibility.window)),cadence_caption(p.training)));
    }
    void refresh_curves(){
        const auto& p=active_profile();mean->comparisons.clear();gains->comparisons.clear();
        mean->values=p.curve?p.curve->measured_spectrum:p.mean_spectrum;mean->hz=p.curve?p.curve->measurement_hz:std::vector<double>{};
        mean->fs=gains->fs=p.compatibility.sample_rate;mean->reference=reference->value();
        mean->magnitude=gains->magnitude=p.compatibility.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude || p.compatibility.projection.representation==dsp_kernels::SpectrumProjection::Magnitude;gains->limit=p.training.max_gain;
        prepare_gain_data(p);
        gains->comparisons.push_back({ui::text("Unity (0 dB)"),std::vector<double>(gains->values.size(),1),true});
        gains->comparisons.push_back({ui::text("Gain limit"),std::vector<double>(gains->values.size(),p.training.max_gain),true});
        refresh_support_bands();mean->update();gains->update();
    }
    void prepare_gain_data(const dsp_kernels::FrequencyProfile& p){
        if(p.curve){
            gains->hz=p.curve->measurement_hz;gains->values.clear();
            if(gains->hz.empty())for(std::size_t i=0;i<p.gains.size();++i)gains->hz.push_back(i*p.compatibility.sample_rate/p.compatibility.fft_size);
            mean->hz=gains->hz;
            for(const auto hz:gains->hz)gains->values.push_back(dsp_kernels::frequency_curve_gain(p,hz));
            append_fit_lines();
        }else {gains->hz.clear();gains->values.assign(p.gains.begin(),p.gains.end());}
    }
    void append_fit_lines(){
        if(curve_analysis){append_profile_fit(curve_analysis->polynomial);append_profile_fit(curve_analysis->piecewise);}
        else append_profile_fit(active_profile());
        if(corrected_spectrum->isChecked()){
            std::vector<double> values;const auto& p=active_profile();
            for(std::size_t i=0;i<mean->hz.size();++i)values.push_back(mean->values[i]*dsp_kernels::frequency_curve_gain(p,mean->hz[i]));
            mean->comparisons.push_back({ui::text("Calculated corrected spectrum"),std::move(values),true});
        }
    }
    void append_profile_fit(const dsp_kernels::FrequencyProfile& profile){
        if(!profile.curve)return;
        std::vector<double> values;
        for(const auto hz:mean->hz)values.push_back(hz>=profile.curve->minimum_hz && hz<=profile.curve->maximum_hz?
            dsp_kernels::frequency_curve_amplitude(profile,hz):std::numeric_limits<double>::quiet_NaN());
        const auto polynomial=profile.curve->options.approximation==dsp_kernels::CurveApproximation::Polynomial;
        mean->comparisons.push_back({ui::text(polynomial?"Polynomial":"Piecewise smooth"),std::move(values),
            curve_analysis && profile.curve->options.approximation!=curve_analysis->settings.approximation});
    }
    void refresh_support_bands(){
        const auto& p=active_profile();std::vector<NumericSeriesView::Band> missing,limited;
        if(p.curve && !mean->hz.empty()){
            missing={{0,p.curve->minimum_hz,false},{p.curve->maximum_hz,mean->hz.back(),false}};
            for(std::size_t i=1;i<gains->hz.size();++i)if(gains->values[i]>=p.training.max_gain*(1-1e-6) && p.training.max_gain>1)
                limited.push_back({gains->hz[i-1],gains->hz[i],true});
        }
        mean->set_bands(missing);limited.insert(limited.end(),missing.begin(),missing.end());gains->set_bands(std::move(limited));
    }
    void refresh_statistics(){
        const auto& p=document.profile;
        statistics->setRowCount(int(p.compositions.size()));for(std::size_t i=0;i<p.compositions.size();++i){const auto& s=p.compositions[i];
            const auto fs=dsp_kernels::measurement_rate(s,p.compatibility.sample_rate);
            const QStringList row{QString::fromStdString(s.source),QString::number(s.rms,'g',6),QString::number(s.active_frames),QString::number(s.frames),QString::number(double(s.samples)/fs,'f',2),QString::number(s.digital_silence_frames),QString::number(s.quiet_frames),QString::number(s.quiet_rms,'g',6),QString::number(s.minimum_frame_rms,'g',6),QString::number(s.source_sample_rate?s.source_sample_rate:fs),QString::number(fs),QString::number(s.measured_max_hz?s.measured_max_hz:fs/2)};
            for(int k=0;k<row.size();++k)statistics->setItem(int(i),k,new QTableWidgetItem(row[k]));}
    }
    QString frequency_statistics_summary(bool peak,double total) const {
        const auto& p=active_profile();
        if(p.curve && !p.curve->measurement_hz.empty())return native_gain_summary();
        QString summary=p.gains.size()>1?ui::text(peak?"Average observed peak across positive bins: %1; DC is kept separate":"Mean over positive frequency bins: %1; DC is kept separate").arg(total/double(p.gains.size()-1),0,'g',10):QString{};
        if(peak && p.gains.size()>1){
            const auto cap=p.training.max_gain;
            const auto capped=std::count_if(p.gains.begin()+1,p.gains.end(),[cap](float value){
                return cap>1 && double(value)>=cap*(1-1e-6);
            });
            summary+=ui::text(" | Linear gain limit: x%1; bins at limit: %2 / %3")
                .arg(cap,0,'g',9).arg(capped).arg(p.gains.size()-1);
        }
        return summary;
    }
    QString native_gain_summary()const {
        const auto cap=active_profile().training.max_gain;
        const auto capped=std::count_if(gains->values.begin(),gains->values.end(),[cap](double v){return cap>1 && v>=cap*(1-1e-6);});
        return ui::text("Amplitude gain limit: x%1 (+%2 dB); points at limit: %3 / %4")
            .arg(cap,0,'g',8).arg(20*std::log10(cap),0,'f',1).arg(capped).arg(gains->values.size());
    }
    void refresh_frequency_statistics(){
        const auto& p=active_profile();
        const bool peak=p.training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization && p.peak_spectrum.size()==p.gains.size();
        if(curve_analysis && curve_ready)frequency_model->set_profile(std::shared_ptr<const dsp_kernels::FrequencyProfile>(curve_analysis,&curve_analysis->selected()));
        else frequency_model->set_profile(legacy_table_profile());
        double total=0;for(const auto value:peak?p.peak_spectrum:p.mean_spectrum)total+=value;
        average->setText(frequency_statistics_summary(peak,total));
        dc->setText(p.mean_spectrum.empty()?QString{}:ui::text("DC (0 Hz), mean spectrum: %1").arg(p.mean_spectrum[0],0,'g',10));
    }
    std::shared_ptr<const dsp_kernels::FrequencyProfile> legacy_table_profile()const {
        const auto& p=active_profile();auto table=std::make_shared<dsp_kernels::FrequencyProfile>();
        table->compatibility=p.compatibility;table->training=p.training;table->curve=p.curve;
        table->gains=p.gains;table->mean_spectrum=p.mean_spectrum;table->peak_spectrum=p.peak_spectrum;table->frequency_presence=p.frequency_presence;
        return table;
    }
    void refresh_result_summary(){
        const auto& p=document.profile;
        const auto processed=p.compositions.size();const auto active=std::count_if(p.compositions.begin(),p.compositions.end(),[](const auto& track){return track.active_frames>0;});
        const auto run=document.preserved.value("analysis_run").toObject();
        result_summary->setText(!has_profile?QString{}:run.contains("requested_count")?
            ui::text("Processed: %1 / %2; contributed to profile: %3; without active windows: %4; failed: %5").arg(processed).arg(run.value("requested_count").toInt()).arg(active).arg(processed-active).arg(run.value("failed_count").toInt()):
            ui::text("Profile contains %1 files; contributed: %2; without active windows: %3. Original failure count was not recorded.").arg(processed).arg(active).arg(processed-active));
    }
    void show_options(const AnalysisOptions& o){
        restoring_controls=true;baseline_options=o;fir->setChecked(o.resample_input);show_frontend_options(o.compatibility);show_training_options(o.training);update_cadence_controls();update_frontend_summary();restoring_controls=false;
    }
    void show_frontend_options(const dsp_kernels::ProfileCompatibility& c){
        rate->setCurrentText(QString::number(c.sample_rate,'g',12));size->setCurrentText(QString::number(c.fft_size));window->setCurrentIndex(int(c.window));
        spectrum->setCurrentIndex(spectrum->findData(int(c.projection.representation)));compensate->setChecked(c.projection.compensate);floor->setValue(c.floor_db);reference->setValue(c.reference);
        channel_policy->setCurrentIndex(c.channel_policy==dsp_kernels::ChannelPolicy::Mean?0:1);channel_index->setValue(int(c.channel_index));
    }
    void show_training_options(const dsp_kernels::ProfileTraining& training){
        interval_numerator=training.interval_numerator?training.interval_numerator:1;interval_denominator=training.interval_denominator;
        analysis_rate->setValue(double(training.interval_denominator)/double(interval_numerator));
        cadence->setCurrentIndex(training.interval_numerator?1:0);hop->setValue(int(training.hop));
        gain_mode->setCurrentIndex(training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization?1:0);
        smooth->setValue(int(training.smoothing_radius));minimum->setValue(training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization?1.:training.min_gain);maximum->setValue(training.max_gain);
        minimum->setEnabled(mode==Mode::Setup&&gain_mode->currentIndex()==0);
    }
    void restore_source_files(){
        QSignalBlocker block(files);files->clear();QStringList paths;
        for(const auto& track:document.profile.compositions)paths.append(QString::fromStdString(track.source));
        append(paths);
    }
    void restore_preview_resolution(){
        const auto resolution=document.preserved.value("preview_settings").toObject();
        preview_rows->setValue(resolution.value("time_cells").toInt(256));preview_bands->setValue(resolution.value("frequency_cells").toInt(256));
    }
    void show_loaded_status(){
        if(document.profile.curve && document.profile.compositions.empty()){status->setText(ui::text("Ready profile loaded. Load a scan to change curve settings."));return;}
        if(!can_save_scan()){status->setText(ui::text("Legacy profile has incomplete track measurements; apply it on its original grid or scan music again."));return;}
        const bool saved_previews=std::any_of(document.previews.begin(),document.previews.end(),[](const auto& item){return item.rows>0;});
        status->setText(saved_previews?ui::text("Profile loaded"):ui::text("Profile loaded without saved spectrograms. A new scan is required to collect them."));
    }
    void install_profile(ProfileDocument candidate,const QString& path){
        discard_curve_result();
        reset_result_viewports();
        stop_preview();fit_quality->clear();applied_status=false;document=std::move(candidate);has_profile=true;curve_ready=true;preview={};profile_path=path;restore_source_files();
        restore_scan_options();restore_quiet_level();
        restore_preview_resolution();update_file_count();refresh();set_busy(false);show_loaded_status();
        restore_curve_options();
    }
    void discard_curve_result(){
        ++curve_revision;curve_pending=false;curve_analysis.reset();corpus.reset();curve_debounce->stop();
    }
    void reset_result_viewports(){
        for(auto* plot:{mean,gains,track_mean,track_peak,target_gains})plot->presentation_model()->navigate("reset",.5,.5);
    }
    void restore_scan_options(){
        AnalysisOptions opt;opt.compatibility=document.profile.compatibility;opt.training=document.profile.training;
        opt.resample_input=document.preserved.value("scanner").toObject().value("resample_input").toBool(opt.compatibility.resampler_mode!=dsp_kernels::ResamplerMode::Bypass);show_options(opt);
    }
    void select_target(){update_application_summary();}
    void refresh_targets(){
        const auto at=target->currentIndex();
        const auto previous=at>=0&&std::size_t(at)<targets.size()?std::optional{targets[std::size_t(at)]}:std::nullopt;
        QSignalBlocker block(target);target->clear();targets=read_targets?read_targets():std::vector<FrequencyProfileTarget>{};
        for(const auto& t:targets)target->addItem(t.label);
        if(previous)restore_target_route(*previous);
        set_mode(mode);
    }
    void restore_target_route(const FrequencyProfileTarget& previous){
        target->setCurrentIndex(-1);
        for(std::size_t i=0;i<targets.size();++i)if(same_target_route(previous,targets[i])){target->setCurrentIndex(int(i));return;}
    }
    void append(QStringList paths){for(const auto& path:paths)if(files->findItems(path,Qt::MatchExactly).empty()&&files->count()<4096)files->addItem(path);}
    void start() {
        applied_status=false;
        if(future.valid() || mode!=Mode::Setup)return;stop_preview();auto paths=scan_paths();
        if(paths.empty()){status->setText(ui::text("Select audio files first"));return;}
        if(!services.scan_files&&!services.analyze_files){status->setText(ui::text("Analysis service unavailable"));return;}
        ++curve_revision;curve_pending=false;curve_debounce->stop();
        prepare_scan_progress(paths.size());launch_scan(std::move(paths));
    }
    QStringList scan_paths()const {
        QStringList paths;for(int i=0;i<files->count();++i)paths.append(files->item(i)->text());
        // The selected composition receives the single bounded full-track preview.
        if(files->currentRow()>0)paths.move(files->currentRow(),0);
        return paths;
    }
    void prepare_scan_progress(qsizetype count){
        cancelled->store(false);completed->store(0);progress->setRange(0,int(count));progress->setValue(0);set_busy(true);status->setText(ui::text("Analyzing audio files"));
    }
    void launch_scan(QStringList paths){
        const auto opt=options();auto stop=cancelled;auto done=completed;
        future=std::async(std::launch::async,[paths=std::move(paths),opt,stop,done,service=services.scan_files?services.scan_files:services.analyze_files]{return service(paths,opt,*stop,[done](auto n,auto){done->store(n);});});timer->start(50);
    }
    void poll() {
        progress->setValue(int(completed->load()));if(!future.valid()||future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        timer->stop();try{accept_analysis_result(future.get());
        }catch(const std::exception& e){status->setText(QString::fromUtf8(e.what()));}set_mode(has_profile&&document.profile.compositions.size()?Mode::Results:Mode::Setup);
    }
    void accept_analysis_result(AnalysisResult result){
        if(result.success){document=std::move(result.document);preview=std::move(result.preview);has_profile=true;restore_quiet_level();reset_curve_source();refresh();
            status->setText(ui::text("Analysis complete: %1 s audio / %2 s processing (%3×)").arg(result.audio_seconds,0,'f',2).arg(result.wall_seconds,0,'f',3).arg(result.audio_seconds/std::max(.000001,result.wall_seconds),0,'f',1));}
        else status->setText(result.cancelled?ui::text("Analysis cancelled"):ui::text("Analysis failed"));
        if(!result.errors.empty())status->setText(status->text()+"\n"+result.errors.join('\n'));
    }
    void build_shell(){
        auto& s=*this;auto* dialog=owner;
    dialog->setObjectName("musicAnalysisDialog");ui::bind(dialog,"windowTitle","Music frequency profile analyzer");dialog->resize(1400,900);
    root=new QVBoxLayout(dialog);header=new QHBoxLayout;s.mode_title=new QLabel(dialog);s.mode_title->setObjectName("musicMode");header->addWidget(s.mode_title);s.new_scan=button("New scan","musicNewScan",dialog);s.show_results=button("Back to results","musicShowResults",dialog);header->addWidget(s.new_scan);header->addWidget(s.show_results);header->addStretch();root->addLayout(header);
    build_pages();
    }
    void build_pages(){
        auto& s=*this;auto* dialog=owner;
    s.pages=new QStackedWidget(dialog);s.pages->setObjectName("musicPages");setup_page=new QWidget(dialog);results_page=new QWidget(dialog);apply_page=new QWidget(dialog);s.pages->addWidget(setup_page);s.pages->addWidget(results_page);s.pages->addWidget(apply_page);root->addWidget(s.pages,1);

    }
    void build_files(){
        auto& s=*this;auto* dialog=owner;
    layout=new QVBoxLayout(setup_page);layout->setAlignment(Qt::AlignTop);auto* top=new QHBoxLayout;s.add=button("Add audio files","musicAddFiles",dialog);s.folder=button("Add folder","musicAddFolder",dialog);s.clear=button("Clear files","musicClearFiles",dialog);
    top->addWidget(s.add);top->addWidget(s.folder);top->addWidget(s.clear);s.file_count=new QLabel(dialog);s.file_count->setObjectName("musicFileCount");top->addWidget(s.file_count);top->addStretch();layout->addLayout(top);
    build_profile_storage_actions();build_file_list();

    }
    void build_file_list(){
        files=new QListWidget(owner);files->setObjectName("musicFiles");files->setMaximumHeight(100);layout->addWidget(files);
    }
    void build_profile_storage_actions(){
        load=button("Load profile","musicLoadProfile",owner);save=button("Save profile","musicSaveProfile",owner);header->addWidget(load);header->addWidget(save);
        build_stage_navigation();
    }
    void build_fft_size(){
        auto& s=*this;auto* dialog=owner;
    s.size=new QComboBox(dialog);for(std::size_t n=512;n<=dsp_kernels::max_fft_samples;n*=2)s.size->addItem(QString::number(n));s.size->setCurrentText("4096");s.size->setObjectName("musicFftSize");left->addRow(label("FFT size",dialog),s.size);

    }
    void build_analysis_forms(){
        auto& s=*this;auto* dialog=owner;
    auto* settings=new QHBoxLayout;left=new QFormLayout;right=new QFormLayout;left->setRowWrapPolicy(QFormLayout::WrapLongRows);right->setRowWrapPolicy(QFormLayout::WrapLongRows);settings->addLayout(left);settings->addLayout(right);layout->addLayout(settings);
    s.rate=new QComboBox(dialog);s.rate->addItems({"32768","32000","44100","48000"});s.rate->setEditable(true);s.rate->setObjectName("musicSampleRate");right->addRow(label("FIR output sample rate, Hz",dialog),s.rate);
    build_fft_size();
    }
    void build_spectrum(){
        auto& s=*this;auto* dialog=owner;
    s.window=new QComboBox(dialog);s.window->setObjectName("musicWindow");s.window->addItems({"Hann","Hamming","Blackman",ui::text("Rectangular"),ui::text("Bartlett"),"Blackman-Harris",ui::text("Flat top")});left->addRow(label("Window function",dialog),s.window);
    s.spectrum=new QComboBox(dialog);s.spectrum->setObjectName("musicSpectrum");s.spectrum->addItem(ui::text("Peak amplitude"),int(dsp_kernels::SpectrumProjection::PeakAmplitude));s.spectrum->hide();
    s.compensate=new QCheckBox(dialog);s.compensate->setObjectName("musicSpectrumCompensation");s.compensate->setChecked(true);s.compensate->hide();
    left->addRow(label("Calibrated peak amplitude; FFT and window compensation",dialog));

    }
    void build_processing(){
        auto& s=*this;auto* dialog=owner;
    build_advanced_settings();
    s.workers=new QSpinBox(dialog);s.workers->setRange(1,50);s.workers->setValue(int(std::clamp(std::thread::hardware_concurrency(),1u,50u)));s.workers->setObjectName("musicWorkers");advanced_form->addRow(label("Analysis workers",dialog),s.workers);
    build_cadence();
    build_channel_controls();
    frontend_status=new QLabel(dialog);frontend_status->setObjectName("musicFrontendSummary");frontend_status->setWordWrap(true);right->addRow(frontend_status);

    }
    void build_channel_controls(){
        channel_policy=new QComboBox(owner);channel_policy->setObjectName("musicChannelPolicy");channel_policy->addItems({ui::text("Mean channels"),ui::text("Select channel")});advanced_form->addRow(label("Channel policy",owner),channel_policy);
        channel_index=new QSpinBox(owner);channel_index->setObjectName("musicChannelIndex");channel_index->setRange(0,255);advanced_form->addRow(label("Channel index (from zero)",owner),channel_index);
        QObject::connect(channel_policy,&QComboBox::currentIndexChanged,owner,[this]{channel_index->setEnabled(mode==Mode::Setup&&channel_policy->currentIndex()==1);update_frontend_summary();});
        QObject::connect(channel_index,&QSpinBox::valueChanged,owner,[this]{update_frontend_summary();});
    }
    void build_cadence(){
        cadence=new QComboBox(owner);cadence->setObjectName("musicCadenceMode");
        cadence->addItems({ui::text("Fixed sample hop"),ui::text("Declared time interval")});
        advanced_form->addRow(label("Analysis cadence",owner),cadence);
        analysis_rate=make_training_scalar(1,10000,120,0);analysis_rate->setObjectName("musicAnalysisRate");left->addRow(label("Analysis rate, frames/s",owner),analysis_rate);
        QObject::connect(analysis_rate,&QDoubleSpinBox::valueChanged,owner,[this]{update_cadence_controls();});
        build_hop();
        cadence_interval=new QLabel(owner);cadence_interval->setObjectName("musicCadenceInterval");cadence_interval->setWordWrap(true);left->addRow(cadence_interval);
        QObject::connect(cadence,&QComboBox::currentIndexChanged,owner,[this]{update_cadence_controls();});
        QObject::connect(hop,&QSpinBox::valueChanged,owner,[this]{update_cadence_controls();});
    }
    void build_hop(){
        hop=new QSpinBox(owner);hop->setObjectName("musicHop");hop->setRange(1,int(dsp_kernels::max_fft_samples));hop->setValue(273);
        advanced_form->addRow(label("Hop, samples",owner),hop);
    }
    void build_preview_resolution(){
        auto& s=*this;auto* dialog=owner;
    s.preview_rows=new QSpinBox(dialog);s.preview_rows->setRange(16,2048);s.preview_rows->setValue(256);s.preview_rows->setObjectName("musicPreviewRows");advanced_form->addRow(label("Preview time cells",dialog),s.preview_rows);
    s.preview_bands=new QSpinBox(dialog);s.preview_bands->setRange(16,2048);s.preview_bands->setValue(256);s.preview_bands->setObjectName("musicPreviewBands");advanced_form->addRow(label("Preview frequency cells",dialog),s.preview_bands);
    s.memory=new QLabel(dialog);s.memory->setWordWrap(true);layout->addWidget(s.memory);

    }
    QDoubleSpinBox* make_training_scalar(double low,double high,double value,int decimals){
        auto* box=new QDoubleSpinBox(owner);box->setDecimals(decimals);box->setRange(low,high);box->setValue(value);return box;
    }
    void build_gain_mode(){
        gain_mode=new QComboBox(owner);gain_mode->setObjectName("musicGainMode");
        gain_mode->addItems({ui::text("Mean-spectrum equalization"),ui::text("Per-bin peak normalization to 1.0")});
        gain_mode->setCurrentIndex(1);gain_mode->hide();
    }
    void build_gain_limits(){
        minimum=make_training_scalar(.000001,1000000,1,6);minimum->setEnabled(false);
        minimum->hide();
        QObject::connect(gain_mode,&QComboBox::currentIndexChanged,owner,[this]{
            minimum->setValue(gain_mode->currentIndex()==0?.1:1.);
            minimum->setEnabled(has_profile&&gain_mode->currentIndex()==0);invalidate_curve();
        });
        maximum=make_training_scalar(1,1000000,100,6);maximum->setObjectName("musicMaximumGain");curve_form->addRow(label("Maximum linear gain",owner),maximum);
    }
    void build_training(){
        fir=new QCheckBox(owner);fir->setObjectName("musicResampleInput");fir->hide();
        resample_mode=new QComboBox(owner);resample_mode->setObjectName("musicResamplingMode");
        resample_mode->addItems({ui::text("Native sample rate per track"),ui::text("Resample through FIR")});right->insertRow(0,label("PCM sample rate",owner),resample_mode);
        QObject::connect(resample_mode,&QComboBox::currentIndexChanged,owner,[this](int index){fir->setChecked(index==1);right->setRowVisible(rate,index==1);update_frontend_summary();});
        QObject::connect(fir,&QCheckBox::toggled,owner,[this](bool enabled){resample_mode->setCurrentIndex(enabled?1:0);});
        right->setRowVisible(rate,false);
        reference=make_training_scalar(.000001,1000000,1,6);reference->setObjectName("musicDisplayReference");
    }
    void build_advanced_settings(){
        auto* section=new ui::CollapsibleSection("Advanced scan settings",owner);section->setObjectName("musicAdvancedScan");
        section->setExpanded(false);
        advanced_form=new QFormLayout;section->contentLayout()->addLayout(advanced_form);layout->addWidget(section);
    }
    void build_progress(){
        auto& s=*this;auto* dialog=owner;
    auto* controls=new QHBoxLayout;s.run=button("Analyze all listed files","musicRun",dialog);s.cancel=button("Cancel analysis","musicCancel",dialog);controls->addWidget(s.run);controls->addWidget(s.cancel);s.progress=new QProgressBar(dialog);s.progress->setObjectName("musicProgress");s.progress->setFormat(ui::text("%v / %m files (%p%)"));controls->addWidget(s.progress);layout->addLayout(controls);layout->addStretch(1);

    }
    void build_routes(){
        auto& s=*this;auto* dialog=owner;
    layout=new QVBoxLayout(results_page);s.scan_summary=new QLabel(dialog);s.scan_summary->setObjectName("musicScanSummary");s.scan_summary->setWordWrap(true);layout->addWidget(s.scan_summary);
    build_analysis_workspace();
    }
    void build_analysis_workspace(){
        auto* workspace=new QSplitter(owner);workspace->setObjectName("musicWorkspace");layout->addWidget(workspace,1);
        result_files=new QListWidget(owner);result_files->setObjectName("musicResultFiles");result_files->setTextElideMode(Qt::ElideMiddle);result_files->setItemDelegate(new TrackNameDelegate(result_files));workspace->addWidget(result_files);
        auto* charts=new QWidget(owner);workspace->addWidget(charts);auto* chart_layout=new QVBoxLayout(charts);
        build_analysis_controls(workspace);
        layout=chart_layout;workspace->setStretchFactor(1,1);workspace->setSizes({210,850,300});
    }
    void build_analysis_controls(QSplitter* workspace){
        auto* scroll=new QScrollArea(owner);scroll->setWidgetResizable(true);scroll->setMinimumWidth(280);workspace->addWidget(scroll);
        auto* controls=new QWidget(owner);scroll->setWidget(controls);controls_layout=new QVBoxLayout(controls);layout=controls_layout;
        build_curve_controls();build_route_controls();controls_layout->addStretch();
    }
    void build_route_controls(){
        auto* dialog=owner;
        pages->removeWidget(apply_page);layout->addWidget(apply_page);apply_page->show();auto* apply_layout=new QVBoxLayout(apply_page);
        apply_layout->addWidget(label("Application to graph",dialog));target=new QComboBox(dialog);target->setObjectName("musicProfileTarget");target->setMinimumContentsLength(8);target->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);apply_layout->addWidget(target);
        reload_targets=button("Read scheme settings","musicReadScheme",dialog);apply_layout->addWidget(reload_targets);
        apply=button("Apply frequency profile","musicApplyProfile",dialog);apply_layout->addWidget(apply);
        build_application_preview(apply_layout);
    }
    void build_analysis_notes(){
        auto* dialog=owner;
    auto* note=label("Only exact digital silence is excluded. Quiet frames remain in the measurements. Build and compare curves without scanning again; save measurements and ready profiles separately.",dialog);note->setWordWrap(true);layout->addWidget(note);

    }
    void build_plots(){
        auto& s=*this;auto* dialog=owner;
    tabs=new QTabWidget(dialog);s.mean=new Curve(dialog);s.mean->setObjectName("musicMeanCurve");s.gains=new Curve(dialog);s.gains->setObjectName("musicGainCurve");s.gains->gain=true;
    auto* measured_page=build_measurement_plots();
    tabs->addTab(measured_page,ui::text("Corpus"));
    build_preview_tab();
    }
    void build_preview_tab(){
        auto& s=*this;auto* dialog=owner;
    auto* page=new QWidget(dialog);auto* image_layout=new QVBoxLayout(page);s.normalized=new QCheckBox(dialog);ui::bind(s.normalized,"text","Preview correction");ui::bind(s.normalized,"toolTip","Preview correction is approximate: display bands use averaged coefficients. Full-resolution spectrum processing is unchanged.");image_layout->addWidget(s.normalized);build_preview_plot(image_layout);tabs->addTab(page,ui::text("Selected track spectrogram"));
    normalized->setObjectName("musicPreviewCorrection");

    }
    void build_preview_plot(QVBoxLayout* image_layout){
        preview_title=new QLabel(owner);preview_title->setObjectName("musicPreviewTitle");preview_title->setWordWrap(true);preview_title->setTextInteractionFlags(Qt::TextSelectableByMouse);image_layout->addWidget(preview_title);
        image=new Spectrogram(owner);image->setObjectName("musicSpectrogram");image_layout->addWidget(image,1);
    }
    void build_statistics(){
        auto& s=*this;auto* dialog=owner;
    s.statistics=new QTableWidget(0,12,dialog);s.statistics->setObjectName("musicCompositionStatistics");set_statistics_headers();s.statistics->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);s.statistics->setEditTriggers(QAbstractItemView::NoEditTriggers);tabs->addTab(s.statistics,ui::text("Composition statistics"));layout->addWidget(tabs,1);
    s.frequencies=new QTableView(dialog);s.frequencies->setObjectName("musicFrequencyStatistics");frequency_model=new MusicFrequencyModel(s.frequencies);s.frequencies->setModel(frequency_model);s.frequencies->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);s.frequencies->setEditTriggers(QAbstractItemView::NoEditTriggers);tabs->addTab(s.frequencies,ui::text("Frequency statistics"));

    }
    void build_summaries(){
        auto& s=*this;auto* dialog=owner;
    s.dc=new QLabel(dialog);s.dc->setObjectName("musicDcValue");s.dc->setWordWrap(true);layout->addWidget(s.dc);
    build_track_spectra();
    s.average=new QLabel(dialog);s.average->setObjectName("musicMeanValue");s.average->setWordWrap(true);layout->addWidget(s.average);
    s.result_summary=new QLabel(dialog);s.result_summary->setObjectName("musicAnalysisSummary");s.result_summary->setWordWrap(true);layout->addWidget(s.result_summary);
    build_status();
    }
    void build_status(){
        auto& s=*this;auto* dialog=owner;
    s.status=new QLabel(dialog);s.status->setObjectName("musicStatus");s.status->setWordWrap(true);s.status->setTextInteractionFlags(Qt::TextSelectableByMouse);root->addWidget(s.status);s.timer=new QTimer(dialog);s.preview_timer=new QTimer(dialog);s.set_busy(false);s.update_preview();
    target_watch=new QTimer(owner);target_watch->setInterval(500);QObject::connect(target_watch,&QTimer::timeout,owner,[this]{watch_target_grid();});target_watch->start();

    }
    void configure_new_scan(){
        stop_preview();profile_load_diagnostic.reset();applied_status=false;set_mode(State::Mode::Setup);
        progress->setRange(0,std::max(1,files->count()));progress->setValue(0);
        status->setText(ui::text("Configure a new scan; previous results remain available."));
    }
    void build_preview_events(){
        auto& s=*this;auto* dialog=owner;
    QObject::connect(s.preview_timer,&QTimer::timeout,dialog,[&s]{s.poll_preview();});
    QObject::connect(s.result_files,&QListWidget::currentTextChanged,dialog,[&s](const QString& path){s.request_preview(path);});
    QObject::connect(s.new_scan,&QPushButton::clicked,dialog,[&s]{s.configure_new_scan();});
    QObject::connect(s.show_results,&QPushButton::clicked,dialog,[&s]{s.profile_load_diagnostic.reset();s.set_mode(State::Mode::Results);s.status->setText(ui::text("Showing completed scan results"));});
    QObject::connect(s.preview_rows,&QSpinBox::valueChanged,dialog,[&s]{s.update_memory();});QObject::connect(s.preview_bands,&QSpinBox::valueChanged,dialog,[&s]{s.update_memory();});

    }
    void build_selection_events(){
        auto& s=*this;auto* dialog=owner;
    QObject::connect(s.statistics,&QTableWidget::cellClicked,dialog,[&s](int row,int){if(auto* item=s.statistics->item(row,0)){
        const auto matches=s.result_files->findItems(item->text(),Qt::MatchExactly);
        if(!matches.empty() && s.result_files->currentItem()!=matches.front())s.result_files->setCurrentItem(matches.front());else s.request_preview(item->text());
    }});
    QObject::connect(s.result_files,&QListWidget::itemChanged,dialog,[&s]{s.update_corpus_selection();});

    }
    void update_corpus_selection(){
        std::vector<std::uint8_t> included;
        for(int i=0;i<result_files->count();++i)included.push_back(result_files->item(i)->checkState()==Qt::Checked?1:0);
        corpus=curve_corpus(document.profile,included);invalidate_curve();
    }
    void build_file_events(){
        auto& s=*this;auto* dialog=owner;
    QObject::connect(s.files->model(),&QAbstractItemModel::rowsInserted,dialog,[&s]{s.update_file_count();});QObject::connect(s.files->model(),&QAbstractItemModel::rowsRemoved,dialog,[&s]{s.update_file_count();});QObject::connect(s.files->model(),&QAbstractItemModel::modelReset,dialog,[&s]{s.update_file_count();});s.update_file_count();
    QObject::connect(s.add,&QPushButton::clicked,dialog,[&s,dialog]{s.append(QFileDialog::getOpenFileNames(dialog,ui::text("Select audio files"),{},ui::text("Audio files (*.wav *.flac *.mp3)")));});
    QObject::connect(s.folder,&QPushButton::clicked,dialog,[&s,dialog]{auto path=QFileDialog::getExistingDirectory(dialog,ui::text("Select audio folder"));if(!path.isEmpty())s.append(s.services.scan?s.services.scan(path,true):QStringList{});});QObject::connect(s.clear,&QPushButton::clicked,s.files,&QListWidget::clear);

    }
    void update_fft_geometry(const QString& text){
        hop->setMaximum(text.toInt());smooth->setMaximum(text.toInt()/2);update_memory();
    }
    void build_processing_events(){
        auto& s=*this;auto* dialog=owner;
    QObject::connect(s.run,&QPushButton::clicked,dialog,[&s]{s.start();});QObject::connect(s.cancel,&QPushButton::clicked,dialog,[&s]{s.cancelled->store(true);});QObject::connect(s.timer,&QTimer::timeout,dialog,[&s]{s.poll();});QObject::connect(s.normalized,&QCheckBox::toggled,dialog,[&s]{s.update_preview();});
    QObject::connect(s.size,&QComboBox::currentTextChanged,dialog,[&s](const QString& text){s.update_fft_geometry(text);});
    QObject::connect(s.target,&QComboBox::currentIndexChanged,dialog,[&s]{if(s.mode!=State::Mode::Results)s.select_target();else s.set_mode(s.mode);});QObject::connect(s.reload_targets,&QPushButton::clicked,dialog,[&s]{s.refresh_targets();});

    }
    void build_apply_events(){
        QObject::connect(apply,&QPushButton::clicked,owner,[this]{apply_current_profile();});
    }
    void apply_current_profile(){
        refresh_targets();const auto at=target->currentIndex();
        if(at<0 || std::size_t(at)>=targets.size() || !apply_profile || !apply->isEnabled() || !curve_ready)return;
        QString error;profile_load_diagnostic.reset();
        applied_status=apply_profile(targets[std::size_t(at)],active_profile(),error);
        if(!applied_status)status->setText(error.isEmpty()?ui::text("Profile could not be applied"):error);
        else status->setText(ui::text("Profile applied to the scheme; save the project to keep the coefficients"));
    }
    void choose_profile_save(){
        const auto suggested=profile_path.endsWith(".json",Qt::CaseInsensitive)?profile_path.chopped(5)+".brprofile":profile_path;
        QFileDialog chooser(owner,ui::text("Save frequency profile"),suggested,ui::text("Barelief profile (*.brprofile)"));
        chooser.setObjectName("musicProfileFileDialog");
        chooser.setAcceptMode(QFileDialog::AcceptSave);chooser.setDefaultSuffix("brprofile");
        if(chooser.exec()!=QDialog::Accepted || chooser.selectedFiles().isEmpty())return;
        const auto path=chooser.selectedFiles().value(0);QString error;
        if(!owner->save_profile_to(path,error))status->setText(error);
        else {profile_path=path;status->setText(ui::text("Profile saved: %1").arg(path));}
    }
    void choose_profile_load(){
        const auto path=QFileDialog::getOpenFileName(owner,ui::text("Load frequency profile"),{},ui::text("Barelief profiles (*.brprofile *.json)"));
        if(path.isEmpty())return;
        QString error;if(!owner->load_profile_from(path,error))status->setText(error);
    }
    void build_storage_events(){
        QObject::connect(save,&QPushButton::clicked,owner,[this]{choose_profile_save();});
        QObject::connect(load,&QPushButton::clicked,owner,[this]{choose_profile_load();});
        QObject::connect(save_scan,&QPushButton::clicked,owner,[this]{choose_scan_storage(true);});
        QObject::connect(load_scan,&QPushButton::clicked,owner,[this]{choose_scan_storage(false);});
    }
    void build_content(){
    build_shell();
    build_files();
    build_analysis_forms();
    build_spectrum();
    build_processing();
    build_preview_resolution();
    build_training();
    build_progress();
    build_routes();
    build_plots();
    build_statistics();
    build_summaries();
    show_options(baseline_options);
    }
    void build_language_events(){
        QObject::connect(&ui::Language::instance(),&ui::Language::changed,owner,[this]{refresh_language();});
    }
    void refresh_language(){
        auto& s=*this;
        s.update_file_count();s.set_mode(s.mode);s.progress->setFormat(ui::text("%v / %m files (%p%)"));
        refresh_control_labels();
        refresh_plot_labels();refresh_spectrum_labels();refresh_table_labels();refresh();
    }
    void refresh_control_labels(){
        auto& s=*this;
        s.cadence->setItemText(0,ui::text("Fixed sample hop"));s.cadence->setItemText(1,ui::text("Declared time interval"));
        s.gain_mode->setItemText(0,ui::text("Mean-spectrum equalization"));
        s.gain_mode->setItemText(1,ui::text("Per-bin peak normalization to 1.0"));
        for(std::size_t i=0;i<s.targets.size();++i)s.target->setItemText(int(i),frequency_profile_target_caption(s.targets[i]));
        if(s.applied_status)s.status->setText(ui::text("Profile applied to the scheme; save the project to keep the coefficients"));
        if(s.profile_load_diagnostic)s.status->setText(profile_read_issue_text(*s.profile_load_diagnostic));
        refresh_curve_labels();
        refresh_scan_selector_labels();
    }
    void refresh_scan_selector_labels(){
        resample_mode->setItemText(0,ui::text("Native sample rate per track"));resample_mode->setItemText(1,ui::text("Resample through FIR"));
        track_units->setItemText(0,ui::text("Peak amplitude"));track_units->setItemText(1,ui::text("Raw FFT magnitude"));
        channel_policy->setItemText(0,ui::text("Mean channels"));channel_policy->setItemText(1,ui::text("Select channel"));
    }
    void refresh_plot_labels(){
        auto& s=*this;
    s.tabs->setTabText(0,ui::text("Corpus"));s.tabs->setTabText(1,ui::text("Selected track spectrogram"));s.tabs->setTabText(2,ui::text("Composition statistics"));s.tabs->setTabText(3,ui::text("Frequency statistics"));s.tabs->setTabText(4,ui::text("Selected track spectra"));
    }
    void refresh_spectrum_labels(){
        auto& s=*this;
        s.window->setItemText(3,ui::text("Rectangular"));s.window->setItemText(4,ui::text("Bartlett"));s.window->setItemText(6,ui::text("Flat top"));for(int i=0;i<s.spectrum->count();++i)s.spectrum->setItemText(i,ui::text(presentation::spectrum_projection_caption(dsp_kernels::SpectrumProjection(s.spectrum->itemData(i).toInt()))));
    }
    void refresh_table_labels(){
        refresh_frequency_statistics();set_statistics_headers();
    }
    void set_statistics_headers(){
        statistics->setHorizontalHeaderLabels({ui::text("Composition"),"RMS",ui::text("Nonzero windows"),ui::text("Total windows"),ui::text("Duration, s"),ui::text("Digital silence"),ui::text("Quiet windows"),ui::text("Quiet RMS"),ui::text("Minimum frame RMS"),ui::text("Source Fs, Hz"),ui::text("Analysis Fs, Hz"),ui::text("Measured upper frequency, Hz")});
    }
    void build_stage_navigation(){
        auto* storage=new QHBoxLayout;header->removeWidget(load);header->removeWidget(save);storage->addWidget(load);storage->addWidget(save);
        save_scan=button("Save scan","musicSaveScan",owner);load_scan=button("Load scan","musicLoadScan",owner);storage->addWidget(load_scan);storage->addWidget(save_scan);storage->addStretch();root->insertLayout(1,storage);
        curve_step=button("Analysis and normalization","musicCurveStep",owner);apply_step=button("Application to graph","musicApplyStep",owner);curve_step->hide();apply_step->hide();
        QObject::connect(curve_step,&QPushButton::clicked,owner,[this]{set_mode(Mode::Results);});
        QObject::connect(apply_step,&QPushButton::clicked,owner,[this]{set_mode(Mode::Application);refresh_targets();});
    }
    void build_curve_controls(){
        curve_form=new QFormLayout;auto* methods=new QFormLayout;fit_form=methods;methods->setRowWrapPolicy(QFormLayout::WrapLongRows);curve_form->setRowWrapPolicy(QFormLayout::WrapLongRows);layout->addLayout(methods);layout->addLayout(curve_form);
        aggregation=new QComboBox(owner);aggregation->setObjectName("musicAggregation");aggregation->addItems({ui::text("Mean of track maxima"),ui::text("Global maximum"),ui::text("Upper percentile of track maxima")});methods->addRow(label("Track aggregation",owner),aggregation);
        approximation=new QComboBox(owner);approximation->setObjectName("musicApproximation");approximation->addItems({ui::text("Polynomial in Hz"),ui::text("Piecewise smooth in Hz")});approximation->setCurrentIndex(1);methods->addRow(label("Approximation",owner),approximation);
        build_fit_resolution(methods);build_curve_gain_controls();build_curve_actions();
    }
    void build_fit_resolution(QFormLayout* methods){
        degree=new QSpinBox(owner);degree->setObjectName("musicPolynomialDegree");degree->setRange(1,12);degree->setValue(6);methods->addRow(label("Polynomial degree",owner),degree);
        segments=new QSpinBox(owner);segments->setObjectName("musicCurveSegments");segments->setRange(2,2048);segments->setValue(64);methods->addRow(label("Curve segments",owner),segments);
        percentile=make_training_scalar(50,100,90,1);percentile->setObjectName("musicPercentile");methods->addRow(label("Upper percentile, %",owner),percentile);
    }
    void build_curve_gain_controls(){
        build_gain_mode();build_gain_limits();
        smooth=new QSpinBox(owner);smooth->setRange(0,int(dsp_kernels::max_fft_samples/2));smooth->setValue(8);smooth->hide();
        floor=make_training_scalar(-300,0,-120,1);floor->setObjectName("musicRegularizationDb");curve_form->addRow(label("Spectral regularization, dBFS",owner),floor);
    }
    void build_curve_actions(){
        fit_quality=new QLabel(owner);fit_quality->setObjectName("musicFitQuality");fit_quality->setWordWrap(true);layout->addWidget(fit_quality);
        curve_debounce=new QTimer(owner);curve_debounce->setSingleShot(true);curve_debounce->setInterval(150);
        curve_poll=new QTimer(owner);curve_poll->setInterval(20);
        QObject::connect(curve_debounce,&QTimer::timeout,owner,[this]{rebuild_curve();});
        QObject::connect(curve_poll,&QTimer::timeout,owner,[this]{poll_curve();});
        build_curve_invalidation_events();
    }
    dsp_kernels::ResamplerMode baseline_resampler() const {
        const auto mode=baseline_options.compatibility.resampler_mode;
        return mode==dsp_kernels::ResamplerMode::Bypass?dsp_kernels::ResamplerMode::ExistingFir:mode;
    }
    bool has_rms_groups() const {
        return !document.profile.compositions.empty() && std::ranges::all_of(document.profile.compositions,[](const auto& t){return t.rms_statistics_version==1;});
    }
    void build_quiet_diagnostics(){
        quiet_level=new QComboBox(owner);quiet_level->setObjectName("musicQuietLevelDb");
        for(int db=-200;db<=0;db+=2)quiet_level->addItem(QString::number(db),db);
        quiet_level->setCurrentIndex(60);quiet_form->addRow(label("Quiet-frame diagnostic level, dB (2 dB steps)",owner),quiet_level);
        ui::bind(quiet_level,"toolTip","Reclassifies saved RMS groups without reading audio. Does not change the normalization curve. Legacy scans keep their original quiet statistics.");
        QObject::connect(quiet_level,&QComboBox::currentIndexChanged,owner,[this]{if(!restoring_controls)update_quiet_statistics();});
    }
    void update_quiet_statistics(){
        if(!has_rms_groups())return;
        const auto db=quiet_level->currentData().toInt();
        for(auto& t:document.profile.compositions)dsp_kernels::classify_quiet_frames(t,db);
        document.profile.training.silence_rms=std::pow(10.,db/20.);
        auto diagnostics=document.preserved.value("diagnostics").toObject();diagnostics["quiet_db"]=db;document.preserved["diagnostics"]=diagnostics;
        refresh_statistics();show_track_mean(result_files->currentItem()?result_files->currentItem()->text():QString{});
    }
    void restore_quiet_level(){
        QSignalBlocker block(quiet_level);
        if(!has_rms_groups()){quiet_level->setPlaceholderText(ui::text("Saved quiet RMS threshold: %1 (fixed)").arg(document.profile.training.silence_rms));quiet_level->setCurrentIndex(-1);return;}
        quiet_level->setPlaceholderText({});
        const auto db=document.preserved.value("diagnostics").toObject().value("quiet_db").toInt(-80);
        quiet_level->setCurrentIndex(quiet_level->findData(db));update_quiet_statistics();
    }
    void build_curve_invalidation_events(){
        for(auto* box:{aggregation,approximation,gain_mode})QObject::connect(box,&QComboBox::currentIndexChanged,owner,[this]{invalidate_curve();});
        for(auto* box:{maximum,minimum,floor,percentile})QObject::connect(box,&QDoubleSpinBox::valueChanged,owner,[this]{invalidate_curve();});
        for(auto* box:{degree,segments,smooth})QObject::connect(box,&QSpinBox::valueChanged,owner,[this]{invalidate_curve();});
    }
    dsp_kernels::FrequencyCurveOptions curve_options() const {
        dsp_kernels::FrequencyCurveOptions o;o.aggregation=dsp_kernels::PeakAggregation(aggregation->currentIndex());o.approximation=dsp_kernels::CurveApproximation(approximation->currentIndex());
        o.percentile=percentile->value();o.polynomial_degree=unsigned(degree->value());o.segments=std::size_t(segments->value());o.regularization_db=floor->value();o.max_gain=maximum->value();return o;
    }
    void invalidate_curve(){
        if(restoring_controls || !corpus)return;
        ++curve_revision;curve_pending=true;curve_ready=false;applied_status=false;
        fit_quality->setText(ui::text("Updating curve…"));curve_debounce->start();set_mode(mode);
    }
    void rebuild_curve(){
        if(!corpus || future.valid() || curve_future.valid())return;
        curve_pending=false;launched_revision=curve_revision;
        const auto source=corpus;const auto previous=curve_analysis;const auto settings=curve_options();const auto compute=services.curves;
        curve_future=std::async(std::launch::async,[source,previous,settings,compute]{return compute?compute(source,settings,previous):analyze_frequency_curves(source,settings,previous);});
        curve_poll->start();
    }
    void poll_curve(){
        if(!curve_future.valid() || curve_future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        try{
            auto result=curve_future.get();
            if(launched_revision==curve_revision)install_curve(std::move(result));
        }catch(const std::exception& error){fit_quality->setText(QString::fromUtf8(error.what()));}
        curve_poll->stop();
        if(curve_pending && !curve_debounce->isActive())rebuild_curve();
    }
    void install_curve(CurveAnalysis result){
        if(!result.error.empty()){
            curve_ready=false;fit_quality->setText(ui::text(result.error.c_str()));set_mode(mode);return;
        }
        preserve_scanner_settings(result.polynomial);preserve_scanner_settings(result.piecewise);
        curve_analysis=std::make_shared<CurveAnalysis>(std::move(result));curve_ready=true;applied_status=false;
        compare_fits(curve_analysis->settings);refresh_curves();refresh_frequency_statistics();update_preview();set_mode(mode==Mode::Setup?Mode::Setup:Mode::Results);
        status->setText(ui::text("Curve ready; measurements were reused without reading audio"));
    }
    void reset_curve_source(){
        ++curve_revision;curve_analysis.reset();curve_ready=false;
        reset_result_viewports();
        reset_curve_defaults();
        corpus=can_save_scan()?curve_corpus(document.profile):nullptr;
        if(corpus)invalidate_curve();
    }
    void reset_curve_defaults(){
        restoring_controls=true;aggregation->setCurrentIndex(0);approximation->setCurrentIndex(1);
        degree->setValue(6);segments->setValue(64);percentile->setValue(90);floor->setValue(-120);maximum->setValue(100);
        gain_mode->setCurrentIndex(1);restoring_controls=false;
    }
    void preserve_scanner_settings(dsp_kernels::FrequencyProfile& candidate) const {
        const auto& source=document.profile.training;auto& target=candidate.training;
        target.silence_rms=source.silence_rms;target.hop=source.hop;
        target.interval_numerator=source.interval_numerator;target.interval_denominator=source.interval_denominator;
    }
    bool build_selected_curve(dsp_kernels::FrequencyProfile& output,std::string& error){
        const auto& source=document.profile;
        if(gain_mode->currentIndex()==1)return dsp_kernels::build_frequency_curve(source.compatibility,source.compositions,curve_options(),output,error);
        auto training=source.training;collect_training(training);
        return dsp_kernels::build_frequency_profile(source.compatibility,training,source.compositions,output,error);
    }
    void compare_fits(dsp_kernels::FrequencyCurveOptions options){
        const auto& p=active_profile();
        if(!p.curve){fit_quality->setText(ui::text("Legacy profile requires an exact frontend match"));return;}
        const auto* other=curve_analysis?(options.approximation==dsp_kernels::CurveApproximation::Polynomial?&curve_analysis->piecewise:&curve_analysis->polynomial):nullptr;
        fit_quality->setText(ui::text("Fit RMS error: selected %1 dB; alternative %2 dB. Measured range: %3–%4 Hz.").arg(p.curve->fit_rmse_db,0,'f',2).arg(other&&other->curve?QString::number(other->curve->fit_rmse_db,'f',2):ui::text("Unavailable")).arg(p.curve->minimum_hz).arg(p.curve->maximum_hz));
    }
    void refresh_fitted_curve(){
        const auto& p=document.profile;fitted->values.clear();
        if(p.curve){
            for(std::size_t k=0;k<p.gains.size();++k)fitted->values.push_back(dsp_kernels::frequency_curve_amplitude(p,k*p.compatibility.sample_rate/p.compatibility.fft_size));
        }
        fitted->fs=p.compatibility.sample_rate;fitted->reference=p.compatibility.reference;fitted->magnitude=true;fitted->update();
    }
    void restore_curve_options(){
        if(!document.profile.curve)return;
        restoring_controls=true;const auto& o=document.profile.curve->options;
        aggregation->setCurrentIndex(int(o.aggregation));approximation->setCurrentIndex(int(o.approximation));degree->setValue(int(o.polynomial_degree));segments->setValue(int(o.segments));percentile->setValue(o.percentile);maximum->setValue(o.max_gain);floor->setValue(o.regularization_db);
        restoring_controls=false;compare_fits(o);
    }
    void update_application_summary(){
        if(!application_summary)return;
        const auto at=target->currentIndex();
        if(at<0 || std::size_t(at)>=targets.size()){show_target_refusal(ui::text("No declared coefficient route is available in the current graph"));return;}
        const auto& frontend=targets[std::size_t(at)].options.compatibility;
        if(curve_ready){
            const auto reason=frequency_profile_incompatibility(targets[std::size_t(at)],active_profile());
            if(!reason.isEmpty()){show_target_refusal(reason);return;}
        }
        application_summary->setText(active_profile().curve?ui::text("Coefficients will be evaluated at %1 Hz, FFT %2. Frequencies outside the measured range keep gain 1. Audio files are not read.").arg(frontend.sample_rate).arg(frontend.fft_size):ui::text("Legacy profile requires an exact frontend match"));
        update_target_gain_plot(frontend);
    }
    void show_target_refusal(const QString& reason){
        target_gains->values.clear();target_gains->update();application_summary->setText(reason);
    }
    void build_application_preview(QVBoxLayout* apply_layout){
        application_summary=new QLabel(owner);application_summary->setObjectName("musicApplicationSummary");application_summary->setWordWrap(true);apply_layout->addWidget(application_summary);
        apply_layout->addWidget(label("Coefficients for current graph",owner));target_gains=new Curve(owner);target_gains->setObjectName("musicTargetCurve");target_gains->gain=true;apply_layout->addWidget(target_gains,1);
    }
    void update_target_gain_plot(const dsp_kernels::ProfileCompatibility& frontend){
        dsp_kernels::FrequencyProfile adapted;std::string error;target_gains->values.clear();
        if(curve_ready && dsp_kernels::resample_frequency_profile(active_profile(),frontend,adapted,error))target_gains->values.assign(adapted.gains.begin(),adapted.gains.end());
        target_gains->fs=frontend.sample_rate;target_gains->limit=active_profile().training.max_gain;target_gains->magnitude=gains->magnitude;target_gains->update();
    }
    void choose_scan_storage(bool saving){
        QFileDialog chooser(owner,ui::text(saving?"Save scan":"Load scan"),{},ui::text(saving?"Barelief scan (*.brscan)":"Barelief scans (*.brscan *.json)"));
        chooser.setObjectName("musicScanFileDialog");chooser.setAcceptMode(saving?QFileDialog::AcceptSave:QFileDialog::AcceptOpen);
        if(saving)chooser.setDefaultSuffix("brscan");
        if(chooser.exec()!=QDialog::Accepted || chooser.selectedFiles().isEmpty())return;
        const auto path=chooser.selectedFiles().value(0);
        QString error;const auto ok=saving?owner->save_scan_to(path,error):owner->load_scan_from(path,error);
        status->setText(ok?ui::text(saving?"Scan saved":"Scan loaded"):error);
    }
    QWidget* build_measurement_plots(){
        auto* page=new QWidget(owner);auto* charts=new QVBoxLayout(page);build_display_controls(charts);
        auto* plots=new QSplitter(Qt::Vertical,owner);plots->addWidget(mean);plots->addWidget(gains);charts->addWidget(plots,1);
        synchronize_plot_axes(mean,gains);synchronize_plot_axes(gains,mean);return page;
    }
    void synchronize_plot_axes(Curve* from,Curve* to){
        QObject::connect(from->presentation_model(),&QuickPlotModel::navigationRequested,owner,[from,to]{
            const auto view=from->viewport();to->set_horizontal_view(view.left(),view.width());
        });
    }
    void build_display_controls(QVBoxLayout* charts){
        auto* section=new ui::CollapsibleSection("Plot display",owner);section->setExpanded(false);auto* form=new QFormLayout;section->contentLayout()->addLayout(form);charts->addWidget(section);
        build_display_limits(form);build_display_options(form);
    }
    void build_display_limits(QFormLayout* form){
        auto_axes=new QCheckBox(owner);auto_axes->setObjectName("musicAutomaticRange");ui::bind(auto_axes,"text","Automatic spectrum range");auto_axes->setChecked(true);form->addRow(auto_axes);
        axis_minimum=make_training_scalar(-300,0,-120,0);axis_minimum->setObjectName("musicDisplayMinimumDb");form->addRow(label("Display minimum, dB",owner),axis_minimum);
        axis_maximum=make_training_scalar(-299,100,20,0);form->addRow(label("Display maximum, dB",owner),axis_maximum);
        axis_minimum->setEnabled(false);axis_maximum->setEnabled(false);
        form->addRow(label("Spectrum reference",owner),reference);
        QObject::connect(auto_axes,&QCheckBox::toggled,owner,[this]{update_display();});
        for(auto* spin:{axis_minimum,axis_maximum,reference})QObject::connect(spin,&QDoubleSpinBox::valueChanged,owner,[this]{update_display();});
    }
    void build_display_options(QFormLayout* form){
        log_frequency=new QCheckBox(owner);ui::bind(log_frequency,"text","Logarithmic frequency axis");form->addRow(log_frequency);
        corrected_spectrum=new QCheckBox(owner);ui::bind(corrected_spectrum,"text","Show calculated corrected spectrum");form->addRow(corrected_spectrum);
        QObject::connect(corrected_spectrum,&QCheckBox::toggled,owner,[this]{refresh_curves();});
        QObject::connect(log_frequency,&QCheckBox::toggled,owner,[this](bool enabled){mean->set_logarithmic_x(enabled);gains->set_logarithmic_x(enabled);});
        auto* reset=button("Reset plot view","musicResetPlots",owner);form->addRow(reset);
        QObject::connect(reset,&QPushButton::clicked,owner,[this]{mean->presentation_model()->navigate("reset",.5,.5);gains->presentation_model()->navigate("reset",.5,.5);});
    }
    void update_display(){
        if(restoring_controls)return;
        axis_minimum->setEnabled(!auto_axes->isChecked());axis_maximum->setEnabled(!auto_axes->isChecked());
        if(axis_maximum->value()<=axis_minimum->value())return;
        mean->automatic=auto_axes->isChecked();mean->display_floor=axis_minimum->value();mean->display_ceiling=axis_maximum->value();
        refresh_curves();update_preview();show_track_mean(result_files->currentItem()?result_files->currentItem()->text():QString{});
    }
    void watch_target_grid(){
        if(!read_targets || mode==Mode::Scanning)return;
        const auto current=read_targets();bool changed=current.size()!=targets.size();
        for(std::size_t i=0;!changed && i<current.size();++i){
            const auto& a=current[i];const auto& b=targets[i];
            changed=a.label!=b.label || !same_target_route(a,b) || a.route.frontend!=b.route.frontend || a.route.training!=b.route.training ||
                a.options.compatibility.sample_rate!=b.options.compatibility.sample_rate ||
                a.options.compatibility.fft_size!=b.options.compatibility.fft_size ||
                a.options.compatibility.projection!=b.options.compatibility.projection ||
                a.options.compatibility.window!=b.options.compatibility.window;
        }
        if(changed){applied_status=false;refresh_targets();}
    }
    void build_track_spectra(){
        auto* page=new QWidget(owner);auto* content=new QVBoxLayout(page);auto* plots=new QHBoxLayout;
        quiet_form=new QFormLayout;quiet_form->setRowWrapPolicy(QFormLayout::WrapLongRows);content->addLayout(quiet_form);build_quiet_diagnostics();
        build_track_units();
        quiet_track=new QCheckBox(owner);quiet_track->setObjectName("musicQuietTrackSpectra");ui::bind(quiet_track,"text","Quiet frames only");content->addWidget(quiet_track);
        track_mean=make_track_plot(plots,"Mean amplitude","musicTrackMean");track_peak=make_track_plot(plots,"Maximum amplitude","musicTrackPeak");content->addLayout(plots,1);
        tabs->addTab(page,ui::text("Selected track spectra"));
        QObject::connect(quiet_track,&QCheckBox::toggled,owner,[this]{show_track_mean(result_files->currentItem()?result_files->currentItem()->text():QString{});});
    }
    void build_track_units(){
        track_units=new QComboBox(owner);track_units->setObjectName("musicTrackUnits");track_units->addItems({ui::text("Peak amplitude"),ui::text("Raw FFT magnitude")});quiet_form->addRow(label("Spectral representation",owner),track_units);
        QObject::connect(track_units,&QComboBox::currentIndexChanged,owner,[this]{show_track_mean(result_files->currentItem()?result_files->currentItem()->text():QString{});});
    }
    Curve* make_track_plot(QHBoxLayout* plots,const char* title,const char* name){
        auto* column=new QVBoxLayout;auto* plot=new Curve(owner);plot->setObjectName(name);
        column->addWidget(label(title,owner));column->addWidget(plot,1);plots->addLayout(column);return plot;
    }
    void refresh_curve_labels(){
        aggregation->setItemText(0,ui::text("Mean of track maxima"));aggregation->setItemText(1,ui::text("Global maximum"));aggregation->setItemText(2,ui::text("Upper percentile of track maxima"));
        approximation->setItemText(0,ui::text("Polynomial in Hz"));approximation->setItemText(1,ui::text("Piecewise smooth in Hz"));
        if(curve_ready && active_profile().curve)compare_fits(active_profile().curve->options);
    }
    void install_scan(ProfileDocument candidate){
        install_profile(std::move(candidate),{});curve_ready=false;
        restoring_controls=true;gain_mode->setCurrentIndex(1);maximum->setValue(100);restoring_controls=false;
        reset_curve_source();set_mode(Mode::Results);
    }
    bool can_save_scan() const {
        const auto bins=document.profile.compatibility.fft_size/2+1;
        return has_profile && !document.profile.compositions.empty() && std::ranges::all_of(document.profile.compositions,[bins](const auto& t){
            return t.mean_spectrum.size()==bins && t.peak_spectrum.size()==bins && ((t.rms_statistics_version==1 && dsp_kernels::valid_rms_bands(t)) || (t.quiet_mean_spectrum.size()==bins && t.quiet_peak_spectrum.size()==bins));
        });
    }

};
MusicAnalysisDialog::MusicAnalysisDialog(QWidget* parent):QDialog(parent),state_(std::make_unique<State>(this)) {
    state_->build_content();
    state_->build_preview_events();
    state_->build_selection_events();
    state_->build_file_events();
    state_->build_processing_events();
    state_->build_apply_events();
    state_->build_storage_events();
    state_->build_language_events();
}

MusicAnalysisDialog::MusicAnalysisDialog(Targets targets,Apply apply,AnalysisServices services,QWidget* parent):MusicAnalysisDialog(parent){state_->services=std::move(services);state_->read_targets=std::move(targets);state_->apply_profile=std::move(apply);state_->refresh_targets();}
MusicAnalysisDialog::~MusicAnalysisDialog(){
    // QListWidget clears its model during QWidget child destruction, after
    // state_ has gone. Disconnect callbacks that borrow State beforehand.
    QObject::disconnect(state_->files->model(),nullptr,this,nullptr);QObject::disconnect(state_->files,nullptr,this,nullptr);QObject::disconnect(state_->result_files,nullptr,this,nullptr);QObject::disconnect(state_->statistics,nullptr,this,nullptr);state_->timer->stop();
    state_->curve_debounce->stop();state_->curve_poll->stop();
    state_->stop_preview();state_->cancelled->store(true);if(state_->future.valid())state_->future.wait();
    if(state_->curve_future.valid())state_->curve_future.wait();
}
bool MusicAnalysisDialog::save_profile_to(const QString& path,QString& error) const {
    state_->profile_load_diagnostic.reset();
    if(!state_->has_profile || !state_->curve_ready || state_->future.valid()){error=ui::text("No completed profile to save");return false;}
    if(!state_->services.save){error=ui::text("Analysis service unavailable");return false;}
    ProfileDocument ready;ready.profile=state_->active_profile();ready.preserved=state_->document.preserved;
    ready.archive_preserved=state_->document.archive_preserved;
    return state_->services.save(path,ready,error);
}
namespace {
QString profile_read_issue_text(const ProfileReadDiagnostic& diagnostic) {
    using I=ProfileReadIssue;using R=domain::ProcessingReason;
    static constexpr std::pair<I,R> reasons[]{
        {I::Encoding,R::ProfileEncoding},{I::FftSize,R::FftSize},{I::SampleRate,R::SampleRate},
        {I::Layout,R::SpectrumLayout},{I::Representation,R::SpectrumRepresentation},{I::Window,R::WindowFunction},
        {I::CoherentGain,R::CoherentGain},{I::PowerGain,R::PowerGain},{I::Frontend,R::Frontend},
        {I::Resampler,R::Resampler},{I::GainCount,R::GainCount},{I::GainValue,R::GainValue}};
    const auto parameter=diagnostic.parameter.toUtf8();
    for(const auto& [issue,reason]:reasons)if(issue==diagnostic.reason)
        return numeric_editor::processing_reason_text(reason,std::string_view(parameter.constData(),std::size_t(parameter.size())));
    return {};
}
}

namespace {
bool read_profile_candidate(const AnalysisServices& services,const QString& path,ProfileDocument& candidate,
                            QString& error,ProfileReadDiagnostic& diagnostic) {
    if(services.load_diagnostic)return services.load_diagnostic(path,candidate,error,diagnostic);
    if(services.load)return services.load(path,candidate,error);
    error=ui::text("Analysis service unavailable");
    return false;
}
}

bool MusicAnalysisDialog::load_profile_from(const QString& path,QString& error) {
    auto& s=*state_;
    if(s.future.valid()){error=ui::text("Analysis is running");return false;}
    s.profile_load_diagnostic.reset();
    ProfileDocument candidate;
    ProfileReadDiagnostic diagnostic;
    if(!read_profile_candidate(s.services,path,candidate,error,diagnostic)) {
        if(diagnostic.reason!=ProfileReadIssue::None) {
            s.profile_load_diagnostic=diagnostic;
            error=profile_read_issue_text(diagnostic);
        }
        return false;
    }
    s.install_profile(std::move(candidate),path);
    return true;
}
bool MusicAnalysisDialog::save_scan_to(const QString& path,QString& error) const {
    if(!state_->has_profile || state_->future.valid()){error=ui::text("No completed scan to save");return false;}
    if(!state_->services.save_scan){error=ui::text("Analysis service unavailable");return false;}
    return state_->services.save_scan(path,state_->document,error);
}
bool MusicAnalysisDialog::load_scan_from(const QString& path,QString& error) {
    auto& s=*state_;
    if(s.future.valid()){error=ui::text("Analysis is running");return false;}
    if(!s.services.load_scan){error=ui::text("Analysis service unavailable");return false;}
    ProfileDocument candidate;if(!s.services.load_scan(path,candidate,error))return false;
    s.install_scan(std::move(candidate));return true;
}
void show_music_analysis(QWidget* parent){auto* dialog=new MusicAnalysisDialog(parent);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->show();}
}
