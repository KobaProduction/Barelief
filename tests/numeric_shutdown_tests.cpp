#include "isolated_gui_platform.h"
#include "synthetic_capture.h"
#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/numeric_default_graph.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QProcess>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QFile>
#include <QtNodes/DataFlowGraphModel>
#include <stdexcept>
#include <cstdio>
static void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
static QByteArray read(const QString&path){QFile f(path);require(f.open(QIODevice::ReadOnly),"read shutdown file");return f.readAll();}
static void shutdown_child(char** argv) {
  require(test_fixture::isolated_gui_platform(),"isolated offscreen only");
  QByteArray path=argv[2];char name[]="shutdown-child",option[]="--config";char* args[]{name,option,path.data(),nullptr};int count=3;
  QElapsedTimer unwind;
  try{CoreApplication app(count,args,test_fixture::synthetic_capture_factory(false));
   MainWindow* window=nullptr;const auto topLevels=QApplication::topLevelWidgets();for(auto*w:topLevels)if(auto* m=qobject_cast<MainWindow*>(w))window=m;
   require(window,"shutdown window");window->show();QCoreApplication::processEvents();
   app._ctx.file_configs.ui.language="en";
   app._ctx.data_flow_graph->setNodeData(1,QtNodes::NodeRole::Position,QPointF(731,47));
   app._ctx.editor_draft_modified=true;
   unwind.start();std::puts("BARELIEF_SHUTDOWN_READY");std::fflush(stdout);
   throw std::runtime_error("intentional shutdown unwind");
  }catch(const std::runtime_error&e){require(QByteArray(e.what())=="intentional shutdown unwind","unexpected child error");}
  require(unwind.elapsed()<=6000,"shutdown child unwind exceeded six seconds");
  std::fprintf(stderr,"shutdown child mode=%s unwind_ms=%lld\n",argv[3],static_cast<long long>(unwind.elapsed()));
}
static bool wait_shutdown_ready(QProcess& child) {
 QElapsedTimer elapsed;elapsed.start();QByteArray output;
 while(elapsed.elapsed()<10000) {
  output+=child.readAllStandardOutput();
  if(output.contains("BARELIEF_SHUTDOWN_READY"))return true;
  const int remaining=10000-int(elapsed.elapsed());
  if(remaining<=0 || !child.waitForReadyRead(remaining))break;
 }
 output+=child.readAllStandardOutput();
 return output.contains("BARELIEF_SHUTDOWN_READY");
}
static void child_failure(QProcess& child,const char* message) {
 child.kill();child.waitForFinished();
 std::fprintf(stderr,"%s\n",child.readAllStandardError().constData());
 throw std::runtime_error(message);
}
static void run_shutdown_child(const QString& executable,const QString& path,const QString& mode) {
 QProcess child;child.start(executable,{"--child",path,mode});
 require(child.waitForStarted(3000),"shutdown child start");
 if(!wait_shutdown_ready(child))child_failure(child,"shutdown child initialization timeout");
 QElapsedTimer elapsed;elapsed.start();
 if(child.state()!=QProcess::NotRunning && !child.waitForFinished(6000))child_failure(child,"shutdown child unwind timeout");
 std::fprintf(stderr,"shutdown mode=%s unwind_ms=%lld\n",qPrintable(mode),static_cast<long long>(elapsed.elapsed()));
 if(child.exitStatus()!=QProcess::NormalExit||child.exitCode()!=0)child_failure(child,"shutdown child failure");
}
static void persisted_shutdown(const QString& path,bool autosave,bool protectedFile,const QByteArray& original) {
  const auto saved=read(path);if(!autosave||protectedFile)require(saved==original,"disabled/protected autosave wrote on unwind");
  else{desktop::NumericProjectDocument reloaded;require(bool(desktop::read_numeric_project(saved,reloaded)),"shutdown canonical reload");desktop::NumericDraftPresentation actual;require(bool(desktop::read_numeric_presentation(reloaded,actual)),"shutdown presentation");
   QJsonObject settings;require(bool(desktop::read_numeric_application_settings(reloaded,settings)),"shutdown application settings");const auto ui=settings["ui"].toObject();require(ui["language"]=="en","pending preferences lost");require(actual.positions["1"].toObject()["x"].toDouble()==731,"pending editor position lost");require(ui["workspace"].toObject()["version"].toInt()==1&&!ui["workspace"].toObject()["ads_state"].toString().isEmpty(),"pending workspace lost");}
}
int main(int argc,char**argv){try{
 if(argc==4&&QByteArray(argv[1])=="--child") {
  shutdown_child(argv);
  return 0;
 }
 QCoreApplication parent(argc,argv);QTemporaryDir directory;require(directory.isValid(),"shutdown isolation");
 const QStringList modes{"enabled","disabled","protected"};for(const auto&mode:modes) {
  const bool autosave=mode!="disabled",protectedFile=mode=="protected";
  desktop::NumericDraftPresentation presentation;presentation.application_settings={{"ui",QJsonObject{{"language","ru"},{"autosave_enabled",autosave},{"pipeline_auto_apply",false}}}};
  desktop::NumericProjectDocument source;require(bool(desktop::create_numeric_project(numeric_default_graph(),presentation,source)),"shutdown project");
  const auto path=directory.filePath(mode+".json");require(bool(desktop::save_numeric_project(source,path)),"save shutdown fixture");
  if(protectedFile){auto object=QJsonDocument::fromJson(read(path)).object();object["schema_version"]=99;const auto bytes=QJsonDocument(object).toJson(QJsonDocument::Compact);QFile f(path);require(f.open(QIODevice::WriteOnly),"future fixture");require(f.write(bytes)==bytes.size(),"write future fixture");}
  const auto original=read(path);run_shutdown_child(parent.applicationFilePath(),path,mode);
  persisted_shutdown(path,autosave,protectedFile,original);
 }
 std::puts("shutdown enabled/disabled/protected exception unwind PASS");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
