#include "curve_json.h"
#include <QJsonArray>
#include <cmath>

namespace desktop_analysis::offline {
namespace {
using namespace dsp_kernels;
QJsonArray values(const std::vector<double>& input) {
    QJsonArray output;for(auto value:input)output.append(value);return output;
}
bool read_values(const QJsonObject& object,const char* key,std::vector<double>& output,std::size_t maximum) {
    const auto value=object.value(key);
    if(!value.isArray() || std::size_t(value.toArray().size())>maximum)return false;
    const auto array=value.toArray();
    for(const auto& item:array){
        if(!item.isDouble() || !std::isfinite(item.toDouble()))return false;
        output.push_back(item.toDouble());
    }
    return true;
}
bool number(const QJsonObject& o,const char* key,double& result) {
    const auto value=o.value(key);result=value.toDouble();return value.isDouble() && std::isfinite(result);
}
bool read_methods(const QJsonObject& o,FrequencyCurveOptions& options) {
    const auto aggregation=o.value("aggregation").toString(),approximation=o.value("approximation").toString();
    if(aggregation=="mean_track_maxima")options.aggregation=PeakAggregation::MeanTrackMaxima;
    else if(aggregation=="global_maximum")options.aggregation=PeakAggregation::GlobalMaximum;
    else if(aggregation=="upper_percentile")options.aggregation=PeakAggregation::UpperPercentile;
    else return false;
    if(approximation=="chebyshev_log_amplitude_hz")options.approximation=CurveApproximation::Polynomial;
    else if(approximation=="pchip_log_amplitude_hz")options.approximation=CurveApproximation::PiecewiseSmooth;
    else return false;
    return true;
}
bool read_options(const QJsonObject& o,FrequencyCurveOptions& options) {
    if(!read_methods(o,options))return false;
    double degree=0,segments=0;
    if(!number(o,"degree",degree) || degree<1 || degree>12 || std::floor(degree)!=degree ||
        !number(o,"segments",segments) || segments<2 || segments>2048 || std::floor(segments)!=segments)return false;
    options.polynomial_degree=unsigned(degree);options.segments=std::size_t(segments);
    return number(o,"percentile",options.percentile) && number(o,"regularization_db",options.regularization_db) && number(o,"max_gain",options.max_gain);
}
void encode_function_data(const FrequencyCurve& c,QJsonObject& o) {
    o["minimum_hz"]=c.minimum_hz;o["maximum_hz"]=c.maximum_hz;o["fit_rmse_db"]=c.fit_rmse_db;
    o["coefficients"]=values(c.coefficients);o["knot_hz"]=values(c.knot_hz);o["log_amplitude"]=values(c.log_amplitude);o["slopes"]=values(c.slopes);
    o["measured_spectrum"]=values(c.measured_spectrum);
}
}
QJsonObject encode_curve(const dsp_kernels::FrequencyCurve& c,QJsonObject o) {
    const auto& options=c.options;
    o["version"]=1;o["domain"]="hz";o["outside_measured_range"]="unity";o["regularization"]="confidence_hypot_v1";
    o["aggregation"]=options.aggregation==PeakAggregation::MeanTrackMaxima?"mean_track_maxima":options.aggregation==PeakAggregation::GlobalMaximum?"global_maximum":"upper_percentile";
    o["approximation"]=options.approximation==CurveApproximation::Polynomial?"chebyshev_log_amplitude_hz":"pchip_log_amplitude_hz";
    o["percentile"]=options.percentile;o["regularization_db"]=options.regularization_db;o["max_gain"]=options.max_gain;
    o["degree"]=int(options.polynomial_degree);o["segments"]=double(options.segments);
    encode_function_data(c,o);return o;
}
bool decode_curve(const QJsonObject& o,dsp_kernels::FrequencyProfile& profile) {
    if(o.value("version").toDouble()!=1 || o.value("domain").toString()!="hz" ||
        o.value("outside_measured_range").toString()!="unity" || o.value("regularization").toString()!="confidence_hypot_v1")return false;
    FrequencyCurve c;
    if(!read_options(o,c.options) || !number(o,"minimum_hz",c.minimum_hz) || !number(o,"maximum_hz",c.maximum_hz) || !number(o,"fit_rmse_db",c.fit_rmse_db))return false;
    if(!read_values(o,"coefficients",c.coefficients,13) || !read_values(o,"knot_hz",c.knot_hz,2050) ||
        !read_values(o,"log_amplitude",c.log_amplitude,2050) || !read_values(o,"slopes",c.slopes,2050) ||
        !read_values(o,"measured_spectrum",c.measured_spectrum,profile.compatibility.fft_size/2+1))return false;
    profile.curve=std::move(c);return valid_frequency_curve(profile);
}
}
