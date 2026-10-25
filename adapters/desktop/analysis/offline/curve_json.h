#pragma once
#include "profile_json.h"
#include "profile_arrays.h"

namespace desktop_analysis::offline {
QJsonObject encode_curve(const dsp_kernels::FrequencyCurve&,QJsonObject previous,ProfileArraySink* sink=nullptr);
bool decode_curve(const QJsonObject&,dsp_kernels::FrequencyProfile&,ProfileArraySource* source=nullptr,std::size_t* budget=nullptr);
}
