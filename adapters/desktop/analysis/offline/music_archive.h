#pragma once
#include "profile_json.h"
#include <QDir>
#include <QJsonArray>

namespace desktop_analysis::offline {
enum class MusicArchiveKind { Scan,Profile };
bool valid_profile_storage(const ProfileDocument&,QString& error);
bool valid_scan_measurements(const ProfileDocument&,QString& error);
bool save_music_archive(const QString&,const ProfileDocument&,MusicArchiveKind,QString& error);
// Common output writer for both a one-part .brscan and a large ZIP64 .brscan.
struct MusicScanSummaryAccumulator {
    std::vector<long double> peak, mean, presence;
    std::vector<std::size_t> contributors;
    std::size_t tracks=0, fft_size=0;
    bool add(const ProfileDocument& part,QString& error);
    QJsonObject result()const;
};
bool save_music_scan_collection(const QDir& part_directory,const QString& path,
                                const QJsonObject& manifest,const QJsonArray& parts,QString& error,
                                const QJsonObject& summary={});
bool load_music_archive(const QString&,MusicArchiveKind,ProfileDocument&,QString& error,ProfileReadDiagnostic&);
}
