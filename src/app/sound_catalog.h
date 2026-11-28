// Free sound effects to download into the soundboard.
//
// Source: Openverse (https://openverse.org, run by WordPress.org), an open
// search engine over openly licensed media. Its audio index covers Freesound
// and Wikimedia Commons, needs no API key (anonymous limit: 100 requests per
// minute, 1000 per day, per IP) and returns direct download links with the
// license of every file.
//
// Searches are restricted to CC0 and public-domain sounds: free to use for
// anything, no credit required, which matters for sounds played into a voice
// chat. The app still keeps and shows the credit of every downloaded sound.
//
// Pure (no Win32), so it is unit-tested.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace somike::app {

struct CatalogItem {
    std::string id;            // Openverse id (UUID)
    std::string title;
    std::string creator;
    std::string license;       // "cc0" or "pdm"
    std::string landing_url;   // the sound's page on Freesound / Wikimedia Commons
    std::string download_url;  // https, on an allowed host
    std::string filetype;      // "mp3", "wav" or "flac"
    std::string source;        // "freesound", "wikimedia_audio", ...
    std::uint32_t duration_ms = 0;
    std::uint64_t filesize = 0;  // bytes, 0 if unknown
};

struct CatalogPage {
    std::vector<CatalogItem> items;
    std::uint32_t page = 1;
    std::uint32_t page_count = 0;
    std::uint32_t result_count = 0;
};

inline constexpr std::uint32_t kCatalogPageSize = 20;
inline constexpr std::uint32_t kCatalogMaxPage = 20;
inline constexpr std::size_t kMaxQueryChars = 100;

// Short clips (< 30 s), CC0/public domain, MP3/WAV/FLAC only.
[[nodiscard]] std::string OpenverseSearchUrl(std::string_view query, std::uint32_t page);
// Items that fail validation (license, host, file type, id) are skipped.
[[nodiscard]] std::optional<CatalogPage> ParseOpenverseResults(std::string_view json);

[[nodiscard]] std::string UrlEncode(std::string_view text);
// https on freesound.org or wikimedia.org (or a subdomain); no user info,
// port or characters that need quoting.
[[nodiscard]] bool IsAllowedDownloadUrl(std::string_view url);
// Pages the app may open in the browser (credit links): the download hosts
// plus openverse.org and creativecommons.org.
[[nodiscard]] bool IsAllowedLinkUrl(std::string_view url);
[[nodiscard]] bool IsValidCatalogId(std::string_view id);

}  // namespace somike::app
