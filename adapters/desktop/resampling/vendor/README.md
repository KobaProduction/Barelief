# SpeexDSP resampler source

`resample.c`, `arch.h`, `speex_resampler.h` and `COPYING` are unmodified files
from Xiph SpeexDSP tag `SpeexDSP-1.2.1`:
https://github.com/xiph/speexdsp/tree/SpeexDSP-1.2.1

BSD 3-clause notices are retained in each source and `COPYING`. Binary
redistributions must reproduce those notices and disclaimers in accompanying
materials. Author/contributor names may not endorse derived products.

The desktop adapter compiles `../speex_bridge.c`, which includes this source
with `OUTSIDE_SPEEX`, `FLOATING_POINT`, and prefix `qtvis_speex`; no SIMD or
platform implementation is selected. The bridge adds compatible prepared state
copy and allocation size inspection without modifying the vendor source.
Only prepare allocates; process/reset/state-copy do not change rate or quality.
The portable SDK does not compile or link this implementation.
