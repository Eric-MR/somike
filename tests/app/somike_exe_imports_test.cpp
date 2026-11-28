// somike.exe must start on every Windows 11 edition. Windows N editions have
// no Media Foundation unless the Media Feature Pack is installed, so it may
// only be delay-loaded (the soundboard then reports it per sound).
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

// somike.exe is built next to the test executable.
std::wstring SomikeExePath() {
    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, length);
    result.resize(result.find_last_of(L'\\') + 1);
    return result + L"somike.exe";
}

std::string Lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct Imports {
    std::vector<std::string> direct;   // resolved by the loader at start
    std::vector<std::string> delayed;  // loaded on first call
};

// Maps the file as an image (no code runs, no imports resolved) and reads
// the names from its import and delay-import directories.
bool ReadImports(const std::wstring& path, Imports& out) {
    const HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == nullptr) {
        return false;
    }
    // The low bits of the handle mark a resource mapping.
    const auto* base = reinterpret_cast<const BYTE*>(reinterpret_cast<ULONG_PTR>(module) & ~ULONG_PTR{3});
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY* dirs = nt->OptionalHeader.DataDirectory;

    if (const IMAGE_DATA_DIRECTORY& dir = dirs[IMAGE_DIRECTORY_ENTRY_IMPORT]; dir.VirtualAddress != 0) {
        for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name != 0; ++d) {
            out.direct.push_back(Lower(reinterpret_cast<const char*>(base + d->Name)));
        }
    }
    if (const IMAGE_DATA_DIRECTORY& dir = dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]; dir.VirtualAddress != 0) {
        for (auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress);
             d->DllNameRVA != 0; ++d) {
            out.delayed.push_back(Lower(reinterpret_cast<const char*>(base + d->DllNameRVA)));
        }
    }
    ::FreeLibrary(module);
    return true;
}

}  // namespace

TEST(SomikeExeImports, MediaFoundationIsOnlyDelayLoaded) {
    Imports imports;
    ASSERT_TRUE(ReadImports(SomikeExePath(), imports)) << "somike.exe not found next to the tests";
    ASSERT_FALSE(imports.direct.empty());
    for (const char* dll : {"mfplat.dll", "mfreadwrite.dll"}) {
        EXPECT_EQ(std::ranges::count(imports.direct, dll), 0) << dll << " is imported directly";
        EXPECT_EQ(std::ranges::count(imports.delayed, dll), 1) << dll << " is not delay-loaded";
    }
}
