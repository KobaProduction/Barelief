#include "editor_ui_test_fixture.h"
#include "isolated_gui_platform.h"
#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <control/numeric_desktop_host.h>
#include <pipeline/numeric_engine.h>
#include <QAction>
#include <QMenu>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}

struct ReferencePreset {const char* id;int output_node;unsigned short output_port;};
constexpr std::array presets{
    ReferencePreset{"aubio-reference",33,4},
    ReferencePreset{"reference-btrack",34,1},
    ReferencePreset{"madmom-superflux-crf",4,0},
    ReferencePreset{"essentia-reference-multifeature",61,0}
};

bool available_output(const pipeline::DeliveredNumericFrame& output) {
    if(output.status!=signal_contracts::Status::ok || !output.frame)return false;
    const auto frame=output.frame.view();
    const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);
    return frame.stamp.available && numeric && numeric->actual[0] && numeric->actual[1];
}

bool wait_for_output(pipeline::NumericEngine& engine,pipeline::EndpointIdentity endpoint) {
    auto ready=std::make_shared<std::atomic_bool>(false);
    const auto token=engine.subscribe_numeric_tap({std::move(endpoint)},[ready](const auto& output){
        if(available_output(output))ready->store(true);
    },{});
    require(token!=0,"Output subscription rejected");
    const bool published=editor_ui_test_fixture::wait_until([&]{return ready->load();},16000);
    engine.unsubscribe_numeric_tap(token);
    return published;
}

void verify_preset(CoreApplication& app,MainWindow& window,const ReferencePreset& preset) {
    std::fprintf(stderr,"GUI preset %s\n",preset.id);
    auto* action=window.findChild<QAction*>(QStringLiteral("example-")+QString::fromLatin1(preset.id));
    require(action && action->isEnabled(),"Research preset QAction missing");
    QSignalSpy applied(&app.presenter(),&GuiPresenter::graph_apply_finished);
    action->trigger();
    require(editor_ui_test_fixture::wait_until([&]{return !applied.isEmpty();},12000),"Research preset Apply did not finish");
    require(applied.last()[1].toBool(),"Research preset Apply rejected");
    auto& host=dynamic_cast<pipeline::NumericDesktopHost&>(app.presenter().client());
    const auto graph=host.snapshot().graph.value("graph_id").toString();
    const pipeline::EndpointIdentity endpoint{graph.toStdString(),preset.output_node,
        pipeline::PortDirection::Output,preset.output_port};
    require(host.local_engine() && wait_for_output(*host.local_engine(),endpoint),
        "Applied research preset did not publish its result");
}

void run_presets(CoreApplication& app) {
    auto* window=editor_ui_test_fixture::main_window(require);
    const auto* menu=window->findChild<QMenu*>("rhythmReferencePresetsMenu");
    require(menu && menu->actions().size()==qsizetype(presets.size()),"Expected exactly four reference presets");
    require(editor_ui_test_fixture::wait_until([&]{return app.presenter().client().snapshot().graph_revision>0 &&
        !app.presenter().graph_request_pending();},10000),"Startup graph unavailable");
    for(const auto& preset:presets)verify_preset(app,*window,preset);
}
}

int main() {
    if(!test_fixture::isolated_gui_platform())return 2;
    try {
        QTemporaryDir directory;
        require(directory.isValid(),"Cannot create isolated project directory");
        auto path=directory.filePath("project.json").toLocal8Bit();
        char name[]="rhythm_reference_gui_tests",option[]="--config";
        char* argv[]={name,option,path.data(),nullptr};
        int argc=3;
        CoreApplication app(argc,argv,test_fixture::synthetic_capture_factory(true));
        require(test_fixture::isolated_gui_platform(),"Expected offscreen Qt platform");
        run_presets(app);
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what());
        return 1;
    }
}
