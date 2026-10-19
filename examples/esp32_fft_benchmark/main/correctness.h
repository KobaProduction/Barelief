#pragma once
#include <limits>
namespace {
template<class Plan> bool verify_plan(Plan& plan, const char* name, std::span<const float> input,
    std::span<const double> expected, double& worst) {
    const auto size=input.size();
    // Offset storage also exercises a caller pointer not aligned to 16 bytes.
    std::vector<float> storage(size+4,-321.f);
    const std::span<float> out(storage.data()+1,size+2);
    if(!plan.process(input,out)) return false;
    for(std::size_t k=0;k<out.size();++k) {
        if(!std::isfinite(out[k])) return false;
        if(std::isnan(expected[k])) continue;
        const double error=std::abs(double(out[k])-expected[k]);
        worst=std::max(worst,error);
        if(error>.0003+5e-6*std::abs(expected[k])) {
            std::printf("fft_correctness fail=%s N=%u component=%u actual=%.9g expected=%.9g\n",name,unsigned(size),unsigned(k),double(out[k]),expected[k]);
            return false;
        }
    }
    if(out[1]!=0 || out[size+1]!=0) return false;
    if(storage.front()!=-321.f || storage.back()!=-321.f) return false;
    const std::vector<float> preserved(out.begin(),out.end());
    std::vector<float> invalid(input.begin(),input.end());
    for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()}) {
        invalid.back()=bad;
        if(plan.process(invalid,out) || !std::equal(out.begin(),out.end(),preserved.begin())) return false;
    }
    if(size==8 || size==4096) {
        std::fill(invalid.begin(),invalid.end(),0.f);
        invalid[0]=std::numeric_limits<float>::denorm_min();
        if(!plan.process(invalid,out) || out[0]!=invalid[0] || out[size]!=invalid[0]) return false;
        invalid[0]=std::numeric_limits<float>::max()/2;
        if(!plan.process(invalid,out)) return false;
        for(std::size_t k=0;k<=size/2;++k)
            if(std::abs(double(out[2*k])/invalid[0]-1)>3e-6 || out[2*k+1]!=0) return false;
        std::fill(invalid.begin(),invalid.end(),std::numeric_limits<float>::max());
        std::fill(out.begin(),out.end(),-321.f);
        if(plan.process(invalid,out) || !std::all_of(out.begin(),out.end(),[](float x){return x==-321.f;})) return false;
    }
    return true;
}
bool verify_math() {
    using namespace dsp_kernels;
    double worst=0;
    for(std::size_t size:{8,16,32,64,512,1024,2048,4096}) {
        std::vector<float> input(size);
        std::vector<double> expected(size+2);
        for(double phase:{0.,.37,1.19}) {
            for(std::size_t i=0;i<size;++i) {
                double x=.17+(i%2?-.11:.11);
                for(std::size_t t=0;t<7;++t)
                    x+=.25/double(t+1)*std::cos(6.283185307179586*(1.25+3*t)*double(i)/size+phase+.13*t);
                input[i]=float(x)*float(.5-.5*std::cos(6.283185307179586*double(i)/(size-1)));
            }
            std::fill(expected.begin(),expected.end(),std::numeric_limits<double>::quiet_NaN());
            if(size>64) {
                // Check every large-N component against CFFT2, then replace
                // selected bins with the independent direct double oracle.
                FFT reference;
                std::vector<float> raw(size+2);
                if(!reference.prepare(size) || !reference.process(input,raw)) return false;
                std::copy(raw.begin(),raw.end(),expected.begin());
            }
            const std::array<std::size_t,16> probes{0,1,3,11,12,23,41,42,67,89,90,111,size/8,size/4,size/2-1,size/2};
            for(std::size_t k=0;k<=size/2;++k) {
                if(size>64 && std::find(probes.begin(),probes.end(),k)==probes.end())continue;
                const double angle=-6.283185307179586*double(k)/size,wr=std::cos(angle),wi=std::sin(angle);
                double re=0,im=0,c=1,s=0;
                for(double x:input) {re+=x*c;im+=x*s;const double next=c*wr-s*wi;s=s*wr+c*wi;c=next;}
                expected[2*k]=re; expected[2*k+1]=im;
                vTaskDelay(1);
            }
            for(bool radix4:{false,true}) {
                if(!radix4 || (size&0x55555555)) {
                    FFT full;
                    if(!full.prepare(size,radix4) || !verify_plan(full,"CFFT",input,expected,worst)) return false;
                }
                for(auto split:{PreparedRfft::Split::Scalar,PreparedRfft::Split::Automatic})
                for(bool platform:{false,true}) {
                    PreparedRfft real;
                    const PreparedRfft::Options options{platform};
                    if(!real.prepare(size,radix4,split,&options) || !verify_plan(real,"RFFT",input,expected,worst)) return false;
                }
            }
        }
    }
    std::printf("fft_correctness result=PASS max_dft_probe_error=%.9g sizes=8,16,32,64,512,1024,2048,4096 phases=3\n",worst);
    return true;
}
}
