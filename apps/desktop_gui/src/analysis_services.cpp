#include <core_app/analysis_services.h>
desktop_analysis::offline::AnalysisServices desktop_analysis_services() {
    using namespace desktop_analysis::offline;
    AnalysisServices services;
    services.analyze_files=analyze_music_files;
    services.scan_files=scan_music_files;
    services.save_scan=save_music_scan;
    services.load_scan=load_music_scan;
    services.preview=analyze_music_preview;
    services.scan=scan_audio_files;
    services.save=save_profile;
    services.load=load_profile;
    services.load_diagnostic=load_profile_diagnostic;
    return services;
}
