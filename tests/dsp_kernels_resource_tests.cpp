#include "dsp_kernels/dsp.h"
#include <cstdlib>
#include <iostream>
#include <limits>
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
    check(dsp_kernels::compiled_backend() == dsp_kernels::Backend::Ansi, "wrong compiled backend for ANSI test");
    dsp_kernels::MelFilterBank mel;
    std::vector<float> input(16, 1), output(50);
    check(mel.prepare(16, 48000, 50) && mel.process(input, output), "valid Mel failed");
    const auto workspace = mel.workspace_bytes();
    check(!mel.prepare(16, 48000, std::numeric_limits<std::size_t>::max()), "overflow band count accepted");
    check(!mel.prepare(std::numeric_limits<std::size_t>::max(), 48000, 50), "overflow bin count accepted");
    check(!mel.prepare(16, 0, 50) && !mel.prepare(0, 48000, 0), "undefined grid accepted");
    check(mel.workspace_bytes() == workspace && mel.process(input, output), "invalid prepare changed the working plan");
    check(mel.prepare(16, 48000, 128), "general Mel bank retains an obsolete 50-band cap");
    output.resize(128);check(mel.process(input, output), "general Mel projection");
    output.resize(50);
    dsp_kernels::StereoProcessor stereo;
    std::vector<float> stereo_input(256, 1);
    check(stereo.prepare(512, 48000, 50) && stereo.process_mel(stereo_input, stereo_input, output, output),
          "valid stereo Mel failed");
    check(!stereo.prepare(512, 48000, 51), "stereo accepted unbounded Mel");
    check(!stereo.process_mel(stereo_input, stereo_input, output, output), "invalid stereo prepare left stale Mel runnable");
    std::cout << "Bounded prepare resources and invalidation passed\n";
}
