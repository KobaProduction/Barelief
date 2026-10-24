#pragma once
#include "profile_json.h"

namespace desktop_analysis::offline {
QJsonObject encode_curve(const dsp_kernels::FrequencyCurve&,QJsonObject previous);
bool decode_curve(const QJsonObject&,dsp_kernels::FrequencyProfile&);
}
