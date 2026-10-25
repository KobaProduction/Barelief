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
void encode_function_data(const FrequencyCurve& c,QJsonObject& o,ProfileArraySink* sink) {
    o["minimum_hz"]=c.minimum_hz;o["maximum_hz"]=c.maximum_hz;o["fit_rmse_db"]=c.fit_rmse_db;
    o["coefficients"]=values(c.coefficients);o["knot_hz"]=values(c.knot_hz);o["log_amplitude"]=values(c.log_amplitude);o["slopes"]=values(c.slopes);
    if(c.measurement_hz.empty()){o["measured_spectrum"]=values(c.measured_spectrum);return;}
    o["edge_hz"]=c.edge_hz;
    o["measurement_hz"]=array_detail::array(c.measurement_hz,o.value("measurement_hz"),sink,0,"profile/function/measurement_hz");
    o["measured_spectrum"]=array_detail::array(c.measured_spectrum,o.value("measured_spectrum"),sink,0,"profile/function/measured_spectrum");
    o["mean_amplitude"]=array_detail::array(c.mean_amplitude,o.value("mean_amplitude"),sink,0,"profile/function/mean_amplitude");
    o["contributors"]=array_detail::array(c.contributors,o.value("contributors"),sink,0,"profile/function/contributors");
}
bool read_measurements(const QJsonObject& o,FrequencyCurve& c,ProfileArraySource* source,std::size_t& budget) {
    const auto value=o.value("measurement_hz");
    const auto count=value.isArray()?double(value.toArray().size()):value.toObject().value("count").toDouble(-1);
    if(count<3 || count>4194304 || std::floor(count)!=count || !number(o,"edge_hz",c.edge_hz))return false;
    return array_detail::vector(value,c.measurement_hz,std::size_t(count),budget,source) &&
        array_detail::vector(o.value("measured_spectrum"),c.measured_spectrum,std::size_t(count),budget,source) &&
        array_detail::vector(o.value("mean_amplitude"),c.mean_amplitude,std::size_t(count),budget,source) &&
        array_detail::vector(o.value("contributors"),c.contributors,std::size_t(count),budget,source);
}
bool install_function(FrequencyProfile& profile,FrequencyCurve curve,double version) {
    profile.curve=std::move(curve);std::string error;
    return version==1?valid_frequency_curve(profile):update_frequency_curve_gains(profile,error);
}
}
QJsonObject encode_curve(const dsp_kernels::FrequencyCurve& c,QJsonObject o,ProfileArraySink* sink) {
    const auto& options=c.options;
    o["version"]=c.measurement_hz.empty()?1:2;o["domain"]="hz";o["outside_measured_range"]="unity";
    o["regularization"]=c.measurement_hz.empty()?"confidence_hypot_v1":"confidence_hypot_dbfs_v2";
    o["aggregation"]=options.aggregation==PeakAggregation::MeanTrackMaxima?"mean_track_maxima":options.aggregation==PeakAggregation::GlobalMaximum?"global_maximum":"upper_percentile";
    o["approximation"]=options.approximation==CurveApproximation::Polynomial?"chebyshev_log_amplitude_hz":"pchip_log_amplitude_hz";
    o["percentile"]=options.percentile;o["regularization_db"]=options.regularization_db;o["max_gain"]=options.max_gain;
    o["degree"]=int(options.polynomial_degree);o["segments"]=double(options.segments);
    encode_function_data(c,o,sink);return o;
}
bool decode_curve(const QJsonObject& o,dsp_kernels::FrequencyProfile& profile,ProfileArraySource* source,std::size_t* shared_budget) {
    const auto version=o.value("version").toDouble();
    if((version!=1 && version!=2) || o.value("domain").toString()!="hz" || o.value("outside_measured_range").toString()!="unity" ||
        o.value("regularization").toString()!=(version==1?"confidence_hypot_v1":"confidence_hypot_dbfs_v2"))return false;
    FrequencyCurve c;
    if(!read_options(o,c.options) || !number(o,"minimum_hz",c.minimum_hz) || !number(o,"maximum_hz",c.maximum_hz) || !number(o,"fit_rmse_db",c.fit_rmse_db))return false;
    if(!read_values(o,"coefficients",c.coefficients,13) || !read_values(o,"knot_hz",c.knot_hz,2050) ||
        !read_values(o,"log_amplitude",c.log_amplitude,2050) || !read_values(o,"slopes",c.slopes,2050))return false;
    std::size_t local_budget=1024ull*1024*1024;auto& budget=shared_budget?*shared_budget:local_budget;
    if(version==1 && !read_values(o,"measured_spectrum",c.measured_spectrum,profile.compatibility.fft_size/2+1))return false;
    if(version==2 && !read_measurements(o,c,source,budget))return false;
    return install_function(profile,std::move(c),version);
}
}
