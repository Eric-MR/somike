#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace somike::core {

// A process that currently owns an audio session on some playback device -
// i.e. something whose output can be routed.
struct AudioSource {
    DWORD process_id = 0;
    std::wstring executable;    // "Spotify.exe"
    std::wstring display_name;  // session display name; often empty
    bool active = false;        // currently playing audio
};

// Enumerates audio sessions on all active render endpoints, merged per
// process, active ones first. Excludes system sounds and the calling process.
// The calling thread must have COM initialised.
[[nodiscard]] std::vector<AudioSource> EnumerateAudioSources();

}  // namespace somike::core
