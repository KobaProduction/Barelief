#include <core_app/music_analysis_dialog.h>
#include <barelief/ui_text.h>
#include <barelief/numeric_image_view.h>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTimer>
#include <QStackedWidget>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QFileInfo>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QProgressBar>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QWheelEvent>
#include <QMouseEvent>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>

namespace {
int failure(int code,int line){std::cerr<<"FAIL music dialog check "<<code<<" at line "<<line<<'\n';return code;}
desktop_analysis::offline::AnalysisServices analysis_services(){
    using namespace desktop_analysis::offline;
    return {analyze_music_files,analyze_music_preview,scan_audio_files,save_profile,load_profile};
}
void put16(QByteArray& bytes,unsigned v){bytes.append(char(v));bytes.append(char(v>>8));}
void put32(QByteArray& bytes,unsigned v){put16(bytes,v);put16(bytes,v>>16);}
QString fixture(const QString& directory){constexpr unsigned rate=32768;QByteArray bytes;bytes.append("RIFF");put32(bytes,36+2*rate);bytes.append("WAVEfmt ");put32(bytes,16);put16(bytes,1);put16(bytes,1);put32(bytes,rate);put32(bytes,2*rate);put16(bytes,2);put16(bytes,16);bytes.append("data");put32(bytes,2*rate);
    for(unsigned i=0;i<rate;++i){double sample=0;for(unsigned t=0;t<7;++t)sample+=.03*std::sin(2*std::numbers::pi*(256+128*t)*i/rate);put16(bytes,std::uint16_t(std::int16_t(sample*32767)));}
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
            if(!QFile::exists(uiPath+".json") || (attempt==1&&!confirmed)) {
                std::cerr<<"Save profile dialog attempt="<<attempt<<" submitted="<<submitted<<" confirmed="<<confirmed
                         <<" expected="<<qPrintable(uiPath+".json")<<" suffix_file="<<QFile::exists(uiPath+".json")
                         <<" bare_file="<<QFile::exists(uiPath)<<'\n';
                return failure(19,__LINE__);
            }
        }
return 0;
}

static int rescan_and_cached_preview(core_app::MusicAnalysisDialog& dialog,const QString& profile,QString& error,QFile& exported,const QByteArray& original,QStackedWidget* pages,QComboBox* size,QPushButton* run,QProgressBar* progress,QListWidget* results,QTableWidget* statistics,const auto& waitPreview,const QString& path,const QString& other) {
        auto* frequencies=dialog.findChild<QTableWidget*>("musicFrequencyStatistics");
        if(frequencies->rowCount()!=256 || frequencies->item(0,0)->text()!="1" || frequencies->item(255,0)->text()!="256" || dialog.findChild<QLabel*>("musicDcValue")->text().isEmpty())return failure(14,__LINE__);
        dialog.findChild<QPushButton*>("musicNewScan")->click();
        if(pages->currentIndex()!=0||!size->isEnabled()||!run->isEnabled()||progress->value()!=0)return failure(22,__LINE__);
        dialog.findChild<QComboBox*>("musicSampleRate")->setCurrentText("48000");
        dialog.findChild<QPushButton*>("musicShowResults")->click();
        if(pages->currentIndex()!=1||size->isEnabled()||!dialog.findChild<QLabel*>("musicScanSummary")->text().contains("32768"))return failure(23,__LINE__);
        if(!dialog.load_profile_from(profile,error)||results->count()!=2 || dialog.findChild<QComboBox*>("musicSampleRate")->currentText()!="32768")return failure(15,__LINE__);
        // Reload restores source paths, and statistics rows can select another preview.
        QMetaObject::invokeMethod(statistics,"cellClicked",Q_ARG(int,1),Q_ARG(int,0));
        if(!waitPreview(path))return failure(16,__LINE__);
        // Persisted results must remain viewable when original audio is unavailable.
        if(!QFile::remove(other))return failure(17,__LINE__);
        results->setCurrentRow(0);if(!waitPreview(other))return failure(18,__LINE__);
        if(!dialog.save_profile_to(profile,error)||!exported.open(QIODevice::ReadOnly)||exported.readAll()!=original)return failure(24,__LINE__);exported.close();
return 0;
}

static int navigate_plot(core_app::MusicAnalysisDialog& dialog,QApplication& app,const char* name) {

            auto* plot=dialog.findChild<QWidget*>(name);if(!plot)return failure(30,__LINE__);
            if(QString::fromLatin1(name)=="musicSpectrogram")plot=plot->findChild<NumericImageView*>();
            if(!plot)return failure(30,__LINE__);
            QWidget* page=plot;while(page->parentWidget()&&!qobject_cast<QStackedWidget*>(page->parentWidget()))page=page->parentWidget();
            auto* stack=qobject_cast<QStackedWidget*>(page->parentWidget());if(stack)stack->setCurrentWidget(page);
            auto* quick=qobject_cast<QuickPlotWidget*>(plot);if(!quick)return failure(30,__LINE__);
            app.processEvents();const auto before=quick->viewport();
            quick->presentation_model()->navigate("zoom",.5,.5,.8);app.processEvents();
            if(quick->viewport()==before){std::cerr<<"FAIL navigation did not zoom "<<name<<'\n';return failure(31,__LINE__);}
            quick->presentation_model()->navigate("reset",.5,.5);app.processEvents();
            if(quick->viewport()!=before){std::cerr<<"FAIL navigation did not reset "<<name<<'\n';return failure(32,__LINE__);}return 0;
}

static int plot_navigation(core_app::MusicAnalysisDialog& dialog,QApplication& app) {
        // The plot navigation model changes its viewport and reset restores it,
        // and profile serialization below remains byte-identical.
        for(const auto* name:{"musicMeanCurve","musicGainCurve","musicSpectrogram","musicTrackMean"}) {
            if(const auto result=navigate_plot(dialog,app,name))return result;
        }
return 0;
}

static int initial_scan(core_app::MusicAnalysisDialog& dialog,QApplication& app,const QTemporaryDir& directory,const char* theme,const QString& path,QString& other,QPushButton* run,QPushButton* cancel,QSpinBox* workers,QComboBox* size) {
        if(!run||!cancel||!workers||!size||!run->isEnabled()||cancel->isEnabled()||workers->minimum()!=1||workers->maximum()!=50)return failure(1,__LINE__);
        if(run->palette().color(QPalette::Button)!=app.palette().color(QPalette::Button))return failure(2,__LINE__);
        if(size->findText("16384")<0)return failure(20,__LINE__);workers->setValue(50);
        size->setCurrentText("512");
        other=directory.filePath(QString("other-%1.wav").arg(theme));if(!QFile::copy(path,other))return failure(5,__LINE__);
        auto* files=dialog.findChild<QListWidget*>("musicFiles");files->addItem(path);files->addItem(other);files->setCurrentRow(1);
        if(!dialog.findChild<QLabel*>("musicFileCount")->text().contains('2'))return failure(6,__LINE__);run->click();
        QElapsedTimer timer;timer.start();while(!dialog.findChild<QPushButton*>("musicSaveProfile")->isEnabled()&&timer.elapsed()<10000)app.processEvents();
        if(run->isEnabled()||!dialog.findChild<QPushButton*>("musicSaveProfile")->isEnabled()||size->isEnabled()||workers->isEnabled())return failure(4,__LINE__);
        auto* statistics=dialog.findChild<QTableWidget*>("musicCompositionStatistics");auto* progress=dialog.findChild<QProgressBar*>("musicProgress");
        if(!statistics || statistics->rowCount()!=2 || statistics->item(0,0)->text()!=other || progress->maximum()!=2 || progress->value()!=2)return failure(7,__LINE__);
        if(!dialog.findChild<QLabel*>("musicAnalysisSummary")->text().contains("2 / 2"))return failure(8,__LINE__);
return 0;
}

static bool wait_preview(QApplication& app,QLabel* title,const QString& source) {
    QElapsedTimer timer;timer.start();while(timer.elapsed()<10000){app.processEvents();if(title->text().startsWith("Preview: ")&&title->text().contains(source))return true;}return false;
}
static void cancel_queued_scan(core_app::MusicAnalysisDialog& dialog,QPushButton* run,QPushButton* cancel) {
        dialog.findChild<QPushButton*>("musicNewScan")->click();run->click();cancel->click();dialog.close();
}
static int latest_preview_selection(QListWidget* results,const auto& waitPreview,const QString& path,const QString& other) {
        results->setCurrentRow(1);if(!waitPreview(path))return failure(11,__LINE__);
        // Latest selection wins; cancelled previews never overwrite the latest track.
        results->setCurrentRow(0);results->setCurrentRow(1);results->setCurrentRow(0);
        if(!waitPreview(other))return failure(12,__LINE__);
return 0;
}
static int theme_dialog(QApplication& app,const QTemporaryDir& directory,const QString& path,const char* theme) {

        ui::Theme::instance().set(theme);core_app::MusicAnalysisDialog dialog({}, {},analysis_services());dialog.show();app.processEvents();
        auto* run=dialog.findChild<QPushButton*>("musicRun");auto* cancel=dialog.findChild<QPushButton*>("musicCancel");auto* workers=dialog.findChild<QSpinBox*>("musicWorkers");
        auto* size=dialog.findChild<QComboBox*>("musicFftSize");
        QString other;
        if(const auto result=initial_scan(dialog,app,directory,theme,path,other,run,cancel,workers,size))return result;
        auto* statistics=dialog.findChild<QTableWidget*>("musicCompositionStatistics");auto* progress=dialog.findChild<QProgressBar*>("musicProgress");
        const auto profile=directory.filePath(QString("profile-%1.json").arg(theme));QString error;
        if(!dialog.save_profile_to(profile,error))return failure(9,__LINE__);
        QFile exported(profile);if(!exported.open(QIODevice::ReadOnly))return failure(10,__LINE__);const auto original=exported.readAll();exported.close();
        auto* title=dialog.findChild<QLabel*>("musicPreviewTitle");
        auto* results=dialog.findChild<QListWidget*>("musicResultFiles");
        auto* pages=dialog.findChild<QStackedWidget*>("musicPages");
        if(!results||results->count()!=2||pages->currentIndex()!=1)return failure(21,__LINE__);
        auto waitPreview=[&](const QString& source){return wait_preview(app,title,source);};
        if(const auto result=latest_preview_selection(results,waitPreview,path,other))return result;
        if(const auto result=plot_navigation(dialog,app))return result;
        if(!dialog.save_profile_to(profile,error)||!exported.open(QIODevice::ReadOnly)||exported.readAll()!=original)return failure(13,__LINE__);exported.close();
        if(const auto result=rescan_and_cached_preview(dialog,profile,error,exported,original,pages,size,run,progress,results,statistics,waitPreview,path,other))return result;
        if(const auto result=save_profile_dialog(dialog,directory,theme))return result;
        // Closing an owned dialog while a new analysis is queued must cancel/join safely.
        cancel_queued_scan(dialog,run,cancel);return 0;
}

int main(int argc,char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc,argv);
    ui::Language::instance().set("en");
    QTemporaryDir directory;const auto path=fixture(directory.path());if(path.isEmpty())return failure(3,__LINE__);
    for(const auto* theme:{"light","dark"}) {
        if(const auto result=theme_dialog(app,directory,path,theme))return result;
    }
    std::cout<<"music_analysis_dialog_tests PASS (offscreen widget lifecycle only)\n";
    return 0;
}
