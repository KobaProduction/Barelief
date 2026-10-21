#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/product_identity.h>
#include <desktop/numeric_project_document.h>
#include <barelief/ui_text.h>
#include "synthetic_capture.h"
#include <QAction>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
void write_empty_project(const QString& path) {
    domain::NumericProcessingGraph graph;graph.graph_id="branding";
    desktop::NumericProjectDocument document;
    require(bool(desktop::create_numeric_startup_project(graph,document)),"create isolated project");
    require(bool(desktop::save_numeric_project(document,path)),"save isolated project");
}
MainWindow& main_window() {
    const auto widgets=QApplication::topLevelWidgets();
    for(auto* widget:widgets)
        if(auto* window=qobject_cast<MainWindow*>(widget))return *window;
    throw std::runtime_error("No main window");
}
void check_identity(MainWindow& window) {
    require(QApplication::applicationName()=="Barelief","application name");
    require(QApplication::organizationName()=="Koba Production","publisher");
    require(QApplication::applicationDisplayName()==barelief::title,"display name");
    require(window.windowTitle()==barelief::title,"window title");
    require(!window.windowIcon().pixmap(32,32).isNull(),"window icon");
    require(!QApplication::windowIcon().pixmap(256,256).isNull(),"application icon");
    require(QApplication::windowIcon().pixmap(32,32).toImage().pixelColor(16,16).alpha()>0,"rendered SVG pixels");
    for(const auto* path:{":/barelief/LICENSE",":/barelief/packaging/licenses/Qt/LGPL-3.0-only.txt"}){
        QFile file(path);require(file.open(QIODevice::ReadOnly)&&file.size()>1000,"embedded licenses");
    }
}
void render_about(MainWindow& window,const QString& language,const QString& theme) {
    ui::Language::instance().set(language);ui::Theme::instance().set(theme);
    auto* action=window.findChild<QAction*>("aboutBareliefAction");require(action,"About action");action->trigger();
    auto* dialog=window.findChild<QDialog*>("bareliefAboutDialog");require(dialog&&dialog->isVisible(),"About dialog");
    require(dialog->windowTitle()==ui::text("About Barelief"),"About translation");
    require(!dialog->windowIcon().isNull(),"inherited dialog icon");
    require(window.windowTitle()==barelief::title,"stable title after language change");
    const auto output=qEnvironmentVariable("BARELIEF_EVIDENCE_DIR");
    if(!output.isEmpty()){
        QDir().mkpath(output);QImage image(dialog->size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
        dialog->render(&image);require(image.save(output+"/about-"+language+"-"+theme+".png"),"About renderer image");
    }
    dialog->hide();
}
void check_appearance(MainWindow& window) {
    for(const auto* language:{"en","ru"})
        for(const auto* theme:{"light","dark"})render_about(window,QString::fromLatin1(language),QString::fromLatin1(theme));
}
}
int main() {
    QTemporaryDir directory;require(directory.isValid(),"test directory");
    auto config=(directory.path()+"/config.json").toLocal8Bit();
    write_empty_project(QString::fromLocal8Bit(config));
    char executable[]="product_branding_tests",option[]="--config";
    char* arguments[]={executable,option,config.data(),nullptr};int count=3;
    CoreApplication app(count,arguments,test_fixture::synthetic_capture_factory());
    app._ctx.file_configs.ui.autosave_enabled=false;
    auto& window=main_window();check_identity(window);check_appearance(window);
    std::puts("Barelief identity, icons, embedded licenses and RU/EN Light/Dark About PASS");
    return 0;
}
