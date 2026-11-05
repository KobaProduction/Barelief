// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_reference_graphs.h>
#include <core_app/numeric_btrack_reference_graph.h>
#include <core_app/numeric_madmom_reference_graph.h>
#include <core_app/numeric_essentia_reference_graph.h>
std::vector<NumericExampleGraph> numeric_library_reference_presets() {
    return {numeric_aubio_reference_example(),numeric_btrack_reference_example(),
        numeric_madmom_combined_reference_example(),numeric_essentia_combined_reference_example()};
}
std::vector<NumericExampleGraph> numeric_library_reference_examples() {
    return {numeric_aubio_reference_example(),numeric_btrack_reference_example(),
        numeric_madmom_reference_example(48000,MadmomReferenceMode::Fixed),
        numeric_madmom_reference_example(48000,MadmomReferenceMode::Local),
        numeric_madmom_reference_example(48000,MadmomReferenceMode::Crf),
        numeric_essentia_reference_example(EssentiaReferenceMethod::Degara),
        numeric_essentia_reference_example(EssentiaReferenceMethod::MultiFeature)};
}
