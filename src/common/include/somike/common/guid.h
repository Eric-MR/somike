#pragma once

#include <guiddef.h>

#include <optional>
#include <string>
#include <string_view>

#include "somike/protocol/apo_identity.h"

namespace somike::guid {

[[nodiscard]] constexpr GUID FromParts(const protocol::GuidParts& parts) noexcept {
    return GUID{parts.data1,
                parts.data2,
                parts.data3,
                {parts.data4[0], parts.data4[1], parts.data4[2], parts.data4[3], parts.data4[4],
                 parts.data4[5], parts.data4[6], parts.data4[7]}};
}

// "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" (upper case, with braces).
[[nodiscard]] std::wstring ToString(const GUID& value);

// Accepts the braced registry form, case-insensitive.
[[nodiscard]] std::optional<GUID> Parse(std::wstring_view text);

}  // namespace somike::guid
