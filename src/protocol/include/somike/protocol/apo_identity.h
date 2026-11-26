// Identity of the Somike capture APO. Shared by the APO DLL (which reports it
// through GetRegistrationProperties) and the installer (which writes it to the
// registry), so both always agree.
#pragma once

#include <cstdint>

namespace somike::protocol {

// {B5F655D4-FC22-4DDB-9240-494757B5BF74}
inline constexpr wchar_t kApoClsidString[] = L"{B5F655D4-FC22-4DDB-9240-494757B5BF74}";

struct GuidParts {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

inline constexpr GuidParts kApoClsid = {
    0xB5F655D4, 0xFC22, 0x4DDB, {0x92, 0x40, 0x49, 0x47, 0x57, 0xB5, 0xBF, 0x74}};

inline constexpr wchar_t kApoFriendlyName[] = L"Somike Microphone Injector";
inline constexpr wchar_t kApoCopyright[] = L"Copyright (c) Somike contributors";
inline constexpr wchar_t kApoDllName[] = L"somike_apo.dll";

inline constexpr std::uint32_t kApoMajorVersion = 0;
inline constexpr std::uint32_t kApoMinorVersion = 1;

// APO_FLAG_INPLACE | APO_FLAG_DEFAULT, where APO_FLAG_DEFAULT =
// SAMPLESPERFRAME_MUST_MATCH | FRAMESPERSECOND_MUST_MATCH | BITSPERSAMPLE_MUST_MATCH.
// Verified against the SDK enum by a static_assert inside the APO.
inline constexpr std::uint32_t kApoFlags = 0x0000000F;

}  // namespace somike::protocol
