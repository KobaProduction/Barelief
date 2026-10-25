#pragma once
#include "profile_json.h"
#include "profile_arrays.h"

namespace desktop_analysis::offline {
QJsonObject encode_profile_object(const ProfileDocument&,ProfileArraySink* sink=nullptr,bool coefficients=true);
bool decode_profile_object(const QJsonObject&,ProfileDocument&,QString&,ProfileReadDiagnostic&,ProfileArraySource* source=nullptr);
QJsonObject encode_scan_object(const ProfileDocument&,ProfileArraySink* sink=nullptr);
bool decode_scan_object(QJsonObject,ProfileDocument&,QString&,ProfileArraySource* source=nullptr);
}
