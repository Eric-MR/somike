#include "injector_apo.h"

#include <cmath>
#include <cstring>
#include <cwchar>
#include <new>

#include "module.h"
#include "somike/common/guid.h"
#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/common/ring_path.h"
#include "somike/protocol/apo_identity.h"
#include "somike/protocol/shared_ring.h"

namespace somike::apo {

namespace proto = somike::protocol;

namespace {

static_assert(proto::kApoFlags == static_cast<std::uint32_t>(APO_FLAG_INPLACE | APO_FLAG_DEFAULT),
              "apo_identity.h flags drifted from the SDK definition");

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally to avoid ksmedia.h/mmreg.h
// include-order pitfalls.
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct FloatFormat {
    UINT32 channels = 0;
    UINT32 sample_rate = 0;
};

// True if `type` is uncompressed 32-bit float, the only format we process.
bool DescribeFloatFormat(IAudioMediaType* type, FloatFormat& out) noexcept {
    if (type == nullptr) {
        return false;
    }
    BOOL compressed = TRUE;
    if (FAILED(type->IsCompressedFormat(&compressed)) || compressed) {
        return false;
    }
    UNCOMPRESSEDAUDIOFORMAT format{};
    if (FAILED(type->GetUncompressedAudioFormat(&format))) {
        return false;
    }
    if (!IsEqualGUID(format.guidFormatType, kSubtypeIeeeFloat) ||
        format.dwBytesPerSampleContainer != 4 || format.dwValidBitsPerSample != 32 ||
        format.dwSamplesPerFrame == 0 || !(format.fFramesPerSecond >= 1.0f)) {
        return false;
    }
    out.channels = format.dwSamplesPerFrame;
    out.sample_rate = static_cast<UINT32>(std::lround(format.fFramesPerSecond));
    return true;
}

bool IsProcessable(const FloatFormat& format) noexcept {
    return dsp::IsSupportedFormat({format.sample_rate, format.channels});
}

// Shared by input and output negotiation: we are in place, so both sides
// must be the same float format.
HRESULT NegotiateFormat(IAudioMediaType* opposite, IAudioMediaType* requested,
                        IAudioMediaType** supported) noexcept {
    if (requested == nullptr || supported == nullptr) {
        return E_POINTER;
    }
    *supported = nullptr;

    FloatFormat wanted;
    const bool wanted_ok = DescribeFloatFormat(requested, wanted) && IsProcessable(wanted);

    FloatFormat other;
    const bool other_ok = opposite != nullptr && DescribeFloatFormat(opposite, other) &&
                          IsProcessable(other);

    if (wanted_ok && (opposite == nullptr || (other_ok && other.channels == wanted.channels &&
                                              other.sample_rate == wanted.sample_rate))) {
        requested->AddRef();
        *supported = requested;
        return S_OK;
    }
    if (other_ok) {
        // Not what was asked for, but this one we can do.
        opposite->AddRef();
        *supported = opposite;
        return S_FALSE;
    }
    return APOERR_FORMAT_NOT_SUPPORTED;
}

}  // namespace

InjectorApo::InjectorApo(IUnknown* outer) noexcept : outer_(outer != nullptr ? outer : &inner_) {
    ModuleAddRef();
    log::Info(L"APO instance {} created ({})", static_cast<const void*>(this),
              outer != nullptr ? L"aggregated" : L"standalone");
}

InjectorApo::~InjectorApo() {
    log::Info(L"APO instance {} destroyed (initialized {}, locked {})", static_cast<const void*>(this),
              initialized_, locked_);
    ModuleRelease();
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
// Delegating IUnknown: every interface we hand out forwards to the controlling
// unknown, so the aggregator sees one object identity and one ref count.
HRESULT InjectorApo::QueryInterface(REFIID riid, void** object) noexcept {
    return outer_->QueryInterface(riid, object);
}

ULONG InjectorApo::AddRef() noexcept { return outer_->AddRef(); }

ULONG InjectorApo::Release() noexcept { return outer_->Release(); }

// Non-delegating IUnknown: identity and lifetime of the object itself.
HRESULT InjectorApo::NonDelegatingUnknown::QueryInterface(REFIID riid, void** object) noexcept {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    InjectorApo& self = owner_;
    if (riid == __uuidof(IUnknown)) {
        *object = static_cast<IUnknown*>(this);  // must be the inner unknown when aggregated
        AddRef();
        return S_OK;
    }
    if (riid == __uuidof(IAudioProcessingObject)) {
        *object = static_cast<IAudioProcessingObject*>(&self);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *object = static_cast<IAudioProcessingObjectRT*>(&self);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *object = static_cast<IAudioProcessingObjectConfiguration*>(&self);
    } else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2) ||
               riid == __uuidof(IAudioSystemEffects3)) {
        *object = static_cast<IAudioSystemEffects3*>(&self);
    } else {
        return E_NOINTERFACE;  // the engine probes many optional interfaces
    }
    // Interface pointers AddRef through the controlling unknown.
    static_cast<IUnknown*>(*object)->AddRef();
    return S_OK;
}

ULONG InjectorApo::NonDelegatingUnknown::AddRef() noexcept {
    return owner_.refs_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG InjectorApo::NonDelegatingUnknown::Release() noexcept {
    const ULONG remaining = owner_.refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete &owner_;
    }
    return remaining;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObject
// ---------------------------------------------------------------------------
HRESULT InjectorApo::Reset() noexcept {
    injector_.Reset();
    return S_OK;
}

HRESULT InjectorApo::GetLatency(HNSTIME* time) noexcept {
    if (time == nullptr) {
        return E_POINTER;
    }
    *time = 0;  // the microphone signal itself is not delayed
    return S_OK;
}

HRESULT InjectorApo::GetRegistrationProperties(APO_REG_PROPERTIES** props) noexcept {
    if (props == nullptr) {
        return E_POINTER;
    }
    *props = nullptr;
    auto* result = static_cast<APO_REG_PROPERTIES*>(::CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (result == nullptr) {
        return E_OUTOFMEMORY;
    }
    std::memset(result, 0, sizeof(*result));
    result->clsid = guid::FromParts(proto::kApoClsid);
    result->Flags = static_cast<APO_FLAG>(proto::kApoFlags);
    wcscpy_s(result->szFriendlyName, proto::kApoFriendlyName);
    wcscpy_s(result->szCopyrightInfo, proto::kApoCopyright);
    result->u32MajorVersion = proto::kApoMajorVersion;
    result->u32MinorVersion = proto::kApoMinorVersion;
    result->u32MinInputConnections = 1;
    result->u32MaxInputConnections = 1;
    result->u32MinOutputConnections = 1;
    result->u32MaxOutputConnections = 1;
    result->u32MaxInstances = 0xFFFFFFFF;
    result->u32NumAPOInterfaces = 1;
    result->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
    *props = result;
    return S_OK;
}

HRESULT InjectorApo::Initialize(UINT32 data_size, BYTE* data) noexcept {
    if (initialized_) {
        return APOERR_ALREADY_INITIALIZED;
    }
    if (data_size != 0 && data == nullptr) {
        return E_POINTER;
    }
    if (data_size != 0 && data_size < sizeof(APOInitBaseStruct)) {
        return E_INVALIDARG;
    }
    // We need nothing from the init structure except the discovery flag; any
    // other (future) structure size is accepted rather than failing.
    if (data_size == sizeof(APOInitSystemEffects3)) {
        const auto* init = reinterpret_cast<const APOInitSystemEffects3*>(data);
        discovery_only_ = init->InitializeForDiscoveryOnly != FALSE;
    } else if (data_size == sizeof(APOInitSystemEffects2)) {
        const auto* init = reinterpret_cast<const APOInitSystemEffects2*>(data);
        discovery_only_ = init->InitializeForDiscoveryOnly != FALSE;
    }
    initialized_ = true;
    log::Info(L"APO initialized (init size {}, discovery only: {})", data_size, discovery_only_);
    return S_OK;
}

HRESULT InjectorApo::IsInputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                            IAudioMediaType** supported) noexcept {
    const HRESULT hr = NegotiateFormat(opposite, requested, supported);
    LogNegotiation(L"input", requested, hr);
    return hr;
}

HRESULT InjectorApo::IsOutputFormatSupported(IAudioMediaType* opposite, IAudioMediaType* requested,
                                             IAudioMediaType** supported) noexcept {
    const HRESULT hr = NegotiateFormat(opposite, requested, supported);
    LogNegotiation(L"output", requested, hr);
    return hr;
}

void InjectorApo::LogNegotiation(const wchar_t* side, IAudioMediaType* requested,
                                 HRESULT hr) noexcept {
    UNCOMPRESSEDAUDIOFORMAT format{};
    if (requested == nullptr || FAILED(requested->GetUncompressedAudioFormat(&format))) {
        log::Info(L"APO: {} format query (undescribable) -> 0x{:08X}", side,
                  static_cast<unsigned>(hr));
        return;
    }
    log::Info(L"APO: {} format {} Hz / {} ch / {}-bit container, {} valid / float {} -> 0x{:08X}",
              side, format.fFramesPerSecond, format.dwSamplesPerFrame,
              format.dwBytesPerSampleContainer * 8, format.dwValidBitsPerSample,
              IsEqualGUID(format.guidFormatType, kSubtypeIeeeFloat), static_cast<unsigned>(hr));
}

HRESULT InjectorApo::GetInputChannelCount(UINT32* channels) noexcept {
    if (channels == nullptr) {
        return E_POINTER;
    }
    *channels = channels_;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectConfiguration
// ---------------------------------------------------------------------------
HRESULT InjectorApo::LockForProcess(UINT32 input_count, APO_CONNECTION_DESCRIPTOR** inputs,
                                    UINT32 output_count,
                                    APO_CONNECTION_DESCRIPTOR** outputs) noexcept {
    if (!initialized_) {
        return APOERR_NOT_INITIALIZED;
    }
    if (locked_) {
        return APOERR_APO_LOCKED;
    }
    if (input_count != 1 || output_count != 1) {
        return APOERR_NUM_CONNECTIONS_INVALID;
    }
    if (inputs == nullptr || outputs == nullptr || inputs[0] == nullptr || outputs[0] == nullptr) {
        return E_POINTER;
    }

    FloatFormat in;
    FloatFormat out;
    if (!DescribeFloatFormat(inputs[0]->pFormat, in) ||
        !DescribeFloatFormat(outputs[0]->pFormat, out) || in.channels != out.channels ||
        in.sample_rate != out.sample_rate) {
        log::Warn(L"APO: LockForProcess rejected the connection format");
        return APOERR_INVALID_CONNECTION_FORMAT;
    }
    if (outputs[0]->u32MaxFrameCount < inputs[0]->u32MaxFrameCount) {
        log::Warn(L"APO: LockForProcess rejected max frame counts {} -> {}",
                  inputs[0]->u32MaxFrameCount, outputs[0]->u32MaxFrameCount);
        return APOERR_INVALID_OUTPUT_MAXFRAMECOUNT;
    }

    channels_ = in.channels;
    sample_rate_ = in.sample_rate;
    max_frames_ = inputs[0]->u32MaxFrameCount;

    try {
        if (!injector_.Configure({sample_rate_, channels_}, max_frames_)) {
            log::Warn(L"APO: format {} Hz / {} ch / {} frames not supported; passing through",
                      sample_rate_, channels_, max_frames_);
        }
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }

    AttachRing();
    locked_ = true;
    log::Info(L"APO locked: {} Hz, {} ch, max {} frames, ring {}", sample_rate_, channels_,
              max_frames_, injector_.attached() ? L"attached" : L"unavailable");
    return S_OK;
}

HRESULT InjectorApo::UnlockForProcess() noexcept {
    if (!locked_) {
        return APOERR_ALREADY_UNLOCKED;
    }
    injector_.Detach();
    ring_.Close();
    locked_ = false;
    return S_OK;
}

void InjectorApo::AttachRing() noexcept {
    if (!injector_.configured()) {
        return;
    }
    try {
        if (!ring_.is_open()) {
            const std::wstring path = paths::RingFile();
            HRESULT hr = ring_.Open(path, proto::kRegionBytes, win32::MappedFile::Access::ReadWrite);
            if (FAILED(hr)) {
                // Read-only still allows injection; only diagnostics are lost.
                hr = ring_.Open(path, proto::kRegionBytes, win32::MappedFile::Access::ReadOnly);
            }
            if (FAILED(hr)) {
                log::Warn(L"APO: cannot map {}: {}", path, win32::FormatHResult(hr));
                return;
            }
        }
        injector_.Attach(ring_.bytes(), ring_.writable());
    } catch (...) {
        ring_.Close();
    }
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectRT - real-time: no allocation, locks, logging or I/O.
// ---------------------------------------------------------------------------
void InjectorApo::APOProcess(UINT32 input_count, APO_CONNECTION_PROPERTY** inputs,
                             UINT32 output_count, APO_CONNECTION_PROPERTY** outputs) noexcept {
    if (input_count != 1 || output_count != 1 || inputs == nullptr || outputs == nullptr ||
        inputs[0] == nullptr || outputs[0] == nullptr) {
        return;
    }
    APO_CONNECTION_PROPERTY* input = inputs[0];
    APO_CONNECTION_PROPERTY* output = outputs[0];
    const UINT32 frames = input->u32ValidFrameCount;

    output->u32ValidFrameCount = frames;
    output->u32BufferFlags = input->u32BufferFlags;
    if (input->u32BufferFlags == BUFFER_INVALID || frames == 0) {
        return;
    }

    auto* source = reinterpret_cast<float*>(input->pBuffer);
    auto* target = reinterpret_cast<float*>(output->pBuffer);
    const bool in_place = source == target;
    if (!locked_ || frames > max_frames_ || source == nullptr || target == nullptr) {
        // Contract violation by the engine: never claim valid data we did
        // not put into a separate output buffer.
        if (!in_place) {
            output->u32BufferFlags = BUFFER_SILENT;
        }
        return;
    }

    const bool silent = input->u32BufferFlags == BUFFER_SILENT;
    if (!silent && !in_place) {
        std::memcpy(target, source, static_cast<std::size_t>(frames) * channels_ * sizeof(float));
    }

    if (injector_.Process(target, frames, silent, ::GetTickCount64())) {
        output->u32BufferFlags = BUFFER_VALID;
    }
}

UINT32 InjectorApo::CalcInputFrames(UINT32 output_frames) noexcept { return output_frames; }

UINT32 InjectorApo::CalcOutputFrames(UINT32 input_frames) noexcept { return input_frames; }

// ---------------------------------------------------------------------------
// IAudioSystemEffects2
// ---------------------------------------------------------------------------
HRESULT InjectorApo::GetEffectsList(LPGUID* effect_ids, UINT* effect_count,
                                    HANDLE /*event*/) noexcept {
    if (effect_ids == nullptr || effect_count == nullptr) {
        return E_POINTER;
    }
    // We expose no user-toggleable effects; routing is controlled by the app.
    *effect_ids = nullptr;
    *effect_count = 0;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioSystemEffects3
// ---------------------------------------------------------------------------
HRESULT InjectorApo::GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects,
                                                      UINT* effect_count, HANDLE /*event*/) noexcept {
    if (effects == nullptr || effect_count == nullptr) {
        return E_POINTER;
    }
    // Nothing for the Settings app to toggle (the app controls routing).
    *effects = nullptr;
    *effect_count = 0;
    return S_OK;
}

HRESULT InjectorApo::SetAudioSystemEffectState(GUID /*effect_id*/,
                                               AUDIO_SYSTEMEFFECT_STATE /*state*/) noexcept {
    return E_NOTFOUND;  // we published no controllable effects
}

}  // namespace somike::apo
