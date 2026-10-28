#include <core_app/music_analysis_dialog.h>
#include <barelief/ui_text.h>
#include <barelief/numeric_image_view.h>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTimer>
#include <QStackedWidget>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableView>
#include <QThread>
#include <barelief/numeric_series_view.h>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QWheelEvent>
#include <QMouseEvent>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>

namespace {
int failure(int code,int line){std::cerr<<"FAIL music dialog check "<<code<<" at line "<<line<<'\n';return code;}
desktop_analysis::offline::AnalysisServices analysis_services(){
    using namespace desktop_analysis::offline;
    return {analyze_music_files,analyze_music_preview,scan_audio_files,save_profile,load_profile,load_profile_diagnostic,scan_music_files,save_music_scan,load_music_scan};
}
void put16(QByteArray& bytes,unsigned v){bytes.append(char(v));bytes.append(char(v>>8));}
void put32(QByteArray& bytes,unsigned v){put16(bytes,v);put16(bytes,v>>16);}
QString fixture(const QString& directory){constexpr unsigned rate=32768;QByteArray bytes;bytes.append("RIFF");put32(bytes,36+2*rate);bytes.append("WAVEfmt ");put32(bytes,16);put16(bytes,1);put16(bytes,1);put32(bytes,rate);put32(bytes,2*rate);put16(bytes,2);put16(bytes,16);bytes.append("data");put32(bytes,2*rate);
    for(unsigned i=0;i<rate;++i){double sample=0;for(unsigned t=0;t<7;++t)sample+=.03*std::sin(2*std::numbers::pi*(256+128*t)*i/rate+.3*std::sin(double(i)/rate+t));put16(bytes,std::uint16_t(std::int16_t(sample*32767)));}
    const auto path=directory+"/synthetic.wav";QFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())return {};return path;}
}

static void drive_save_dialog(QWidget* widget,bool& confirmed,bool& submitted,QElapsedTimer& deadline,const QString& uiPath) {

                    if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->isVisible()){
                        if(auto* yes=box->button(QMessageBox::Yes)){confirmed=true;yes->click();}else box->reject();
                    }else if(auto* chooser=qobject_cast<QFileDialog*>(widget);chooser&&chooser->isVisible()){
                        if(deadline.elapsed()>5000){chooser->reject();return;}
                        if(!submitted){
                            chooser->selectFile(uiPath);
                            // selectFile does not replace an already focused filename editor.
                            auto* filename=chooser->findChild<QLineEdit*>("fileNameEdit");
                            if(!filename)return;
                            filename->setText(QFileInfo(uiPath).fileName());submitted=true;
                            QMetaObject::invokeMethod(chooser,"accept",Qt::QueuedConnection);
                        }
                    }}

static int save_profile_dialog(core_app::MusicAnalysisDialog& dialog,const QTemporaryDir& directory,const char* theme) {
        const auto uiPath=directory.filePath(QString("ui-profile-%1").arg(theme));
        for(int attempt=0;attempt<2;++attempt){
            bool submitted=false,confirmed=false;QElapsedTimer deadline;deadline.start();QTimer driver;
            QObject::connect(&driver,&QTimer::timeout,&driver,[&]{
                const auto topLevels=QApplication::topLevelWidgets();for(auto* widget:topLevels){drive_save_dialog(widget,confirmed,submitted,deadline,uiPath);
                }
            });driver.start(10);dialog.findChild<QPushButton*>("musicSaveProfile")->click();driver.stop();
            if(!QFile::exists(uiPath+".brprofile") || (attempt==1&&!confirmed)) {
                std::cerr<<"Save profile dialog attempt="<<attempt<<" submitted="<<submitted<<" confirmed="<<confirmed
                         <<" expected="<<qPrintable(uiPath+".brprofile")<<" suffix_file="<<QFile::exists(uiPath+".brprofile")
                         <<" bare_file="<<QFile::exists(uiPath)<<'\n';
                return failure(19,__LINE__);
            }
        }
return 0;
}

static bool same_ready_function(const QByteArray& first,const QByteArray& second) {
    const auto a=QJsonDocument::fromJson(first).object(),b=QJsonDocument::fromJson(second).object();
    for(const auto* key:{"function","gains","mean_spectrum","peak_spectrum","frequency_presence","metadata","training"})if(a.value(key)!=b.value(key))return false;
    return true;
}

static bool ready(core_app::MusicAnalysisDialog& dialog) {
    QElapsedTimer deadline;deadline.start();
    while(deadline.elapsed()<15000){QApplication::processEvents();
        if(dialog.findChild<QPushButton*>("musicSaveProfile")->isEnabled())return true;
        QThread::msleep(1);
    }
    return false;
}
static int interactive_methods(core_app::MusicAnalysisDialog& dialog,const unsigned& scans) {
    auto* choice=dialog.findChild<QComboBox*>("musicApproximation");
    if(!choice || choice->count()!=1)return failure(40,__LINE__);
    for(int method=0;method<3;++method){
        dialog.findChild<QComboBox*>("musicAggregation")->setCurrentIndex(method);
        if(!ready(dialog) || scans!=1)return failure(40,__LINE__);
    }
    return 0;
}
static int independent_display(core_app::MusicAnalysisDialog& dialog,const QString& path) {
    QString error;if(!dialog.save_profile_to(path,error))return failure(41,__LINE__);
    desktop_analysis::offline::ProfileDocument before,after;
    if(!desktop_analysis::offline::load_profile(path,before,error))return failure(42,__LINE__);
    auto* automatic=dialog.findChild<QCheckBox*>("musicAutomaticRange");automatic->setChecked(false);
    dialog.findChild<QDoubleSpinBox*>("musicDisplayMinimumDb")->setValue(-180);
    auto* plot=dynamic_cast<NumericSeriesView*>(dialog.findChild<QWidget*>("musicMeanCurve"));
    if(!plot)return failure(43,__LINE__);
    unsigned publications=0;QObject subscription;
    const auto connection=QObject::connect(plot->presentation_model(),&QuickPlotModel::changed,&subscription,[&]{++publications;});
    dialog.findChild<QDoubleSpinBox*>("musicDisplayReference")->setValue(.1);QObject::disconnect(connection);
    if(publications!=1)return failure(47,__LINE__);
    if(!plot || plot->y_range().first!=-180 || !dialog.save_profile_to(path,error) || !desktop_analysis::offline::load_profile(path,after,error))return failure(43,__LINE__);
    if(before.profile.gains!=after.profile.gains || before.profile.curve->coefficients!=after.profile.curve->coefficients)return failure(44,__LINE__);
    const auto viewport=plot->viewport();plot->presentation_model()->navigate("zoom",.5,.5,.8);
    auto* gains=dynamic_cast<NumericSeriesView*>(dialog.findChild<QWidget*>("musicGainCurve"));
    if(plot->viewport()==viewport || gains->viewport().left()!=plot->viewport().left() || gains->viewport().width()!=plot->viewport().width())return failure(45,__LINE__);
    dialog.findChild<QPushButton*>("musicResetPlots")->click();
    if(plot->viewport()!=viewport)return failure(46,__LINE__);
    return 0;
}
static int storage_without_audio(core_app::MusicAnalysisDialog& dialog,const QTemporaryDir& directory,const QString& first,const QString& second,const unsigned& scans) {
    QString error;const auto scan=directory.filePath("reusable.brscan"),profile=directory.filePath("ready.brprofile");
    dialog.findChild<QComboBox*>("musicQuietLevelDb")->setCurrentIndex(91);
    if(!dialog.save_scan_to(scan,error) || !dialog.save_profile_to(profile,error) || !QFile::remove(first) || !QFile::remove(second))return failure(50,__LINE__);
    auto* tracks=dialog.findChild<QListWidget*>("musicResultFiles");tracks->setCurrentRow(1);tracks->setCurrentRow(0);
    if(!dialog.findChild<QLabel*>("musicPreviewTitle")->text().startsWith("Preview: "))return failure(51,__LINE__);
    if(!dialog.load_profile_from(profile,error) || dialog.findChild<QComboBox*>("musicAggregation")->isEnabled())return failure(52,__LINE__);
    if(!dialog.load_scan_from(scan,error) || !ready(dialog) || scans!=1)return failure(53,__LINE__);
    if(dialog.findChild<QComboBox*>("musicQuietLevelDb")->currentData().toInt()!=-18)return failure(54,__LINE__);
    auto* table=dialog.findChild<QTableView*>("musicFrequencyStatistics");
    if(!table || table->model()->rowCount()!=256 || table->model()->data(table->model()->index(0,0)).toString()!="1")return failure(55,__LINE__);
    tracks->item(0)->setCheckState(Qt::Unchecked);
    if(!ready(dialog) || table->model()->data(table->model()->index(50,5)).toString()!="1")return failure(56,__LINE__);
    tracks->item(0)->setCheckState(Qt::Checked);return ready(dialog)?0:failure(57,__LINE__);
}
static int initial_scan_controls(core_app::MusicAnalysisDialog& dialog,QApplication& app){
    auto* run=dialog.findChild<QPushButton*>("musicRun");auto* size=dialog.findChild<QComboBox*>("musicFftSize");
    auto* resampling=dialog.findChild<QComboBox*>("musicResamplingMode");
    if(!run->isEnabled() || resampling->currentIndex()!=0 || size->findText("16384")<0 || run->palette().color(QPalette::Button)!=app.palette().color(QPalette::Button))return failure(1,__LINE__);
    resampling->setCurrentIndex(1);
    if(!dialog.findChild<QLabel*>("musicFrontendSummary")->text().contains("0.40") || !dialog.findChild<QComboBox*>("musicSampleRate")->isVisible())return failure(6,__LINE__);
    resampling->setCurrentIndex(0);
    if(dialog.findChild<QLabel*>("musicFrontendSummary")->text().contains("0.40") || dialog.findChild<QComboBox*>("musicSampleRate")->isVisible())return failure(7,__LINE__);
    return 0;
}
static int theme_dialog(QApplication& app,const QTemporaryDir& directory,const QString& base,const char* theme) {
    ui::Theme::instance().set(theme);auto services=analysis_services();unsigned scans=0;
    services.scan_files=[&](const auto& paths,const auto& options,const auto& cancel,const auto& progress){++scans;return desktop_analysis::offline::scan_music_files(paths,options,cancel,progress);};
    core_app::MusicAnalysisDialog dialog({}, {},std::move(services));dialog.resize(1600,900);dialog.show();app.processEvents();
    auto* run=dialog.findChild<QPushButton*>("musicRun");auto* size=dialog.findChild<QComboBox*>("musicFftSize");
    if(const auto code=initial_scan_controls(dialog,app))return code;
    size->setCurrentText("512");dialog.findChild<QComboBox*>("musicCadenceMode")->setCurrentIndex(0);dialog.findChild<QSpinBox*>("musicHop")->setValue(128);
    const auto first=directory.filePath(QString("first-%1.wav").arg(theme)),second=directory.filePath(QString("second-%1.wav").arg(theme));
    if(!QFile::copy(base,first) || !QFile::copy(base,second))return failure(2,__LINE__);
    auto* files=dialog.findChild<QListWidget*>("musicFiles");files->addItem(first);files->addItem(second);files->setCurrentRow(1);run->click();
    if(!ready(dialog) || scans!=1 || run->isEnabled() || dialog.findChild<QPushButton*>("musicBuildCurve"))return failure(3,__LINE__);
    if(const auto code=interactive_methods(dialog,scans))return code;
    if(const auto code=independent_display(dialog,directory.filePath("display.brprofile")))return code;
    if(const auto code=storage_without_audio(dialog,directory,first,second,scans))return code;
    if(const auto code=save_profile_dialog(dialog,directory,theme))return code;
    dialog.findChild<QPushButton*>("musicNewScan")->click();size->setCurrentText("1024");
    dialog.findChild<QPushButton*>("musicShowResults")->click();
    if(!dialog.findChild<QLabel*>("musicScanSummary")->text().contains("512"))return failure(4,__LINE__);
    dialog.findChild<QPushButton*>("musicNewScan")->click();run->click();dialog.findChild<QPushButton*>("musicCancel")->click();dialog.close();
    return 0;
}

static int large_folder_display_and_start(QApplication& app,const QTemporaryDir& directory) {
    using namespace desktop_analysis::offline;
    const auto input=directory.filePath("large-source");
    if(!QDir().mkpath(input))return failure(61,__LINE__);
    QStringList fake_paths;
    fake_paths.reserve(106574);
    for(int i=0;i<106574;++i)
        fake_paths.append(input+QString("/audio-%1.mp3").arg(i,6,10,QLatin1Char('0')));
    auto services=analysis_services();
    int enumerations=0;
    std::atomic_int analyses=0;
    services.scan=[&](const QString&,bool recursive){
        ++enumerations;
        if(!recursive)return QStringList{};
        return fake_paths;
    };
    services.scan_files=[&](const QStringList&,const AnalysisOptions&,
                             const std::atomic_bool&,const AnalysisServices::Progress&){
        ++analyses;
        AnalysisResult failed;
        failed.errors.append("Intentional test stop");
        return failed;
    };
    core_app::MusicAnalysisDialog dialog({}, {},std::move(services));
    dialog.resize(1600,900);dialog.show();app.processEvents();
    bool file_dialog_seen=false,confirmed=false;
    QTimer chooser_driver;
    QObject::connect(&chooser_driver,&QTimer::timeout,&dialog,[&]{
        for(auto* top:QApplication::topLevelWidgets()) {
            auto* chooser=qobject_cast<QFileDialog*>(top);
            if(!chooser||!chooser->isVisible())continue;
            file_dialog_seen=true;
            if(!confirmed) {
                confirmed=true;
                chooser->setDirectory(input);
                chooser->selectFile(input);
                QMetaObject::invokeMethod(chooser,"accept",Qt::QueuedConnection);
            }
        }
    });
    chooser_driver.start(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&dialog,[&]{
        for(auto* top:QApplication::topLevelWidgets())
            if(auto* chooser=qobject_cast<QFileDialog*>(top);chooser&&chooser->isVisible())chooser->reject();
    });
    deadline.start(4000);
    dialog.findChild<QPushButton*>("musicAddFolder")->click();
    chooser_driver.stop();deadline.stop();app.processEvents();
    if(!file_dialog_seen || !confirmed || enumerations!=1)return failure(62,__LINE__);
    auto* list=dialog.findChild<QListWidget*>("musicFiles");
    auto* count=dialog.findChild<QLabel*>("musicFileCount");
    auto* page=dialog.findChild<QLabel*>("musicFilesPage");
    auto* next=dialog.findChild<QPushButton*>("musicFilesNext");
    auto* prev=dialog.findChild<QPushButton*>("musicFilesPrevious");
    if(!list||!count||!page||!next||!prev ||
       list->count()!=512 || !count->text().contains("106574") ||
       !page->text().contains("512") || !next->isEnabled() ||
       list->item(0)->text()!=fake_paths[0] ||
       list->item(511)->text()!=fake_paths[511])return failure(63,__LINE__);
    next->click();app.processEvents();
    if(list->count()!=512 || list->item(0)->text()!=fake_paths[512] ||
       !prev->isEnabled())return failure(64,__LINE__);
    prev->click();app.processEvents();
    if(list->item(0)->text()!=fake_paths[0])return failure(65,__LINE__);

    int unexpected_dialogs=0;
    QTimer refusal;
    QObject::connect(&refusal,&QTimer::timeout,&dialog,[&]{
        for(auto* top:QApplication::topLevelWidgets())
            if(auto* chooser=qobject_cast<QFileDialog*>(top);chooser&&chooser->isVisible()){
                ++unexpected_dialogs;
                chooser->reject();
            }
    });
    refusal.start(5);
    dialog.findChild<QPushButton*>("musicRun")->click();
    QElapsedTimer elapsed;elapsed.start();
    while(analyses.load()==0 && elapsed.elapsed()<3000){
        app.processEvents();QThread::msleep(1);
    }
    app.processEvents();refusal.stop();
    if(unexpected_dialogs!=0 || analyses.load()!=1)return failure(66,__LINE__);
    dialog.close();
    return 0;
}
int main(int argc,char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);QApplication app(argc,argv);ui::Language::instance().set("en");
    QTemporaryDir directory;const auto path=fixture(directory.path());if(path.isEmpty())return failure(5,__LINE__);
    for(const auto* theme:{"light","dark"})if(const auto result=theme_dialog(app,directory,path,theme))return result;
    if(const auto result=large_folder_display_and_start(app,directory))return result;
    std::cout<<"music_analysis_dialog_tests PASS (offscreen widget lifecycle only)\n";
}
