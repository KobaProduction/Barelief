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
    check(!mel.prepare(16, 48000, 51), "above-budget Mel bands accepted");
    check(!mel.process(input, output), "failed prepare left stale Mel plan runnable");
    check(mel.workspace_bytes() == workspace, "rejected prepare allocated storage");
    check(!mel.prepare(dsp_kernels::max_fft_samples / 2 + 1, 48000, 50), "above-profile bins accepted");
    check(!mel.prepare(16, 48000, std::numeric_limits<std::size_t>::max()), "overflow band count accepted");
    check(!mel.prepare(std::numeric_limits<std::size_t>::max(), 48000, 50), "overflow bin count accepted");
    check(mel.prepare(16, 0, 50) && mel.process(input, output), "legacy zero-rate behavior lost");
    for (auto x : output) check(x == 0, "zero-rate Mel not zero");
    check(mel.prepare(0, 48000, 0) && mel.process({}, {}), "legacy empty Mel behavior lost");
    dsp_kernels::StereoProcessor stereo;
    std::vector<float> stereo_input(256, 1);
    check(stereo.prepare(512, 48000, 50) && stereo.process_mel(stereo_input, stereo_input, output, output),
          "valid stereo Mel failed");
    check(!stereo.prepare(512, 48000, 51), "stereo accepted unbounded Mel");
    check(!stereo.process_mel(stereo_input, stereo_input, output, output), "invalid stereo prepare left stale Mel runnable");
    std::cout << "Bounded prepare resources and invalidation passed\n";
}
