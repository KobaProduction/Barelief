#include <core_app/plot_composition.h>
#include <barelief/ui_text.h>
#include <QJsonArray>
namespace presentation {
namespace {
QStringList preferred_plot_modes(const QJsonArray& modes) {
    QStringList selected;
    const QStringList supported{"line","points","bars","pie","image2d","surface3d","piano","metrics","signedbar"};
    for(const auto& mode:modes)if(mode.isString() && supported.contains(mode.toString()) && !selected.contains(mode.toString()))
        selected.append(mode.toString());
    return selected;
}
QStringList preferred_category_path(const QJsonArray& path) {
    QStringList selected;
    for(const auto& part:path)if(part.isString()&&!part.toString().isEmpty())selected.append(part.toString());
    return selected;
}
void apply_plot_specialization(PlotDescriptor& result,const QJsonObject& preferences) {
    const auto specialization=preferences.value("plot_specialization").toString();
    if(specialization=="audio")result.audio_scales=true;
    if(specialization=="generic")result.audio_scales=false;
}
}
PlotDescriptor plot_descriptor_from_preferences(const QJsonObject& preferences,const PlotDescriptor& fallback) {
    auto result=fallback;
    if(fallback.modes.size()>1) {
        const auto selected=preferred_plot_modes(preferences.value("plot_modes").toArray());
        if(!selected.isEmpty())result.modes=selected;
    }
    const auto path=preferred_category_path(preferences.value("plot_category_path").toArray());
    if(!path.isEmpty())result.category_path=path;
    if(fallback.modes.size()>1)apply_plot_specialization(result,preferences);
    return result;
}
QJsonObject plot_descriptor_preferences(const PlotDescriptor& descriptor) {
    QJsonArray modes,path;
    for(const auto& mode:descriptor.modes)modes.append(mode);
    for(const auto& part:descriptor.category_path)path.append(part);
    return {{"plot_modes",modes},{"plot_category_path",path},
        {"plot_specialization",descriptor.audio_scales?"audio":"generic"}};
}
PlotCapabilities plot_capabilities(const PlotDescriptor& descriptor) {
    return {descriptor.modes,descriptor.audio_scales};
}
namespace {
const char* broadband_value_title(signal_contracts::Quantity quantity) {
    using enum signal_contracts::Quantity;
    switch(quantity){
    case BroadbandEnergy:return "Sum of power-spectrum values (Y)";
    case BroadbandPower:return "Mean of power-spectrum values (Y)";
    case BroadbandRms:return "Root sum of power-spectrum values (Y)";
    case BroadbandMaximum:return "Maximum of power-spectrum values (Y)";
    case SpectralFlux:return "Positive power-spectrum flux (Y)";
    case BroadbandBinPowerSum:return "Sum of bin powers (Y)";
    case BroadbandBinPowerMean:return "Mean bin power (Y)";
    case BroadbandBinPowerRms:return "Root sum of bin powers (Y)";
    case BroadbandBinPowerMaximum:return "Maximum bin power (Y)";
    case BroadbandBinPowerFlux:return "Positive bin-power flux (Y)";
    default:return nullptr;
    }
}
QString audio_unit_title(signal_contracts::NumericUnit unit) {
    using enum signal_contracts::NumericUnit;
    if(unit==RawAmplitude || unit==NormalizedAmplitude)return "Amplitude (Y)";
    if(unit==Radians)return "Phase, radians (Y)";
    if(unit==SquaredAmplitude)return "Power (Y)";
    if(unit==SquaredAmplitudePerHertz)return "PSD (Y)";
    if(unit==Decibels)return "Level, dB (Y)";
    return {};
}
QString audio_value_title(const signal_contracts::NumericContract& value) {
    if(const auto* title=broadband_value_title(value.quantity))return title;
    if(value.quantity==signal_contracts::Quantity::NoteSalience)return "Note salience (Y)";
    return audio_unit_title(value.unit);
}
void apply_audio_axis_titles(const signal_contracts::NumericContract& value,PlotPresentation& result) {
    using namespace signal_contracts;
    const auto axis=value.shape.rank==Rank::Matrix?1:0;
    if(value.coordinates[axis].unit==NumericUnit::Hertz){result.vector_x_title="Frequency, Hz (X)";result.column_title="Frequency, Hz (Z)";}
    if(value.quantity==Quantity::NoteSalience)result.column_title="Note salience, MIDI (Z)";
    if(value.coordinates[0].unit==NumericUnit::Seconds)result.row_title="Time, seconds (X)";
    if(value.semantic==Semantic::History){result.row_title="Time delay, seconds (X)";result.row_index_title="Temporal frame (X)";}
}
}
PlotPresentation audio_plot_presentation(const signal_contracts::NumericContract& value) {
    using namespace signal_contracts;PlotPresentation result;
    result.piano_frequency_coordinates=value.coordinates[0].unit==NumericUnit::Hertz;
    result.value_scale.source_decibels=value.unit==NumericUnit::Decibels;
    const bool power=value.unit==NumericUnit::SquaredAmplitude || value.unit==NumericUnit::SquaredAmplitudePerHertz;
    result.value_scale.factor=power?10:20;
    const auto title=audio_value_title(value);
    if(!title.isEmpty())result.value_title=title;
    apply_audio_axis_titles(value,result);
    return result;
}
QStringList plot_category_path(const PlotDescriptor& descriptor) {return descriptor.category_path;}
QString explicit_plot_sections(const QJsonArray& sections) {
    QStringList labels;
    for(const auto& item:sections){const auto section=item.toObject();const auto count=section.value("count").toInteger();
        if(count<=0)continue;
        const auto first=section.value("begin").toInteger();
        labels.append(ui::text(section.value("label").toString().toUtf8().constData())+QStringLiteral(": %1-%2").arg(first).arg(first+count-1));
    }
    return labels.join(QStringLiteral("  /  "));
}
}
