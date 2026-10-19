#include <core_app/analysis_services.h>
desktop_analysis::offline::AnalysisServices desktop_analysis_services() {
    using namespace desktop_analysis::offline;
    AnalysisServices services;
    services.analyze_files=analyze_music_files;
    services.preview=analyze_music_preview;
    services.scan=scan_audio_files;
    services.save=save_profile;
    services.load=load_profile;
    services.load_diagnostic=load_profile_diagnostic;
    return services;
}
