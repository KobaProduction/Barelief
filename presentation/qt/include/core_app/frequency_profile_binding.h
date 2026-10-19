#pragma once
#include <application/frequency_profile_application.h>
#include <offline/music_analysis.h>
#include <QString>

namespace core_app {
class DspProfileMatcher final:public application::ProfileMatcher {
public:
    explicit DspProfileMatcher(const dsp_kernels::FrequencyProfile* profile=nullptr):profile_(profile){}
    bool valid_declaration(const application::ProfileRoute&) const override;
    std::vector<application::ProfileMismatchReason> mismatches(const application::ProfileRoute&,
        std::span<const float> gains) const override;
private:
    const dsp_kernels::FrequencyProfile* profile_;
};
struct FrequencyProfileTarget {
    application::ProfileRoute route;
    domain::NodeId coefficients=-1, arithmetic=-1, projection=-1;
    QString label;
    std::size_t gain_first_bin=1,gain_count=0;
    desktop_analysis::offline::AnalysisOptions options;
};
QString frequency_profile_target_caption(const FrequencyProfileTarget&);
QString frequency_profile_incompatibility(const FrequencyProfileTarget&,const dsp_kernels::FrequencyProfile&);
// UI/application boundary: locate the explicit numeric coefficient route.
// No JSON, file access or DSP execution is introduced into the live runtime.
std::vector<FrequencyProfileTarget> frequency_profile_targets(const domain::NumericProcessingGraph&);
bool frequency_profile_edits(const domain::NumericProcessingGraph&,const FrequencyProfileTarget&,
    const dsp_kernels::FrequencyProfile&,domain::ConfiguredProcessingNode& coefficients,
    domain::ConfiguredProcessingNode& arithmetic,QString& error);
}
