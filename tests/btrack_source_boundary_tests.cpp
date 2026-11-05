// SPDX-License-Identifier: AGPL-3.0-or-later
#include <rhythm_reference_ports/pcm_predictive_onset.h>
#include <kiss_backend.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
namespace r=rhythm_reference_ports;
void check(bool ok,const char* message) {
    if(!ok){std::fprintf(stderr,"BTrack source boundary FAIL: %s\n",message);std::abort();}
}
void samples(std::span<float> pcm,std::size_t offset) {
    constexpr std::array tones{110.,251.,503.,997.,1999.,4001.,6007.,10007.};
    for(std::size_t i=0;i<pcm.size();++i){
        const auto sample=offset+i;const double time=double(sample)/44100.,phase=double(sample%22050)/44100.;
        pcm[i]=0;
        for(std::size_t k=0;k<tones.size();++k)pcm[i]+=float(.06*std::exp(-phase/.006)*
            std::sin(6.283185307179586*tones[k]*time+.17*k+.06*std::sin(time*(k+1))));
    }
}
void complex_boundary() {
    r::PcmPredictiveOnsetConfig config;config.precision=dsp_kernels::NumericalPrecision::DesktopReference;
    r::PreparedPcmPredictiveOnset whole;
    r::detail::PreparedPredictiveReferenceFrontend split;
    check(whole.prepare(config,&desktop_fourier::kiss_backend_factory()) && split.prepare_complex(1024),"prepare source arithmetic");
    std::array<float,512> pcm{};float expected=0,actual=0;
    for(std::size_t frame=0;frame<1024;++frame){
        samples(pcm,frame*pcm.size());
        check(whole.process_hop(pcm,expected) && split.process_complex(whole.complex_values(),actual),"source complex input");
        check(actual==expected && split.value()==whole.reference_stage().value(),"exact Double ODF across explicit FFT boundary");
    }
    const double saved=split.value();std::array<float,2048> invalid{};invalid[17]=std::numeric_limits<float>::quiet_NaN();
    check(!split.process_complex(invalid,actual) && split.value()==saved,"invalid complex input does not commit ODF state");
    whole.restart();split.restart();samples(pcm,0);
    check(whole.process_hop(pcm,expected) && split.process_complex(whole.complex_values(),actual) && actual==expected,"reset source histories");
}
}
int main(){complex_boundary();std::puts("BTrack source complex boundary PASS");}
