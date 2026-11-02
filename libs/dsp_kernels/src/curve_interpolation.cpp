// Not-a-knot knot construction follows scipy.interpolate.make_interp_spline,
// SciPy v1.16.3, _bsplines.py:_not_a_knot (de Boor XIII(12)).
// Copyright (c) 2001-2002 Enthought, Inc. 2003, SciPy Developers.
// BSD-3-Clause, full notice in
// packaging/licenses/scipy.txt. Float32 banded collocation and decomposition
// Copyright (c) 2026 Koba Production.
#include <dsp_kernels/curve_interpolation.h>
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/detail/source_spectral_math.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
namespace dsp_kernels {
namespace {
std::size_t order(CurveInterpolationConfig c) noexcept {
    if(c.kind==CurveInterpolation::Quadratic)return 2;
    if(c.kind==CurveInterpolation::Cubic)return 3;
    return c.kind==CurveInterpolation::Spline?c.spline_order:0;
}
bool valid(CurveInterpolationConfig c) noexcept {
    return unsigned(c.kind)<=unsigned(CurveInterpolation::Cubic) &&
        std::isfinite(c.fill_below) && std::isfinite(c.fill_above) && !(c.bounds_error && c.extrapolate);
}
}
bool PreparedCurveInterpolation::storage_bytes(std::size_t count,CurveInterpolationConfig config,std::size_t& bytes) noexcept {
    if(!count || !valid(config) || order(config)>=count || order(config)>(SIZE_MAX-1)/2)return false;
    if(config.kind==CurveInterpolation::Linear && count<2)return false;
    std::size_t required=0;
    if(!prepared_storage::add_array(required,count,sizeof(std::size_t)+4*sizeof(float)))return false;
    const auto degree=order(config);
    if(degree){
        std::size_t width_bytes=0;
        if(count>SIZE_MAX-degree-1 || !prepared_storage::mul_bytes(2*degree+1,sizeof(float),width_bytes) || !prepared_storage::add_array(required,count,width_bytes) ||
           !prepared_storage::add_array(required,count+degree+1,sizeof(float)) ||
           !prepared_storage::add_array(required,degree+1,3*sizeof(float)))return false;
    }
    bytes=required;return true;
}
bool PreparedCurveInterpolation::prepare(std::span<const float> coordinates,CurveInterpolationConfig config) {
    std::size_t bound=0;if(!storage_bytes(coordinates.size(),config,bound))return false;
    PreparedCurveInterpolation next;if(!next.configure(coordinates,config))return false;
    if(next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedCurveInterpolation::dynamic_storage_bytes(std::size_t count,CurveInterpolationConfig config,std::size_t& bytes) noexcept {
    std::size_t required=0;
    if(!count || count>16777216 || !storage_bytes(count,config,required) || !prepared_storage::add_array(required,count,sizeof(float)))return false;
    bytes=required;return true;
}
bool PreparedCurveInterpolation::prepare_dynamic(std::size_t count,CurveInterpolationConfig c) {
    std::size_t bound=0,actual=0;
    if(!dynamic_storage_bytes(count,c,bound) || !storage_bytes(count,c,actual))return false;
    std::vector<float> grid(count);
    if(!prepared_storage::add_array(actual,grid.capacity(),sizeof(float)) || actual>bound)return false;
    for(std::size_t i=0;i<count;++i)grid[i]=float(i);
    return prepare(grid,c);
}
bool PreparedCurveInterpolation::coordinates(std::span<const float> input) noexcept {
    if(input.size()>indices_.capacity() || input.size()>x_.capacity())return false;
    indices_.clear();x_.clear();
    if(config_.keep_infinite_coordinates && std::any_of(input.begin(),input.end(),[](float x){return std::isnan(x);}))return false;
    for(std::size_t i=0;i<input.size();++i)
        if(std::isfinite(input[i]) || (config_.keep_infinite_coordinates && std::isinf(input[i])))indices_.push_back(i);
    std::sort(indices_.begin(),indices_.end(),[input](auto a,auto b){return input[a]<input[b];});
    if(indices_.empty())return false;
    for(auto index:indices_)x_.push_back(input[index]);
    return std::adjacent_find(x_.begin(),x_.end(),[](float a,float b){return a>=b;})==x_.end();
}
bool PreparedCurveInterpolation::configure(std::span<const float> input,CurveInterpolationConfig config) {
    if(!valid(config))return false;
    config_=config;input_count_=input.size();indices_.reserve(input.size());x_.reserve(input.size());
    if(!coordinates(input))return false;
    degree_=order(config);spline_=degree_>0;
    if(degree_>=x_.size() || degree_>(SIZE_MAX-1)/2)return false;
    if(config.kind==CurveInterpolation::Linear && x_.size()<2)return false;
    if(!allocate())return false;
    geometry_ready_=!spline_;
    if(!spline_)return true;
    if(!std::all_of(x_.begin(),x_.end(),[](float x){return std::isfinite(x);}))return false;
    knots();geometry_ready_=factor();return geometry_ready_;
}
bool PreparedCurveInterpolation::allocate() {
    values_.reserve(input_count_);coefficients_.reserve(input_count_);scratch_.reserve(input_count_);
    values_.resize(x_.size());coefficients_.resize(x_.size());scratch_.resize(x_.size());
    if(!spline_)return true;
    width_=2*degree_+1;
    if(input_count_>SIZE_MAX/width_ || input_count_>SIZE_MAX-degree_-1)return false;
    lu_.reserve(input_count_*width_);knots_.reserve(input_count_+degree_+1);
    lu_.assign(x_.size()*width_,0);knots_.resize(x_.size()+degree_+1);
    basis_.resize(degree_+1);left_.resize(degree_+1);right_.resize(degree_+1);
    return true;
}
void PreparedCurveInterpolation::knots() {
    std::fill_n(knots_.begin(),degree_+1,x_.front());
    std::fill(knots_.end()-std::ptrdiff_t(degree_+1),knots_.end(),x_.back());
    const auto start=degree_%2?(degree_+1)/2:degree_/2;
    for(std::size_t i=0;i<x_.size()-degree_-1;++i){
        const auto source=start+i;
        knots_[degree_+1+i]=degree_%2?x_[source]:.5f*x_[source]+.5f*x_[source+1];
    }
}
float& PreparedCurveInterpolation::matrix(std::size_t row,std::size_t column) noexcept {
    return lu_[row*width_+(column>=row?degree_+(column-row):degree_-(row-column))];
}
float PreparedCurveInterpolation::matrix(std::size_t row,std::size_t column) const noexcept {
    return lu_[row*width_+(column>=row?degree_+(column-row):degree_-(row-column))];
}
std::size_t PreparedCurveInterpolation::interval(float query) const noexcept {
    if(query>=x_.back())return x_.size()-1;
    const auto stop=knots_.end()-std::ptrdiff_t(degree_+1);
    const auto at=std::upper_bound(knots_.begin()+std::ptrdiff_t(degree_),stop,query);
    return at==knots_.begin()+std::ptrdiff_t(degree_)?degree_:std::size_t(at-knots_.begin()-1);
}
void PreparedCurveInterpolation::basis(float query,std::size_t span) const noexcept {
    basis_[0]=1;
    for(std::size_t level=1;level<=degree_;++level){
        left_[level]=query-knots_[span+1-level];right_[level]=knots_[span+level]-query;
        float saved=0;
        for(std::size_t r=0;r<level;++r){
            const float term=basis_[r]/(right_[r+1]+left_[level-r]);
            basis_[r]=saved+right_[r+1]*term;saved=left_[level-r]*term;
        }
        basis_[level]=saved;
    }
}
bool PreparedCurveInterpolation::factor() {
    for(std::size_t row=0;row<x_.size();++row){
        const auto span=interval(x_[row]);basis(x_[row],span);
        for(std::size_t j=0;j<=degree_;++j)matrix(row,span-degree_+j)=basis_[j];
    }
    // Strictly increasing interpolation sites give a totally positive
    // B-spline collocation matrix. Banded elimination needs no pivot swaps.
    for(std::size_t pivot=0;pivot<x_.size();++pivot)if(!eliminate(pivot))return false;
    return std::all_of(lu_.begin(),lu_.end(),[](float value){return std::isfinite(value);});
}
bool PreparedCurveInterpolation::eliminate(std::size_t pivot) noexcept {
    if(!std::isfinite(matrix(pivot,pivot)) || matrix(pivot,pivot)<=0)return false;
    const auto end=std::min(x_.size(),pivot+degree_+1);
    for(auto row=pivot+1;row<end;++row){
        const float multiplier=matrix(row,pivot)/matrix(pivot,pivot);matrix(row,pivot)=multiplier;
        for(auto column=pivot+1;column<end;++column)matrix(row,column)-=multiplier*matrix(pivot,column);
    }
    return true;
}
bool PreparedCurveInterpolation::solve() noexcept {
    for(std::size_t row=0;row<x_.size();++row){
        const auto start=row>degree_?row-degree_:0;
        for(auto column=start;column<row;++column)scratch_[row]-=matrix(row,column)*scratch_[column];
    }
    for(auto row=x_.size();row-->0;){
        const auto end=std::min(x_.size(),row+degree_+1);
        for(auto column=row+1;column<end;++column)scratch_[row]-=matrix(row,column)*scratch_[column];
        scratch_[row]/=matrix(row,row);
        if(!std::isfinite(scratch_[row]))return false;
    }
    return true;
}
bool PreparedCurveInterpolation::resize_geometry() noexcept {
    if(degree_>=x_.size() || (config_.kind==CurveInterpolation::Linear && x_.size()<2))return false;
    values_.resize(x_.size());coefficients_.resize(x_.size());scratch_.resize(x_.size());
    if(!spline_)return true;
    if(!std::all_of(x_.begin(),x_.end(),[](float x){return std::isfinite(x);}))return false;
    lu_.resize(x_.size()*width_);std::fill(lu_.begin(),lu_.end(),0.f);
    knots_.resize(x_.size()+degree_+1);knots();return factor();
}
bool PreparedCurveInterpolation::set_coordinates(std::span<const float> input) noexcept {
    ready_=geometry_ready_=false;
    if(input.data()==x_.data() || input.size()!=input_count_ || !coordinates(input) || !resize_geometry())return false;
    geometry_ready_=true;return true;
}
bool PreparedCurveInterpolation::set_values(std::span<const float> input) noexcept {
    if(!geometry_ready_ || indices_.empty() || input.size()!=input_count_)return false;
    for(std::size_t i=0;i<indices_.size();++i){
        const float value=input[indices_[i]];
        if(!std::isfinite(value))return false;
        scratch_[i]=value;
    }
    if(spline_ && !solve())return false;
    for(std::size_t i=0;i<indices_.size();++i)values_[i]=input[indices_[i]];
    std::copy(scratch_.begin(),scratch_.end(),coefficients_.begin());ready_=true;return true;
}
bool PreparedCurveInterpolation::outside(float query,float& value) const noexcept {
    if(query>=x_.front() && query<=x_.back())return false;
    const bool below=query<x_.front();
    value=below?config_.fill_below:config_.fill_above;
    if(!config_.extrapolate)return true;
    if((below && config_.kind==CurveInterpolation::Previous) || (!below && config_.kind==CurveInterpolation::Next)){
        value=std::numeric_limits<float>::quiet_NaN();return true;
    }
    return false;
}
float PreparedCurveInterpolation::interpolate(float query) const noexcept {
    const auto at=std::lower_bound(x_.begin(),x_.end(),query);
    auto upper=std::size_t(at-x_.begin());
    if(config_.kind==CurveInterpolation::Next)return values_[std::min(upper,x_.size()-1)];
    if(config_.keep_infinite_coordinates && (config_.kind==CurveInterpolation::Nearest || config_.kind==CurveInterpolation::NearestUp))return nearest(query,upper);
    if(config_.source_linear && config_.kind==CurveInterpolation::Linear)return linear(query,upper);
    if(upper<x_.size() && x_[upper]==query)return values_[upper];
    if(config_.kind==CurveInterpolation::Zero || config_.kind==CurveInterpolation::Previous || config_.kind==CurveInterpolation::Spline)
        return values_[upper?upper-1:0];
    if(config_.kind==CurveInterpolation::Nearest || config_.kind==CurveInterpolation::NearestUp)return nearest(query,upper);
    return linear(query,upper);
}
float PreparedCurveInterpolation::nearest(float query,std::size_t upper) const noexcept {
    if(config_.keep_infinite_coordinates){
        std::size_t first=0,last=x_.size()-1;
        while(first<last){
            const auto middle=first+(last-first)/2;const float boundary=.5f*x_[middle]+.5f*x_[middle+1];
            if(std::isnan(boundary) || query<boundary || (query==boundary && config_.kind==CurveInterpolation::Nearest))last=middle;
            else first=middle+1;
        }
        return values_[first];
    }
    if(!upper)return values_.front();
    if(upper==x_.size())return values_.back();
    const float midpoint=.5f*x_[upper-1]+.5f*x_[upper];
    return values_[query<midpoint || (query==midpoint && config_.kind==CurveInterpolation::Nearest)?upper-1:upper];
}
float PreparedCurveInterpolation::linear(float query,std::size_t upper) const noexcept {
    upper=std::clamp(upper,std::size_t{1},x_.size()-1);
    if(config_.source_linear)return detail::source_linear_segment(x_[upper-1],x_[upper],values_[upper-1],values_[upper],query);
    const float fraction=(query-x_[upper-1])/(x_[upper]-x_[upper-1]);
    return values_[upper-1]+fraction*(values_[upper]-values_[upper-1]);
}
bool PreparedCurveInterpolation::sample(float query,float& value) const noexcept {
    if(!ready_)return false;
    if(std::isnan(query)){value=std::numeric_limits<float>::quiet_NaN();return true;}
    if(config_.bounds_error && (query<x_.front() || query>x_.back()))return false;
    float result=0;
    if(outside(query,result)){value=result;return true;}
    if(spline_){
        const auto span=interval(query);basis(query,span);
        for(std::size_t j=0;j<=degree_;++j)result+=basis_[j]*coefficients_[span-degree_+j];
    }else result=interpolate(query);
    value=result;return true;
}
}
