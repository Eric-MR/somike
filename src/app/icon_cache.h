// App icons for the source tiles: the executable's shell icon rendered to a
// PNG data URL ("data:image/png;base64,...") the HTML UI can show directly.
#pragma once

#include <map>
#include <optional>
#include <string>

namespace somike::app {

class IconCache {
public:
    // Requires COM on the calling thread. Results (including failures) are
    // cached per path, so each executable is rendered once.
    [[nodiscard]] std::optional<std::string> DataUrl(const std::wstring& image_path);

private:
    std::map<std::wstring, std::optional<std::string>> cache_;
};

}  // namespace somike::app
