#include "somike/core/process_tree.h"

#include <tlhelp32.h>

#include <cwchar>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"

namespace somike::core {

std::vector<ProcessEntry> SnapshotProcesses() {
    std::vector<ProcessEntry> result;
    win32::UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) {
        static_cast<void>(snapshot.release());  // INVALID_HANDLE_VALUE must not be closed
        return result;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = ::Process32FirstW(snapshot.get(), &entry); ok;
         ok = ::Process32NextW(snapshot.get(), &entry)) {
        result.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    }
    return result;
}

DWORD FindAppRoot(const std::vector<ProcessEntry>& processes, DWORD pid) {
    std::unordered_map<DWORD, const ProcessEntry*> by_pid;
    for (const ProcessEntry& entry : processes) {
        by_pid.emplace(entry.pid, &entry);
    }
    const auto start = by_pid.find(pid);
    if (start == by_pid.end()) {
        return pid;
    }
    const ProcessEntry* current = start->second;
    std::unordered_set<DWORD> visited{pid};
    for (;;) {
        const auto parent = by_pid.find(current->parent_pid);
        if (parent == by_pid.end() || !visited.insert(parent->first).second ||
            !text::EqualsIgnoreCase(parent->second->executable, current->executable)) {
            return current->pid;
        }
        current = parent->second;
    }
}

DWORD FindAppRootByExecutable(const std::vector<ProcessEntry>& processes,
                              const std::wstring& executable) {
    DWORD fallback = 0;
    for (const ProcessEntry& entry : processes) {
        if (!text::EqualsIgnoreCase(entry.executable, executable)) {
            continue;
        }
        const DWORD root = FindAppRoot(processes, entry.pid);
        if (root == entry.pid) {
            return root;  // a real root: done
        }
        if (fallback == 0) {
            fallback = root;
        }
    }
    return fallback;
}

std::wstring ProcessImagePath(DWORD pid) {
    win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (!::QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
        return {};
    }
    return std::wstring(path, size);
}

std::wstring StripExeExtension(const std::wstring& executable) {
    constexpr std::wstring_view kExe = L".exe";
    if (executable.size() > kExe.size() &&
        text::EqualsIgnoreCase(std::wstring_view(executable).substr(executable.size() - kExe.size()),
                               kExe)) {
        return executable.substr(0, executable.size() - kExe.size());
    }
    return executable;
}

std::wstring FriendlyAppName(const std::wstring& image_path, const std::wstring& executable) {
    if (!image_path.empty()) {
        DWORD ignored = 0;
        const DWORD size = ::GetFileVersionInfoSizeW(image_path.c_str(), &ignored);
        if (size != 0) {
            std::vector<BYTE> data(size);
            if (::GetFileVersionInfoW(image_path.c_str(), 0, size, data.data())) {
                struct Translation {
                    WORD language;
                    WORD codepage;
                };
                Translation* translations = nullptr;
                UINT length = 0;
                if (::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                                     reinterpret_cast<void**>(&translations), &length) &&
                    length >= sizeof(Translation)) {
                    wchar_t key[64];
                    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                               translations[0].language, translations[0].codepage);
                    wchar_t* description = nullptr;
                    UINT chars = 0;
                    if (::VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&description),
                                         &chars) &&
                        description != nullptr && chars > 1) {
                        // Some apps (e.g. Spotify) report a length beyond the
                        // string's terminator: stop at the first NUL.
                        const std::size_t name_length = ::wcsnlen(description, chars);
                        std::wstring name(text::Trim(std::wstring_view(description, name_length)));
                        if (!name.empty()) {
                            return name;
                        }
                    }
                }
            }
        }
    }
    return StripExeExtension(executable);
}

}  // namespace somike::core
