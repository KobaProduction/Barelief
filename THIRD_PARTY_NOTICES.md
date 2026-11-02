# Barelief — third-party software

Barelief application source is licensed under MIT, Copyright (c) 2026 Koba Production.
This does not change the licenses or copyrights of bundled dependencies.

Qt 6 is dynamically linked. Qt Base, Declarative/Quick, SVG, SerialPort and runtime
plugins retain their upstream licenses (LGPL-3.0 and applicable component licenses).
Replacement of shared libraries and reverse engineering for debugging modifications
are permitted under the applicable licenses. Qt source and build instructions:
https://download.qt.io/archive/qt/6.10/6.10.2/single/
Full license texts and available component attributions are in licenses/Qt.
Only components actually shipped in a package apply to that package.

Other bundled dependencies and original notices in licenses/:

- Qt Advanced Docking System — LGPL-2.1-or-later, Uwe Kindler and contributors.
- QtNodes — BSD-3-Clause, Dmitry Pinaev and contributors.
- BDSP — MIT, BDSP contributors.
- miniaudio — dual public-domain / MIT-0, David Reid; see original license.
- SpeexDSP resampler — BSD-style license, Xiph.Org Foundation and contributors.
- tinycbor — MIT, Intel Corporation and contributors.
- nanopb — zlib license, Petteri Aimonen and contributors.
- ESP-DSP sources — Apache-2.0, Espressif Systems; see source notices.
- librosa 1.0.0 onset_strength S variant, Mel filter bank, power_to_db and
  dynamic-programming beat tracker, temporal Fourier/autocorrelation features,
  tempo selection, peak selection/backtracking and local pulse reconstruction,
  adapted to C++ — ISC, librosa
  development team; see licenses/librosa.txt and the source attribution.
- madmom classical beat-state decoder, logarithmic filter bank, ComplexFlux,
  activation peak picking and event combination, spectral correlation alignment,
  forward/backward comb banks, offline/online interval histograms, histogram
  tempo peaks, fixed/local interval beat search and classical CRF sequence
  decoding with multi-interval selection,
  adapted to C++ — BSD-style license;
  Department of Computational Perception, Johannes Kepler University and OFAI.
  See licenses/madmom.txt. No madmom model or training-data files are bundled.
- SciPy not-a-knot spline knot construction, adapted for Float32 curve
  interpolation - BSD-3-Clause, SciPy Developers; see licenses/scipy.txt.
- PocketFFT real-transform backend for the explicit desktop reference precision
  profile — BSD-3-Clause, Max-Planck-Society, Peter Bell and contributors;
  see licenses/pocketfft.txt. The pinned source and prepared-scratch patch are
  recorded in libs/dsp_kernels/vendor/pocketfft/PIN.json.
- NumPy complex Float32 scaled-ratio magnitude expression, adapted for the
  explicit desktop reference profile — BSD-3-Clause, NumPy Developers;
  see licenses/numpy.txt and the pinned source attribution in complex_values.cpp.
- MinGW GCC runtime and winpthreads — upstream GPL/LGPL with the GCC Runtime
  Library Exception where applicable; complete supplied notices in licenses/MinGW.

The licenses directory is installed beside this notice. Barelief's About dialog
includes the application license and this dependency index without network access.

Release packages require review of their actual dependency manifest before publication.

Developer-only tooling: Tracy Profiler 0.14.1 — BSD-3-Clause, Bartosz Taudul
and contributors (https://github.com/wolfpld/tracy). Downloaded only for the
telemetry-enabled desktop configuration; excluded from release installers.
