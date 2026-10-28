#pragma once
#include "profile_json.h"
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

namespace desktop_analysis::offline {
struct CurveCorpus;
struct CurveAnalysis;
struct AnalysisOptions {
    dsp_kernels::ProfileCompatibility compatibility;
    dsp_kernels::ProfileTraining training;
    unsigned workers=2;
    std::size_t preview_time_bins=256;
    std::size_t preview_frequency_bins=256;
    bool resample_input=false;
};
struct AnalysisResult {
    ProfileDocument document;
    SpectrogramPreview preview;
    QStringList errors;
    bool cancelled=false, success=false;
    double wall_seconds=0, audio_seconds=0;
    QString batch_directory;
    std::size_t saved_part_count=0;
    std::size_t requested_track_count=0;
};
struct PreviewResult {
    SpectrogramPreview preview;
    QString error;
    bool success=false, cancelled=false;
};
// Computes one bounded preview without training or changing a frequency profile.
PreviewResult analyze_music_preview(const QString&,const AnalysisOptions&,const std::atomic_bool& cancelled);
QStringList scan_audio_files(const QString& folder, bool recursive=true);
AnalysisResult analyze_music_files(const QStringList&, const AnalysisOptions&,
    const std::atomic_bool& cancelled, const std::function<void(std::size_t,std::size_t)>& progress={});
// Measurement only: retains silent tracks and never builds normalization gains.
AnalysisResult scan_music_files(const QStringList&, const AnalysisOptions&,
    const std::atomic_bool& cancelled, const std::function<void(std::size_t,std::size_t)>& progress={});
// Presentation supplies scheduling/cancellation; composition supplies these services.
struct AnalysisServices {
    using Progress=std::function<void(std::size_t,std::size_t)>;
    std::function<AnalysisResult(const QStringList&,const AnalysisOptions&,const std::atomic_bool&,const Progress&)> analyze_files;
    std::function<PreviewResult(const QString&,const AnalysisOptions&,const std::atomic_bool&)> preview;
    std::function<QStringList(const QString&,bool)> scan;
    std::function<bool(const QString&,const ProfileDocument&,QString&)> save;
    std::function<bool(const QString&,ProfileDocument&,QString&)> load;
    std::function<bool(const QString&,ProfileDocument&,QString&,ProfileReadDiagnostic&)> load_diagnostic;
    decltype(analyze_files) scan_files;
    decltype(save) save_scan;
    decltype(load) load_scan;
    // Pure curve computation; UI owns scheduling and publication of revisions.
    std::function<CurveAnalysis(std::shared_ptr<const CurveCorpus>,
        const dsp_kernels::FrequencyCurveOptions&,std::shared_ptr<const CurveAnalysis>)> curves;
};
// Persists independent bounded scan archives; never materializes the entire
// music corpus in one ProfileDocument. Completed archives remain valid on cancel.
AnalysisResult scan_music_in_parts(
    const QStringList&, const AnalysisOptions&, const std::atomic_bool&,
    const AnalysisServices::Progress&, const decltype(AnalysisServices::scan_files)&,
    const decltype(AnalysisServices::save_scan)&, const QString& output_directory,
    qsizetype tracks_per_part=128);
}
