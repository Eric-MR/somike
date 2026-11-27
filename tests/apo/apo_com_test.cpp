// COM contract of somike_apo.dll, exercised the way audiodg.exe uses it.
//
// Regression guard: the audio engine creates every APO *aggregated*
// (CreateInstance(outer, IID_IUnknown)) and probes IAudioSystemEffects3.
// Refusing either makes Windows silently disable all effects on the endpoint.
#include <windows.h>
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>

#include <gtest/gtest.h>

#include <atomic>
#include <string>

#include "somike/common/guid.h"
#include "somike/protocol/apo_identity.h"

namespace {

using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);

// somike_apo.dll is built next to the test executable.
std::wstring ApoDllPath() {
    wchar_t path[MAX_PATH];
    const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring result(path, length);
    result.resize(result.find_last_of(L'\\') + 1);
    return result + L"somike_apo.dll";
}

// Minimal aggregator: owns the inner unknown and forwards everything else.
class FakeOuter final : public IUnknown {
public:
    IUnknown* inner = nullptr;
    std::atomic<ULONG> refs{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return inner != nullptr ? inner->QueryInterface(riid, object) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }  // stack object
};

class ApoComTest : public ::testing::Test {
protected:
    void SetUp() override {
        module_ = ::LoadLibraryW(ApoDllPath().c_str());
        ASSERT_NE(module_, nullptr) << "somike_apo.dll not found next to the tests";
        auto get_class_object =
            reinterpret_cast<GetClassObjectFn>(::GetProcAddress(module_, "DllGetClassObject"));
        ASSERT_NE(get_class_object, nullptr);
        const GUID clsid = somike::guid::FromParts(somike::protocol::kApoClsid);
        ASSERT_EQ(get_class_object(clsid, __uuidof(IClassFactory),
                                   reinterpret_cast<void**>(&factory_)),
                  S_OK);
    }

    void TearDown() override {
        if (factory_ != nullptr) factory_->Release();
        // Not unloaded: the module stays mapped for the process lifetime.
    }

    HMODULE module_ = nullptr;
    IClassFactory* factory_ = nullptr;
};

TEST_F(ApoComTest, SupportsAggregationLikeTheAudioEngine) {
    FakeOuter outer;
    IUnknown* inner = nullptr;
    ASSERT_EQ(factory_->CreateInstance(&outer, __uuidof(IUnknown), reinterpret_cast<void**>(&inner)),
              S_OK);
    ASSERT_NE(inner, nullptr);
    outer.inner = inner;

    // Interfaces come from the inner unknown and AddRef the controlling (outer) one.
    const ULONG outer_refs = outer.refs;
    IAudioProcessingObject* apo = nullptr;
    ASSERT_EQ(inner->QueryInterface(__uuidof(IAudioProcessingObject), reinterpret_cast<void**>(&apo)),
              S_OK);
    EXPECT_EQ(outer.refs, outer_refs + 1);

    // Their IUnknown delegates to the aggregator: same COM identity.
    IUnknown* identity = nullptr;
    ASSERT_EQ(apo->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&identity)), S_OK);
    EXPECT_EQ(identity, static_cast<IUnknown*>(&outer));
    identity->Release();

    apo->Release();
    EXPECT_EQ(outer.refs, outer_refs);
    EXPECT_EQ(inner->Release(), 0u);  // destroys the APO
}

TEST_F(ApoComTest, AggregationRequiresIUnknown) {
    FakeOuter outer;
    void* object = nullptr;
    EXPECT_EQ(factory_->CreateInstance(&outer, __uuidof(IAudioProcessingObject), &object),
              CLASS_E_NOAGGREGATION);
    EXPECT_EQ(object, nullptr);
}

TEST_F(ApoComTest, ExposesSystemEffects3WithNoControllableEffects) {
    IAudioSystemEffects3* effects = nullptr;
    ASSERT_EQ(factory_->CreateInstance(nullptr, __uuidof(IAudioSystemEffects3),
                                       reinterpret_cast<void**>(&effects)),
              S_OK);
    AUDIO_SYSTEMEFFECT* list = reinterpret_cast<AUDIO_SYSTEMEFFECT*>(1);
    UINT count = 99;
    EXPECT_EQ(effects->GetControllableSystemEffectsList(&list, &count, nullptr), S_OK);
    EXPECT_EQ(list, nullptr);
    EXPECT_EQ(count, 0u);

    IAudioProcessingObjectRT* rt = nullptr;
    EXPECT_EQ(effects->QueryInterface(__uuidof(IAudioProcessingObjectRT), reinterpret_cast<void**>(&rt)),
              S_OK);
    if (rt != nullptr) rt->Release();
    EXPECT_EQ(effects->Release(), 0u);
}

TEST_F(ApoComTest, InitializeAcceptsSystemEffects3Structure) {
    IAudioProcessingObject* apo = nullptr;
    ASSERT_EQ(factory_->CreateInstance(nullptr, __uuidof(IAudioProcessingObject),
                                       reinterpret_cast<void**>(&apo)),
              S_OK);
    APOInitSystemEffects3 init{};
    init.APOInit.cbSize = sizeof(init);
    init.APOInit.clsid = somike::guid::FromParts(somike::protocol::kApoClsid);
    init.InitializeForDiscoveryOnly = TRUE;
    EXPECT_EQ(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)), S_OK);
    EXPECT_EQ(apo->Initialize(sizeof(init), reinterpret_cast<BYTE*>(&init)),
              APOERR_ALREADY_INITIALIZED);
    apo->Release();
}

}  // namespace
