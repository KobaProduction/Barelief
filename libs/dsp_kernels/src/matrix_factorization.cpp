#include <dsp_kernels/matrix_factorization.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dsp_kernels {
namespace {
bool square(std::size_t n,std::size_t count) noexcept {
    return n && n<=std::numeric_limits<std::size_t>::max()/n && n*n==count;
}
bool factor_entry(std::span<const float> input,std::span<float> lower,std::size_t n,
                   std::size_t row,std::size_t column,float shift) noexcept {
    float sum=0;
    for(std::size_t k=0;k<column;++k)sum+=lower[row*n+k]*lower[column*n+k];
    const float entry=input[row*n+column]+(row==column?shift:0.f);
    const float residual=entry-sum;
    if(row==column && residual<=0)return false;
    const float value=row==column?std::sqrt(residual):residual/lower[column*n+column];
    if(!std::isfinite(value))return false;
    lower[row*n+column]=value;return true;
}
}
bool cholesky_lower(std::span<const float> input,std::size_t n,std::span<float> lower,float shift) noexcept {
    if(!square(n,input.size()) || lower.size()!=input.size() || !std::isfinite(shift) || shift<0 ||
       !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t row=0;row<n;++row){
        for(std::size_t column=0;column<=row;++column)
            if(!factor_entry(input,lower,n,row,column,shift))return false;
        std::fill(lower.begin()+std::ptrdiff_t(row*n+row+1),lower.begin()+std::ptrdiff_t((row+1)*n),0.f);
    }
    return true;
}
bool lower_triangular_solve(std::span<const float> lower,std::span<const float> rhs,std::span<float> output) noexcept {
    const auto n=rhs.size();
    if(!square(n,lower.size()) || output.size()!=n)return false;
    for(std::size_t row=0;row<n;++row){
        float sum=0;
        for(std::size_t column=0;column<row;++column)sum+=lower[row*n+column]*output[column];
        const float diagonal=lower[row*n+row],value=(rhs[row]-sum)/diagonal;
        if(!std::isfinite(diagonal) || diagonal<=0 || !std::isfinite(value))return false;
        output[row]=value;
    }
    return true;
}
}
