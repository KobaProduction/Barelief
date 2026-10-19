#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/settings_widget.h>
#include <core_app/graph_panel.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <desktop/display_refresh_rates.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <QElapsedTimer>
#include <QThread>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScreen>
#include <QMenu>
#include <QCheckBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QtGui/qscreen_platform.h>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <cstdio>
#include <functional>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);return 1;}}while(false)
static bool wait(const std::function<bool()>& predicate){QElapsedTimer timer;timer.start();while(timer.elapsed()<4000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(5);}return predicate();}
int main(int argc,char** argv) {
    if(argc==2 && QByteArray(argv[1])=="--inspect") {
        QApplication app(argc,argv);
        for(auto* screen:app.screens()) {
            std::wstring device;
#ifdef Q_OS_WIN
            if(auto* native=screen->nativeInterface<QNativeInterface::QWindowsScreen>())
                device=desktop::display_device_for_monitor(reinterpret_cast<std::uintptr_t>(native->handle()));
#endif
            CHECK(!device.empty());
            const auto rates=desktop::supported_screen_refresh_rates(device);
            std::printf("Qt screen=%s Win32device=%s QtHz=%.6f nativeHz=%.6f supported=",qPrintable(screen->name()),qPrintable(QString::fromStdWString(device)),screen->refreshRate(),rates.current);
            for(double rate:rates.supported)std::printf(" %.6f",rate);
            std::puts("");
            CHECK(rates.current>0&&!rates.supported.empty());
        }
        std::puts("Read-only Qt/Win32 display mapping PASS; no mode applied");return 0;
    }
    if(argc==3 && QByteArray(argv[1])=="--verify-recovery") {
        CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
        QTemporaryDir temp;CHECK(temp.isValid());const auto copy=temp.filePath("project.json");
        CHECK(QFile::copy(QString::fromLocal8Bit(argv[2]),copy));
        desktop::NumericProjectDocument expected_document;domain::NumericProcessingGraph expected_graph;
        CHECK(desktop::load_numeric_project(copy,expected_document,expected_graph));
        auto encoded=(copy+QStringLiteral("\\")).toLocal8Bit();char executable[]="display_mode_application_tests",config[]="--config";
        char* arguments[]{executable,config,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
        app._ctx.file_configs.ui.autosave_enabled=false;
        auto* model=dynamic_cast<NumericEditorGraphModel*>(app._ctx.data_flow_graph.get());CHECK(model);
        model->set_capture_device_provider([](int,QString*){return QJsonArray{QJsonObject{{"id","synthetic-capture"},{"name","Synthetic capture"},{"is_default",true},{"native_sample_rate",48000},{"native_channels",1}}};});
        CHECK(wait([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending();}));
        CHECK(app.presenter().saved_graph_warning().isEmpty());
        CHECK(model->numeric_graph().nodes.size()==expected_graph.nodes.size());
        for(const auto& expected:expected_graph.nodes) {
            bool found=false;for(const auto& loaded:model->numeric_graph().nodes)if(loaded.id==expected.id)found=true;
            CHECK(found);
        }
        pipeline::DeliveredNumericFrame frame;std::uint64_t ordinal=0,filled=0;
        CHECK(wait([&]{return app.pipeline().query_numeric({"main",7,domain::PortDirection::Output,0},frame,&ordinal,&filled)==signal_contracts::Status::ok&&ordinal>=10;}));
        for(const auto& expected:expected_graph.nodes)if(expected.kind=="MatrixFFT") {
            const auto tap=app.pipeline().subscribe_numeric_tap({{"main",expected.id,domain::PortDirection::Output,0}},[](const pipeline::DeliveredNumericFrame&){},{});
            CHECK(tap);
            pipeline::DeliveredNumericFrame fft;
            CHECK(wait([&]{return app.pipeline().query_numeric({"main",expected.id,domain::PortDirection::Output,0},fft)==signal_contracts::Status::ok&&fft.frame&&fft.frame.view().stamp.available;}));
            const auto matrix=std::get<signal_contracts::NumericView>(fft.frame.view().payload);
            CHECK(matrix.actual[0]>0&&matrix.actual[1]>0);
            std::printf("Saved example MatrixFFT available: %zu x %zu\n",matrix.actual[0],matrix.actual[1]);
            app.pipeline().unsubscribe_numeric_tap(tap);
        }
        std::printf("Recovered user project actual editor/Apply/History: %zu nodes, ordinal %llu, offscreen/synthetic PASS; original untouched\n",model->numeric_graph().nodes.size(),static_cast<unsigned long long>(ordinal));
        return 0;
    }
    CHECK(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen");
    QTemporaryDir temp;CHECK(temp.isValid());const auto path=temp.filePath("project.json");
    desktop::NumericDraftPresentation presentation;
    presentation.application_settings={{"ui",QJsonObject{{"autosave_enabled",false},{"graph_rendering",QJsonObject{{"gui_fps_mode","maximum"}}}}}};
    desktop::NumericProjectDocument project;CHECK(desktop::create_numeric_project(numeric_generator_graph(),presentation,project));
    CHECK(desktop::save_numeric_project(project,path));
    desktop::DisplayModeSnapshot actual{3072,1920,32,0,false,60};bool reject60=false,uncertain165=false;unsigned applications=0,enumerations=0,missingReads=0;
    double supportedMaximum=165;
    desktop::DisplayModeApi api;
    api.query=[&](std::wstring_view)->std::optional<desktop::DisplayModeSnapshot>{if(missingReads){--missingReads;return {};}return actual;};
    api.enumerate=[&](std::wstring_view){++enumerations;auto a=actual,b=actual,c=actual;a.refresh_rate=60;b.refresh_rate=120;c.refresh_rate=supportedMaximum;return std::vector{a,b,c};};
    api.apply=[&](std::wstring_view,double rate){++applications;if(reject60&&rate==60)return false;actual.refresh_rate=rate;if(uncertain165&&rate==165)missingReads=2;return true;};
    {
        auto encoded=path.toLocal8Bit();char executable[]="display_mode_application_tests",config[]="--config";
        char* arguments[]{executable,config,encoded.data(),nullptr};int count=3;
        CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory(),api);
        MainWindow* window=nullptr;for(auto* widget:QApplication::topLevelWidgets())if(auto* found=qobject_cast<MainWindow*>(widget))window=found;
        CHECK(window);auto* settings=window->findChild<SettingsWidget*>();CHECK(settings);
        window->show();CHECK(wait([&]{return actual.refresh_rate==165;}));
        auto* mode=settings->findChild<QComboBox*>("guiFpsMode");auto* custom=settings->findChild<QSpinBox*>("guiTargetFps");CHECK(mode&&custom);
        CHECK(mode->findData(60)>=0&&mode->findData(120)>=0&&mode->findData(165)>=0);
        auto panels_at=[&](int rate){const auto panels=window->findChildren<GraphPanel*>();if(panels.empty())return false;for(auto* panel:panels)if(panel->render_fps()!=rate)return false;return true;};
        CHECK(wait([&]{return app.presenter().client().snapshot().graph_revision>0&&!app.presenter().graph_request_pending()&&panels_at(165);}));
        QCoreApplication::processEvents();
        const auto catalog_before_popup=enumerations;
        QMenu popup(window);popup.addAction("Window function");
        popup.show();QCoreApplication::processEvents();QCoreApplication::processEvents();
        popup.hide();QCoreApplication::processEvents();QCoreApplication::processEvents();
        CHECK(enumerations==catalog_before_popup);
        auto* mesh=settings->findChild<QCheckBox*>("surface_mesh_grid");CHECK(mesh);
        mesh->setChecked(!mesh->isChecked());mesh->setChecked(!mesh->isChecked());
        settings->surface_settings_changed();
        for(int i=0;i<10;++i)CHECK(settings->display_refresh_rate()==165);
        settings->cadence_changed();
        CHECK(enumerations==catalog_before_popup);
        settings->refresh_graph_preferences();CHECK(enumerations==catalog_before_popup);
#ifdef Q_OS_WIN
        const auto catalogChanged=[&]{MSG message{};message.message=WM_DISPLAYCHANGE;app.nativeEventFilter("windows_generic_MSG",&message,nullptr);};
        supportedMaximum=240;catalogChanged();
        CHECK(wait([&]{return actual.refresh_rate==240&&mode->findData(240)>0&&panels_at(240);}));
        supportedMaximum=144;actual.refresh_rate=60;catalogChanged();
        CHECK(wait([&]{return actual.refresh_rate==144&&mode->findData(240)<0&&panels_at(144);}));
        supportedMaximum=165;catalogChanged();
        CHECK(wait([&]{return actual.refresh_rate==165&&mode->findData(165)>0&&panels_at(165);}));
#endif
        mode->setCurrentIndex(mode->findData(120));CHECK(actual.refresh_rate==120&&panels_at(120));
        CHECK(app._ctx.file_configs.ui.graph_rendering["gui_fps_mode"]=="display");
        const auto saved_path=temp.filePath("display-120.json");app.save_configuration_as(saved_path);
        desktop::NumericProjectDocument saved;domain::NumericProcessingGraph saved_graph;QJsonObject saved_settings;
        CHECK(desktop::load_numeric_project(saved_path,saved,saved_graph)&&desktop::read_numeric_application_settings(saved,saved_settings));
        CHECK(saved_settings["ui"].toObject()["graph_rendering"].toObject()["gui_fps_mode"]=="display");
        CHECK(saved_settings["ui"].toObject()["graph_rendering"].toObject()["gui_target_fps"]==120);
        reject60=true;mode->setCurrentIndex(mode->findData(60));CHECK(actual.refresh_rate==120&&mode->currentData().toInt()==120);
        CHECK(app._ctx.file_configs.ui.graph_rendering["gui_target_fps"]==120);reject60=false;
        uncertain165=true;mode->setCurrentIndex(mode->findData(165));uncertain165=false;
        CHECK(app._ctx.file_configs.ui.graph_rendering["gui_target_fps"]==120 && mode->currentData().toInt()==120);
        CHECK(wait([&]{return actual.refresh_rate==120&&panels_at(120);}));
        mode->setCurrentIndex(mode->findData(-1));custom->setValue(170);
        CHECK(actual.refresh_rate==60&&panels_at(170));
        mode->setCurrentIndex(mode->findData(0));CHECK(actual.refresh_rate==165&&panels_at(165));
        app._ctx.file_configs.ui.graph_rendering["gui_fps_mode"]="fixed";
        app._ctx.file_configs.ui.graph_rendering["gui_target_fps"]=170;
        missingReads=1;settings->cadence_changed();
        CHECK(actual.refresh_rate==165); // First release readback was unavailable.
        CHECK(wait([&]{return actual.refresh_rate==60&&panels_at(170);}));
        mode->setCurrentIndex(mode->findData(0));CHECK(actual.refresh_rate==165&&panels_at(165));
        const auto maximum_path=temp.filePath("display-maximum.json");app.save_configuration_as(maximum_path);
        CHECK(desktop::load_numeric_project(maximum_path,saved,saved_graph)&&desktop::read_numeric_application_settings(saved,saved_settings));
        CHECK(saved_settings["ui"].toObject()["graph_rendering"].toObject()["gui_fps_mode"]=="maximum");
        CHECK(mode->findData(-2)>=0);
        mode->setCurrentIndex(mode->findData(-2));
        CHECK(app._ctx.file_configs.ui.graph_rendering["gui_fps_mode"]=="screen");
        CHECK(wait([&]{return actual.refresh_rate==60&&panels_at(60);}));
        actual.refresh_rate=120;settings->cadence_changed();
        CHECK(actual.refresh_rate==120&&panels_at(120)&&mode->currentData().toInt()==-2);
        const auto system_path=temp.filePath("display-system.json");app.save_configuration_as(system_path);
        CHECK(desktop::load_numeric_project(system_path,saved,saved_graph)&&desktop::read_numeric_application_settings(saved,saved_settings));
        CHECK(saved_settings["ui"].toObject()["graph_rendering"].toObject()["gui_fps_mode"]=="screen");
        actual.refresh_rate=60;settings->cadence_changed();
        mode->setCurrentIndex(mode->findData(0));CHECK(actual.refresh_rate==165&&panels_at(165));
        // A visible floating host must stop owning its monitor when its final
        // graph leaves, even when unrelated content keeps that host visible.
        auto* panel=window->findChild<GraphPanel*>();CHECK(panel);
        QPointer<QWidget> originalParent=panel->parentWidget();CHECK(originalParent);
        QWidget floating;auto* layout=new QVBoxLayout(&floating);
        layout->addWidget(new QLabel("Other dock",&floating));layout->addWidget(panel);
        floating.show();QCoreApplication::processEvents();window->hide();
        CHECK(wait([&]{return actual.refresh_rate==165;}));
        panel->setParent(originalParent);panel->hide();
        CHECK(wait([&]{return actual.refresh_rate==60;}));CHECK(floating.isVisible());
        floating.hide();panel->show();window->show();
        CHECK(wait([&]{return actual.refresh_rate==165;}));
        window->close();CHECK(wait([&]{return actual.refresh_rate==60;}));
    }
    CHECK(actual.refresh_rate==60&&applications>0);
    std::puts("Injected display mode application: Maximum165, explicit120, failed60 rollback, manual170 cadence, session restore PASS; no native mode applied");
    return 0;
}
