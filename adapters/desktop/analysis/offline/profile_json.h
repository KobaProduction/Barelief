#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace desktop_analysis::offline {
struct SpectrogramPreview {
    QString source;
    std::size_t rows=0, bands=0;
    double duration_seconds=0, sample_rate=0, bin_hz=0;
    // Frame-major, complete track compressed by temporal averaging; contiguous FFT bins averaged into bounded display bands.
    std::vector<float> values;
    // Explicit bucket centers: time from track start, frequency from averaged FFT bins.
    std::vector<double> time_seconds, frequency_hz;
    std::vector<std::uint64_t> counts;
    std::vector<float> normalized(const std::vector<float>& gains) const;
};
struct ProfileDocument {
    dsp_kernels::FrequencyProfile profile;
    QJsonObject preserved;
    // Optional bounded full-track images, aligned with profile.compositions.
    std::vector<SpectrogramPreview> previews;
    QJsonObject archive_preserved;
};
enum class ProfileReadIssue { None, Encoding, FftSize, SampleRate, Layout, Representation, Window,
    CoherentGain, PowerGain, Frontend, Resampler, GainCount, GainValue };
struct ProfileReadDiagnostic { ProfileReadIssue reason=ProfileReadIssue::None; QString parameter; };
bool decode_profile_diagnostic(const QByteArray&,ProfileDocument&,QString&,ProfileReadDiagnostic&);
bool load_profile_diagnostic(const QString&,ProfileDocument&,QString&,ProfileReadDiagnostic&);

bool decode_profile(const QByteArray&, ProfileDocument& output, QString& error);
QByteArray encode_profile(const ProfileDocument&);
bool save_profile(const QString& path, const ProfileDocument&, QString& error);
bool load_profile(const QString& path, ProfileDocument&, QString& error);
QByteArray encode_music_scan(const ProfileDocument&);
bool decode_music_scan(const QByteArray&,ProfileDocument&,QString& error);
bool save_music_scan(const QString&,const ProfileDocument&,QString& error);
// Internal chunks only. Public save_music_scan always produces unified .brscan.
bool save_music_scan_part(const QString&,const ProfileDocument&,QString& error);
bool load_music_scan(const QString&,ProfileDocument&,QString& error);
struct MusicScanCollectionInfo {
    std::size_t requested_count=0;
    std::size_t processed_count=0;
    std::size_t failed_count=0;
    std::size_t part_count=0;
};
// One external .brscan API for both classic scans and ZIP64 collections.
// The callback receives exactly ONE bounded part at a time. No corpus-wide
// CompositionStatistics vector is ever retained.
using MusicScanPartVisitor=std::function<bool(ProfileDocument&&,std::size_t part_index)>;
bool visit_music_scan_parts(const QString&,const MusicScanPartVisitor&,MusicScanCollectionInfo&,
                            QString& error);
bool inspect_music_scan(const QString&,MusicScanCollectionInfo&,QString& error);

}
