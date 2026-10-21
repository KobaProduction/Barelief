#pragma once
#include <QString>
#include <QGuiApplication>

namespace test_fixture {
// X11 tests must explicitly opt in to a private Xvfb display. WSLg's :0 and
// ordinary desktop launches cannot pass this guard.
inline bool isolated_gui_platform() {
    const auto platform=qEnvironmentVariable("QT_QPA_PLATFORM");
    if(qobject_cast<QGuiApplication*>(QCoreApplication::instance()) &&
       QGuiApplication::platformName()!=platform)return false;
    if(platform=="offscreen")return true;
    if(platform!="xcb" || qEnvironmentVariable("BARELIEF_TEST_VIRTUAL_DISPLAY")!="1")return false;
    const auto display=qEnvironmentVariable("DISPLAY");
    if(!display.startsWith(':'))return false;
    bool valid=false;
    const auto number=display.mid(1).section('.',0,0).toUInt(&valid);
    return valid && number>=99;
}
}
