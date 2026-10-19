#include <core_app/plot_composition.h>
#include <visualization/ui_text.h>
#include <QJsonArray>
namespace presentation {
PlotDescriptor plot_descriptor_from_preferences(const QJsonObject& preferences,const PlotDescriptor& fallback) {
    auto result=fallback;
    const auto modes=preferences.value("plot_modes").toArray();
    if(fallback.modes.size()>1 && !modes.isEmpty()){
        QStringList selected;
        const QStringList supported{"line","points","bars","pie","image2d","surface3d","piano","metrics","signedbar"};
        for(const auto& mode:modes)if(mode.isString() && supported.contains(mode.toString()) && !selected.contains(mode.toString()))
            selected.append(mode.toString());
        if(!selected.isEmpty())result.modes=selected;
    }
    const auto path=preferences.value("plot_category_path").toArray();
    if(!path.isEmpty()){
        QStringList selected;
        for(const auto& part:path)if(part.isString()&&!part.toString().isEmpty())selected.append(part.toString());
        if(!selected.isEmpty())result.category_path=selected;
    }
    const auto specialization=preferences.value("plot_specialization").toString();
    if(fallback.modes.size()>1){
        if(specialization=="audio")result.audio_scales=true;
        if(specialization=="generic")result.audio_scales=false;
    }
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
PlotPresentation audio_plot_presentation(const signal_contracts::NumericContract& value) {
    using namespace signal_contracts;PlotPresentation result;
    result.piano_frequency_coordinates=value.coordinates[0].unit==NumericUnit::Hertz;
    result.value_scale.source_decibels=value.unit==NumericUnit::Decibels;
    const bool power=value.unit==NumericUnit::SquaredAmplitude || value.unit==NumericUnit::SquaredAmplitudePerHertz;
    result.value_scale.factor=power?10:20;
    if(value.unit==NumericUnit::RawAmplitude || value.unit==NumericUnit::NormalizedAmplitude)result.value_title="Amplitude (Y)";
    if(value.unit==NumericUnit::Radians)result.value_title="Phase, radians (Y)";
    if(value.unit==NumericUnit::SquaredAmplitude)result.value_title="Power (Y)";
    if(value.unit==NumericUnit::SquaredAmplitudePerHertz)result.value_title="PSD (Y)";
    if(value.unit==NumericUnit::Decibels)result.value_title="Level, dB (Y)";
    if(value.quantity==Quantity::NoteSalience)result.value_title="Note salience (Y)";
    const auto axis=value.shape.rank==Rank::Matrix?1:0;
    if(value.coordinates[axis].unit==NumericUnit::Hertz){result.vector_x_title="Frequency, Hz (X)";result.column_title="Frequency, Hz (Z)";}
    if(value.quantity==Quantity::NoteSalience)result.column_title="Note salience, MIDI (Z)";
    if(value.coordinates[0].unit==NumericUnit::Seconds)result.row_title="Time, seconds (X)";
    if(value.semantic==Semantic::History){result.row_title="Time delay, seconds (X)";result.row_index_title="Temporal frame (X)";}
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
