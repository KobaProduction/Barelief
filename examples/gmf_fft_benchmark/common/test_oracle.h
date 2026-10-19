#pragma once
#include <vector>
#include <complex>
#include <algorithm>
#include <cstdint>
namespace gmf_test {
inline std::vector<std::complex<double>> reference(const std::vector<int16_t>& input) {
    const auto n=input.size();
    std::vector<std::complex<double>> a(n);
    for(size_t i=0;i<n;++i) a[i]=input[i];
    for(size_t i=1,j=0;i<n;++i) {
        size_t bit=n/2;
        while(j&bit) {j^=bit;bit/=2;}
        j^=bit;
        if(i<j) std::swap(a[i],a[j]);
    }
    for(size_t len=2;len<=n;len*=2) {
        auto step=std::polar(1.,-6.2831853071795864769/len);
        for(size_t i=0;i<n;i+=len) {
            std::complex<double> w=1;
            for(size_t j=0;j<len/2;++j) {
                auto u=a[i+j],v=a[i+j+len/2]*w;
                a[i+j]=u+v;a[i+j+len/2]=u-v;w*=step;
            }
        }
    }
    return a;
}
}
