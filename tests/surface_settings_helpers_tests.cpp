#include "visualization/surface_settings.h"
#include "visualization/camera_controls.h"
#include <cstdio>
#include <cmath>

// Pure settings contracts; deliberately no QApplication, dialogs, timers or GL.
int main() {
    using namespace SurfaceSettings;
    auto check = [](bool valid, const char *message) {
        if (!valid)
            std::fprintf(stderr, "FAIL: %s\n", message);
        return valid;
    };
    const auto initial = normalized({});
    if (!check(initial["background_mode"] == "auto" && initial["mesh_color"] == "auto" &&
                   initial["axis_precision"].toInt() == 0 && !initial["reverse_amplitude"].toBool() &&
                   initial["y_range_mode"] == "auto",
               "Automatic/theme defaults"))
        return 1;
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
    const auto invalid = normalized(QJsonObject{{"axis_precision", 1e300}, {"y_min", 4}, {"y_max", 2}});
    if (!check(invalid["axis_precision"].toInt() == 8 && invalid["y_min"].toDouble() == 0 &&
                   invalid["y_max"].toDouble() == 1 && renderFps(QJsonObject{{"gui_target_fps", 1e300}}) == 240 &&
                   renderFps(QJsonObject{{"gui_target_fps", 30}}) == 30 &&
                   renderFps(QJsonObject{{"gui_fps_mode", "screen"}}, 59.94) == 60,
               "Axis/FPS bounds and invalid Y range fallback"))
        return 1;
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
    if (!check(original[0].toObject()["name"] == "Existing" && updated.size() == 2 && entry["id"] == "first" &&
                   entry["name"] == "Renamed" && entry["entry_extension"].toInt() == 42 &&
                   values["value_extension"].toInt() == 17 &&
                   values["gradient_stops"].toArray()[0].toObject()["extension"].toInt() == 7 &&
                   gradientStops(values)[1].first == .123456789,
               "Preset transaction preserves ID, unknown fields and exact custom positions"))
        return 1;
    const auto added = saveColorPreset(updated, "second", "New", QJsonObject{{"gradient", "ice_fire"}});
    const auto removed = removeColorPreset(added, "first");
    if (!check(added.size() == 3 && removed.size() == 2 && removed[0].toObject()["unknown_entry"].toInt() == 91 &&
                   removed[1].toObject()["id"] == "second" && removeColorPreset(added, "missing") == added &&
                   saveColorPreset(added, "bad", "  ", colors) == added,
               "Preset add/delete preserve unrelated and unknown entries"))
        return 1;
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
    if (!check(effective(QJsonObject{{"defaults", QJsonObject{{"axis_precision", 7}}}}, oldOverride)["axis_precision"]
                           .toInt() == 7 &&
                   oldOverride["surface_override"].toObject()["values"].toObject()["axis_precision"].toInt() == 2,
               "Global precision wins without mutating stored view"))
        return 1;
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
                   normalizedControls["forward"].toArray()[0].toObject()["binding_extension"].toInt() == 42 &&
                   normalizedControls["movement_speed"].toDouble() == 4.123456,
               "Common speed bounds/defaults preserve settings and binding extensions"))
        return 1;
    const auto resetControls = CameraControls::restoreDefaultBindings(normalizedControls);
    if (!check(resetControls["movement_speed"] == normalizedControls["movement_speed"] &&
                   resetControls["extension"] == normalizedControls["extension"],
               "Restoring keys erased common speed or extensions"))
        return 1;
    std::puts("PASS: pure surface settings, precision/Y/cadence bounds, color presets and unknown metadata");
    return 0;
}
