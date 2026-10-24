#include "frequency_curve_math.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dsp_kernels::profile_curve_detail {
namespace {
double coordinate(const FrequencyCurve& c,double hz) {
    return 2*(hz-c.minimum_hz)/(c.maximum_hz-c.minimum_hz)-1;
}
std::vector<double> basis(double x,unsigned degree) {
    std::vector<double> values(degree+1,1);
    if(degree)values[1]=x;
    for(unsigned j=2;j<=degree;++j)values[j]=2*x*values[j-1]-values[j-2];
    return values;
}
void add_equation(std::vector<std::vector<double>>& matrix,const std::vector<double>& row,double y) {
    for(std::size_t i=0;i<row.size();++i){
        for(std::size_t j=0;j<row.size();++j)matrix[i][j]+=row[i]*row[j];
        matrix[i].back()+=row[i]*y;
    }
}
bool normalize_pivot(std::vector<std::vector<double>>& a,std::size_t column) {
    auto pivot=column;
    for(auto i=column+1;i<a.size();++i)if(std::abs(a[i][column])>std::abs(a[pivot][column]))pivot=i;
    if(std::abs(a[pivot][column])<1e-14)return false;
    std::swap(a[column],a[pivot]);
    const auto divisor=a[column][column];
    for(auto j=column;j<=a.size();++j)a[column][j]/=divisor;
    return true;
}
bool eliminate(std::vector<std::vector<double>>& a,std::size_t column) {
    if(!normalize_pivot(a,column))return false;
    for(std::size_t i=0;i<a.size();++i)if(i!=column){
        const auto factor=a[i][column];
        for(auto j=column;j<=a.size();++j)a[i][j]-=factor*a[column][j];
    }
    return true;
}
bool polynomial_equations(const FrequencyCurve& c,double bin_hz,std::vector<std::vector<double>>& a) {
    std::size_t observations=0;
    for(std::size_t k=1;k<c.measured_spectrum.size();++k)if(c.measured_spectrum[k]>0){
        add_equation(a,basis(coordinate(c,k*bin_hz),unsigned(a.size()-1)),std::log(c.measured_spectrum[k]));
        ++observations;
    }
    return observations>=a.size();
}
bool solve_polynomial(FrequencyCurve& c,std::vector<std::vector<double>>& a) {
    for(std::size_t i=0;i<a.size();++i)a[i][i]+=1e-10;
    for(std::size_t i=0;i<a.size();++i)if(!eliminate(a,i))return false;
    for(const auto& row:a)c.coefficients.push_back(row.back());
    return true;
}
bool polynomial(FrequencyCurve& c,double bin_hz) {
    const auto count=c.options.polynomial_degree+1;
    std::vector<std::vector<double>> a(count,std::vector<double>(count+1));
    return polynomial_equations(c,bin_hz,a) && solve_polynomial(c,a);
}
void append_knot(FrequencyCurve& c,std::size_t first,std::size_t last,double bin_hz) {
    double log_sum=0,hz_sum=0,count=0;
    for(auto k=first;k<last;++k)if(c.measured_spectrum[k]>0){
        log_sum+=std::log(c.measured_spectrum[k]);hz_sum+=k*bin_hz;++count;
    }
    if(!count)return;
    c.knot_hz.push_back(hz_sum/count);c.log_amplitude.push_back(log_sum/count);
}
void prepare_slopes(FrequencyCurve& c) {
    const auto n=c.knot_hz.size();c.slopes.assign(n,0);
    std::vector<double> secants(n-1);
    for(std::size_t i=0;i+1<n;++i)secants[i]=(c.log_amplitude[i+1]-c.log_amplitude[i])/(c.knot_hz[i+1]-c.knot_hz[i]);
    c.slopes.front()=secants.front();c.slopes.back()=secants.back();
    for(std::size_t i=1;i+1<n;++i)if(secants[i-1]*secants[i]>0){
        const auto left=c.knot_hz[i]-c.knot_hz[i-1],right=c.knot_hz[i+1]-c.knot_hz[i];
        c.slopes[i]=3*(left+right)/((left+2*right)/secants[i-1]+(2*left+right)/secants[i]);
    }
}
bool piecewise(FrequencyCurve& c,double bin_hz) {
    const auto first=std::size_t(std::ceil(c.minimum_hz/bin_hz)),last=std::size_t(std::floor(c.maximum_hz/bin_hz));
    append_knot(c,first,first+1,bin_hz);
    const auto width=std::max(std::size_t{1},(last-first+c.options.segments-1)/c.options.segments);
    for(auto begin=first+1;begin<last;begin+=width)append_knot(c,begin,std::min(last,begin+width),bin_hz);
    append_knot(c,last,last+1,bin_hz);
    if(c.knot_hz.size()<2)return false;
    prepare_slopes(c);return true;
}
double cubic(const FrequencyCurve& c,std::size_t i,double hz) noexcept {
    const auto h=c.knot_hz[i+1]-c.knot_hz[i],t=(hz-c.knot_hz[i])/h;
    const auto t2=t*t,t3=t2*t;
    return (2*t3-3*t2+1)*c.log_amplitude[i]+(t3-2*t2+t)*h*c.slopes[i]+
        (-2*t3+3*t2)*c.log_amplitude[i+1]+(t3-t2)*h*c.slopes[i+1];
}
double rmse(const FrequencyCurve& c,double bin_hz) {
    double sum=0,count=0;
    for(std::size_t k=1;k<c.measured_spectrum.size();++k)if(c.measured_spectrum[k]>0){
        const auto delta=20/std::log(10.)*(fitted_log_amplitude(c,k*bin_hz)-std::log(c.measured_spectrum[k]));
        sum+=delta*delta;++count;
    }
    return count?std::sqrt(sum/count):0;
}
}
double fitted_log_amplitude(const FrequencyCurve& c,double hz) noexcept {
    if(c.options.approximation==CurveApproximation::Polynomial){
        const auto x=coordinate(c,hz);double a=0,b=0;
        for(auto i=c.coefficients.size();i-->1;){const auto next=2*x*a-b+c.coefficients[i];b=a;a=next;}
        return x*a-b+c.coefficients.front();
    }
    const auto at=std::upper_bound(c.knot_hz.begin(),c.knot_hz.end(),hz);
    const auto i=std::clamp(std::size_t(at-c.knot_hz.begin()),std::size_t{1},c.knot_hz.size()-1)-1;
    return cubic(c,i,hz);
}
bool fit(FrequencyCurve& c,double bin_hz) {
    if(!(c.options.approximation==CurveApproximation::Polynomial?polynomial(c,bin_hz):piecewise(c,bin_hz)))return false;
    c.fit_rmse_db=rmse(c,bin_hz);return std::isfinite(c.fit_rmse_db);
}
}
