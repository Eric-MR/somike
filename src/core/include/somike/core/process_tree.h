// Process helpers for picking what to capture.
//
// Many apps (Spotify, Chrome, Discord - anything Chromium/Electron based) run
// several processes with the same executable name, and the one that owns the
// audio session is often a child. Capturing the *root* of that same-named
// process tree (process loopback "include tree" mode) catches all of them and
// survives the audio child being restarted.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace somike::core {

struct ProcessEntry {
    DWORD pid = 0;
    DWORD parent_pid = 0;
    std::wstring executable;  // file name only, e.g. "Spotify.exe"
};

// All running processes (Toolhelp snapshot). Empty on failure.
[[nodiscard]] std::vector<ProcessEntry> SnapshotProcesses();

// Pure: walks up the parent chain from `pid` while the parent runs the same
// executable (case-insensitive) and returns the top-most such process.
// Returns `pid` itself if it is not in `processes`. Cycle-safe.
[[nodiscard]] DWORD FindAppRoot(const std::vector<ProcessEntry>& processes, DWORD pid);

// Pure: the root (see FindAppRoot) of the first process running `executable`,
// preferring roots, or 0 if none runs.
[[nodiscard]] DWORD FindAppRootByExecutable(const std::vector<ProcessEntry>& processes,
                                            const std::wstring& executable);

// Full image path of a running process, or empty.
[[nodiscard]] std::wstring ProcessImagePath(DWORD pid);

// Human-readable app name from the executable's version resource
// ("Spotify", "Google Chrome"); falls back to the file name without ".exe".
[[nodiscard]] std::wstring FriendlyAppName(const std::wstring& image_path,
                                           const std::wstring& executable);

// Pure: "Spotify.exe" -> "Spotify".
[[nodiscard]] std::wstring StripExeExtension(const std::wstring& executable);

}  // namespace somike::core
