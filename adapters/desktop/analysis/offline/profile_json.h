#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <QJsonObject>
#include <QString>

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
bool load_music_scan(const QString&,ProfileDocument&,QString& error);
}
