#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "somike/common/registry.h"
#include "somike/installer/fx_config.h"
#include "somike/installer/install_plan.h"
#include "somike/protocol/apo_identity.h"

namespace somike::installer {
namespace {

const std::wstring kSomike = protocol::kApoClsidString;
const std::wstring kOem = L"{11111111-2222-3333-4444-555555555555}";
const std::wstring kOther = L"{99999999-8888-7777-6666-555555555555}";

FxSnapshot Endpoint() {
    FxSnapshot snapshot;
    snapshot.fx_key_exists = true;
    return snapshot;
}

void SetString(FxSnapshot& s, const wchar_t* name, const std::wstring& value) {
    s.Set(name, reg::MakeString(value));
}

void SetMulti(FxSnapshot& s, const wchar_t* name, const std::vector<std::wstring>& value) {
    s.Set(name, reg::MakeMultiString(value));
}

InstallState StateAfter(FxSnapshot snapshot, const InstallPlan& plan) {
    ApplyToSnapshot(snapshot, plan.fx_edits);
    return QueryInstallState(snapshot);
}

TEST(InstallPlan, FreshMicrophoneGetsAnEndpointEffect) {
    const FxSnapshot s = Endpoint();
    const InstallPlan plan = PlanInstall(s, Strategy::Auto);

    ASSERT_EQ(plan.status, PlanStatus::Ready);
    ASSERT_EQ(plan.fx_edits.size(), 2u);
    EXPECT_EQ(plan.fx_edits[0].name, fx::kEndpointEffect);
    EXPECT_EQ(plan.fx_edits[0].string_value, kSomike);
    EXPECT_EQ(plan.fx_edits[1].name, fx::kEndpointProcessingModes);
    EXPECT_EQ(plan.fx_edits[1].multi_value, (std::vector<std::wstring>{fx::kModeDefault}));
    EXPECT_FALSE(plan.enable_effects);
    EXPECT_EQ(StateAfter(s, plan), InstallState::InstalledEndpointEffect);
}

TEST(InstallPlan, ExistingProcessingModesAreKept) {
    FxSnapshot s = Endpoint();
    SetMulti(s, fx::kEndpointProcessingModes, {fx::kModeDefault});
    const InstallPlan plan = PlanInstall(s, Strategy::Auto);

    ASSERT_EQ(plan.status, PlanStatus::Ready);
    ASSERT_EQ(plan.fx_edits.size(), 1u);
    EXPECT_EQ(plan.fx_edits[0].name, fx::kEndpointEffect);
}

TEST(InstallPlan, AutoNeverDisplacesADriverEndpointEffect) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kEndpointEffect, kOem);
    const InstallPlan plan = PlanInstall(s, Strategy::Auto);

    EXPECT_EQ(plan.status, PlanStatus::Refused);
    EXPECT_TRUE(plan.fx_edits.empty());
    EXPECT_FALSE(plan.notes.empty());
}

TEST(InstallPlan, AutoAppendsToAnExistingCompositeChain) {
    FxSnapshot s = Endpoint();
    SetMulti(s, fx::kCompositeEndpointEffect, {kOem});
    const InstallPlan plan = PlanInstall(s, Strategy::Auto);

    ASSERT_EQ(plan.status, PlanStatus::Ready);
    ASSERT_EQ(plan.fx_edits.size(), 1u);
    EXPECT_EQ(plan.fx_edits[0].kind, RegistryEdit::Kind::SetMultiString);
    EXPECT_EQ(plan.fx_edits[0].multi_value, (std::vector<std::wstring>{kOem, kSomike}));
    EXPECT_EQ(StateAfter(s, plan), InstallState::InstalledCompositeEndpoint);
}

TEST(InstallPlan, AutoRefusesWhenOnlyLegacyEffectsExist) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kPreMixEffect, kOem);
    EXPECT_EQ(PlanInstall(s, Strategy::Auto).status, PlanStatus::Refused);
}

TEST(InstallPlan, AutoCoexistsWithStreamAndModeEffects) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kOem);
    SetString(s, fx::kModeEffect, kOther);
    const InstallPlan plan = PlanInstall(s, Strategy::Auto);

    ASSERT_EQ(plan.status, PlanStatus::Ready);
    FxSnapshot after = s;
    ApplyToSnapshot(after, plan.fx_edits);
    EXPECT_EQ(QueryInstallState(after), InstallState::InstalledEndpointEffect);
    EXPECT_EQ(after.String(fx::kStreamEffect), kOem);
    EXPECT_EQ(after.String(fx::kModeEffect), kOther);
}

TEST(InstallPlan, CompositeStrategyChainsAfterTheDriverEffect) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kEndpointEffect, kOem);
    const InstallPlan plan = PlanInstall(s, Strategy::Composite);

    ASSERT_EQ(plan.status, PlanStatus::Ready);
    ASSERT_EQ(plan.fx_edits.size(), 1u);
    EXPECT_EQ(plan.fx_edits[0].name, fx::kCompositeEndpointEffect);
    EXPECT_EQ(plan.fx_edits[0].multi_value, (std::vector<std::wstring>{kOem, kSomike}));
    // The single-slot value is left alone.
    FxSnapshot after = s;
    ApplyToSnapshot(after, plan.fx_edits);
    EXPECT_EQ(after.String(fx::kEndpointEffect), kOem);
}

TEST(InstallPlan, ReplaceStrategyTakesOverTheEndpointSlot) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kEndpointEffect, kOem);
    SetMulti(s, fx::kCompositeEndpointEffect, {kOther});
    const InstallPlan plan = PlanInstall(s, Strategy::Replace);

    ASSERT_EQ(plan.status, PlanStatus::Ready);
    FxSnapshot after = s;
    ApplyToSnapshot(after, plan.fx_edits);
    EXPECT_EQ(after.String(fx::kEndpointEffect), kSomike);
    EXPECT_FALSE(after.Has(fx::kCompositeEndpointEffect));
    EXPECT_EQ(after.MultiString(fx::kEndpointProcessingModes),
              (std::vector<std::wstring>{fx::kModeDefault}));
}

TEST(InstallPlan, DetectsAnExistingInstallationCaseInsensitively) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kEndpointEffect, L"{b5f655d4-fc22-4ddb-9240-494757b5bf74}");
    const InstallPlan plan = PlanInstall(s, Strategy::Auto);

    EXPECT_EQ(plan.status, PlanStatus::AlreadyInstalled);
    EXPECT_TRUE(plan.fx_edits.empty());
}

// The 0.1.0 layout (stream effect) is skipped by raw-mode capture (Discord).
TEST(InstallPlan, UpgradesAStreamEffectInstallationToTheEndpointEffect) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kSomike);
    SetMulti(s, fx::kStreamProcessingModes, {fx::kModeDefault, fx::kModeCommunications});
    ASSERT_TRUE(NeedsUpgrade(QueryInstallState(s)));

    const InstallPlan plan = PlanInstall(s, Strategy::Auto);
    ASSERT_EQ(plan.status, PlanStatus::Upgrade);
    FxSnapshot after = s;
    ApplyToSnapshot(after, plan.fx_edits);
    EXPECT_FALSE(after.Has(fx::kStreamEffect));
    EXPECT_EQ(after.String(fx::kEndpointEffect), kSomike);
    EXPECT_EQ(QueryInstallState(after), InstallState::InstalledEndpointEffect);
    EXPECT_FALSE(NeedsUpgrade(QueryInstallState(after)));
}

TEST(InstallPlan, UpgradeKeepsADriverStreamEffectInTheCompositeChain) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kOem);
    SetMulti(s, fx::kCompositeStreamEffect, {kOem, kSomike});

    const InstallPlan plan = PlanInstall(s, Strategy::Auto);
    ASSERT_EQ(plan.status, PlanStatus::Upgrade);
    FxSnapshot after = s;
    ApplyToSnapshot(after, plan.fx_edits);
    EXPECT_EQ(after.String(fx::kStreamEffect), kOem);
    EXPECT_EQ(after.MultiString(fx::kCompositeStreamEffect), (std::vector<std::wstring>{kOem}));
    EXPECT_EQ(QueryInstallState(after), InstallState::InstalledEndpointEffect);
}

TEST(InstallPlan, UpgradeIsRefusedWhenTheEndpointSlotIsTaken) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kSomike);
    SetString(s, fx::kEndpointEffect, kOem);

    const InstallPlan plan = PlanInstall(s, Strategy::Auto);
    EXPECT_EQ(plan.status, PlanStatus::Refused);
    EXPECT_TRUE(plan.fx_edits.empty());  // the working 0.1.0 installation stays
}

TEST(InstallPlan, UpgradeOnlyRemovesTheStreamEffectWhenBothSlotsHaveSomike) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kSomike);
    SetString(s, fx::kEndpointEffect, kSomike);

    const InstallPlan plan = PlanInstall(s, Strategy::Auto);
    ASSERT_EQ(plan.status, PlanStatus::Upgrade);
    ASSERT_EQ(plan.fx_edits.size(), 1u);
    EXPECT_EQ(plan.fx_edits[0].name, fx::kStreamEffect);
    EXPECT_EQ(plan.fx_edits[0].kind, RegistryEdit::Kind::Delete);
}

TEST(InstallPlan, ReEnablesDisabledAudioEnhancements) {
    FxSnapshot s = Endpoint();
    s.disable_sysfx = reg::MakeDword(1);
    ASSERT_TRUE(s.EffectsDisabled());

    const InstallPlan plan = PlanInstall(s, Strategy::Auto);
    EXPECT_EQ(plan.status, PlanStatus::Ready);
    EXPECT_TRUE(plan.enable_effects);
}

TEST(InstallPlan, RemovalTouchesOnlySomike) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kSomike);
    SetMulti(s, fx::kCompositeStreamEffect, {kOem, kSomike});
    SetString(s, fx::kEndpointEffect, kSomike);
    SetMulti(s, fx::kCompositeEndpointEffect, {kOther, kSomike});

    FxSnapshot after = s;
    ApplyToSnapshot(after, PlanRemoval(s));
    EXPECT_FALSE(after.Has(fx::kStreamEffect));
    EXPECT_EQ(after.MultiString(fx::kCompositeStreamEffect), (std::vector<std::wstring>{kOem}));
    EXPECT_FALSE(after.Has(fx::kEndpointEffect));
    EXPECT_EQ(after.MultiString(fx::kCompositeEndpointEffect), (std::vector<std::wstring>{kOther}));
    EXPECT_EQ(QueryInstallState(after), InstallState::NotInstalled);
}

TEST(InstallPlan, RemovalDeletesAChainThatOnlyContainedSomike) {
    FxSnapshot s = Endpoint();
    SetMulti(s, fx::kCompositeEndpointEffect, {kSomike});
    const std::vector<RegistryEdit> edits = PlanRemoval(s);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0].kind, RegistryEdit::Kind::Delete);
}

TEST(InstallPlan, RemovalOfAForeignConfigurationIsANoOp) {
    FxSnapshot s = Endpoint();
    SetString(s, fx::kStreamEffect, kOem);
    SetString(s, fx::kEndpointEffect, kOther);
    EXPECT_TRUE(PlanRemoval(s).empty());
}

TEST(InstallPlan, ExpectedValuesMatchTheRegistryWriterEncoding) {
    EXPECT_EQ(ExpectedValue(RegistryEdit::SetString(L"n", kSomike)), reg::MakeString(kSomike));
    EXPECT_EQ(ExpectedValue(RegistryEdit::SetMultiString(L"n", {kOem})), reg::MakeMultiString({kOem}));
    EXPECT_FALSE(ExpectedValue(RegistryEdit::Delete(L"n")).has_value());
}

TEST(InstallPlan, ParsesStrategies) {
    EXPECT_EQ(ParseStrategy(L"auto"), Strategy::Auto);
    EXPECT_EQ(ParseStrategy(L"Composite"), Strategy::Composite);
    EXPECT_EQ(ParseStrategy(L"REPLACE"), Strategy::Replace);
    EXPECT_FALSE(ParseStrategy(L"force").has_value());
}

}  // namespace
}  // namespace somike::installer
