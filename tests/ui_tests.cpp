#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/settings_widget.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <desktop/numeric_project_document.h>
#include <visualization/ui_text.h>
#include <QTemporaryDir>
#include <QFile>
#include <QAction>
#include <QMenu>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTabWidget>
#include <QDialog>
#include <QTimer>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonArray>
#include <QLineF>
#include <QLineEdit>
#include <QtNodes/GraphicsView>
#include <cstdio>
#include <functional>
#include <stdexcept>
static void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
static bool wait_until(const std::function<bool()>& test){QElapsedTimer timer;timer.start();while(timer.elapsed()<2500){QCoreApplication::processEvents();if(test())return true;QThread::msleep(5);}return test();}
static QByteArray read(const QString& path){QFile file(path);require(file.open(QIODevice::ReadOnly),"Saved fixture unreadable");return file.readAll();}
int main(int argc,char** argv){
    if(qEnvironmentVariable("QT_QPA_PLATFORM")!="offscreen")return 2;
    try{
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary fixture directory missing");const auto path=temporary.filePath("settings.json");
        domain::NumericProcessingGraph graph;graph.graph_id="main";desktop::NumericDraftPresentation presentation;
        presentation.application_settings={{"ui",QJsonObject{{"language","ru"},{"theme","system"},{"autosave_enabled",false},{"pipeline_auto_apply",false},{"show_spectrogram",false},{"show_data_flow",true}}}};
        desktop::NumericProjectDocument source;require(bool(desktop::create_numeric_project(graph,presentation,source)) && bool(desktop::save_numeric_project(source,path)),"Canonical UI fixture creation failed");const auto initialFile=read(path);
        auto encoded=path.toLocal8Bit();char executable[]="ui_tests",option[]="--config";char* arguments[]={executable,option,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(false));MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* candidate=qobject_cast<MainWindow*>(widget))window=candidate;require(window,"Main window missing");window->show();
        auto* settings=window->findChild<SettingsWidget*>();auto* editor=window->findChild<SchemeEditorWidget*>();auto* sections=settings?settings->findChild<QTabWidget*>("settingsSections"):nullptr;auto* language=settings?settings->findChild<QComboBox*>("interfaceLanguage"):nullptr;auto* theme=settings?settings->findChild<QComboBox*>("interfaceTheme"):nullptr;auto* menu=window->findChild<QMenu*>("graphsMenu");require(settings && editor && sections && language && theme && menu,"Current UI controls missing");
        require(sections->count()==6 && sections->tabText(5)==ui::text("Controls"),"Current settings sections not translated");require(!settings->findChild<QCheckBox*>("enableAnalyzer") && !settings->findChild<QComboBox*>("captureSource") && !window->findChild<QCheckBox*>("pipelineAnalyzer"),"Processing controls returned outside canonical graph");
        require(wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 && !app.presenter().graph_request_pending();}),"Continuous startup not acknowledged");const auto initial=app.presenter().client().snapshot();auto* canvas=editor->findChild<QtNodes::GraphicsView*>();require(canvas,"Canvas missing");const auto initialScale=canvas->getScale();const auto initialCenter=canvas->mapToScene(canvas->viewport()->rect().center());
        for(const auto& page:SettingsWidget::available_sections()) {
            const int i=page.section;
            const auto name=i?QString("openSettingsSection%1").arg(i):QString("openSettings");
            auto* action=window->findChild<QAction*>(name);require(action,"Settings action missing");
            action->trigger();require(sections->currentIndex()==i,"Settings action selects incorrect section");
        }
        require(!window->findChild<QAction*>("openSettingsSection3") && !window->findChild<QAction*>("openSettingsSection4"),"Hidden pages have navigation actions");
        auto* automatic=settings->findChild<QCheckBox*>("pipelineAutoApply");auto* fps=settings->findChild<QCheckBox*>("showFPS");auto* autosave=settings->findChild<QCheckBox*>("enableAutosave");require(!automatic && fps && autosave,"Current preferences missing");fps->setChecked(false);require(!app._ctx.file_configs.ui.show_fps,"FPS preference not stored");
        for(const int index:{1,2,0}){theme->setCurrentIndex(index);QCoreApplication::processEvents();require(app._ctx.file_configs.ui.theme==QStringList{"system","dark","light"}[index],"Theme preference not stored");if(index)require((QApplication::palette().color(QPalette::Window).lightness()<128)==(index==1),"Central theme palette not applied");
            QLineEdit fresh;fresh.setEnabled(false);require(fresh.palette().color(QPalette::Disabled,QPalette::Text)==QApplication::palette().color(QPalette::Disabled,QPalette::Text),"New disabled standard control breaks palette inheritance");require(settings->palette().color(QPalette::WindowText)==QApplication::palette().color(QPalette::WindowText),"Settings break inherited theme");}
        auto* sameEditor=editor;language->setCurrentIndex(1);QCoreApplication::processEvents();require(menu->title()=="Display" && sections->tabText(0)=="General" && app._ctx.file_configs.ui.language=="en", "Current menus fail live EN translation");language->setCurrentIndex(0);QCoreApplication::processEvents();require(menu->title()==ui::text("Display") && sections->tabText(0)==ui::text("General") && window->findChild<SchemeEditorWidget*>()==sameEditor,"RU switch recreates editor or misses menu translation");
        auto assign=[&](const char* name,int key,int scan){auto* button=settings->findChild<QPushButton*>(name);require(button,"Camera binding button missing");bool delivered=false;QTimer::singleShot(0,[&]{auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;QKeyEvent event(QEvent::KeyPress,key,Qt::NoModifier,scan,0,0);QApplication::sendEvent(dialog,&event);delivered=true;});QTimer::singleShot(1000,settings,[]{if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});button->click();require(delivered,"App-owned key dialog did not receive event");};
        assign("bind_forward_0",0x0428,23);assign("bind_forward_1",0x041b,37);const auto forward=app._ctx.file_configs.ui.camera_controls["forward"].toArray();require(forward.size()==2 && forward[0].toObject()["scan"].toInt()==23 && settings->findChild<QPushButton*>("bind_forward_0")->text()=="I","Physical key binding identity lost");
        const auto down=app._ctx.file_configs.ui.camera_controls["down"];assign("bind_down_0",Qt::Key_I,23);require(app._ctx.file_configs.ui.camera_controls["down"]==down,"Duplicate physical binding accepted");auto* clear=settings->findChild<QPushButton*>("clear_forward");auto* restore=settings->findChild<QPushButton*>("restoreCameraKeys");require(clear && restore,"Camera binding actions missing");clear->click();require(app._ctx.file_configs.ui.camera_controls["forward"].toArray().size()==1,"Alternative camera binding not cleared");restore->click();require(app._ctx.file_configs.ui.camera_controls==CameraControls::defaults(),"Camera defaults not restored");
        const auto observed=app.presenter().client().snapshot();require(observed.graph==initial.graph && observed.graph_revision==initial.graph_revision,"Theme/language/settings applied DSP graph");require(canvas->getScale()==initialScale && QLineF(initialCenter,canvas->mapToScene(canvas->viewport()->rect().center())).length()<3/initialScale,"Preferences reset editor camera");require(read(path)==initialFile,"Disabled autosave writes user-style changes");
        autosave->setChecked(true);require(wait_until([&]{return read(path)!=initialFile;}),"Enabling autosave does not persist current UI choices");desktop::NumericProjectDocument saved;domain::NumericProcessingGraph savedGraph;require(bool(desktop::load_numeric_project(path,saved,savedGraph)) && savedGraph.nodes.empty(),"UI autosave corrupted canonical draft");
        std::puts("canonical UI: actual settings actions, central themes/disabled inheritance, RU/EN, physical camera bindings, manual mode and autosave PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
