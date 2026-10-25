#pragma once
#include "profile_json.h"

namespace desktop_analysis::offline {
enum class MusicArchiveKind { Scan,Profile };
bool valid_profile_storage(const ProfileDocument&,QString& error);
bool valid_scan_measurements(const ProfileDocument&,QString& error);
bool save_music_archive(const QString&,const ProfileDocument&,MusicArchiveKind,QString& error);
bool load_music_archive(const QString&,MusicArchiveKind,ProfileDocument&,QString& error,ProfileReadDiagnostic&);
}
