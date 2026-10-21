#include "node_field_builder.h"
#include <dsp_kernels/musical_features.h>
#include <array>
#include <cmath>
#include <limits>
namespace {
QString note_caption(int midi,double a4) {
    static constexpr const char* names[]{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    return QStringLiteral("%1 (%2%3, %4 Hz)").arg(midi).arg(names[(midi%12+12)%12]).arg(midi/12-1).arg(dsp_kernels::note_frequency_hz(midi,a4),0,'f',2);
}
}
void NodeParameterModel::appendReadout(QString label,QString key,std::function<QString()> read,bool primary) {
    auto field=makeField(std::move(label),std::move(key),Editor::Caption);field.primary=primary;
    field.read=[read=std::move(read)]()->Value{return read();};_fields.push_back(std::move(field));
}
QString NodeParameterModel::musicRowLayout() const {
    if(_node.inputs.empty())return ui::text("Row layout unavailable");
    const auto data=_node.inputs.front().contract.view();const auto* input=std::get_if<signal_contracts::NumericContract>(&data);
    std::array<domain::OwnedProcessingContract,4> outputs;std::array<std::size_t,4> counts{};
    if(!input || domain::musical_spectrogram_output_contracts(_node,*input,outputs,&counts)!=signal_contracts::Status::ok)return ui::text("Row layout unavailable");
    if(_node.kind=="BroadbandRow")return ui::text("[Low %1] [Mid %2] [High %3]").arg(qulonglong(counts[0])).arg(qulonglong(counts[1])).arg(qulonglong(counts[2]));
    if(_node.kind=="NoteSpectrum")return ui::text("[Notes %1]").arg(qulonglong(counts[3]));
    return ui::text("[Low %1] [Mid %2] [High %3] [Notes %4]").arg(qulonglong(counts[0])).arg(qulonglong(counts[1])).arg(qulonglong(counts[2])).arg(qulonglong(counts[3]));
}
double NodeParameterModel::parameterNumber(domain::NodeKind kind,std::string_view key) const {
    const auto* descriptor=domain::parameter_descriptor(kind,key);if(!descriptor)return std::numeric_limits<double>::quiet_NaN();
    const auto value=parameterValue(*descriptor);const auto* literal=std::get_if<signal_contracts::NumericLiteral>(&value);
    if(!literal)return std::numeric_limits<double>::quiet_NaN();
    const double number=std::visit([](auto item){return double(item);},*literal);
    if(!std::isfinite(number) || number<descriptor->minimum || number>descriptor->maximum)return std::numeric_limits<double>::quiet_NaN();
    return number;
}
QString NodeParameterModel::noteGridDescription() const {
    const double first=parameterNumber(domain::NodeKind::NoteSalience,"first_midi"),count=parameterNumber(domain::NodeKind::NoteSalience,"notes"),a4=parameterNumber(domain::NodeKind::NoteSalience,"a4_hz");
    if(!std::isfinite(first) || !std::isfinite(count) || !std::isfinite(a4))return ui::text("Note grid unavailable");
    return ui::text("MIDI %1 to %2; A4 = %3 Hz; note salience").arg(note_caption(int(first),a4),note_caption(int(first+count-1),a4),QString::number(a4,'f',2));
}
QString NodeParameterModel::frequencyProfileStatus() const {
    for(const auto& parameter:std::get<domain::TransformProcessingParameters>(_node.parameters).values)
        if(parameter.key=="gains")if(const auto* data=std::get_if<domain::NumericArray>(&parameter.value);data && !data->values.empty())return ui::text("Frequency profile attached");
    return ui::text("No frequency profile attached");
}
void NodeParameterModel::append_coefficient_summary() {
    if(_node.kind!="ArrayFill")return;
    std::size_t count=0;
    if(domain::array_generator_values(_node,{},count)==signal_contracts::Status::ok && count)
        appendReadout("Array coefficients","coefficientSummary",[this]{std::size_t count=0;domain::array_generator_values(_node,{},count);return ui::text("Explicit coefficients: %1").arg(count);},true);
}
void NodeParameterModel::append_music_fields() {
    if(_node.kind=="MusicalSpectrogram" || _node.kind=="BroadbandRow" || _node.kind=="NoteSpectrum")appendReadout("Row layout","rowLayoutReadout",[this]{return musicRowLayout();},true);
    if(_node.kind=="NoteSalience")appendReadout("Note grid","noteGridReadout",[this]{return noteGridDescription();},true);
    append_coefficient_summary();
    if(_node.kind=="FrequencyNormalizer")appendReadout("Frequency profile","frequencyProfileStatus",[this]{return frequencyProfileStatus();});
}
