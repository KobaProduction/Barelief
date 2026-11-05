set(PORTABLE_DSP_MAX_FFT_SIZE 4096 CACHE STRING "Maximum portable FFT size (512 .. 16384)")
set_property(CACHE PORTABLE_DSP_MAX_FFT_SIZE PROPERTY STRINGS 512 1024 2048 4096 8192 16384)
if(NOT PORTABLE_DSP_MAX_FFT_SIZE MATCHES "^(512|1024|2048|4096|8192|16384)$")
    message(FATAL_ERROR "PORTABLE_DSP_MAX_FFT_SIZE must be a power of two from 512 to 16384")
endif()
set(DSP_KERNELS_BACKEND "ANSI" CACHE STRING "DSP backend: ANSI or ESP_DSP")
set_property(CACHE DSP_KERNELS_BACKEND PROPERTY STRINGS ANSI ESP_DSP)
if(DSP_KERNELS_BACKEND STREQUAL "ANSI")
    add_subdirectory(vendor/esp_dsp)
    target_compile_definitions(dsp PRIVATE CONFIG_DSP_MAX_FFT_SIZE=${PORTABLE_DSP_MAX_FFT_SIZE})
    set(dsp_backend_target dsp)
    set(dsp_backend_source src/backend_ansi.cpp)
elseif(DSP_KERNELS_BACKEND STREQUAL "ESP_DSP")
    set(DSP_KERNELS_ESP_DSP_TARGET "idf::esp-dsp" CACHE STRING "Existing external ESP-DSP component target")
    if(NOT TARGET "${DSP_KERNELS_ESP_DSP_TARGET}")
        message(FATAL_ERROR "ESP_DSP requires an existing ESP-DSP SDK target; no SDK is downloaded or installed")
    endif()
    set(dsp_backend_target "${DSP_KERNELS_ESP_DSP_TARGET}")
    set(dsp_backend_source src/backend_esp_dsp.cpp)
else()
    message(FATAL_ERROR "DSP_KERNELS_BACKEND must be ANSI or ESP_DSP")
endif()
add_library(dsp_kernels STATIC src/dsp.cpp src/rfft_split.cpp src/backend_tables.cpp src/scalar_reduce.cpp
    src/kernel_support.cpp src/numeric_convert.cpp src/numeric_reduce.cpp ${dsp_backend_source})
target_compile_definitions(dsp_kernels PUBLIC PORTABLE_DSP_MAX_FFT_SIZE=${PORTABLE_DSP_MAX_FFT_SIZE})
target_sources(dsp_kernels PRIVATE src/prepared_resampler.cpp src/recursive_filter.cpp src/spectral_autocorrelation.cpp src/blocked_fourier.cpp src/value_histogram.cpp src/envelope_follower.cpp src/sequence_statistics.cpp src/model_prediction_selection.cpp src/tempo_probability_histogram.cpp src/beat_feature_synchronization.cpp
    src/sparse_markov.cpp src/metrical_state_space.cpp src/metrical_transitions.cpp
    src/metrical_observations.cpp src/bar_state_decoder.cpp src/downbeat_state_decoder.cpp
    src/matrix_factorization.cpp src/gaussian_mixture.cpp src/metrical_events.cpp src/pattern_state_decoder.cpp
    src/rectangular_filter_bank.cpp src/pattern_feature_recipe.cpp src/pcm_pattern_recipe.cpp
    src/tempo_histogram_sequence.cpp src/tempo_analysis_sequence.cpp
    src/beat_tracking_sequence.cpp src/crf_beat_sequence.cpp src/state_beat_sequence.cpp
    src/frame_sampling.cpp src/spectral_frame_recipe.cpp src/spectral_frame_storage.cpp src/interpolated_peaks.cpp src/spectral_novelty.cpp src/spectral_novelty_storage.cpp src/pcm_spectral_novelty.cpp
    src/discrete_triangle.cpp src/sinc_resampling_window.cpp src/resampling_window.cpp
    src/onset_strength_sequence.cpp src/pcm_onset_strength.cpp src/onset_detection_sequence.cpp src/pcm_onset_events.cpp src/pcm_onset_envelope.cpp
    src/tempogram_sequence.cpp src/envelope_tempo.cpp src/pcm_tempo.cpp
    src/feature_reduction.cpp src/tempo_ratio_sequence.cpp src/metrical_tempogram_sequence.cpp
    src/envelope_tempo_ratios.cpp src/envelope_beat_tracking.cpp src/envelope_local_pulse.cpp src/pcm_beat_tracking.cpp)
target_sources(dsp_kernels PRIVATE src/musical_features.cpp)
target_sources(dsp_kernels PRIVATE src/frequency_profile.cpp src/frequency_curve.cpp src/frequency_curve_fit.cpp src/frequency_measurements.cpp src/frequency_measurement_grid.cpp)
target_sources(dsp_kernels PRIVATE src/onsets.cpp src/onset_fusion.cpp src/correlation.cpp src/tempo_evidence.cpp)
target_sources(dsp_kernels PRIVATE src/onset_strength.cpp src/onset_lag.cpp)
target_sources(dsp_kernels PRIVATE src/beat_tracker.cpp)
target_sources(dsp_kernels PRIVATE src/spectral_alignment.cpp src/comb_filter_bank.cpp src/interval_histogram.cpp)
target_sources(dsp_kernels PRIVATE src/convolution.cpp src/histogram_tempo_peaks.cpp src/interval_beat_search.cpp src/interval_sequence_decoder.cpp src/state_interval_histogram.cpp)
target_sources(dsp_kernels PRIVATE src/local_interval_beat_search.cpp src/interval_sequence_search.cpp)

target_sources(dsp_kernels PRIVATE
    src/fourier_backend.cpp src/fourier_transform.cpp src/temporal_window.cpp src/tempogram.cpp
    src/tempo_estimator.cpp src/onset_peaks.cpp src/overlap_add.cpp src/local_pulse.cpp)

target_sources(dsp_kernels PRIVATE src/beat_state_decoder.cpp src/log_frequency_filter_bank.cpp src/complex_flux.cpp src/energy_novelty.cpp)
target_sources(dsp_kernels PRIVATE src/complex_values.cpp src/spectral_descriptors.cpp src/log_compression.cpp src/activation_peaks.cpp src/activation_peak_matrix.cpp src/curve_interpolation.cpp src/sample_rotation.cpp)
target_sources(dsp_kernels PRIVATE src/mel_filter_bank.cpp src/filter_projection.cpp src/power_to_db.cpp)
target_sources(dsp_kernels PRIVATE
    src/reference_fourier.cpp src/reference_autocorrelation.cpp src/reference_hybrid.cpp)
target_sources(dsp_kernels PRIVATE
    src/rhythm/analyzer.cpp src/rhythm/math.cpp src/rhythm/observer.cpp
    src/rhythm/tracking.cpp src/rhythm/autocorrelation.cpp
    src/rhythm/hierarchy.cpp src/rhythm/ranking.cpp
    src/rhythm/decision.cpp src/rhythm/publication.cpp src/rhythm/packing.cpp)
target_include_directories(dsp_kernels PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(dsp_kernels PUBLIC cxx_std_20)
target_link_libraries(dsp_kernels PUBLIC signal_contracts PRIVATE ${dsp_backend_target})
# Preserve the historical consumer target without a second implementation.
add_library(portable_dsp INTERFACE)
target_link_libraries(portable_dsp INTERFACE dsp_kernels)
install(TARGETS dsp_kernels EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/librosa.txt DESTINATION share/licenses/barelief)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/madmom.txt DESTINATION share/licenses/barelief)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/scipy.txt DESTINATION share/licenses/barelief)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/scikit-learn.txt DESTINATION share/licenses/barelief)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/libsamplerate.txt DESTINATION share/licenses/barelief)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/pocketfft.txt DESTINATION share/licenses/barelief)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/licenses/numpy.txt DESTINATION share/licenses/barelief)

target_sources(dsp_kernels PRIVATE src/fourier_storage.cpp)
