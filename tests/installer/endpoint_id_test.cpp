#include <gtest/gtest.h>

#include "somike/installer/endpoint_id.h"

namespace somike::installer {
namespace {

constexpr wchar_t kCaptureId[] = L"{0.0.1.00000000}.{A1B2C3D4-1111-2222-3333-444455556666}";

TEST(EndpointId, ParsesCaptureIds) {
    const auto id = ParseEndpointId(kCaptureId);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->flow, Flow::Capture);
    EXPECT_EQ(id->guid, L"{a1b2c3d4-1111-2222-3333-444455556666}");
    EXPECT_EQ(id->full, kCaptureId);
}

TEST(EndpointId, ParsesRenderIds) {
    const auto id = ParseEndpointId(L"{0.0.0.00000000}.{A1B2C3D4-1111-2222-3333-444455556666}");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->flow, Flow::Render);
}

TEST(EndpointId, RejectsMalformedIds) {
    EXPECT_FALSE(ParseEndpointId(L""));
    EXPECT_FALSE(ParseEndpointId(L"{0.0.1.00000000}."));
    EXPECT_FALSE(ParseEndpointId(L"{0.0.2.00000000}.{A1B2C3D4-1111-2222-3333-444455556666}"));
    EXPECT_FALSE(ParseEndpointId(L"{0.0.1.00000000}.{not-a-guid}"));
    EXPECT_FALSE(ParseEndpointId(L"{0.0.1.00000000}.{A1B2C3D4-1111-2222-3333-444455556666}x"));
}

TEST(EndpointId, BuildsRegistryPaths) {
    const auto id = ParseEndpointId(kCaptureId);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(FxPropertiesKeyPath(*id),
              L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\"
              L"{a1b2c3d4-1111-2222-3333-444455556666}\\FxProperties");
    EXPECT_EQ(PropertiesKeyPath(*id),
              L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\"
              L"{a1b2c3d4-1111-2222-3333-444455556666}\\Properties");
    EXPECT_EQ(BackupKeyPath(*id),
              L"SOFTWARE\\Somike\\Endpoints\\{a1b2c3d4-1111-2222-3333-444455556666}");
}

}  // namespace
}  // namespace somike::installer
