#include "barelief/surface_settings.h"
#include "barelief/camera_controls.h"
#include "barelief/camera_geometry.h"
#include <cstdio>
#include <cmath>

static bool check(bool valid,const char* message) {
 if(!valid)std::fprintf(stderr,"FAIL: %s\n",message);
 return valid;
}
// Pure settings contracts; deliberately no QApplication, dialogs, timers or GL.
static int camera_bindings() {
 using namespace SurfaceSettings;
    const QJsonObject controls{
        {"movement_speed", 4.123456},
        {"extension", 99},
        {"forward", QJsonArray{QJsonObject{{"scan", 17}, {"key", Qt::Key_W}, {"binding_extension", 42}}}}};
    const auto normalizedControls = CameraControls::normalized(controls);
    if (!check(CameraControls::movementSpeed({}) == 3 &&
                   CameraControls::movementSpeed(QJsonObject{{"movement_speed", 1e300}}) == 10 &&
                   CameraControls::movementSpeed(QJsonObject{{"movement_speed", -.1}}) == .5 &&
                   CameraControls::movementSpeed(QJsonObject{{"movement_speed", "bad"}}) == 3 &&
                   normalizedControls["extension"].toInt() == 99 &&
                   normalizedControls["forward"].toArray().at(0).toObject().value("binding_extension").toInt() == 42 &&
                   normalizedControls["movement_speed"].toDouble() == 4.123456,
               "Common speed bounds/defaults preserve settings and binding extensions"))
        return 1;
    const auto resetControls = CameraControls::restoreDefaultBindings(normalizedControls);
    const auto up=CameraControls::defaults().value("up").toArray().at(0).toObject();
    const auto down=CameraControls::defaults().value("down").toArray().at(0).toObject();
    const auto forward=CameraControls::defaults().value("forward").toArray().at(0).toObject();
    const QKeyEvent space(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier);
    const QKeyEvent ctrlSpace(QEvent::KeyPress,Qt::Key_Space,Qt::ControlModifier);
    const QKeyEvent ctrl(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier);
    const QKeyEvent ctrlW(QEvent::KeyPress,Qt::Key_W,Qt::ControlModifier);
    const QKeyEvent shiftW(QEvent::KeyPress,Qt::Key_W,Qt::ShiftModifier);
    const QKeyEvent ctrlShift(QEvent::KeyPress,Qt::Key_Shift,Qt::ShiftModifier|Qt::ControlModifier);
    const auto fast=CameraControls::defaults().value("fast").toArray().at(0).toObject();
    if(!check(CameraControls::matches(up,space) && !CameraControls::matches(down,space) &&
              CameraControls::matches(down,ctrlSpace) && !CameraControls::matches(up,ctrlSpace) &&
              !CameraControls::matches(down,ctrl) && !CameraControls::matches(forward,ctrlW) &&
              CameraControls::matches(forward,shiftW) && CameraControls::matches(fast,ctrlShift,true) &&
              !CameraControls::same(up,down) &&
              CameraControls::label(57,Qt::Key_Space,Qt::ControlModifier)=="Ctrl+Space" &&
              CameraControls::normalized({{"down",QJsonArray{CameraControls::binding(29,Qt::Key_Control)}}}).value("down")==CameraControls::defaults().value("down"),
              "Space/Ctrl+Space are distinct; Ctrl shortcuts remain available and Shift acceleration works"))return 1;
    if (!check(resetControls["movement_speed"] == normalizedControls["movement_speed"] &&
                   resetControls["extension"] == normalizedControls["extension"],
               "Restoring keys erased common speed or extensions"))
        return 1;
 return 0;
}

static int camera_geometry() {
    for(const float yaw:{-170.f,-90.f,0.f,45.f,90.f,170.f})for(const float pitch:{-89.f,-60.f,0.f,60.f,89.f}) {
        const auto orientation=QQuaternion::fromEulerAngles(pitch,yaw,0);
        const auto forward=CameraGeometry::horizontalMovement(orientation,{0,0,-1});
        auto expected=orientation.rotatedVector({0,0,-1});expected.setY(0);expected.normalize();
        const auto backward=CameraGeometry::horizontalMovement(orientation,{0,0,1});
        const auto right=CameraGeometry::horizontalMovement(orientation,{1,0,0});
        if(!check((forward-expected).length()<1e-4 && std::abs(forward.y())<1e-6 &&
                  (forward+backward).length()<1e-5 && std::abs(QVector3D::dotProduct(forward,right))<1e-5 &&
                  std::abs(forward.length()-1)<1e-5 &&
                  (CameraGeometry::horizontalMovement(orientation,{0,1,0})-QVector3D(0,1,0)).length()<1e-6,
                  "WASD follows heading at full speed without changing height at any pitch"))return 1;
        for(const float drag:{-720.f,720.f}) {
            const auto look=CameraGeometry::uprightLook(orientation,30,drag);
            if(!check(look.rotatedVector({0,1,0}).y()>0 &&
                      std::abs(QVector3D::dotProduct(look.rotatedVector({1,0,0}),QVector3D(0,1,0)))<1e-5,
                      "Large mouse drags cannot invert or roll the camera"))return 1;
        }
    }
    const QVector3D edge{6,8,0};
    const auto clipped=CameraGeometry::translatedPosition(edge,{1,0,0});
    if(!check(clipped.y()==edge.y() && clipped.length()<=10.00001f,
              "Horizontal movement keeps height at the camera distance limit"))return 1;
return 0;
}

static int color_presets() {
using namespace SurfaceSettings;
    const QJsonArray stops{QJsonObject{{"position", 0}, {"color", "#123456"}, {"extension", 7}},
                           QJsonObject{{"position", .123456789}, {"color", "#abcdef"}},
                           QJsonObject{{"position", 1}, {"color", "#ffffff"}}};
    const QJsonObject colors{{"gradient", "viridis"}, {"gradient_layout", "log"}, {"gradient_stops", stops}};
    QJsonArray original{QJsonObject{{"id", "first"},
                                    {"name", "Existing"},
                                    {"entry_extension", 42},
                                    {"values", QJsonObject{{"value_extension", 17}, {"gradient", "green_red"}}}},
                        QJsonObject{{"unknown_entry", 91}}};
    const auto updated = saveColorPreset(original, "first", " Renamed ", colors);
    const auto entry = updated[0].toObject(), values = entry["values"].toObject();
    if (!check(original[0].toObject().value("name") == "Existing" && updated.size() == 2 && entry["id"] == "first" &&
                   entry["name"] == "Renamed" && entry["entry_extension"].toInt() == 42 &&
                   values["value_extension"].toInt() == 17 &&
                   values["gradient_stops"].toArray().at(0).toObject().value("extension").toInt() == 7 &&
                   gradientStops(values).at(1).first == .123456789,
               "Preset transaction preserves ID, unknown fields and exact custom positions"))
        return 1;
    const auto added = saveColorPreset(updated, "second", "New", QJsonObject{{"gradient", "ice_fire"}});
    const auto removed = removeColorPreset(added, "first");
    if (!check(added.size() == 3 && removed.size() == 2 && removed[0].toObject().value("unknown_entry").toInt() == 91 &&
                   removed[1].toObject().value("id") == "second" && removeColorPreset(added, "missing") == added &&
                   saveColorPreset(added, "bad", "  ", colors) == added,
               "Preset add/delete preserve unrelated and unknown entries"))
        return 1;
return 0;
}

int main() {
    using namespace SurfaceSettings;
    if(const auto result=camera_geometry())return result;
    const auto initial = normalized({});
    if(!check(initial["auto_range_interval_seconds"].toDouble()==10 &&
        normalized({{"auto_range_interval_seconds",0}}).value("auto_range_interval_seconds").toDouble()==1 &&
        normalized({{"auto_range_interval_seconds",120}}).value("auto_range_interval_seconds").toDouble()==60 &&
        normalized({{"auto_range_interval_seconds","bad"}}).value("auto_range_interval_seconds").toDouble()==10,
        "Automatic amplitude interval defaults and validated seconds bounds"))return 1;
    if (!check(initial["background_mode"] == "auto" && initial["mesh_color"] == "auto" &&
                   initial["axis_precision"].toInt() == 0 && !initial["reverse_amplitude"].toBool() &&
                   initial["y_range_mode"] == "auto",
               "Automatic/theme defaults"))
        return 1;
    const auto aspect=normalized(QJsonObject{{"scene_aspect_mode","unknown"},{"scene_time_aspect",100},
        {"scene_band_aspect",-.1},{"extension",42}});
    if(!check(initial["scene_aspect_mode"]=="fixed" && initial["scene_time_aspect"].toDouble()==2 &&
        initial["scene_band_aspect"].toDouble()==1 && aspect["scene_time_aspect"].toDouble()==4 &&
        aspect["scene_band_aspect"].toDouble()==.25 && aspect["extension"].toInt()==42,
        "Visual aspect defaults, bounds and extensions"))return 1;
    const auto inherited=effective(QJsonObject{{"defaults",QJsonObject{{"scene_time_aspect",3}}}},
        QJsonObject{{"surface_override",QJsonObject{{"enabled",true},{"values",QJsonObject{
            {"scene_aspect_mode","sample_counts"},{"scene_band_aspect",.5},{"extension",77}}}}}});
    if(!check(inherited["scene_time_aspect"].toDouble()==3 && inherited["scene_band_aspect"].toDouble()==.5 &&
        inherited["scene_aspect_mode"]=="sample_counts" && inherited["extension"].toInt()==77,
        "Per-view scene aspect inherits unrelated global settings"))return 1;
    const auto custom = normalized(QJsonObject{{"background_mode", "custom"},
                                               {"background_color", "#123456"},
                                               {"mesh_color", "#abcdef"},
                                               {"mesh_width", 1.23456},
                                               {"axis_precision", 8},
                                               {"y_range_mode", "manual"},
                                               {"y_min", -.75},
                                               {"y_max", 2.25},
                                               {"extension", 99}});
    if (!check(custom["background_color"] == "#123456" && custom["mesh_color"] == "#abcdef" &&
                   custom["mesh_width"].toDouble() == 1.23456 && custom["axis_precision"].toInt() == 8 &&
                   custom["y_min"].toDouble() == -.75 && custom["y_max"].toDouble() == 2.25 &&
                   custom["extension"].toInt() == 99,
               "Custom settings and precision preserved"))
        return 1;
    const auto defaultAppearance=defaults();
    if(!check(defaultAppearance["mesh_width"].toDouble()==.67 &&
              defaultAppearance["mesh_contrast"].toDouble()==.2,
              "Subtle spectrogram grid defaults"))return 1;
    const auto invalid = normalized(QJsonObject{{"axis_precision", 1e300}, {"y_min", 4}, {"y_max", 2}});
    if(!check(normalizedCadence({}).value("gui_fps_mode")=="screen" && renderFps({},165)==165 && renderFps({},360)==360 &&
              renderFps(QJsonObject{{"gui_fps_mode","display"},{"gui_target_fps",120}},165)==165 &&
              renderFps(QJsonObject{{"gui_fps_mode","maximum"}},360)==360 &&
              renderFps(QJsonObject{{"gui_fps_mode","fixed"},{"gui_target_fps",170}},165)==170 &&
              renderFps(QJsonObject{{"gui_target_fps",170}},165)==170,
              "Missing cadence follows OS refresh, explicit maximum and manual preferences survive"))return 1;
    if (!check(invalid["axis_precision"].toInt() == 8 && invalid["y_min"].toDouble() == 0 &&
                   invalid["y_max"].toDouble() == 1 && renderFps(QJsonObject{{"gui_target_fps", 1e300}}) == 1000 &&
                   renderFps(QJsonObject{{"gui_target_fps", 0}}) == 1 && renderFps({},1000)==1000 &&
                   renderFps(QJsonObject{{"gui_target_fps", 30}}) == 30 &&
                   renderFps(QJsonObject{{"gui_fps_mode", "screen"}}, 59.94) == 60,
               "Axis/FPS bounds and invalid Y range fallback"))
        return 1;
    if(const auto result=color_presets())return result;
    const auto override = effective(
        QJsonObject{{"defaults", custom}},
        QJsonObject{
            {"surface_override",
             QJsonObject{{"enabled", true},
                         {"values", QJsonObject{{"y_range_mode", "manual"}, {"y_min", .25}, {"y_max", .75}}}}}});
    if (!check(override["y_min"].toDouble() == .25 && override["background_color"] == "#123456" &&
                   override["axis_precision"].toInt() == 8,
               "Per-view Y range inherits remaining appearance"))
        return 1;
    const auto oldOverride = QJsonObject{
        {"surface_override", QJsonObject{{"enabled", true}, {"values", QJsonObject{{"axis_precision", 2}}}}}};
    if (!check(effective(QJsonObject{{"defaults", QJsonObject{{"axis_precision", 7}}}}, oldOverride).value("axis_precision")
                           .toInt() == 7 &&
                   oldOverride["surface_override"].toObject().value("values").toObject().value("axis_precision").toInt() == 2,
               "Global precision wins without mutating stored view"))
        return 1;
    if(const auto result=camera_bindings())return result;
    std::puts("PASS: pure surface settings, precision/Y/cadence bounds, color presets and unknown metadata");
    return 0;
}
