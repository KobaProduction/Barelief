#include "numeric_preview_cadence_fixture.h"
#include <QApplication>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

// Opt-in native delivery clock evidence, separate from offscreen/functional
// CTest. Owns one bounded nonactivating window and no capture/audio resources.
int main(int argc,char** argv){
 QApplication app(argc,argv);
 if(QApplication::platformName()!=QStringLiteral("windows")){
  std::fprintf(stderr,"Native Windows platform is required; current=%s\n",qPrintable(QApplication::platformName()));return 2;
 }
 QWidget window;window.setAttribute(Qt::WA_ShowWithoutActivating);
 window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
 window.setWindowTitle(QStringLiteral("Numeric preview cadence probe"));window.resize(320,120);
 auto* layout=new QVBoxLayout(&window);layout->addWidget(new QLabel(QStringLiteral("Bounded native preview clock measurement\nNo audio or desktop capture"),&window));
 window.show();QCoreApplication::processEvents();
 try{cadence_fixture::preview_cadence(true);std::puts("Native preview cadence120 PASS");return 0;}
 catch(const std::exception& error){std::fprintf(stderr,"Native preview cadence120 FAIL: %s\n",error.what());return 1;}
}
