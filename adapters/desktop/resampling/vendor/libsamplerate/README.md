# libsamplerate coefficient provider

Upstream: https://github.com/libsndfile/libsamplerate
Revision: c96f5e3de9c4488f4e6c97f59f5245f22fda22f7 (release 0.2.2).
File: src/high_qual_coeffs.h, retained unchanged.
SHA-256: 4857e638b21e6d98bf0dc3614109072350719aa365ff85c5c792ee3469bb450e

The BSD-2-Clause notice is in COPYING and packaging/licenses/libsamplerate.txt.
The source table has 340239 Float32 coefficients and increment 2381; no taps
are removed. It occupies 1360956 shared read-only bytes, once per linked image,
rather than per prepared node. Backend owned_bytes excludes this static storage.
The backend object is linked from the static archive when its factory is used.

The portable sinc_resampling_window operation adapts calc_output_single and
sinc_mono_vari_process from src/src_sinc.c. Its contract is an independent
constant-ratio mono src_simple call with end_of_input=1 and fresh zero history.
Planar channels repeat that mono contract independently; this is not the
upstream interleaved stereo routine. The ratio is output_samples/input_samples,
within the upstream 1/256..256 range. Ratio 1 still filters.
All interpolation, dot products and output scaling use Float32; cold position
planning uses source Double phase arithmetic and 12-bit rounded coefficient
indices. Every call reports the actual generated count; it does not fill an
unproduced tail. The table lifetime exceeds the prepared plan.
