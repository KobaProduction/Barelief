#pragma once
#include <QJsonObject>
#include <QColor>
#include <QGradient>
#include <QJsonArray>
#include <QStringList>
#include <map>
#include <algorithm>
#include <cmath>

namespace SurfaceSettings {
inline QJsonArray normalizedStops(const QJsonArray &input) {
    if (input.isEmpty())
        return {};
    if (input.size() < 2 || input.size() > 16)
        return {};
    std::map<double, QJsonObject> sorted;
    for (const auto &entry : input) {
        auto stop = entry.toObject();
        const auto position = stop.value("position");
        const double p = position.toDouble(-1);
        const QColor color(stop.value("color").toString());
        if (!position.isDouble() || !std::isfinite(p) || p < 0 || p > 1 || !color.isValid())
            return {};
        if (sorted.count(p))
            return {};
        stop["position"] = p;
        stop["color"] = color.name();
        sorted[p] = stop;
    }
    if (sorted.size() < 2 || sorted.begin()->first != 0 || sorted.rbegin()->first != 1)
        return {};
    QJsonArray output;
    for (const auto &[position, stop] : sorted)
        output.append(stop);
    return output;
}
inline QGradientStops gradientStops(const QJsonObject &options) {
    const auto custom = normalizedStops(options.value("gradient_stops").toArray());
    QGradientStops result;
    if (!custom.isEmpty()) {
        for (const auto &v : custom) {
            const auto s = v.toObject();
            result.append({s["position"].toDouble(), QColor(s["color"].toString())});
        }
        return result;
    }
    const auto name = options.value("gradient").toString();
    if (name == "black_yellow")
        result = {{0, Qt::black}, {.33, Qt::blue}, {.67, Qt::red}, {1, Qt::yellow}};
    else if (name == "viridis")
        result = {{0, QColor("#440154")},
                  {.25, QColor("#3b528b")},
                  {.5, QColor("#21918c")},
                  {.75, QColor("#5ec962")},
                  {1, QColor("#fde725")}};
    else if (name == "ice_fire")
        result = {{0, QColor("#00204c")},
                  {.25, QColor("#2c7bb6")},
                  {.5, QColor("#ffffbf")},
                  {.75, QColor("#f46d43")},
                  {1, QColor("#a50026")}};
    else if (name == "rainbow")
        result = {{0, QColor("#6a00a8")}, {.25, Qt::blue}, {.5, Qt::cyan}, {.75, Qt::yellow}, {1, Qt::red}};
    else
        result = {{0, Qt::darkGreen}, {.5, Qt::yellow}, {.8, Qt::red}, {1, Qt::darkRed}};
    const auto spacing = options.value("gradient_layout").toString("preset");
    if (spacing == "linear" || spacing == "log")
        for (int i = 0; i < result.size(); ++i) {
            const double p = double(i) / (result.size() - 1);
            result[i].first = spacing == "log" ? std::log10(1 + 9 * p) : p;
        }
    return result;
}

// GUI cadence is global presentation state; it never controls DSP cadence.
inline QJsonObject normalizedCadence(QJsonObject schema) {
    if (schema["gui_fps_mode"] != "screen")
        schema["gui_fps_mode"] = "fixed";
    const auto fps = schema["gui_target_fps"];
    schema["gui_target_fps"] =
        fps.isDouble() && std::isfinite(fps.toDouble()) ? int(std::clamp(std::round(fps.toDouble()), 20.0, 240.0)) : 60;
    return schema;
}
inline int renderFps(const QJsonObject &schema, double screenRefreshRate = 60) {
    const auto cadence = normalizedCadence(schema);
    if (cadence["gui_fps_mode"] != "screen")
        return cadence["gui_target_fps"].toInt(60);
    if (!std::isfinite(screenRefreshRate) || screenRefreshRate <= 0)
        screenRefreshRate = 60;
    return int(std::clamp(std::round(screenRefreshRate), 20.0, 240.0));
}

inline QJsonObject defaults() {
    return {{"mesh_grid", true},
            {"axis_grid", true},
            {"floor_grid", true},
            {"shadows", false},
            {"light", true},
            {"brightness", 1.0},
            {"light_strength", 1.0},
            {"ambient_strength", 1.0},
            {"background_mode", "auto"},
            {"background_color", "#202020"},
            {"gradient", "green_red"},
            {"gradient_layout", "preset"},
            {"axis_precision", 0},
            {"y_range_mode", "auto"},
            {"y_min", 0.0},
            {"y_max", 1.0},
            {"reverse_time", false},
            {"reverse_bands", false},
            {"reverse_amplitude", false},
            {"gradient_stops", QJsonArray{}},
            {"mesh_width", 1.0},
            {"mesh_color", "auto"},
            {"mesh_contrast", 1.0}};
}
inline QJsonObject normalized(QJsonObject value) {
    const auto base = defaults();
    for (auto it = base.begin(); it != base.end(); ++it)
        if (!value.contains(it.key()))
            value[it.key()] = it.value();
    for (const auto *key : {"mesh_grid", "axis_grid", "floor_grid", "shadows", "light", "reverse_time", "reverse_bands",
                            "reverse_amplitude"})
        if (!value[key].isBool())
            value[key] = base[key];
    auto number = [&](const char *key, double lower, double upper) {
        const auto input = value[key];
        const double n = input.toDouble(base[key].toDouble());
        value[key] = std::isfinite(n) ? std::clamp(n, lower, upper) : base[key].toDouble();
    };
    number("brightness", 0.1, 2.0);
    number("light_strength", 0.0, 2.0);
    number("ambient_strength", 0.0, 2.0);
    number("mesh_width", 0.5, 3.0);
    number("mesh_contrast", 0.0, 1.0);
    if (value["mesh_color"] != "auto" && !QColor(value["mesh_color"].toString()).isValid())
        value["mesh_color"] = "auto";
    value["gradient_stops"] = normalizedStops(value.value("gradient_stops").toArray());
    if (value["background_mode"] != "custom")
        value["background_mode"] = "auto";
    if (!QColor(value["background_color"].toString()).isValid())
        value["background_color"] = base["background_color"];
    const QStringList presets{"green_red", "black_yellow", "viridis", "ice_fire", "rainbow"};
    if (!presets.contains(value["gradient"].toString()))
        value["gradient"] = "green_red";
    if (value["gradient_layout"] != "linear" && value["gradient_layout"] != "log")
        value["gradient_layout"] = "preset";
    const auto precision = value["axis_precision"];
    value["axis_precision"] = precision.isDouble() && std::isfinite(precision.toDouble())
                                  ? int(std::clamp(precision.toDouble(), 0.0, 8.0))
                                  : 0;
    if (value["y_range_mode"] != "manual")
        value["y_range_mode"] = "auto";
    number("y_min", -1e6, 1e6);
    number("y_max", -1e6, 1e6);
    if (value["y_max"].toDouble() - value["y_min"].toDouble() < 1e-6) {
        value["y_min"] = 0.0;
        value["y_max"] = 1.0;
    }
    return value;
}
// Replace known color-preset fields without erasing extensions in the entry or values.
inline QJsonArray saveColorPreset(QJsonArray entries, const QString &id, const QString &name,
                                  const QJsonObject &colors) {
    if (id.isEmpty() || name.trimmed().isEmpty())
        return entries;
    int index = -1;
    for (int i = 0; i < entries.size(); ++i)
        if (entries[i].toObject()["id"].toString() == id) {
            index = i;
            break;
        }
    auto entry = index < 0 ? QJsonObject{} : entries[index].toObject();
    auto values = entry["values"].toObject();
    const auto normalizedColors = normalized(colors);
    for (const auto *key : {"gradient", "gradient_layout", "gradient_stops"})
        values[key] = normalizedColors[key];
    entry["id"] = id;
    entry["name"] = name.trimmed();
    entry["values"] = values;
    if (index < 0)
        entries.append(entry);
    else
        entries[index] = entry;
    return entries;
}
inline QJsonArray removeColorPreset(QJsonArray entries, const QString &id) {
    if (id.isEmpty())
        return entries;
    for (int i = entries.size() - 1; i >= 0; --i)
        if (entries[i].toObject()["id"].toString() == id)
            entries.removeAt(i);
    return entries;
}

inline QJsonObject effective(const QJsonObject &schema, const QJsonObject &view = {}) {
    auto result = normalized(schema.value("defaults").toObject());
    const QJsonValue globalPrecision = result.value("axis_precision");
    const auto override = view.value("surface_override").toObject();
    if (override.value("enabled").toBool()) {
        const auto values = override.value("values").toObject();
        for (auto it = values.begin(); it != values.end(); ++it)
            result[it.key()] = it.value();
    }
    // Precision is an application preference; retained legacy per-view keys are ignored.
    result["axis_precision"] = globalPrecision;
    return normalized(result);
}
} // namespace SurfaceSettings
