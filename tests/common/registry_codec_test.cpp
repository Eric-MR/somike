#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "somike/common/registry.h"

namespace somike::reg {
namespace {

TEST(RegistryCodec, MultiStringRoundTrips) {
    const std::vector<std::wstring> items = {L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}", L"b"};
    EXPECT_EQ(ParseMultiString(SerializeMultiString(items)), items);
}

TEST(RegistryCodec, EmptyListIsADoubleTerminator) {
    const std::vector<std::uint8_t> data = SerializeMultiString({});
    EXPECT_EQ(data.size(), 2 * sizeof(wchar_t));
    EXPECT_TRUE(ParseMultiString(data).empty());
}

TEST(RegistryCodec, EmptyEntriesAreDropped) {
    EXPECT_EQ(ParseMultiString(SerializeMultiString({L"a", L"", L"c"})),
              (std::vector<std::wstring>{L"a", L"c"}));
}

TEST(RegistryCodec, ParsingToleratesMissingTerminators) {
    const std::wstring chars = L"one";  // no terminator at all
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(chars.data());
    const std::vector<std::uint8_t> data(bytes, bytes + chars.size() * sizeof(wchar_t));
    EXPECT_EQ(ParseMultiString(data), (std::vector<std::wstring>{L"one"}));
}

TEST(RegistryCodec, MakeStringIncludesTheTerminator) {
    const RawValue value = MakeString(L"ab");
    EXPECT_EQ(value.type, static_cast<DWORD>(REG_SZ));
    EXPECT_EQ(value.data.size(), 3 * sizeof(wchar_t));
}

TEST(RegistryCodec, MakeDword) {
    const RawValue value = MakeDword(0x01020304);
    EXPECT_EQ(value.type, static_cast<DWORD>(REG_DWORD));
    EXPECT_EQ(value.data, (std::vector<std::uint8_t>{0x04, 0x03, 0x02, 0x01}));
}

}  // namespace
}  // namespace somike::reg
