#include "somike/core/process_tree.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace somike::core {
namespace {

// Typical Chromium/Electron layout: explorer -> Spotify (root) -> Spotify
// children (renderer, GPU, audio utility...).
const std::vector<ProcessEntry> kTree = {
    {4, 0, L"System"},
    {1000, 4, L"explorer.exe"},
    {2000, 1000, L"Spotify.exe"},  // root
    {2001, 2000, L"Spotify.exe"},  // gpu
    {2002, 2000, L"spotify.exe"},  // audio utility (case differs)
    {2003, 2002, L"Spotify.exe"},  // grandchild
    {3000, 1000, L"Discord.exe"},
    {3001, 3000, L"crashpad_handler.exe"},
};

TEST(ProcessTree, ChildResolvesToTheTopMostSameNamedAncestor) {
    EXPECT_EQ(FindAppRoot(kTree, 2003), 2000u);
    EXPECT_EQ(FindAppRoot(kTree, 2002), 2000u);
    EXPECT_EQ(FindAppRoot(kTree, 2000), 2000u);
}

TEST(ProcessTree, StopsAtADifferentExecutable) {
    EXPECT_EQ(FindAppRoot(kTree, 3001), 3001u);  // crashpad's parent is Discord
    EXPECT_EQ(FindAppRoot(kTree, 3000), 3000u);  // Discord's parent is explorer
}

TEST(ProcessTree, UnknownPidResolvesToItself) { EXPECT_EQ(FindAppRoot(kTree, 4242), 4242u); }

TEST(ProcessTree, SurvivesParentCycles) {
    // PID reuse can produce cycles in a snapshot.
    const std::vector<ProcessEntry> cycle = {{10, 11, L"a.exe"}, {11, 10, L"a.exe"}};
    const DWORD root = FindAppRoot(cycle, 10);
    EXPECT_TRUE(root == 10u || root == 11u);
}

TEST(ProcessTree, FindsTheRootByExecutableName) {
    EXPECT_EQ(FindAppRootByExecutable(kTree, L"SPOTIFY.EXE"), 2000u);
    EXPECT_EQ(FindAppRootByExecutable(kTree, L"Discord.exe"), 3000u);
    EXPECT_EQ(FindAppRootByExecutable(kTree, L"vlc.exe"), 0u);
}

TEST(ProcessTree, StripsTheExeExtension) {
    EXPECT_EQ(StripExeExtension(L"Spotify.exe"), L"Spotify");
    EXPECT_EQ(StripExeExtension(L"GAME.EXE"), L"GAME");
    EXPECT_EQ(StripExeExtension(L".exe"), L".exe");
    EXPECT_EQ(StripExeExtension(L"tool"), L"tool");
}

TEST(ProcessTree, SnapshotContainsTheCurrentProcess) {
    const std::vector<ProcessEntry> processes = SnapshotProcesses();
    const DWORD self = ::GetCurrentProcessId();
    EXPECT_TRUE(std::any_of(processes.begin(), processes.end(),
                            [&](const ProcessEntry& p) { return p.pid == self; }));
    EXPECT_FALSE(ProcessImagePath(self).empty());
}

}  // namespace
}  // namespace somike::core
