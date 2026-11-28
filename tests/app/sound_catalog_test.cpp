// Openverse search URLs and the parsing/validation of its answers.
#include "sound_catalog.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace somike::app {
namespace {

using nlohmann::json;

json Result(const char* id, const char* url, const char* license = "cc0", const char* filetype = "mp3") {
    return {{"id", id},
            {"title", "Industrial Air Horn"},
            {"creator", "mcpable"},
            {"license", license},
            {"url", url},
            {"filetype", filetype},
            {"foreign_landing_url", "https://freesound.org/people/mcpable/sounds/131930"},
            {"source", "freesound"},
            {"duration", 2571},
            {"filesize", 58551},
            {"mature", false}};
}

TEST(SoundCatalog, SearchUrlEncodesTheQueryAndRestrictsLicenses) {
    const std::string url = OpenverseSearchUrl("air horn & \xC3\xA9", 2);
    EXPECT_TRUE(url.starts_with("https://api.openverse.org/v1/audio/?q=air%20horn%20%26%20%C3%A9&")) << url;
    EXPECT_NE(url.find("license=cc0,pdm"), std::string::npos);
    EXPECT_NE(url.find("length=shortest"), std::string::npos);
    EXPECT_NE(url.find("page=2"), std::string::npos);
    EXPECT_NE(OpenverseSearchUrl("x", 999).find("page=20"), std::string::npos);
}

TEST(SoundCatalog, KeepsOnlySafeFreeDownloadableItems) {
    json mature = Result("44444444-4444-4444-4444-444444444444", "https://cdn.freesound.org/d.mp3");
    mature["mature"] = true;
    const json answer = {
        {"result_count", 188},
        {"page_count", 10},
        {"page", 1},
        {"results",
         {Result("d2c87ca6-ec3e-4963-9407-5684c36cfadc", "https://cdn.freesound.org/previews/131/131930_1542102-hq.mp3"),
          Result("11111111-1111-1111-1111-111111111111", "https://cdn.freesound.org/a.mp3", "by"),
          Result("22222222-2222-2222-2222-222222222222", "http://cdn.freesound.org/b.mp3"),
          Result("33333333-3333-3333-3333-333333333333", "https://evil.example/c.mp3"),
          mature,
          Result("55555555-5555-5555-5555-555555555555", "https://upload.wikimedia.org/x.ogg", "pdm", "ogg"),
          Result("66666666-6666-6666-6666-666666666666", "https://upload.wikimedia.org/x.flac", "pdm", "flac"),
          Result("../../evil", "https://cdn.freesound.org/e.mp3")}},
    };
    const auto page = ParseOpenverseResults(answer.dump());
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->result_count, 188u);
    EXPECT_EQ(page->page_count, 10u);
    ASSERT_EQ(page->items.size(), 2u);
    EXPECT_EQ(page->items[0].title, "Industrial Air Horn");
    EXPECT_EQ(page->items[0].duration_ms, 2571u);
    EXPECT_EQ(page->items[0].license, "cc0");
    EXPECT_EQ(page->items[1].filetype, "flac");
}

TEST(SoundCatalog, GarbageIsNotAPage) {
    EXPECT_FALSE(ParseOpenverseResults("").has_value());
    EXPECT_FALSE(ParseOpenverseResults("<html>").has_value());
    EXPECT_FALSE(ParseOpenverseResults(R"({"detail":"throttled"})").has_value());
}

TEST(SoundCatalog, DownloadsOnlyFromKnownHostsOverHttps) {
    EXPECT_TRUE(IsAllowedDownloadUrl("https://cdn.freesound.org/previews/1/1-hq.mp3"));
    EXPECT_TRUE(IsAllowedDownloadUrl("https://upload.wikimedia.org/wikipedia/commons/a/ab/X.flac"));
    EXPECT_FALSE(IsAllowedDownloadUrl("http://cdn.freesound.org/x.mp3"));
    EXPECT_FALSE(IsAllowedDownloadUrl("https://freesound.org.evil.com/x.mp3"));
    EXPECT_FALSE(IsAllowedDownloadUrl("https://evilfreesound.org/x.mp3"));
    EXPECT_FALSE(IsAllowedDownloadUrl("https://user@cdn.freesound.org/x.mp3"));
    EXPECT_FALSE(IsAllowedDownloadUrl("https://cdn.freesound.org:8443/x.mp3"));
    EXPECT_FALSE(IsAllowedDownloadUrl("https://cdn.freesound.org/x y.mp3"));
    EXPECT_FALSE(IsAllowedDownloadUrl("https://openverse.org/x.mp3"));  // a link host, not a download host

    EXPECT_TRUE(IsAllowedLinkUrl("https://freesound.org/people/x/sounds/1"));
    EXPECT_TRUE(IsAllowedLinkUrl("https://creativecommons.org/publicdomain/zero/1.0/"));
    EXPECT_FALSE(IsAllowedLinkUrl("file:///C:/Windows/System32/calc.exe"));
    EXPECT_FALSE(IsAllowedLinkUrl("javascript:alert(1)"));
}

TEST(SoundCatalog, CatalogIdsAreUuids) {
    EXPECT_TRUE(IsValidCatalogId("d2c87ca6-ec3e-4963-9407-5684c36cfadc"));
    EXPECT_FALSE(IsValidCatalogId("D2C87CA6-EC3E-4963-9407-5684C36CFADC"));
    EXPECT_FALSE(IsValidCatalogId("..\\..\\..\\windows\\x"));
    EXPECT_FALSE(IsValidCatalogId(""));
}

}  // namespace
}  // namespace somike::app
