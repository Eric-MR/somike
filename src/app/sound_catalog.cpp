#include "sound_catalog.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <nlohmann/json.hpp>

#include "soundboard_library.h"

namespace somike::app {

using nlohmann::json;

namespace {

constexpr std::array<std::string_view, 2> kDownloadDomains = {"freesound.org", "wikimedia.org"};
constexpr std::array<std::string_view, 4> kLinkDomains = {"freesound.org", "wikimedia.org", "openverse.org",
                                                          "creativecommons.org"};
constexpr std::array<std::string_view, 3> kFileTypes = {"mp3", "wav", "flac"};
constexpr std::array<std::string_view, 2> kLicenses = {"cc0", "pdm"};
constexpr std::size_t kMaxUrlBytes = 2048;

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    return out;
}

// "https://host/path" -> "host" (lower case), or "" if the URL is unusable.
std::string HttpsHost(std::string_view url) {
    constexpr std::string_view kScheme = "https://";
    if (url.size() > kMaxUrlBytes || !Lower(url.substr(0, kScheme.size())).starts_with(kScheme)) {
        return {};
    }
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`' || c == '{' ||
            c == '}' || c == '|' || c == '^') {
            return {};
        }
    }
    const std::string_view rest = url.substr(kScheme.size());
    const std::string_view host = rest.substr(0, rest.find_first_of("/?#"));
    if (host.empty() || host.find_first_of("@:") != std::string_view::npos) {
        return {};  // user info or explicit port
    }
    return Lower(host);
}

template <std::size_t N>
bool HostIn(const std::string& host, const std::array<std::string_view, N>& domains) {
    if (host.empty()) {
        return false;
    }
    return std::any_of(domains.begin(), domains.end(), [&](std::string_view domain) {
        return host == domain || (host.size() > domain.size() && host.ends_with(domain) &&
                                  host[host.size() - domain.size() - 1] == '.');
    });
}

std::string GetString(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::uint64_t GetNumber(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return 0;
    }
    const double value = it->get<double>();
    return std::isfinite(value) && value > 0.0 && value < 1e15 ? static_cast<std::uint64_t>(value) : 0;
}

template <std::size_t N>
bool OneOf(const std::string& value, const std::array<std::string_view, N>& allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

}  // namespace

std::string UrlEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[u >> 4];
            out += kHex[u & 0x0F];
        }
    }
    return out;
}

std::string OpenverseSearchUrl(std::string_view query, std::uint32_t page) {
    const std::string q = CleanText(query, kMaxQueryChars);
    page = std::clamp<std::uint32_t>(page, 1, kCatalogMaxPage);
    return "https://api.openverse.org/v1/audio/?q=" + UrlEncode(q) +
           "&license=cc0,pdm&length=shortest&extension=mp3,wav,flac&mature=false&page_size=" +
           std::to_string(kCatalogPageSize) + "&page=" + std::to_string(page);
}

bool IsAllowedDownloadUrl(std::string_view url) { return HostIn(HttpsHost(url), kDownloadDomains); }

bool IsAllowedLinkUrl(std::string_view url) { return HostIn(HttpsHost(url), kLinkDomains); }

bool IsValidCatalogId(std::string_view id) {
    if (id.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::optional<CatalogPage> ParseOpenverseResults(std::string_view text) {
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object()) {
        return std::nullopt;
    }
    const auto results = root.find("results");
    if (results == root.end() || !results->is_array()) {
        return std::nullopt;
    }

    CatalogPage page;
    page.result_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(GetNumber(root, "result_count"), 1u << 30));
    page.page_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(GetNumber(root, "page_count"), kCatalogMaxPage));
    page.page = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(GetNumber(root, "page"), 1, kCatalogMaxPage));

    for (const json& r : *results) {
        if (!r.is_object() || page.items.size() >= kCatalogPageSize * 2) {
            continue;
        }
        if (const auto mature = r.find("mature"); mature != r.end() && mature->is_boolean() && mature->get<bool>()) {
            continue;
        }
        CatalogItem item;
        item.id = GetString(r, "id");
        item.license = Lower(GetString(r, "license"));
        item.download_url = GetString(r, "url");
        item.filetype = Lower(GetString(r, "filetype"));
        if (!IsValidCatalogId(item.id) || !OneOf(item.license, kLicenses) ||
            !IsAllowedDownloadUrl(item.download_url) || !OneOf(item.filetype, kFileTypes)) {
            continue;
        }
        item.title = CleanText(GetString(r, "title"), 120);
        if (item.title.empty()) {
            item.title = "Untitled";
        }
        item.creator = CleanText(GetString(r, "creator"), 80);
        item.source = CleanText(GetString(r, "source"), 40);
        item.landing_url = GetString(r, "foreign_landing_url");
        if (!IsAllowedLinkUrl(item.landing_url)) {
            item.landing_url.clear();
        }
        item.duration_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(GetNumber(r, "duration"), 3600u * 1000u));
        item.filesize = GetNumber(r, "filesize");
        page.items.push_back(std::move(item));
    }
    return page;
}

}  // namespace somike::app
