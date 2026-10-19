#include <bdsp_protocol/numeric_data_message.h>
#include <QQuickWidget>
#include <QQuickItem>
#include "numeric_gui_fixture.h"
#include <control/numeric_control_server.h>
#include <pipeline/numeric_engine.h>
#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/graph_panel.h"
#include "core_app/workspace/processor_directory_model.h"
#include "core_app/settings_widget.h"
#include "core_app/config_manager.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/ui_text.h"
#include "visualization/surface_settings.h"
#include "visualization/scene_aspect_control.h"
#include <QtNodes/StyleCollection>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QPushButton>
#include <QScrollArea>
#include <QLayout>
#include <QToolButton>
#include "core_app/scheme_editor/editor_widget.h"
#include <QStyleHints>
#include <QScreen>
#include <QMenu>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QElapsedTimer>
#include "aggregator/aggregator_server.h"
#include "aggregator/aggregator_client.h"
#include <QAction>
#include <QDockWidget>
#include <QAbstractItemView>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleFactory>
#include <QTableWidget>
#include <QDialogButtonBox>
#include <QColorDialog>
#include <QTimer>
#include <QDialog>
#include <QInputDialog>
#include <QTreeWidget>
#include <QDockWidget>
#include <QTabWidget>
#include <QtTest/QTest>
#include <stdexcept>
static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr,"Graph settings oracle: %s\n",message);std::fflush(stderr);
        throw std::runtime_error(message);
    }
}
int main() {
    QJsonObject rendering{{"version",1},{"extension",42},{"defaults",QJsonObject{{"gradient","green_red"},{"extra",23},{"axis_grid",true},{"floor_grid",false},{"light_strength",1.2345},{"mesh_width",1.2345}}}};
    gui_fixture::App fixture(numeric_test_graph(),{{"theme","system"},{"language","ru"},{"graph_rendering",rendering}});
    fixture.apply();
    auto& app=*fixture.application;
    const auto path=QString::fromLocal8Bit(fixture.path);QFile file(path);
    check(file.open(QIODevice::ReadOnly),"read canonical baseline");const auto originalBytes=file.readAll();file.close();
    try {
        MainWindow *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(widget))
                window = candidate;
        check(window, "MainWindow missing");
        QTest::qWait(100);
        fixture.editor->graph_view_requested(10);
        WorkspaceDock* namedGraph=nullptr;
        for(auto* dock:window->workspaceDocks())
            if(dock->property("graphNode").toInt()==10)namedGraph=dock;
        for(auto* dock:window->workspaceDocks())
            if(dock->property("graphViewKey").toString()=="fft" && dock->property("graphNode").toInt()==10)namedGraph=dock;
        check(namedGraph,"Rename graph fixture missing");
        const auto namedGraphKey=namedGraph->property("graphViewKey").toString();
        check(namedGraphKey=="fft","Rename regression must exercise the primary non-hex graph key");
        const auto defaultGraphTitle=namedGraph->windowTitle();
        auto* graphPanel=qobject_cast<GraphPanel*>(namedGraph->widget());
        auto* graphSettings=graphPanel?graphPanel->findChild<QQuickWidget*>("numericGraphSettings"):nullptr;
        auto* renameGraph=graphSettings && graphSettings->rootObject()?graphSettings->rootObject()->findChild<QObject*>("graphRename"):nullptr;
        check(renameGraph && graphSettings && graphSettings->status()==QQuickWidget::Ready,"Rename graph sidebar control missing");
        const auto chartPanels=window->findChildren<GraphPanel*>();
        check(!chartPanels.empty(),"Chart panels missing");
        for(auto* panel:chartPanels) {
            auto* sidebar=panel->findChild<QQuickWidget*>("numericGraphSettings");
            auto* diagnostic=sidebar && sidebar->rootObject()?sidebar->rootObject()->findChild<QObject*>("numericExactValue"):nullptr;
            check(sidebar && diagnostic && sidebar->status()==QQuickWidget::Ready,"Numeric diagnostics must live inside the settings sidebar");
            check(panel->layout()->count()==1,"Graph still reserves a footer row below its splitter");
        }
        check(namedGraph->contextMenuPolicy()!=Qt::ActionsContextMenu && !namedGraph->findChild<QAction*>("graphRename"),"Canvas still exposes rename context action");
        QTest::qWait(400);
        domain::NumericProcessingGraph processingBeforeRename;check(fixture.editor->numeric_draft(processingBeforeRename),"Read processing before presentation rename");QByteArray processingBytesBeforeRename;check(bool(desktop::encode_numeric_graph(processingBeforeRename,processingBytesBeforeRename)),"Encode processing before presentation rename");
        const auto revisionBeforeRename=app.presenter().client().snapshot().graph_revision;
        const auto undoBeforeRename=fixture.editor->history().count();
        const auto renameTo=[&](const QString& name,bool accept=true,QPushButton* origin=nullptr) {
            QTimer::singleShot(0,[name,accept]{
                for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QInputDialog*>(widget)) {
                    dialog->setTextValue(name);if(accept)dialog->accept();else dialog->reject();
                }
            });
            if(origin)origin->click();else graphPanel->rename();
            QCoreApplication::processEvents();
        };
        renameTo(QString::fromUtf8("Мой спектр"));
        auto* renameModel=fixture.editor->findChild<QtNodes::DataFlowGraphicsScene*>();
        auto* namedNode=renameModel?dynamic_cast<QtNodes::DataFlowGraphModel*>(&renameModel->graphModel())->delegateModel<AudioNodeModel>(10):nullptr;
        check(namedNode && namedNode->caption()==QString::fromUtf8("Мой спектр"),"Graph sidebar rename must update the processing-editor block caption");
        check(namedGraph->windowTitle()==QString::fromUtf8("Мой спектр") &&
              namedGraph->toggleViewAction()->text()==namedGraph->windowTitle() &&
              app._ctx.file_configs.ui.graph_views[namedGraphKey].toObject()["display_name"]==QString::fromUtf8("Мой спектр"),
              "Custom graph name not reflected or persisted");
        ui::Language::instance().set("en");
        check(namedGraph->windowTitle()==QString::fromUtf8("Мой спектр"),"Language replaced custom graph name");
        renameTo("Cancelled",false);
        check(namedGraph->windowTitle()==QString::fromUtf8("Мой спектр"),"Cancel renamed graph");
        renameTo(QString(257,'x'));
        check(namedGraph->windowTitle()==QString::fromUtf8("Мой спектр"),"Oversize graph name accepted");
        auto* renameScene=fixture.editor->findChild<QtNodes::DataFlowGraphicsScene*>();
        check(renameScene,"Pipeline rename scene missing");
        renameScene->nodeSelected(10);
        auto* pipelineRename=fixture.editor->findChild<QPushButton*>("pipelineRenameGraph");
        check(pipelineRename,"Pipeline sidebar rename control missing");
        renameTo("Pipeline title",true,pipelineRename);
        check(namedGraph->windowTitle()=="Pipeline title","Pipeline sidebar did not rename the representation");
        renameTo("");
        check(!app._ctx.file_configs.ui.graph_views[namedGraphKey].toObject().contains("display_name"),"Empty graph name did not restore default");
        ui::Language::instance().set("ru");
        check(namedGraph->windowTitle()==defaultGraphTitle,"Default graph name no longer localized");
        QTest::qWait(400);
        domain::NumericProcessingGraph processingAfterRename;QByteArray processingBytesAfterRename;check(fixture.editor->numeric_draft(processingAfterRename) && bool(desktop::encode_numeric_graph(processingAfterRename,processingBytesAfterRename)),"Read processing after presentation rename");
        if(processingBytesAfterRename!=processingBytesBeforeRename || fixture.editor->history().count()!=undoBeforeRename+3 || app.presenter().client().snapshot().graph_revision!=revisionBeforeRename)std::fprintf(stderr,"Rename diagnostic processing_equal=%d undo=%d expected=%d revision=%llu expected=%llu pending=%d\n",int(processingBytesAfterRename==processingBytesBeforeRename),fixture.editor->history().count(),undoBeforeRename+3,(unsigned long long)app.presenter().client().snapshot().graph_revision,(unsigned long long)revisionBeforeRename,int(app.presenter().graph_request_pending()));
        check(processingBytesAfterRename==processingBytesBeforeRename && fixture.editor->history().count()==undoBeforeRename+3 &&
              app.presenter().client().snapshot().graph_revision==revisionBeforeRename,"Three accepted renames must be presentation-only undo commands without changing the applied graph revision");
        fixture.editor->history().undo();
        check(namedGraph->windowTitle()=="Pipeline title" && namedNode && namedNode->caption()=="Pipeline title","Graph rename undo must synchronize block caption and graph title");
        fixture.editor->history().redo();
        check(namedGraph->windowTitle()==defaultGraphTitle,"Graph rename redo restores the translated default");
        // Offscreen does not restore the parent window's active context after a modal dialog.
        window->activateWindow();window->setFocus();QTest::qWait(20);
        auto *settings = window->findChild<SettingsWidget *>();
        check(settings, "Settings missing");
        auto* renderTimer=window->findChild<QTimer*>("numericRenderTimer");
        auto* guiMode=settings->findChild<QComboBox*>("guiFpsMode");auto* guiRate=settings->findChild<QSpinBox*>("guiTargetFps");
        check(renderTimer && renderTimer->isActive() && renderTimer->timerType()==Qt::PreciseTimer && guiMode && guiRate,"Numeric rendering must use the global cadence and a precise timer");
        const auto rateGraph=fixture.editor->document();const auto rateRevision=app.presenter().client().snapshot().graph_revision;
        const auto rateUndo=fixture.editor->history().index();const auto previousCadence=app._ctx.file_configs.ui.graph_rendering;
        SettingsWidget::DisplayRates simulatedDisplay{{60,120,165,165},165};std::vector<double> modeRequests;bool rejectMode=false;
        const auto simulatedProvider=[&](QScreen*){return simulatedDisplay;};
        settings->set_display_current_rate_provider([&](QScreen*){return simulatedDisplay.current;});
        settings->set_display_rate_provider(simulatedProvider);
        window->set_display_refresh_rate_provider([&](QScreen*){return simulatedDisplay.current;});
        settings->set_display_mode_requester([&](QScreen*,double rate){modeRequests.push_back(rate);if(rejectMode)return false;simulatedDisplay.current=rate>0?rate:*std::max_element(simulatedDisplay.supported.begin(),simulatedDisplay.supported.end());return true;});
        check(guiMode->count()==6 && guiMode->findData(-2)>=0 && guiMode->findData(0)>=0 &&
              guiMode->findData(-1)>=0 && guiMode->findData(30)<0 && guiMode->findData(100)<0 &&
              guiMode->findData(170)<0 && guiMode->findData(165)>0,
              "Cadence choices must contain System, Maximum, Custom and only distinct supported screen modes");
        check(guiMode->currentData().toInt()==0 && guiMode->currentText()==ui::text("Maximum") && guiMode->findText(ui::text("Screen refresh rate"))<0 && guiRate->value()==165 && renderTimer->interval()==qMax(1,1000/165),"Default cadence must select Maximum and use the actual OS screen rate");
        check(guiRate->suffix().isEmpty() && guiRate->prefix().isEmpty() && guiRate->text()=="165","Editable cadence must contain only a number");
        int removedCadenceRows=0;
        const auto cadenceRowsConnection=QObject::connect(guiMode->model(),&QAbstractItemModel::rowsRemoved,settings,[&removedCadenceRows]{++removedCadenceRows;});
        for(const int hz:{60,120,165}){
            guiMode->setCurrentIndex(guiMode->findData(hz));
            check(renderTimer->interval()==qMax(1,1000/hz),"Fixed global cadence did not reach the numeric projection scheduler");
            check(app._ctx.file_configs.ui.graph_rendering["gui_fps_mode"]=="display" && app._ctx.file_configs.ui.graph_rendering["gui_target_fps"].toInt()==hz && modeRequests.back()==hz && simulatedDisplay.current==hz,"Supported dropdown choice must request an OS mode and persist its selected rate");
        }
        const auto beforeFailedMode=app._ctx.file_configs.ui.graph_rendering;rejectMode=true;guiMode->setCurrentIndex(guiMode->findData(120));rejectMode=false;
        check(app._ctx.file_configs.ui.graph_rendering==beforeFailedMode && guiMode->currentData().toInt()==165 && renderTimer->interval()==qMax(1,1000/165),"Rejected display switch must preserve the previous preference, selection and actual cadence");
        const auto requestsBeforeCustom=modeRequests.size();
        guiMode->setCurrentIndex(guiMode->findData(-1));guiRate->setValue(120);
        check(renderTimer->interval()==qMax(1,1000/120) && guiMode->currentData().toInt()==-1,"Manual and dropdown cadence must share the same application path");
        guiRate->setValue(170);
        check(renderTimer->interval()==qMax(1,1000/170),"Custom global cadence did not reach the numeric projection scheduler");
        check(modeRequests.size()==requestsBeforeCustom,"Manual GUI cadence must not request an unsupported OS display mode");
        check(removedCadenceRows==0,"Selecting a cadence must not rebuild the active popup model synchronously");
        QObject::disconnect(cadenceRowsConnection);
        simulatedDisplay={{60,144},144};settings->set_display_rate_provider(simulatedProvider);
        check(guiMode->findData(165)<0 && guiMode->findData(144)>0 && guiMode->findData(170)<0 && guiMode->currentData().toInt()==-1 && guiRate->value()==170 && renderTimer->interval()==qMax(1,1000/170),"Screen changes must rebuild choices and preserve an explicit custom override");
        guiMode->setCurrentIndex(guiMode->findData(0));
        const int screenRate=SurfaceSettings::renderFps(app.presenter().state().file_configs.ui.graph_rendering,settings->display_refresh_rate());
        check(modeRequests.back()==0 && app._ctx.file_configs.ui.graph_rendering["gui_fps_mode"]=="maximum" && guiRate->value()==screenRate && renderTimer->interval()==qMax(1,1000/screenRate),"Maximum must request the current monitor's fastest mode and use its actual cadence");
        simulatedDisplay={{60,165,360},360};settings->set_display_rate_provider(simulatedProvider);
        check(guiMode->currentData().toInt()==0 && guiRate->value()==360 && renderTimer->interval()==qMax(1,1000/360) && guiMode->findData(360)>0,"Screen cadence above 240 Hz must preserve the current physical refresh rate");
        guiMode->setCurrentIndex(guiMode->findData(360));
        check(guiRate->maximum()==1000 && guiRate->minimum()==1 && guiRate->value()==360 && renderTimer->interval()==qMax(1,1000/360),"Supported 360 Hz dropdown selection was silently capped");
        for(auto* panel:window->findChildren<GraphPanel*>())check(panel->render_fps()==360,"Renderer cadence above 240 Hz was silently capped");
        auto restoredCadence=app._ctx.file_configs.ui.graph_rendering;restoredCadence["gui_fps_mode"]="fixed";restoredCadence["gui_target_fps"]=170;app._ctx.file_configs.ui.graph_rendering=restoredCadence;
        settings->refresh_graph_preferences();settings->surface_settings_changed();
        check(guiMode->currentData().toInt()==-1 && guiRate->value()==170 && renderTimer->interval()==qMax(1,1000/170),"Reloaded explicit fixed override must synchronize the existing cadence controls");
        auto unavailableDisplayRate=app._ctx.file_configs.ui.graph_rendering;unavailableDisplayRate["gui_fps_mode"]="display";unavailableDisplayRate["gui_target_fps"]=240;app._ctx.file_configs.ui.graph_rendering=unavailableDisplayRate;
        settings->refresh_graph_preferences();settings->surface_settings_changed();
        check(guiMode->currentData().toInt()==0 && guiRate->value()==360 && renderTimer->interval()==qMax(1,1000/360),"A physical rate unavailable on the current monitor must display its Maximum fallback, not a manual cadence");
        app._ctx.file_configs.ui.graph_rendering=previousCadence;settings->set_display_mode_requester({});settings->set_display_current_rate_provider({});settings->set_display_rate_provider({});window->set_display_refresh_rate_provider({});settings->refresh_graph_preferences();settings->surface_settings_changed();
        check(fixture.editor->document()==rateGraph && app.presenter().client().snapshot().graph_revision==rateRevision && fixture.editor->history().index()==rateUndo,"GUI render cadence changed analysis graph/revision/Undo");
        settings->select_section(SettingsWidget::Graphs);
        auto *tabs = settings->findChild<QTabWidget *>("settingsSections");
        check(tabs && tabs->count() == 6 && tabs->currentIndex() == 2, "Graphs section order");
        for (const auto section : {SettingsWidget::Graphs, SettingsWidget::Controls}) {
            auto *action = window->findChild<QAction *>(QString("openSettingsSection%1").arg(section));
            check(action, "Settings section action missing");
            action->trigger();
            check(tabs->currentIndex() == section, "Settings menu targets wrong section");
        }
        check(!window->findChild<QAction *>("openSettingsSection3") &&
              !window->findChild<QAction *>("openSettingsSection4"), "Hidden settings retain navigation commands");
        settings->select_section(SettingsWidget::Graphs);
        check(tabs->tabText(SettingsWidget::Graphs) == QString::fromUtf8("Графики"),
              "Graphs Russian translation missing");
        auto *language = settings->findChild<QComboBox *>("interfaceLanguage");
        language->setCurrentIndex(1);
        check(tabs->tabText(SettingsWidget::Graphs) == "Graphs", "Graphs English translation missing");
        language->setCurrentIndex(0);
        check(!settings->findChild<QComboBox *>("surfaceSettingsTarget") &&
                  !settings->findChild<QCheckBox *>("surfaceUseGlobal") &&
                  !settings->findChild<QComboBox *>("surface_gradient") &&
                  !settings->findChild<QComboBox *>("surface_y_range_mode"),
              "Per-graph settings remain in global form");
        check(!settings->findChild<QGroupBox *>("graphViewsSettings") &&
                  !settings->findChild<QCheckBox *>("graphViewToggle0"),
              "Project graph visibility controls remain in global Graphs settings");
        for (const auto* name : {"showRawSamples", "showWindowSignal", "showWindowedSignal",
                                 "showWindowFunction", "showFFT", "showMel", "showSpectrogram"})
            check(!settings->findChild<QCheckBox *>(name),
                  "Project graph visibility control remains in global General settings");
        auto* spectrogramDock=window->workspaceDock("spectrogramDock");
        check(spectrogramDock && spectrogramDock->toggleViewAction()->isCheckable(),
              "Workspace graph visibility action disappeared with Settings controls");
        auto *precision = settings->findChild<QSlider *>("surface_axis_precision");
        check(precision && precision->minimum() == 0 && precision->maximum() == 8,
              "Global axis precision slider missing");
        auto legacyView = app._ctx.file_configs.ui.graph_views["spectrogram"].toObject();
        auto legacyOverride = legacyView["surface_override"].toObject();
        legacyOverride["enabled"] = true;
        auto legacyValues = legacyOverride["values"].toObject();
        legacyValues["axis_precision"] = 2;
        legacyOverride["values"] = legacyValues;
        legacyView["surface_override"] = legacyOverride;
        app._ctx.file_configs.ui.graph_views["spectrogram"] = legacyView;
        precision->setValue(8);
        check(SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering, legacyView)["axis_precision"]
                          .toInt() == 8 &&
                  app._ctx.file_configs.ui.graph_views["spectrogram"]
                          .toObject()["surface_override"]
                          .toObject()["values"]
                          .toObject()["axis_precision"]
                          .toInt() == 2,
              "Global precision priority rewrote legacy override");
        // Retain metadata but restore style inheritance for the following global checks.
        legacyOverride["enabled"] = false;
        legacyView["surface_override"] = legacyOverride;
        app._ctx.file_configs.ui.graph_views["spectrogram"] = legacyView;
        auto *movement = settings->findChild<QSlider *>("commonMovementSpeed");
        auto *movementReset = settings->findChild<QPushButton *>("resetCommonMovementSpeed");
        check(movement && movementReset && movement->minimum() == 50 && movement->maximum() == 1000,
              "Common movement slider missing");
        const auto savedCamera = app._ctx.file_configs.ui.camera;
        movement->setValue(425);
        check(CameraControls::movementSpeed(app._ctx.file_configs.ui.camera_controls) == 4.25 &&
                  app._ctx.file_configs.ui.camera == savedCamera,
              "Movement preference modified saved camera state");
        movementReset->click();
        check(CameraControls::movementSpeed(app._ctx.file_configs.ui.camera_controls) == 3, "Movement reset baseline");
        auto *mesh = settings->findChild<QCheckBox *>("surface_mesh_grid");
        auto *brightness = settings->findChild<QSlider *>("surface_brightness");
        check(mesh && brightness, "Global surface style controls missing");
        mesh->setChecked(false);
        brightness->setValue(140);
        auto global = SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering);
        check(!global["mesh_grid"].toBool() && global["brightness"].toDouble() == 1.4 &&
                  global["gradient"] == "green_red",
              "Global controls apply");
        check(global["extra"].toInt() == 23, "Unknown defaults lost");
        check(global["axis_grid"].toBool() && !global["floor_grid"].toBool(),
              "Old mixed space grids were overwritten by unrelated setting");
        check(std::abs(global["light_strength"].toDouble() - 1.2345) < 1e-8, "Slider rounded unrelated saved value");
        check(brightness->minimum() == 10 && brightness->maximum() == 200, "Brightness slider limits");
        auto *valueLabel = settings->findChild<QLabel *>("surface_brightness_value");
        auto *reset = settings->findChild<QPushButton *>("surface_brightness_default");
        check(valueLabel && reset && valueLabel->text() == "140%", "Slider number/default missing");
        reset->click();
        check(brightness->value() == 100 &&
                  SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["brightness"].toDouble() == 1.0,
              "Slider reset did not apply baseline");
        brightness->setValue(140);
        auto *space = settings->findChild<QCheckBox *>("surface_space_grid");
        check(space && space->checkState() == Qt::PartiallyChecked, "Mixed legacy grids not represented");
        space->setCheckState(Qt::Checked);
        auto grid = SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering);
        check(grid["axis_grid"].toBool() && grid["floor_grid"].toBool() && !grid.contains("space_grid"),
              "Space grid toggle/schema compatibility");
        auto *lighting = settings->findChild<QCheckBox *>("surface_light");
        check(lighting, "Lighting checkbox missing");
        QTest::mouseClick(lighting, Qt::LeftButton, Qt::NoModifier, QPoint(9, lighting->height() / 2));
        check(!SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["light"].toBool(),
              "Lighting checkbox indicator is not clickable");
        lighting->setChecked(true);
        check(reset->text() == QString::fromUtf8("Сброс") && brightness->tickPosition() == QSlider::NoTicks,
              "Compact slider/reset translation missing");
        auto* aspectMode=settings->findChild<QComboBox*>("surface_scene_aspect_mode");
        auto* timeAspect=settings->findChild<QDoubleSpinBox*>("surface_scene_time_aspect");
        auto* bandAspect=settings->findChild<QDoubleSpinBox*>("surface_scene_band_aspect");
        check(aspectMode && timeAspect && bandAspect && aspectMode->currentIndex()==0 &&
              timeAspect->value()==2 && bandAspect->value()==1,"Scene aspect defaults/form missing");
        timeAspect->setValue(3.25);bandAspect->setValue(.75);
        auto appearance=SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering);
        check(appearance["scene_time_aspect"].toDouble()==3.25 && appearance["scene_band_aspect"].toDouble()==.75,
              "Global scene ratios did not persist");
        aspectMode->setCurrentIndex(1);
        check(!timeAspect->isEnabled() && !bandAspect->isEnabled() &&
              SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["scene_aspect_mode"]=="sample_counts",
              "Sample count mode/disabled custom ratios not synchronized");
        aspectMode->setCurrentIndex(0);
        check(timeAspect->isEnabled() && bandAspect->isEnabled(),"Fixed aspect controls not restored");
        auto *meshWidth = settings->findChild<QSlider *>("surface_mesh_width");
        auto *meshContrast = settings->findChild<QSlider *>("surface_mesh_contrast");
        check(
            meshWidth && meshContrast &&
                std::abs(SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["mesh_width"].toDouble() -
                         1.2345) < 1e-8,
            "Unrelated edits rounded mesh thickness");
        meshWidth->setValue(250);
        meshContrast->setValue(35);
        check(SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["mesh_width"].toDouble() == 2.5 &&
                  SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["mesh_contrast"].toDouble() ==
                      .35,
              "Mesh controls do not persist");
        const QString key =
            QString::fromLatin1(QJsonDocument(QJsonArray{app.presenter().device_id(), app.presenter().graph_id(), 8})
                                    .toJson(QJsonDocument::Compact)
                                    .toHex());
        auto preservedView = app._ctx.file_configs.ui.graph_views[key].toObject();
        preservedView["view_extension"] = 91;
        auto preservedOverride = preservedView["surface_override"].toObject();
        preservedOverride["override_extension"] = 92;
        preservedView["surface_override"] = preservedOverride;
        app._ctx.file_configs.ui.graph_views[key] = preservedView;
        const auto beforeCancelViews = app._ctx.file_configs.ui.graph_views;
        const auto beforeCancelSchema = app._ctx.file_configs.ui.graph_rendering;
        QTimer::singleShot(0, settings, [&] {
            if (auto *dialog = settings->findChild<QDialog *>("gradientEditor"))
                dialog->reject();
        });
        settings->edit_color_scale(key);
        check(app._ctx.file_configs.ui.graph_views == beforeCancelViews &&
                  app._ctx.file_configs.ui.graph_rendering == beforeCancelSchema,
              "Color editor Cancel mutated settings");
        bool edited = false;
        QTimer::singleShot(0, settings, [&] {
            auto *dialog = settings->findChild<QDialog *>("gradientEditor");
            if (!dialog)
                return;
            auto *table = dialog->findChild<QTableWidget *>("gradientStops");
            auto *add = dialog->findChild<QPushButton *>("addGradientStop");
            auto *remove = dialog->findChild<QPushButton *>("removeGradientStop");
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (!table || !add || !remove || !buttons) {
                dialog->reject();
                return;
            }
            auto *source = dialog->findChild<QComboBox *>("gradientPreset");
            auto *copy = dialog->findChild<QPushButton *>("copyColorPreset");
            auto *deletePreset = dialog->findChild<QPushButton *>("deleteColorPreset");
            if (!source || !copy || !deletePreset || source->findData("custom") >= 0 || table->isEnabled() ||
                add->isEnabled() || deletePreset->isEnabled()) {
                dialog->reject();
                return;
            }
            const auto builtinCode = source->currentData();
            copy->click();
            if (!table->isEnabled() || !add->isEnabled() || deletePreset->isEnabled() ||
                source->currentData() != builtinCode) {
                dialog->reject();
                return;
            }
            const int count = table->rowCount();
            add->click();
            if (table->rowCount() != count + 1) {
                dialog->reject();
                return;
            }
            remove->click();
            if (table->rowCount() != count) {
                dialog->reject();
                return;
            }
            QTimer::singleShot(0, dialog, [&] {
                if (auto *picker = qobject_cast<QColorDialog *>(QApplication::activeModalWidget())) {
                    picker->setCurrentColor(QColor("#2255bb"));
                    picker->accept();
                }
            });
            qobject_cast<QPushButton *>(table->cellWidget(0, 1))->click();
            edited = table->cellWidget(0, 1)->property("stopColor").toString() == "#2255bb";
            buttons->button(QDialogButtonBox::Save)->click();
        });
        settings->edit_color_scale(key);
        const auto editedView = app._ctx.file_configs.ui.graph_views[key].toObject();
        const auto stops =
            SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering, editedView)["gradient_stops"]
                .toArray();
        check(editedView["view_extension"].toInt() == 91 &&
                  editedView["surface_override"].toObject()["override_extension"].toInt() == 92,
              "Color editor lost unknown view/override metadata");
        check(
            SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["gradient_stops"].toArray().isEmpty(),
            "Per-graph color scale leaked to global defaults");
        check(edited && stops.size() >= 2 && stops[0].toObject()["color"] == "#2255bb",
              "Real color picker/add/remove/save did not change gradient");
        check(SurfaceSettings::normalizedStops({QJsonObject{{"position", 0}, {"color", "#000000"}},
                                                QJsonObject{{"position", 0}, {"color", "#ffffff"}},
                                                QJsonObject{{"position", 1}, {"color", "#ffffff"}}})
                  .isEmpty(),
              "Duplicate gradient positions accepted");
        auto *surface = window->workspaceDock("spectrogramDock")->findChild<SpectrogramGraph*>();
        check(surface, "Surface missing");
        check(!surface->surface_settings()["mesh_grid"].toBool() &&
                  surface->surface_settings()["brightness"].toDouble() == 1.4,
              "Global settings did not reach renderer API");
        check(surface->surface_settings()["gradient_stops"].toArray() == stops,
              "Per-graph color scale did not reach renderer");
        // Exercise the same shared control used beside a native spectrogram,
        // through its real surface API and per-view persistence signal.
        ui::SceneAspectControl preciseAspect;
        const QJsonObject preciseOptions{{"scene_time_aspect",2.123456789},{"scene_band_aspect",.75},
            {"extension",43},{"y_range_mode","manual"},{"y_min",-.123456789},{"y_max",3.23456789}};
        int preciseWrites=0;preciseAspect.changed=[&]{++preciseWrites;};
        preciseAspect.set_options(preciseOptions);
        auto preciseResult=preciseAspect.apply(preciseOptions);
        check(preciseWrites==0 && preciseResult["scene_time_aspect"].toDouble()==2.123456789,
              "Loading aspect control quantized or wrote exact stored ratio");
        preciseAspect.findChild<QDoubleSpinBox*>("surface_scene_band_aspect")->setValue(1.5);
        preciseResult=preciseAspect.apply(preciseOptions);
        check(preciseWrites==1 && preciseResult["scene_time_aspect"].toDouble()==2.123456789 &&
              preciseResult["scene_band_aspect"].toDouble()==1.5 && preciseResult["extension"].toInt()==43 &&
              preciseResult["y_min"].toDouble()==-.123456789 && preciseResult["y_max"].toDouble()==3.23456789,
              "Editing band ratio quantized time ratio or changed unknown metadata/manual Y");
        ui::SceneAspectControl localAspect;
        int localWrites=0;
        localAspect.changed=[&]{
            ++localWrites;
            const auto options=localAspect.apply(surface->surface_settings());
            surface->set_surface_settings(options);emit surface->surface_settings_changed(options);
        };
        localAspect.set_options(surface->surface_settings());
        check(localWrites==0,"Loading scene controls wrote view preferences");
        localAspect.findChild<QDoubleSpinBox*>("surface_scene_band_aspect")->setValue(1.5);
        const auto aspectView=SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering,
            app._ctx.file_configs.ui.graph_views[key].toObject());
        check(localWrites==1 && aspectView["scene_band_aspect"].toDouble()==1.5 &&
              aspectView["scene_time_aspect"].toDouble()==3.25 &&
              SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering)["scene_band_aspect"].toDouble()==.75,
              "Per-view visual aspect was not isolated from global defaults");
        check(surface->projected_scene_geometry().scaleX==3.25f &&
              surface->projected_scene_geometry().scaleZ==1.5f,"Effective visual ratios did not reach geometry");
        // Coincidental global equality must not erase an explicitly pinned
        // scene policy when the user edits only its colour scale.
        bandAspect->setValue(1.5);
        auto colourOnly=surface->surface_settings();colourOnly["gradient_layout"]="linear";
        surface->set_surface_settings(colourOnly);emit surface->surface_settings_changed(colourOnly);
        const auto retainedAspect=app._ctx.file_configs.ui.graph_views[key].toObject()["surface_override"]
            .toObject()["values"].toObject();
        check(retainedAspect["scene_aspect_mode"]=="fixed" && retainedAspect["scene_time_aspect"].toDouble()==3.25 &&
              retainedAspect["scene_band_aspect"].toDouble()==1.5,
              "Unrelated colour edit removed pinned geometry after coincidental global equality");
        aspectMode->setCurrentIndex(1);
        const auto fixedView=SurfaceSettings::effective(app._ctx.file_configs.ui.graph_rendering,
            app._ctx.file_configs.ui.graph_views[key].toObject());
        check(fixedView["scene_aspect_mode"]=="fixed" && fixedView["scene_time_aspect"].toDouble()==3.25 &&
              fixedView["scene_band_aspect"].toDouble()==1.5 &&
              surface->projected_scene_geometry().scaleX==3.25f,
              "Changing global aspect mode disabled explicit fixed per-view geometry");
        aspectMode->setCurrentIndex(0);bandAspect->setValue(.75);
        const auto editedOverride = app._ctx.file_configs.ui.graph_views[key].toObject()["surface_override"].toObject();
        brightness->setValue(150);
        check(app._ctx.file_configs.ui.graph_views[key].toObject()["surface_override"].toObject() == editedOverride,
              "Editing global style overwrote a graph override");
        check(surface->surface_settings()["brightness"].toDouble() == 1.5,
              "Per-graph color scale pinned unrelated global lighting defaults");
        brightness->setValue(140);
        const auto intervalPanels=window->findChildren<GraphPanel*>();
        check(!intervalPanels.empty(),"Chart panels missing before interval Save/load");
        const auto chartIntervalKey=intervalPanels.front()->objectName().chopped(QStringLiteral("GraphPanel").size());
        intervalPanels.front()->changePreference("auto_range_interval_seconds",7.5);
        check(intervalPanels.front()->preferences().value("auto_range_interval_seconds").toDouble()==7.5,"Chart auto range interval missing");
        // Offscreen skips native 3D controls; exercise their settings signal
        // through the same persistence boundary used by a native user edit.
        auto intervalOptions=surface->surface_settings();intervalOptions["auto_range_interval_seconds"]=6.5;
        surface->set_surface_settings(intervalOptions);emit surface->surface_settings_changed(intervalOptions);
        check(app._ctx.file_configs.ui.graph_views[chartIntervalKey].toObject()["auto_range_interval_seconds"].toDouble()==7.5,
              "Chart interval edit did not update presentation preferences");
        check(file.open(QIODevice::ReadOnly), "Read config failed");
        check(file.readAll() == originalBytes, "Autosave disabled wrote file");
        file.close();
        window->activateWindow(); window->setFocus();
        auto* save=window->findChild<QAction*>("saveSettings");
        check(gui_fixture::wait([&]{return save && save->isEnabled();}),"explicit Save in active workspace");save->trigger();
        ConfigManager manager(path);desktop::NumericProjectDocument source;domain::NumericProcessingGraph graph;
        check(manager.loadNumeric(source,graph),"canonical roundtrip load");FileSaveApplicationConfig loaded;
        check(manager.readNumericSettings(source,loaded),"canonical settings projection");
        check(loaded.ui.graph_views[chartIntervalKey].toObject()["auto_range_interval_seconds"].toDouble()==7.5 &&
              loaded.ui.graph_views[key].toObject()["surface_override"].toObject()["values"].toObject()["auto_range_interval_seconds"].toDouble()==6.5,
              "Per-graph auto range intervals did not survive explicit Save/load");
        check(loaded.ui.graph_rendering==app._ctx.file_configs.ui.graph_rendering,"settings roundtrip");
        check(loaded.ui.graph_rendering["extension"].toInt()==42 && loaded.ui.graph_rendering["defaults"].toObject()["extra"].toInt()==23,"unknown rendering descendants lost");
        surface->surface_settings_error("Test shadow unavailable");
        auto *status = window->findChild<QLabel *>("surfaceSettingsStatus");
        check(status && status->text().contains("Test shadow unavailable"), "Renderer failure not visible");
        surface->surface_settings_error({});
        check(status->isHidden(), "Renderer status did not clear");
        // Legacy gradient migration is unsupported by the canonical-only document contract.
        auto *statusTheme = window->findChild<QComboBox *>("statusTheme");
        check(statusTheme, "Status theme selector missing");
        statusTheme->setCurrentIndex(statusTheme->findData("light"));
        check(app._ctx.file_configs.ui.theme == "light" &&
                  settings->findChild<QComboBox *>("interfaceTheme")->currentIndex() == 2,
              "Status/settings theme not synchronized");
        statusTheme->setCurrentIndex(statusTheme->findData("dark"));
        check(app._ctx.file_configs.ui.theme == "dark", "Status dark theme not persisted in memory");
        check(!window->findChild<QLabel *>("graphApplyStatus"), "Global applied status indicator must be removed");
        auto* diagnosticEditor=window->findChild<SchemeEditorWidget*>();check(diagnosticEditor,"Editor diagnostics missing");
        auto* validation=diagnosticEditor->toolbar()->findChild<QLabel*>("pipelineValidation");check(validation,"Editor warning indicator missing");
        emit app.presenter().apply_status_changed("Error: test");
        check(!validation->isHidden() && validation->toolTip().contains("test"),"Error is absent from editor toolbar");
        const auto errorTooltip=validation->toolTip();
        emit app.presenter().apply_status_changed("Running");
        check(!validation->isHidden() && validation->toolTip()==errorTooltip,"Routine status cleared editor error");
        check(!window->findChild<QLabel*>("savedPipelineWarning"),"Duplicate schema warning remains in global strip");
        auto *renderStatus = window->findChild<QLabel *>("renderStatus");
        check(!renderStatus,"Aggregate FPS remains in main window");
        auto *pipelineToggle = window->findChild<QToolButton *>("pipelineToggle");
        check(pipelineToggle, "Pipeline toggle missing");
        check(pipelineToggle->text() == QString::fromUtf8("Поток данных"), "Data Flow rename missing");
        check(pipelineToggle->defaultAction()->shortcut() == QKeySequence("Alt+D"),
              "Data Flow shortcut conflicts with camera Ctrl");
        auto *themeButton = window->findChild<QToolButton *>("themeMenuButton");
        check(themeButton && themeButton->menu() && themeButton->text() == QString::fromUtf8("Тема"),
              "Theme menu missing");
        for (auto *action : window->findChildren<QAction *>())
            for (const auto &shortcut : action->shortcuts()) {
                if(!shortcut.isEmpty() && (shortcut[0].keyboardModifiers() & Qt::ControlModifier))
                    std::fprintf(stderr,"Ctrl action name=%s shortcut=%s context=%d parent=%s\n",qPrintable(action->objectName()),qPrintable(shortcut.toString()),int(action->shortcutContext()),action->parent()?action->parent()->metaObject()->className():"null");
                check(shortcut.isEmpty() || !(shortcut[0].keyboardModifiers() & Qt::ControlModifier) ||
                          (action->objectName() == "saveSettings" && shortcut == QKeySequence(QKeySequence::Save)) ||
                          (action->objectName() == "saveSettingsAs" && shortcut == QKeySequence(QKeySequence::SaveAs)) ||
                          (action->objectName() == "openSettingsFile" && shortcut == QKeySequence(QKeySequence::Open)) ||
                          (action->objectName() == "workspaceToggleTools" && shortcut == QKeySequence("Ctrl+Shift+F12")) ||
                          (action->objectName() == "workspaceSwitcher" && shortcut == QKeySequence("Ctrl+Tab")) ||
                          (action->objectName() == "undoDataFlow" && shortcut == QKeySequence("Ctrl+Z")) ||
                          (action->objectName() == "redoDataFlow" && shortcut == QKeySequence("Ctrl+Shift+Z")) ||
                          (action->shortcutContext()==Qt::WidgetWithChildrenShortcut &&
                           qobject_cast<QtNodes::GraphicsView*>(action->parent()) && shortcut==QKeySequence("Ctrl+D")) ||
                          ((action->shortcutContext() == Qt::WidgetShortcut || action->shortcutContext() == Qt::WidgetWithChildrenShortcut) &&
                           (shortcut == QKeySequence(QKeySequence::Copy) ||
                            shortcut == QKeySequence(QKeySequence::Cut) ||
                            shortcut == QKeySequence(QKeySequence::Paste))),
                      "Unexpected application Ctrl shortcut remains");
            }
        auto *saveShortcut = window->findChild<QAction *>("saveSettings");
        check(saveShortcut && saveShortcut->shortcut() == QKeySequence(QKeySequence::Save),
              "Save settings Ctrl+S missing");
        auto &theme = ui::Theme::instance();
        const auto revision = app.presenter().client().snapshot().graph_revision;
        for (const auto *mode : {"dark", "light"}) {
            theme.set(mode);
            QTest::qWait(20);
            const bool dark = QString(mode) == "dark";
            QComboBox fresh;
            fresh.addItems({"First", "Second"});
            check(fresh.palette().color(QPalette::Base) == QApplication::palette().color(QPalette::Base) &&
                      (fresh.palette().color(QPalette::Base).lightness() < 128) == dark &&
                      std::abs(fresh.palette().color(QPalette::Text).lightness() -
                               fresh.palette().color(QPalette::Base).lightness()) > 100,
                  "New standard widgets do not inherit the central theme");
            check((QApplication::palette().color(QPalette::Window).lightness() < 128) == dark, "Application theme");
            for (const auto *menuName : {"graphsMenu", "settingsMenu"}) {
                auto *menu = window->findChild<QMenu *>(menuName);
                check(menu, "Menu missing");
                check((menu->palette().color(QPalette::Window).lightness() < 128) == dark, "Menu theme mismatch");
            }
            check((QtNodes::StyleCollection::flowViewStyle().BackgroundColor.lightness() < 128) == dark,
                  "QtNodes background theme");
            for (int id : app._ctx.data_flow_graph->allNodeIds())
                if (auto *model = app._ctx.data_flow_graph->delegateModel<AudioNodeModel>(id)) {
                    const auto &s = model->nodeStyle();
                    check(std::abs(s.backgroundColor().lightness() - s.FontColor.lightness()) > 100,
                          "Pipeline text contrast");
                }
        }
        // Test the actual popup viewport, not just the combo button palette.
        settings->select_section(SettingsWidget::Graphs);
        auto popupPixels = [&](QComboBox *combo, bool dark) {
            auto *view = combo->view();
            if (!view->isVisible())
                qCritical("Popup visibility combo=%s theme=%s buttonVisible=%d enabled=%d windowVisible=%d",
                          qPrintable(combo->objectName()), qPrintable(theme.code()), combo->isVisible(),
                          combo->isEnabled(), combo->window()->isVisible());
            check(view->isVisible(), "Popup closed during theme switch");
            // Keep the pointer on the selected row: a native hover can select
            // the background sample row without changing the combo value.
            const auto firstRow = view->visualRect(view->model()->index(0, 0));
            QTest::mouseMove(view->viewport(), firstRow.center());
            view->setCurrentIndex(view->model()->index(0, 0));
            QTest::qWait(20);
            const auto frame = view->viewport()->grab().toImage();
            auto colorAt = [&](int x, int y) {
                return frame.pixelColor(
                    std::clamp(qRound(x * frame.width() / double(view->viewport()->width())), 0, frame.width() - 1),
                    std::clamp(qRound(y * frame.height() / double(view->viewport()->height())), 0, frame.height() - 1));
            };
            const auto row = view->visualRect(view->model()->index(1, 0));
            check(row.isValid() && row.center().y() < frame.height(), "Popup second row not visible");
            const auto background = colorAt(std::min(view->viewport()->width() - 3, row.right() - 8), row.center().y());
            if ((background.lightness() < 128) != dark)
                qCritical("Popup actual=%s viewBase=%s appBase=%s theme=%s", qPrintable(background.name()),
                          qPrintable(view->palette().color(QPalette::Base).name()),
                          qPrintable(QApplication::palette().color(QPalette::Base).name()), qPrintable(theme.code()));
            frame.save(QCoreApplication::applicationDirPath() + "/../" + combo->objectName() + "-" + theme.code() +
                       "-viewport.png");
            check((background.lightness() < 128) == dark, "Real popup background theme mismatch");
            const auto selected = view->visualRect(view->model()->index(0, 0));
            const auto highlight =
                colorAt(std::min(view->viewport()->width() - 3, selected.right() - 8), selected.center().y());
            check(highlight.blue() > highlight.red() + 50, "Popup selected item highlight missing");
            bool readable = false;
            for (int y = row.top() + 3; y < std::min(row.bottom() - 2, frame.height()); ++y)
                for (int x = 8; x < std::min(row.right() - 15, frame.width()); ++x) {
                    const auto pixel = colorAt(x, y);
                    if (std::abs(pixel.lightness() - background.lightness()) > 35)
                        readable = true;
                }
            check(readable, "Popup item text is unreadable");
        };
        for (const char *name : {"guiFpsMode"}) {
            auto *combo = settings->findChild<QComboBox *>(name);
            check(combo && combo->isEnabled(), "Popup combo unavailable");
            qInfo("Testing native popup %s", name);
            combo->setCurrentIndex(0);
            theme.set("light");
            combo->window()->raise();
            combo->window()->activateWindow();
            QTest::qWait(100);
            qInfo("combo count=%d size=%dx%d nativeHint=%d popupHint=%d style=%s", combo->count(), combo->width(),
                  combo->height(), combo->style()->styleHint(QStyle::SH_ComboBox_UseNativePopup, nullptr, combo),
                  combo->style()->styleHint(QStyle::SH_ComboBox_Popup, nullptr, combo),
                  qPrintable(combo->style()->objectName()));
            combo->showPopup();
            qInfo("Popup immediate=%d activeWindow=%s", combo->view()->isVisible(),
                  qPrintable(QApplication::activeWindow() ? QApplication::activeWindow()->objectName()
                                                          : QString("none")));
            QTest::qWait(200);
            popupPixels(combo, false);
            qInfo("Switch open popup dark");
            theme.set("dark");
            QTest::qWait(200);
            popupPixels(combo, true);
            qInfo("Switch open popup light");
            theme.set("light");
            QTest::qWait(200);
            popupPixels(combo, false);
            combo->view()->window()->grab().save(QCoreApplication::applicationDirPath() + "/../" + QString(name) +
                                                 "-light-popup.png");
            theme.set("system");
            QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Dark);
            QTest::qWait(200);
            popupPixels(combo, true);
            QGuiApplication::styleHints()->colorSchemeChanged(Qt::ColorScheme::Light);
            QTest::qWait(200);
            popupPixels(combo, false);
            combo->hidePopup();
            theme.set("dark");
            combo->showPopup();
            QTest::qWait(200);
            popupPixels(combo, true);
            combo->hidePopup();
            theme.set("light");
            combo->showPopup();
            QTest::qWait(200);
            popupPixels(combo, false);
            if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
                auto *item = model->item(1);
                const auto flags = item->flags();
                item->setFlags(flags & ~Qt::ItemIsEnabled);
                QTest::qWait(200);
                popupPixels(combo, false);
                theme.set("dark");
                QTest::qWait(200);
                popupPixels(combo, true);
                item->setFlags(flags);
            }
            combo->hidePopup();
        }
        qInfo("Actual popup pixels: GUI cadence light-dark-light while open, system events, newly "
              "opened, disabled text PASS");
        theme.set("system");
        auto *hints = QGuiApplication::styleHints();
        hints->colorSchemeChanged(Qt::ColorScheme::Dark);
        QTest::qWait(20);
        check(QApplication::palette().color(QPalette::Window).lightness() < 128, "System dark change");
        hints->colorSchemeChanged(Qt::ColorScheme::Light);
        QTest::qWait(20);
        check(QApplication::palette().color(QPalette::Window).lightness() > 128, "System light change");
        theme.set("dark");
        hints->colorSchemeChanged(Qt::ColorScheme::Light);
        check(theme.code() == "dark" && QApplication::palette().color(QPalette::Window).lightness() < 128,
              "Explicit theme overridden");
        hints->colorSchemeChanged(hints->colorScheme());
        auto *registryHost = window->findChild<QLineEdit *>("registryHost");
        auto *registryStatus = window->findChild<QLabel *>("engineRegistryStatus");
        auto *discover = window->findChild<QPushButton *>("discoverEngines");
        auto *devices = window->findChild<presentation::ProcessorDirectoryModel *>();
        const auto oldRegistry = app._ctx.file_configs.raw_document.value("aggregator");
        check(registryHost && registryStatus && discover && devices && devices->processors().size() == 1,
              "Registry local/observer state invalid");
        auto *selector = window->findChild<QToolButton *>("engineSelectorButton");
        auto *list = window->findChild<QTreeWidget *>("processorManagerList");
        check(selector && selector->menu() && list && list->topLevelItemCount() == 1,
              "Processor manager integration missing");
        emit selector->menu()->aboutToShow();
        check(selector->menu()->actions().last() == window->findChild<QAction *>("openEngineRegistry"),
              "Processor menu lacks connection management");
        window->findChild<QLineEdit *>("processorHost")->setText("invalid-address");
        window->findChild<QPushButton *>("connectProcessor")->click();
        check(registryStatus->text() == ui::text("Processor address must be an IP address") && devices->processors().size() == 1,
              "Invalid processor endpoint was added");
        registryHost->setText("not-an-ip");
        discover->click();
        check(registryStatus->text().contains(QString::fromUtf8("IP-адрес")) &&
                  oldRegistry == app._ctx.file_configs.raw_document.value("aggregator"),
              "Discovery error hidden or discovery rewrote settings");
        check(selector->text() == QString::fromUtf8("Обработка в приложении"),
              "Local processor explanation missing");
        {
            const auto waitRegistry = [](auto predicate) {
                QElapsedTimer timer;
                timer.start();
                while (!predicate() && timer.elapsed() < 1600)
                    QTest::qWait(5);
                return predicate();
            };
            AggregatorServer directory;
            check(directory.start(0), "GUI registry fixture listen");
            pipeline::NumericEngine external(test_fixture::synthetic_capture_factory(false));
            pipeline::NumericControlServer endpoint(external);
            check(endpoint.listen(QHostAddress::LocalHost, 0), "External processor fixture listen");
            const auto uuid = QUuid::createUuid();
            AggregatorClient processor({uuid, ComponentType::core, endpoint.port()});
            processor.set_capabilities({{"control_version",4},{"data_version",int(bdsp_protocol::numeric_data::PayloadVersion)},{"stream_port",endpoint.port()},
                {"publisher_incarnation",QString::number(external.snapshot().epoch)},
                {"graph_revision",QString::number(external.snapshot().graph_revision)},{"streams",QJsonArray{}}});
            processor.connect_to_aggregator_server(QHostAddress::LocalHost, directory.serverPort(), 1000);
            registryHost->setText("127.0.0.1");
            window->findChild<QSpinBox *>("registryPort")->setValue(directory.serverPort());
            discover->click();
            check(waitRegistry([&] { return devices->processors().size() == 2 &&
                QUuid(devices->processors()[1].id)==uuid && devices->processors()[1].available &&
                devices->processors()[1].port == endpoint.port(); }),
                  "Registry did not expose external processor in GUI");
            check(devices->processors()[1].available && devices->processors()[1].port == endpoint.port(),
                  "Discovered endpoint metadata invalid");
            processor.disconnect_from_aggregator_server();
            check(waitRegistry([&] { return !devices->processors()[1].available; }),
                  "Offline processor remains selectable in GUI");
            processor.connect_to_aggregator_server(QHostAddress::LocalHost, directory.serverPort(), 1000);
            check(waitRegistry([&] { return devices->processors()[1].available; }),
                  "Reconnected processor not re-enabled in GUI");
            directory.stop();
            check(waitRegistry([&] { return !devices->processors()[1].available; }),
                  "Directory loss leaves stale processor enabled");
            check(oldRegistry == app._ctx.file_configs.raw_document.value("aggregator"),
                  "Registry connection rewrote user preferences");
            devices->disconnect_registry();
        }
        check(app.presenter().client().snapshot().graph_revision == revision, "Presentation changed processing graph");
        qInfo("Graphs global/per-node/apply/manual-save/unknown-field roundtrip and light/dark/system/menu/QtNodes "
              "contrast PASS");
        return 0;
    } catch (const std::exception &e) {
        qCritical("%s", e.what());
        return 1;
    }
}
