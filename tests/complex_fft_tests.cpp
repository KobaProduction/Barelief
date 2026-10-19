#include <dsp_kernels/dsp.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL complex FFT: %s\n", message); std::exit(1); }
}
constexpr double tau = 6.283185307179586476925286766559;
std::vector<double> oracle(const std::vector<float>& real, const std::vector<float>& imaginary) {
    const auto n = real.size(); std::vector<double> result(2 * n);
    for (std::size_t k = 0; k < n; ++k) for (std::size_t t = 0; t < n; ++t) {
        const auto angle = tau * double(k) * double(t) / double(n);
        result[2*k] += real[t]*std::cos(angle) + imaginary[t]*std::sin(angle);
        result[2*k+1] += imaginary[t]*std::cos(angle) - real[t]*std::sin(angle);
    }
    return result;
}
void compare(std::size_t n, bool radix4) {
    dsp_kernels::FFT fft; check(fft.prepare(n, radix4), "prepare");
    std::vector<float> re(n), im(n), output(2*n);
    // Both signs of frequency, imaginary DC and imaginary Nyquist must survive.
    for (std::size_t t=0; t<n; ++t) {
        re[t]=float(.3*std::cos(tau*3*t/n)+.2*std::sin(tau*5*t/n));
        im[t]=float(.17+.4*std::sin(tau*3*t/n)+.13*(t%2?-1:1));
    }
    const auto expected=oracle(re,im); const auto beforeRe=re, beforeIm=im;
    const auto bytes=fft.workspace_bytes();
    check(fft.process_complex(re,im,output), "complex transform");
    check(re==beforeRe && im==beforeIm, "inputs preserved");
    check(fft.workspace_bytes()==bytes, "workspace stable");
    for(std::size_t i=0;i<output.size();++i)
        check(std::abs(double(output[i])-expected[i])<double(n)*3e-6, "independent DFT oracle");
    check(std::abs(output[1]-.17*n)<n*3e-6, "imaginary DC preserved");
    check(std::abs(output[n+1]-.13*n)<n*3e-6, "imaginary Nyquist preserved");
    check(std::abs(std::hypot(output[6],output[7])-std::hypot(output[2*(n-3)],output[2*(n-3)+1]))>.04*n,
          "complex spectrum is not forced symmetric");
    const auto good=output;
    im[2]=std::numeric_limits<float>::quiet_NaN();
    check(!fft.process_complex(re,im,output) && output==good, "NaN rejected atomically");
    im[2]=std::numeric_limits<float>::infinity();
    check(!fft.process_complex(re,im,output) && output==good, "infinity rejected atomically");
    im=beforeIm;
    check(!fft.process_complex(re,std::span(im).first(n-1),output) && output==good, "wrong imaginary extent");
    check(!fft.process_complex(re,im,std::span(output).first(n+2)) && output==good, "full output required");
    std::fill(re.begin(),re.end(),std::numeric_limits<float>::max());
    std::fill(im.begin(),im.end(),std::numeric_limits<float>::max());
    check(!fft.process_complex(re,im,output) && output==good, "overflow rejected atomically");
    re=beforeRe; im=beforeIm;
    check(fft.process_complex(re,im,output), "reuse after rejected frame");
    fft.reset();check(!fft.process_complex(re,im,output), "reset disables processing");
}
void real_equivalence() {
    constexpr std::size_t n=512;
    dsp_kernels::FFT full; dsp_kernels::PreparedRfft real;
    check(full.prepare(n) && real.prepare(n), "prepare real comparison");
    std::vector<float> input(n), zero(n), a(2*n), b(n+2);
    for(std::size_t t=0;t<n;++t)input[t]=float(.2+.3*std::cos(tau*7*t/n)+.1*(t%2?-1:1));
    check(full.process_complex(input,zero,a) && real.process(input,b), "real comparison transform");
    for(std::size_t i=0;i<b.size();++i)check(std::abs(a[i]-b[i])<.002,"full and real DFT agree including endpoints");
    check(std::abs(b[n]-.1*n)<.002,"real Nyquist retained");
}
}
int main() {
    for(const auto n:{16u,64u,256u})for(const bool radix4:{false,true})compare(n,radix4);
    real_equivalence();
    std::puts("PASS complex FFT: independent oracle, asymmetric spectrum, imaginary endpoints, atomic errors, RFFT equivalence");
}
