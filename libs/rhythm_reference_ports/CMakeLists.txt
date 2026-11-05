# GPL source ports, kept separate from the MIT/ISC/BSD dsp_kernels target.
# A dedicated target is available for the later adaptation stage; the main
# application and SDK do not link or install it implicitly.
add_library(rhythm_reference_storage STATIC src/scalar_history.cpp)
target_include_directories(rhythm_reference_storage PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(rhythm_reference_storage PUBLIC cxx_std_20)
target_link_libraries(rhythm_reference_storage PRIVATE dsp_kernels)
set_property(TARGET rhythm_reference_storage PROPERTY SPDX_LICENSE_IDENTIFIER "GPL-3.0-or-later")
add_library(rhythm_reference_ports STATIC
    src/context_beat_tracker.cpp src/predictive_beat_tracker.cpp
    src/histogram_onsets.cpp src/adaptive_peak_picker.cpp src/spectral_whitening.cpp
    src/onset_decision.cpp src/onset_recipes.cpp src/context_beat_sequence.cpp src/reference_window.cpp
    src/activation_peak_sequence.cpp src/spectral_difference_sequence.cpp src/pcm_spectral_difference.cpp
    src/hop_spectrum.cpp src/onset_descriptor.cpp src/onset_magnitude.cpp
    src/pcm_onset_sequence.cpp src/pcm_context_tempo.cpp src/pcm_onset_storage.cpp src/pcm_context_storage.cpp
    src/pcm_predictive_onset.cpp src/pcm_predictive_beat_tracker.cpp src/predictive_reference_frontend.cpp
    src/predictive_reference_comb.cpp)
target_include_directories(rhythm_reference_ports PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(rhythm_reference_ports PUBLIC cxx_std_20)
target_link_libraries(rhythm_reference_ports PUBLIC dsp_kernels rhythm_reference_storage)
set_property(TARGET rhythm_reference_ports PROPERTY SPDX_LICENSE_IDENTIFIER "GPL-3.0-or-later")
add_library(rhythm_probabilistic_reference STATIC
    src/beat_agreement.cpp src/period_path.cpp src/renewal_beat_decoder.cpp
    src/global_onset.cpp src/erb_filter_bank.cpp src/polar_novelty.cpp src/period_beat_tracker.cpp
    src/causal_average.cpp src/interpolated_peaks.cpp
    src/pulse_train_analysis.cpp src/lag_tempo_consensus.cpp src/pulse_correlation.cpp src/pulse_onset.cpp
    src/beat_interval_histogram.cpp src/tempo_change_sections.cpp src/harmonic_tempo.cpp
    src/novelty_weights.cpp src/band_novelty.cpp src/tempo_distribution_peaks.cpp src/fixed_tempo_candidates.cpp
    src/fourier_tempo_candidates.cpp src/weighted_tempo_histogram.cpp
    src/frame_tempo_path.cpp src/phase_tempo_pulse.cpp src/fourier_tempo_histogram.cpp
    src/spectral_window.cpp src/novelty_tempo_histogram.cpp src/band_tempo_features.cpp
    src/periodic_comb.cpp src/comb_period_phase.cpp src/period_phase_ticks.cpp
    src/tick_sequence_refinement.cpp src/lag_tempo_vote.cpp
    src/frame_silence_policy.cpp src/lag_observation_pool.cpp src/legacy_rhythm_recipe.cpp
    src/buffered_maximum.cpp src/buffered_band_flux.cpp src/contrast_peak_picker.cpp
    src/triangular_spectral_bands.cpp src/trajectory_band_edges.cpp src/trajectory_onset_recipe.cpp
    src/loop_length_confidence.cpp src/pulse_tempo_recipe.cpp src/loop_tempo_recipe.cpp src/beat_interval_summary.cpp
    src/spectral_band_ratios.cpp src/beat_segment_energy.cpp src/beat_loudness_sequence.cpp
    src/beat_band_salience.cpp src/correlation_meter.cpp
    src/band_rhythm_transform.cpp src/detrended_fluctuation.cpp src/activity_fluctuation_recipe.cpp src/onset_rate_recipe.cpp
    src/spectral_onset.cpp src/global_onset_recipe.cpp src/pcm_beat_recipe.cpp src/pcm_rhythm_summary.cpp
    src/normalized_feature_fusion.cpp src/adaptive_onset_selection.cpp src/onset_events.cpp
    src/multi_feature_beat_tracker.cpp src/spectral_onset_storage.cpp
    src/pulse_tempo_storage.cpp src/novelty_tempo_storage.cpp src/fourier_histogram_storage.cpp
    src/beat_tracking_storage.cpp src/legacy_tracking_storage.cpp src/loop_loudness_storage.cpp
    src/trajectory_onset_storage.cpp src/onset_rate_storage.cpp)
target_include_directories(rhythm_probabilistic_reference PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(rhythm_probabilistic_reference PUBLIC cxx_std_20)
target_link_libraries(rhythm_probabilistic_reference PUBLIC dsp_kernels rhythm_reference_storage)
set_property(TARGET rhythm_probabilistic_reference PROPERTY SPDX_LICENSE_IDENTIFIER "AGPL-3.0-or-later")
