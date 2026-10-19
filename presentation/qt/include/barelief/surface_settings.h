#pragma once
#include <QJsonObject>
#include <QColor>
#include <QGradient>
#include <QJsonArray>
#include <QStringList>
#include <map>
#include <algorithm>
#include <cmath>
#include <utility>

namespace SurfaceSettings {
inline bool appendNormalizedStop(const QJsonValue& entry,std::map<double,QJsonObject>& sorted) {
    auto stop=entry.toObject();
    const auto position=stop.value("position");
    const double p=position.toDouble(-1);
    const QColor color(stop.value("color").toString());
    if(!position.isDouble() || !std::isfinite(p) || p<0 || p>1 || !color.isValid() || sorted.contains(p))return false;
    stop["position"]=p;
    stop["color"]=color.name();
    sorted.emplace(p,std::move(stop));
    return true;
}
inline QJsonArray normalizedStops(const QJsonArray &input) {
    if (input.size() < 2 || input.size() > 16)
        return {};
    std::map<double, QJsonObject> sorted;
    for (const auto &entry : input)if(!appendNormalizedStop(entry,sorted))return {};
    if (sorted.size() < 2 || sorted.begin()->first != 0 || sorted.rbegin()->first != 1)
        return {};
    QJsonArray output;
    for (const auto &[position, stop] : sorted)
        output.append(stop);
    return output;
}
inline QGradientStops presetGradientStops(const QString& name) {
    QGradientStops result;
    if (name == "black_yellow")
        result = {{0, Qt::black}, {.33, Qt::blue}, {.67, Qt::red}, {1, Qt::yellow}};
    else if (name == "viridis")
        result = {{0, QColor(0x44,0x01,0x54)},
                  {.25, QColor(0x3b,0x52,0x8b)},
                  {.5, QColor(0x21,0x91,0x8c)},
                  {.75, QColor(0x5e,0xc9,0x62)},
                  {1, QColor(0xfd,0xe7,0x25)}};
    else if (name == "ice_fire")
        result = {{0, QColor(0x00,0x20,0x4c)},
                  {.25, QColor(0x2c,0x7b,0xb6)},
                  {.5, QColor(0xff,0xff,0xbf)},
                  {.75, QColor(0xf4,0x6d,0x43)},
                  {1, QColor(0xa5,0x00,0x26)}};
    else if (name == "rainbow")
        result = {{0, QColor(0x6a,0x00,0xa8)}, {.25, Qt::blue}, {.5, Qt::cyan}, {.75, Qt::yellow}, {1, Qt::red}};
    else
        result = {{0, Qt::darkGreen}, {.5, Qt::yellow}, {.8, Qt::red}, {1, Qt::darkRed}};
    return result;
}
inline QGradientStops customGradientStops(const QJsonArray& custom) {
    QGradientStops result;
    for (const auto& value:custom) {
        const auto stop=value.toObject();
        result.append({stop.value("position").toDouble(),QColor(stop.value("color").toString())});
    }
    return result;
}
inline QGradientStops gradientStops(const QJsonObject &options) {
    const auto custom=normalizedStops(options.value("gradient_stops").toArray());
    if(!custom.isEmpty())return customGradientStops(custom);
    auto result=presetGradientStops(options.value("gradient").toString());
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
    const QJsonValue fps = schema.value("gui_target_fps");
    const auto mode = schema.value("gui_fps_mode");
    if (mode != "screen" && mode != "maximum" && mode != "display")
        schema["gui_fps_mode"] = mode=="fixed" || fps.isDouble()?"fixed":"maximum";
    schema["gui_target_fps"] =
        fps.isDouble() && std::isfinite(fps.toDouble()) ? int(std::clamp(std::round(fps.toDouble()), 1.0, 1000.0)) : 60;
    return schema;
}
inline int renderFps(const QJsonObject &schema, double screenRefreshRate = 60) {
    const auto cadence = normalizedCadence(schema);
    if (cadence["gui_fps_mode"] == "fixed")
        return cadence["gui_target_fps"].toInt(60);
    if (!std::isfinite(screenRefreshRate) || screenRefreshRate <= 0)
        screenRefreshRate = 60;
    return int(std::clamp(std::round(screenRefreshRate), 1.0, 1000.0));
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
            {"scene_aspect_mode", "fixed"},
            {"scene_time_aspect", 2.0},
            {"scene_band_aspect", 1.0},
            {"y_range_mode", "auto"},
            {"auto_range_interval_seconds", 10.0},
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
inline void fillMissing(QJsonObject& value,const QJsonObject& base) {
    for (auto it = base.begin(); it != base.end(); ++it)
        if (!value.contains(it.key()))
            value[it.key()] = it.value();
}
inline void normalizeBooleans(QJsonObject& value,const QJsonObject& base) {
    for (const auto *key : {"mesh_grid", "axis_grid", "floor_grid", "shadows", "light", "reverse_time", "reverse_bands",
                            "reverse_amplitude"})
        if (!value[key].isBool())
            value[key] = base[key];
}
inline void normalizeNumber(QJsonObject& value,const QJsonObject& base,const char* key,double lower,double upper) {
    const auto input=value[key];
    const double number=input.toDouble(base[key].toDouble());
    value[key]=std::isfinite(number)?std::clamp(number,lower,upper):base[key].toDouble();
}
inline void normalizeScalarSettings(QJsonObject& value,const QJsonObject& base) {
    if(value["scene_aspect_mode"]!="sample_counts")value["scene_aspect_mode"]="fixed";
    normalizeNumber(value,base,"scene_time_aspect",.25,4.0);
    normalizeNumber(value,base,"scene_band_aspect",.25,4.0);
    normalizeNumber(value,base,"auto_range_interval_seconds",1.0,60.0);
    normalizeNumber(value,base,"brightness",0.1,2.0);
    normalizeNumber(value,base,"light_strength",0.0,2.0);
    normalizeNumber(value,base,"ambient_strength",0.0,2.0);
    normalizeNumber(value,base,"mesh_width",0.5,3.0);
    normalizeNumber(value,base,"mesh_contrast",0.0,1.0);
}
inline void normalizeAppearance(QJsonObject& value,const QJsonObject& base) {
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
}
inline void normalizeRange(QJsonObject& value,const QJsonObject& base) {
    const auto precision = value["axis_precision"];
    value["axis_precision"] = precision.isDouble() && std::isfinite(precision.toDouble())
                                  ? int(std::clamp(precision.toDouble(), 0.0, 8.0))
                                  : 0;
    if (value["y_range_mode"] != "manual")
        value["y_range_mode"] = "auto";
    normalizeNumber(value,base,"y_min",-1e6,1e6);
    normalizeNumber(value,base,"y_max",-1e6,1e6);
    if (!(float(value["y_max"].toDouble()) > float(value["y_min"].toDouble()))) {
        value["y_min"] = 0.0;
        value["y_max"] = 1.0;
    }
}
inline QJsonObject normalized(QJsonObject value) {
    const auto base=defaults();
    fillMissing(value,base);
    normalizeBooleans(value,base);
    normalizeScalarSettings(value,base);
    normalizeAppearance(value,base);
    normalizeRange(value,base);
    return value;
}
// Replace known color-preset fields without erasing extensions in the entry or values.
inline int colorPresetIndex(const QJsonArray& entries,const QString& id) {
    for(int index=0;index<entries.size();++index)
        if(entries[index].toObject().value("id").toString()==id)return index;
    return -1;
}
inline QJsonObject updatedColorPreset(QJsonObject entry,const QString& id,const QString& name,const QJsonObject& colors) {
    auto values=entry.value("values").toObject();
    const auto normalizedColors=normalized(colors);
    for(const auto* key:{"gradient","gradient_layout","gradient_stops"})
        values[key]=normalizedColors.value(key);
    entry["id"]=id;
    entry["name"]=name.trimmed();
    entry["values"]=values;
    return entry;
}
inline QJsonArray saveColorPreset(QJsonArray entries, const QString &id, const QString &name,
                                  const QJsonObject &colors) {
    if (id.isEmpty() || name.trimmed().isEmpty())
        return entries;
    const int index=colorPresetIndex(entries,id);
    const auto entry=updatedColorPreset(index<0?QJsonObject{}:entries[index].toObject(),id,name,colors);
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
