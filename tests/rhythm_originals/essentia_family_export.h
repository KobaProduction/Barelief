#pragma once
#include "native_export_io.h"
#include <dsp_kernels/numeric_rows.h>
#include <algorithm>
namespace essentia_family_export {
struct Case {
    std::vector<float> input,aux;
    std::string prefix;
    unsigned rate=44100,rows=0,columns=0,seed=0;
};
struct Matrix {
    const std::vector<float>& values;
    std::size_t columns;
    static bool read(const void* p,std::size_t row,std::span<float> out) noexcept {
        const auto& m=*static_cast<const Matrix*>(p);
        if(out.size()!=m.columns || row>=m.values.size()/m.columns)return false;
        std::copy_n(m.values.data()+row*m.columns,m.columns,out.data());return true;
    }
    dsp_kernels::FloatMatrixSource source() const noexcept {return {this,read};}
};
void intervals(const Case&,const std::string&);
void matrix(const Case&,const std::string&);
void pcm(const Case&,const std::string&);
void summary(const Case&,const std::string&);
void diagnostics(const Case&,const std::string&);
}
