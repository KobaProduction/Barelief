#include <core_app/music_analysis_dialog.h>
#include <core_app/scheme_editor/numeric_node_operations.h>
#include <optional>
#include <utility>
#include <core_app/spectrum_projection_ui.h>
#include <core_app/resampler_presentation.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_image_view.h>
#include <barelief/numeric_plot_limits.h>
#include <QWheelEvent>
#include <QMouseEvent>
#include <barelief/ui_text.h>
#include <offline/music_analysis.h>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QStandardItemModel>
#include <QDoubleSpinBox>
#include <QFileDialog>
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
#include <thread>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

namespace core_app {
using namespace desktop_analysis::offline;
namespace {
bool preview_geometry_valid(const SpectrogramPreview& preview){
    return presentation::NumericPlotLimits::accepts(preview.rows,preview.bands) && preview.time_seconds.size()==preview.rows && preview.frequency_hz.size()==preview.bands && preview.values.size()==preview.rows*preview.bands;
}
class Curve final:public LineChartView {
public:
    std::vector<double> values;double fs=0,reference=1;bool gain=false,magnitude=false;
    explicit Curve(QWidget* parent){setParent(parent);setMinimumHeight(170);set_render_fps(30);}
    void update(){
        ValueDisplayScale scale;scale.mode=gain?ValueDisplayScale::Mode::Linear:ValueDisplayScale::Mode::Decibels;
        scale.factor=magnitude?20:10;scale.reference=reference;
        set_value_scale(scale);
        // Normalizer gains are dimensionless multipliers, never stored in dB.
        // The dB projection is used only when displaying a measured spectrum.
        const auto high=gain && !values.empty()?*std::max_element(values.begin(),values.end()):1.;
        set_range(gain?0.:-120.,gain?std::max(1.2,high*1.08):20.);
        std::vector<float> data(values.begin(),values.end());std::vector<double> x(values.size());
        for(std::size_t i=0;i<x.size();++i)x[i]=double(i)*fs/(2*double(std::max(std::size_t{1},x.size()-1)));
        set_x_coordinates(x);set_axis_titles(ui::text("Frequency, Hz"),ui::text(gain?"Linear gain, x":"Mean spectrum, dB"));
        LineChartView::update(data);
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
QLabel* label(const char* title,QWidget* parent){auto* l=new QLabel(parent);ui::bind(l,"text",title);return l;}
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
    QWidget *setup_page=nullptr,*results_page=nullptr;
    QTabWidget* tabs=nullptr;
    AnalysisServices services;
    std::optional<ProfileReadDiagnostic> profile_load_diagnostic;
    AnalysisOptions baseline_options;
    MusicAnalysisDialog* owner;QListWidget *files,*result_files;QComboBox *rate,*size,*window,*spectrum,*gain_mode;QSpinBox *workers,*hop,*smooth,*preview_rows,*preview_bands;QDoubleSpinBox *silence,*minimum,*maximum,*reference,*floor;
    QPushButton *run,*cancel,*save,*load,*add,*folder,*clear,*apply,*reload_targets,*new_scan,*show_results;QLabel *status,*average,*file_count,*result_summary,*preview_title,*dc,*mode_title,*scan_summary,*memory;QStackedWidget* pages;QProgressBar* progress;QTableWidget *statistics,*frequencies;Curve *mean,*gains,*track_mean;Spectrogram* image;QCheckBox* normalized,*compensate;QTimer* timer;
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
        image->preview=preview;image->reference=c.reference;image->floor_db=c.floor_db;
        image->magnitude=c.projection.representation==dsp_kernels::SpectrumProjection::Magnitude || c.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude;
        image->data=preview_geometry_valid(preview)&&normalized->isChecked()?preview.normalized(document.profile.gains):preview.values;image->image_dirty=true;image->update();
        preview_title->setText(preview_pending || preview_future.valid()?ui::text("Building preview: %1").arg(preview_requested):
            preview.rows?ui::text("Preview: %1 (%2 s)").arg(preview.source).arg(preview.duration_seconds,0,'f',2):ui::text("Select a track to view its spectrogram"));
    }
    void stop_preview(){
        preview_cancel->store(true);preview_pending=false;preview_requested.clear();preview_timer->stop();
        if(preview_future.valid()){preview_future.wait();preview_future.get();}
    }
    void launch_preview(){
        if(!services.preview){preview_pending=false;return;}
        preview_pending=false;preview_cancel=std::make_shared<std::atomic_bool>(false);
        AnalysisOptions opt;opt.compatibility=document.profile.compatibility;opt.training=document.profile.training;
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
        if(!preview_future.valid() && preview.source==path && preview.rows)return;
        preview_requested=path;preview_pending=true;preview_cancel->store(true);preview={};update_preview();
        if(!preview_future.valid())launch_preview();preview_timer->start(30);
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
    enum class Mode{Setup,Scanning,Results};Mode mode=Mode::Setup;
    explicit State(MusicAnalysisDialog* dialog):owner(dialog){
        baseline_options.training.gain_mode=dsp_kernels::ProfileGainMode::PeakNormalization;
        baseline_options.compatibility.projection.representation=dsp_kernels::SpectrumProjection::PeakAmplitude;
    }
    void set_mode(Mode value){
        mode=value;const bool busy=mode==Mode::Scanning,results=mode==Mode::Results;
        pages->setCurrentIndex(results?1:0);
        mode_title->setText(ui::text(results?"Scan results":busy?"Scanning":"Scan settings"));
        update_action_availability(busy,results);update_target_availability(results);
        for(QWidget* widget:std::initializer_list<QWidget*>{rate,size,window,spectrum,compensate,workers,hop,smooth,gain_mode,silence,minimum,maximum,reference,floor,files,preview_rows,preview_bands})widget->setEnabled(!busy&&!results);
        minimum->setEnabled(!busy&&!results&&gain_mode->currentIndex()==0);
        update_cadence_controls();update_frontend_summary();
        if(results && result_files->currentItem())request_preview(result_files->currentItem()->text());
    }
    void update_action_availability(bool busy,bool results){
        for(auto* b:{add,folder,clear})b->setEnabled(!busy);run->setEnabled(!busy&&!results);
        load->setEnabled(!busy);save->setEnabled(results&&has_profile);cancel->setEnabled(busy);
        new_scan->setVisible(results);show_results->setVisible(!results&&has_profile);show_results->setEnabled(!busy);
        target->setEnabled(!busy);reload_targets->setEnabled(!busy);
    }
    void update_target_availability(bool results){
        auto* items=qobject_cast<QStandardItemModel*>(target->model());
        bool selected_compatible=false;
        for(std::size_t i=0;i<targets.size();++i){
            const auto reason=results&&has_profile?frequency_profile_incompatibility(targets[i],document.profile):QString{};
            if(items)if(auto* item=items->item(int(i))){item->setEnabled(reason.isEmpty());item->setToolTip(reason);}
            if(int(i)==target->currentIndex()){selected_compatible=reason.isEmpty();target->setToolTip(reason);}
        }
        apply->setEnabled(results&&has_profile&&bool(apply_profile)&&selected_compatible);
    }
    void update_memory(){
        const auto bytes=double(files->count())*preview_rows->value()*std::min(preview_bands->value(),size->currentText().toInt()/2+1)*4;
        memory->setText(ui::text("Preview storage before compression: up to %1 MiB. FFT statistics retain every frequency bin.").arg(bytes/(1024*1024),0,'f',1));
    }
    void update_frontend_summary(){
        const auto frontend=options().compatibility;
        const auto channel=frontend.channel_policy==dsp_kernels::ChannelPolicy::Mean?ui::text("Mean channels"):ui::text("Selected channel %1").arg(frontend.channel_index);
        frontend_status->setText(ui::text("Declared frontend: %1; %2 (quality %3)").arg(channel,ui::text(presentation::resampler_caption(frontend.resampler_mode))).arg(frontend.resampler_quality));
    }
    void show_track_mean(const QString& path){
        const auto& p=document.profile;track_mean->values.clear();
        for(const auto& track:p.compositions)if(QString::fromStdString(track.source)==path){track_mean->values=track.mean_spectrum;break;}
        track_mean->fs=p.compatibility.sample_rate;track_mean->reference=p.compatibility.reference;
        track_mean->magnitude=p.compatibility.projection.representation==dsp_kernels::SpectrumProjection::Magnitude || p.compatibility.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude;track_mean->update();
    }
    void update_file_count(){file_count->setText(ui::text("Files in list: %1").arg(files->count()));update_memory();}
    void collect_cadence(dsp_kernels::ProfileTraining& training)const {
        training.interval_numerator=cadence->currentIndex()==1?interval_numerator:0;
        training.interval_denominator=interval_denominator;
        training.hop=std::size_t(hop->value());
    }
    void update_cadence_controls(){
        const bool editable=mode==Mode::Setup,rational=cadence->currentIndex()==1;
        cadence->setEnabled(editable);hop->setEnabled(editable&&!rational);
        dsp_kernels::ProfileTraining training;collect_cadence(training);
        cadence_interval->setText(cadence_caption(training));
    }
    AnalysisOptions options()const {
        auto o=baseline_options;
        collect_frontend(o.compatibility);collect_cadence(o.training);collect_training(o.training);
        o.workers=unsigned(workers->value());o.preview_time_bins=std::size_t(preview_rows->value());o.preview_frequency_bins=std::size_t(preview_bands->value());return o;
    }
    void collect_frontend(dsp_kernels::ProfileCompatibility& frontend)const {
        frontend.sample_rate=rate->currentText().toDouble();frontend.fft_size=size->currentText().toUInt();
        frontend.window=dsp_kernels::WindowFunction(window->currentIndex());
        frontend.projection={dsp_kernels::SpectrumProjection(spectrum->currentData().toInt()),compensate->isChecked()};frontend.reference=reference->value();frontend.floor_db=floor->value();
    }
    void collect_training(dsp_kernels::ProfileTraining& training)const {
        training.gain_mode=gain_mode->currentIndex()==1?dsp_kernels::ProfileGainMode::PeakNormalization:dsp_kernels::ProfileGainMode::MeanEqualization;
        training.smoothing_radius=std::size_t(smooth->value());training.silence_rms=silence->value();
        training.min_gain=training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization?1.:minimum->value();
        training.max_gain=maximum->value();
    }
    void set_busy(bool busy){if(busy)profile_load_diagnostic.reset();set_mode(busy?Mode::Scanning:has_profile?Mode::Results:Mode::Setup);}
    void refresh() {
        refresh_result_files();refresh_scan_summary();refresh_curves();update_preview();refresh_statistics();refresh_frequency_statistics();refresh_result_summary();
    }
    void refresh_result_files(){
        const auto& p=document.profile;
        {QSignalBlocker block(result_files);const auto selected=result_files->currentItem()?result_files->currentItem()->text():preview.source;result_files->clear();
            for(const auto& track:p.compositions)result_files->addItem(QString::fromStdString(track.source));
            const auto match=result_files->findItems(selected,Qt::MatchExactly);if(!match.empty())result_files->setCurrentItem(match.front());else if(result_files->count())result_files->setCurrentRow(0);}
    }
    void refresh_scan_summary(){
        const auto& p=document.profile;
        if(has_profile)scan_summary->setText(ui::text("Completed scan: %1 Hz; FFT %2; %3; %4; %5; %6 tracks").arg(p.compatibility.sample_rate,0,'g',10).arg(p.compatibility.fft_size).arg(window->itemText(int(p.compatibility.window)),ui::text(presentation::spectrum_projection_caption(p.compatibility.projection.representation))+" · "+ui::text(presentation::spectrum_compensation_caption)+": "+ui::text(p.compatibility.projection.compensate?"On":"Off"),cadence_caption(p.training)).arg(p.compositions.size()));
    }
    void refresh_curves(){
        const auto& p=document.profile;
        mean->values=p.mean_spectrum;mean->fs=p.compatibility.sample_rate;mean->reference=p.compatibility.reference;mean->magnitude=p.compatibility.projection.representation==dsp_kernels::SpectrumProjection::Magnitude || p.compatibility.projection.representation==dsp_kernels::SpectrumProjection::PeakAmplitude;mean->update();gains->values.assign(p.gains.begin(),p.gains.end());gains->fs=p.compatibility.sample_rate;gains->magnitude=mean->magnitude;gains->update();
    }
    void refresh_statistics(){
        const auto& p=document.profile;
        statistics->setRowCount(int(p.compositions.size()));for(std::size_t i=0;i<p.compositions.size();++i){const auto& s=p.compositions[i];
            const QStringList row{QString::fromStdString(s.source),QString::number(s.rms,'g',6),QString::number(s.active_frames),QString::number(s.frames),QString::number(double(s.samples)/p.compatibility.sample_rate,'f',2)};
            for(int k=0;k<row.size();++k)statistics->setItem(int(i),k,new QTableWidgetItem(row[k]));}
    }
    void refresh_frequency_statistics(){
        const auto& p=document.profile;
        frequencies->setRowCount(int(p.gains.empty()?0:p.gains.size()-1));double total=0;
        const bool peak=p.training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization && p.peak_spectrum.size()==p.gains.size();
        frequencies->horizontalHeaderItem(2)->setText(ui::text(peak?"Peak spectrum":"Mean spectrum"));
        frequencies->horizontalHeaderItem(4)->setText(ui::text(peak?"Corrected peak":"Corrected mean"));
        for(std::size_t k=1;k<p.gains.size();++k){const auto before=peak?p.peak_spectrum[k]:p.mean_spectrum[k];total+=before;const QStringList row{QString::number(k),QString::number(k*p.compatibility.sample_rate/p.compatibility.fft_size,'g',10),QString::number(before,'g',10),QString::number(p.gains[k],'g',9),QString::number(before*p.gains[k],'g',10),p.frequency_presence.size()==p.gains.size()?QString::number(100*p.frequency_presence[k],'f',2):QString{}};
            for(int column=0;column<row.size();++column)frequencies->setItem(int(k-1),column,new QTableWidgetItem(row[column]));}
        QString summary=p.gains.size()>1?ui::text(peak?"Average observed peak across positive bins: %1; DC is kept separate":"Mean over positive frequency bins: %1; DC is kept separate").arg(total/double(p.gains.size()-1),0,'g',10):QString{};
        if(peak && p.gains.size()>1){
            const auto cap=p.training.max_gain;
            const auto capped=std::count_if(p.gains.begin()+1,p.gains.end(),[cap](float value){
                return cap>1 && double(value)>=cap*(1-1e-6);
            });
            summary+=ui::text(" | Linear gain limit: x%1; bins at limit: %2 / %3")
                .arg(cap,0,'g',9).arg(capped).arg(p.gains.size()-1);
        }
        average->setText(summary);
        dc->setText(p.mean_spectrum.empty()?QString{}:ui::text("DC (0 Hz), mean spectrum: %1").arg(p.mean_spectrum[0],0,'g',10));
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
        baseline_options=o;show_frontend_options(o.compatibility);show_training_options(o.training);update_cadence_controls();update_frontend_summary();
    }
    void show_frontend_options(const dsp_kernels::ProfileCompatibility& c){
        rate->setCurrentText(QString::number(c.sample_rate,'g',12));size->setCurrentText(QString::number(c.fft_size));window->setCurrentIndex(int(c.window));
        spectrum->setCurrentIndex(spectrum->findData(int(c.projection.representation)));compensate->setChecked(c.projection.compensate);floor->setValue(c.floor_db);reference->setValue(c.reference);
    }
    void show_training_options(const dsp_kernels::ProfileTraining& training){
        interval_numerator=training.interval_numerator?training.interval_numerator:1;interval_denominator=training.interval_denominator;
        cadence->setCurrentIndex(training.interval_numerator?1:0);hop->setValue(int(training.hop));
        gain_mode->setCurrentIndex(training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization?1:0);
        smooth->setValue(int(training.smoothing_radius));silence->setValue(training.silence_rms);minimum->setValue(training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization?1.:training.min_gain);maximum->setValue(training.max_gain);
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
        const bool saved_previews=std::any_of(document.previews.begin(),document.previews.end(),[](const auto& item){return item.rows>0;});
        status->setText(saved_previews?ui::text("Profile loaded"):ui::text("Profile loaded without saved track previews. A new scan will collect all track results; previews currently require the source audio."));
    }
    void install_profile(ProfileDocument candidate,const QString& path){
        stop_preview();applied_status=false;document=std::move(candidate);has_profile=true;preview={};profile_path=path;restore_source_files();
        AnalysisOptions opt;opt.compatibility=document.profile.compatibility;opt.training=document.profile.training;show_options(opt);
        restore_preview_resolution();update_file_count();refresh();set_busy(false);show_loaded_status();
    }
    void select_target(){const auto at=target->currentIndex();if(at<0 || std::size_t(at)>=targets.size())return;show_options(targets[std::size_t(at)].options);}
    void refresh_targets(){QSignalBlocker block(target);target->clear();targets=read_targets?read_targets():std::vector<FrequencyProfileTarget>{};for(const auto& t:targets)target->addItem(t.label);if(mode!=Mode::Results)select_target();set_mode(mode);}
    void append(QStringList paths){for(const auto& path:paths)if(files->findItems(path,Qt::MatchExactly).empty()&&files->count()<4096)files->addItem(path);}
    void start() {
        applied_status=false;
        if(future.valid() || mode!=Mode::Setup)return;stop_preview();auto paths=scan_paths();
        if(paths.empty()){status->setText(ui::text("Select audio files first"));return;}
        if(!services.analyze_files){status->setText(ui::text("Analysis service unavailable"));return;}
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
        future=std::async(std::launch::async,[paths=std::move(paths),opt,stop,done,service=services.analyze_files]{return service(paths,opt,*stop,[done](auto n,auto){done->store(n);});});timer->start(50);
    }
    void poll() {
        progress->setValue(int(completed->load()));if(!future.valid()||future.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        timer->stop();try{accept_analysis_result(future.get());
        }catch(const std::exception& e){status->setText(QString::fromUtf8(e.what()));}set_mode(has_profile&&document.profile.compositions.size()?Mode::Results:Mode::Setup);
    }
    void accept_analysis_result(AnalysisResult result){
        if(result.success){document=std::move(result.document);preview=std::move(result.preview);has_profile=true;refresh();
            status->setText(ui::text("Analysis complete: %1 s audio / %2 s processing (%3×)").arg(result.audio_seconds,0,'f',2).arg(result.wall_seconds,0,'f',3).arg(result.audio_seconds/std::max(.000001,result.wall_seconds),0,'f',1));}
        else status->setText(result.cancelled?ui::text("Analysis cancelled"):ui::text("Analysis failed"));
        if(!result.errors.empty())status->setText(status->text()+"\n"+result.errors.join('\n'));
    }
    void build_shell(){
        auto& s=*this;auto* dialog=owner;
    dialog->setObjectName("musicAnalysisDialog");ui::bind(dialog,"windowTitle","Music frequency profile analyzer");dialog->resize(1100,820);
    root=new QVBoxLayout(dialog);header=new QHBoxLayout;s.mode_title=new QLabel(dialog);s.mode_title->setObjectName("musicMode");header->addWidget(s.mode_title);s.new_scan=button("New scan","musicNewScan",dialog);s.show_results=button("Back to results","musicShowResults",dialog);header->addWidget(s.new_scan);header->addWidget(s.show_results);header->addStretch();root->addLayout(header);
    build_pages();
    }
    void build_pages(){
        auto& s=*this;auto* dialog=owner;
    s.pages=new QStackedWidget(dialog);s.pages->setObjectName("musicPages");setup_page=new QWidget(dialog);results_page=new QWidget(dialog);s.pages->addWidget(setup_page);s.pages->addWidget(results_page);root->addWidget(s.pages,1);

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
    }
    void build_fft_size(){
        auto& s=*this;auto* dialog=owner;
    s.size=new QComboBox(dialog);for(std::size_t n=512;n<=dsp_kernels::max_fft_samples;n*=2)s.size->addItem(QString::number(n));s.size->setCurrentText("4096");s.size->setObjectName("musicFftSize");left->addRow(label("FFT size",dialog),s.size);

    }
    void build_analysis_forms(){
        auto& s=*this;auto* dialog=owner;
    auto* settings=new QHBoxLayout;left=new QFormLayout;right=new QFormLayout;settings->addLayout(left);settings->addLayout(right);layout->addLayout(settings);
    s.rate=new QComboBox(dialog);s.rate->addItems({"32768","32000","44100","48000"});s.rate->setEditable(true);s.rate->setObjectName("musicSampleRate");left->addRow(label("Analysis sample rate, Hz",dialog),s.rate);
    build_fft_size();
    }
    void build_spectrum(){
        auto& s=*this;auto* dialog=owner;
    s.window=new QComboBox(dialog);s.window->setObjectName("musicWindow");s.window->addItems({"Hann","Hamming","Blackman",ui::text("Rectangular"),ui::text("Bartlett"),"Blackman-Harris",ui::text("Flat top")});left->addRow(label("Window function",dialog),s.window);
    s.spectrum=new QComboBox(dialog);s.spectrum->setObjectName("musicSpectrum");for(const auto value:dsp_kernels::amplitude_spectrum_projections())s.spectrum->addItem(ui::text(presentation::spectrum_projection_caption(value)),int(value));s.spectrum->setCurrentIndex(s.spectrum->findData(int(dsp_kernels::SpectrumProjection::PeakAmplitude)));left->addRow(label("Spectral representation",dialog),s.spectrum);
    s.compensate=new QCheckBox(dialog);s.compensate->setObjectName("musicSpectrumCompensation");s.compensate->setChecked(true);left->addRow(label(presentation::spectrum_compensation_caption,dialog),s.compensate);

    }
    void build_processing(){
        auto& s=*this;auto* dialog=owner;
    s.workers=new QSpinBox(dialog);s.workers->setRange(1,50);s.workers->setValue(int(std::clamp(std::thread::hardware_concurrency(),1u,50u)));s.workers->setObjectName("musicWorkers");left->addRow(label("Analysis workers",dialog),s.workers);
    build_cadence();
    s.smooth=new QSpinBox(dialog);s.smooth->setRange(0,int(dsp_kernels::max_fft_samples/2));s.smooth->setValue(8);right->addRow(label("Smoothing radius, bins",dialog),s.smooth);
    frontend_status=new QLabel(dialog);frontend_status->setObjectName("musicFrontendSummary");frontend_status->setWordWrap(true);right->addRow(frontend_status);

    }
    void build_cadence(){
        cadence=new QComboBox(owner);cadence->setObjectName("musicCadenceMode");
        cadence->addItems({ui::text("Fixed sample hop"),ui::text("Declared time interval")});
        left->addRow(label("Analysis cadence",owner),cadence);
        build_hop();
        cadence_interval=new QLabel(owner);cadence_interval->setObjectName("musicCadenceInterval");cadence_interval->setWordWrap(true);left->addRow(cadence_interval);
        QObject::connect(cadence,&QComboBox::currentIndexChanged,owner,[this]{update_cadence_controls();});
        QObject::connect(hop,&QSpinBox::valueChanged,owner,[this]{update_cadence_controls();});
    }
    void build_hop(){
        hop=new QSpinBox(owner);hop->setObjectName("musicHop");hop->setRange(1,int(dsp_kernels::max_fft_samples));hop->setValue(273);
        left->addRow(label("Hop, samples",owner),hop);
    }
    void build_preview_resolution(){
        auto& s=*this;auto* dialog=owner;
    s.preview_rows=new QSpinBox(dialog);s.preview_rows->setRange(16,2048);s.preview_rows->setValue(256);s.preview_rows->setObjectName("musicPreviewRows");left->addRow(label("Preview time cells",dialog),s.preview_rows);
    s.preview_bands=new QSpinBox(dialog);s.preview_bands->setRange(16,2048);s.preview_bands->setValue(256);s.preview_bands->setObjectName("musicPreviewBands");left->addRow(label("Preview frequency cells",dialog),s.preview_bands);
    s.memory=new QLabel(dialog);s.memory->setWordWrap(true);layout->addWidget(s.memory);

    }
    void build_training(){
        auto& s=*this;auto* dialog=owner;
    auto scalar=[&](double low,double high,double value,int decimals){auto* b=new QDoubleSpinBox(dialog);b->setDecimals(decimals);b->setRange(low,high);b->setValue(value);return b;};
    s.gain_mode=new QComboBox(dialog);s.gain_mode->setObjectName("musicGainMode");
    s.gain_mode->addItems({ui::text("Mean-spectrum equalization"),ui::text("Per-bin peak normalization to 1.0")});
    s.gain_mode->setCurrentIndex(1);right->addRow(label("Gain calculation method",dialog),s.gain_mode);
    s.silence=scalar(0,1,.0001,6);right->addRow(label("Silence threshold, PCM RMS",dialog),s.silence);
    s.minimum=scalar(.000001,1000000,1,6);s.minimum->setEnabled(false);
    right->addRow(label("Minimum linear gain (mean mode)",dialog),s.minimum);
    QObject::connect(s.gain_mode,&QComboBox::currentIndexChanged,dialog,[&s]{
        s.minimum->setValue(s.gain_mode->currentIndex()==0?.1:1.);
        s.minimum->setEnabled(s.mode==State::Mode::Setup&&s.gain_mode->currentIndex()==0);
    });
    s.maximum=scalar(1,1000000,100,6);right->addRow(label("Maximum linear gain",dialog),s.maximum);s.reference=scalar(.000001,1000000,1,6);right->addRow(label("Spectrum reference",dialog),s.reference);s.floor=scalar(-300,-1,-120,1);right->addRow(label("Spectrum floor, dB",dialog),s.floor);

    }
    void build_progress(){
        auto& s=*this;auto* dialog=owner;
    auto* controls=new QHBoxLayout;s.run=button("Analyze all listed files","musicRun",dialog);s.cancel=button("Cancel analysis","musicCancel",dialog);controls->addWidget(s.run);controls->addWidget(s.cancel);s.progress=new QProgressBar(dialog);s.progress->setObjectName("musicProgress");s.progress->setFormat(ui::text("%v / %m files (%p%)"));controls->addWidget(s.progress);layout->addLayout(controls);layout->addStretch(1);

    }
    void build_routes(){
        auto& s=*this;auto* dialog=owner;
    layout=new QVBoxLayout(results_page);s.scan_summary=new QLabel(dialog);s.scan_summary->setObjectName("musicScanSummary");s.scan_summary->setWordWrap(true);layout->addWidget(s.scan_summary);
    s.result_files=new QListWidget(dialog);s.result_files->setObjectName("musicResultFiles");s.result_files->setMaximumHeight(100);layout->addWidget(s.result_files);
    build_route_controls();build_analysis_notes();
    }
    void build_route_controls(){
        auto* dialog=owner;
        auto* route=new QHBoxLayout;route->addWidget(label("Coefficient route in the scheme",dialog));target=new QComboBox(dialog);target->setObjectName("musicProfileTarget");route->addWidget(target,1);reload_targets=button("Read scheme settings","musicReadScheme",dialog);header->addWidget(reload_targets);apply=button("Apply frequency profile","musicApplyProfile",dialog);route->addWidget(apply);layout->addLayout(route);
    }
    void build_analysis_notes(){
        auto* dialog=owner;
    auto* route_note=label("A profile supplies linear coefficients through the declared Fill frontend and the selected array arithmetic input. Phase is unchanged. Decibels only affect chart display. No upstream block changes the declared frontend automatically.",dialog);route_note->setWordWrap(true);layout->addWidget(route_note);
    auto* note=label("Active files contribute equally to spectral equalization. The declared analyzer frontend chooses mean channels or one selected channel and the resampling policy. Standalone scans use the displayed baseline frontend. Preview cells average the declared time and frequency buckets; they do not represent individual FFT bins.",dialog);note->setWordWrap(true);layout->addWidget(note);
    auto* transfer=label("Results use the completed scan settings. Select a track to inspect its saved spectrogram and mean spectrum. New scan opens editable settings for another run. Save profile stores statistics, coefficients and previews; load it to browse without reading audio again.",dialog);transfer->setWordWrap(true);layout->addWidget(transfer);

    }
    void build_plots(){
        auto& s=*this;auto* dialog=owner;
    tabs=new QTabWidget(dialog);s.mean=new Curve(dialog);s.mean->setObjectName("musicMeanCurve");s.gains=new Curve(dialog);s.gains->setObjectName("musicGainCurve");s.gains->gain=true;tabs->addTab(s.mean,ui::text("Mean spectrum of all tracks"));tabs->addTab(s.gains,ui::text("Normalization curve"));
    build_preview_tab();
    }
    void build_preview_tab(){
        auto& s=*this;auto* dialog=owner;
    auto* page=new QWidget(dialog);auto* image_layout=new QVBoxLayout(page);s.normalized=new QCheckBox(dialog);ui::bind(s.normalized,"text","Apply band-averaged profile to preview");ui::bind(s.normalized,"toolTip","Preview correction is approximate: display bands use averaged coefficients. Full-resolution spectrum processing is unchanged.");image_layout->addWidget(s.normalized);build_preview_plot(image_layout);tabs->addTab(page,ui::text("Selected track spectrogram"));
    normalized->setObjectName("musicPreviewCorrection");

    }
    void build_preview_plot(QVBoxLayout* image_layout){
        preview_title=new QLabel(owner);preview_title->setObjectName("musicPreviewTitle");preview_title->setWordWrap(true);preview_title->setTextInteractionFlags(Qt::TextSelectableByMouse);image_layout->addWidget(preview_title);
        image=new Spectrogram(owner);image->setObjectName("musicSpectrogram");image_layout->addWidget(image,1);
    }
    void build_statistics(){
        auto& s=*this;auto* dialog=owner;
    s.statistics=new QTableWidget(0,5,dialog);s.statistics->setObjectName("musicCompositionStatistics");s.statistics->setHorizontalHeaderLabels({ui::text("Composition"),"RMS",ui::text("Active windows"),ui::text("Total windows"),ui::text("Duration, s")});s.statistics->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);s.statistics->setEditTriggers(QAbstractItemView::NoEditTriggers);tabs->addTab(s.statistics,ui::text("Composition statistics"));layout->addWidget(tabs,1);
    s.frequencies=new QTableWidget(0,6,dialog);s.frequencies->setObjectName("musicFrequencyStatistics");s.frequencies->setHorizontalHeaderLabels({ui::text("Bin"),ui::text("Frequency, Hz"),ui::text("Mean spectrum"),ui::text("Linear gain"),ui::text("Corrected mean"),ui::text("Presence, %")});s.frequencies->horizontalHeaderItem(5)->setToolTip(ui::text("Share of active windows above the spectral floor; equal weight per active file"));s.frequencies->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);s.frequencies->setEditTriggers(QAbstractItemView::NoEditTriggers);tabs->addTab(s.frequencies,ui::text("Frequency statistics"));

    }
    void build_summaries(){
        auto& s=*this;auto* dialog=owner;
    s.dc=new QLabel(dialog);s.dc->setObjectName("musicDcValue");layout->addWidget(s.dc);
    s.track_mean=new Curve(dialog);s.track_mean->setObjectName("musicTrackMean");tabs->addTab(s.track_mean,ui::text("Selected track mean spectrum"));
    s.average=new QLabel(dialog);s.average->setObjectName("musicMeanValue");layout->addWidget(s.average);
    s.result_summary=new QLabel(dialog);s.result_summary->setObjectName("musicAnalysisSummary");s.result_summary->setWordWrap(true);layout->addWidget(s.result_summary);
    build_status();
    }
    void build_status(){
        auto& s=*this;auto* dialog=owner;
    s.status=new QLabel(dialog);s.status->setObjectName("musicStatus");s.status->setWordWrap(true);s.status->setTextInteractionFlags(Qt::TextSelectableByMouse);root->addWidget(s.status);s.timer=new QTimer(dialog);s.preview_timer=new QTimer(dialog);s.set_busy(false);s.update_preview();

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
        auto& s=*this;auto* dialog=owner;
    QObject::connect(s.apply,&QPushButton::clicked,dialog,[&s]{const auto at=s.target->currentIndex();if(at<0 || std::size_t(at)>=s.targets.size() || !s.apply_profile)return;QString error;
        s.profile_load_diagnostic.reset();
        s.applied_status=s.apply_profile(s.targets[std::size_t(at)],s.document.profile,error);
        if(!s.applied_status)s.status->setText(error.isEmpty()?ui::text("Profile could not be applied"):error);
        else s.status->setText(ui::text("Profile applied to the scheme; save the project to keep the coefficients"));});

    }
    void choose_profile_save(){
        QFileDialog chooser(owner,ui::text("Save frequency profile"),profile_path,"JSON (*.json)");
        chooser.setAcceptMode(QFileDialog::AcceptSave);chooser.setDefaultSuffix("json");
        if(chooser.exec()!=QDialog::Accepted || chooser.selectedFiles().isEmpty())return;
        const auto path=chooser.selectedFiles().value(0);QString error;
        if(!owner->save_profile_to(path,error))status->setText(error);
        else {profile_path=path;status->setText(ui::text("Profile saved: %1").arg(path));}
    }
    void choose_profile_load(){
        const auto path=QFileDialog::getOpenFileName(owner,ui::text("Load frequency profile"),{},"JSON (*.json)");
        if(path.isEmpty())return;
        QString error;if(!owner->load_profile_from(path,error))status->setText(error);
    }
    void build_storage_events(){
        QObject::connect(save,&QPushButton::clicked,owner,[this]{choose_profile_save();});
        QObject::connect(load,&QPushButton::clicked,owner,[this]{choose_profile_load();});
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
    }
    void refresh_plot_labels(){
        auto& s=*this;
        s.tabs->setTabText(0,ui::text("Mean spectrum of all tracks"));s.tabs->setTabText(1,ui::text("Normalization curve"));s.tabs->setTabText(2,ui::text("Selected track spectrogram"));s.tabs->setTabText(3,ui::text("Composition statistics"));s.tabs->setTabText(4,ui::text("Frequency statistics"));s.tabs->setTabText(5,ui::text("Selected track mean spectrum"));
    }
    void refresh_spectrum_labels(){
        auto& s=*this;
        s.window->setItemText(3,ui::text("Rectangular"));s.window->setItemText(4,ui::text("Bartlett"));s.window->setItemText(6,ui::text("Flat top"));for(int i=0;i<s.spectrum->count();++i)s.spectrum->setItemText(i,ui::text(presentation::spectrum_projection_caption(dsp_kernels::SpectrumProjection(s.spectrum->itemData(i).toInt()))));
    }
    void refresh_table_labels(){
        auto& s=*this;
        s.frequencies->setHorizontalHeaderLabels({ui::text("Bin"),ui::text("Frequency, Hz"),ui::text("Mean spectrum"),ui::text("Linear gain"),ui::text("Corrected mean"),ui::text("Presence, %")});s.frequencies->horizontalHeaderItem(5)->setToolTip(ui::text("Share of active windows above the spectral floor; equal weight per active file"));
        const auto& p=s.document.profile;
        if(s.has_profile){
            const bool peak=p.training.gain_mode==dsp_kernels::ProfileGainMode::PeakNormalization;
            s.frequencies->horizontalHeaderItem(2)->setText(ui::text(peak?"Peak spectrum":"Mean spectrum"));
            s.frequencies->horizontalHeaderItem(4)->setText(ui::text(peak?"Corrected peak":"Corrected mean"));
        }
        s.statistics->setHorizontalHeaderLabels({ui::text("Composition"),"RMS",ui::text("Active windows"),ui::text("Total windows"),ui::text("Duration, s")});
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
    state_->stop_preview();state_->cancelled->store(true);if(state_->future.valid())state_->future.wait();
}
bool MusicAnalysisDialog::save_profile_to(const QString& path,QString& error) const {
    state_->profile_load_diagnostic.reset();
    if(!state_->has_profile || state_->future.valid()){error=ui::text("No completed profile to save");return false;}
    if(!state_->services.save){error=ui::text("Analysis service unavailable");return false;}
    return state_->services.save(path,state_->document,error);
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
void show_music_analysis(QWidget* parent){auto* dialog=new MusicAnalysisDialog(parent);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->show();}
}
