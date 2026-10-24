#pragma once
#include "profile_json.h"
#include <QStringList>
#include <atomic>
#include <functional>

namespace desktop_analysis::offline {
struct AnalysisOptions {
    dsp_kernels::ProfileCompatibility compatibility;
    dsp_kernels::ProfileTraining training;
    unsigned workers=2;
    std::size_t preview_time_bins=256;
    std::size_t preview_frequency_bins=256;
};
struct AnalysisResult {
    ProfileDocument document;
    SpectrogramPreview preview;
    QStringList errors;
    bool cancelled=false, success=false;
    double wall_seconds=0, audio_seconds=0;
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
};
}
