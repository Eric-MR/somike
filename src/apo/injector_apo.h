// The Somike capture stream effect (SFX) APO.
//
// Loaded by audiodg.exe into every capture stream of the microphone endpoint
// it is registered on. It is a thin COM shell around dsp::Injector:
//   * format negotiation: 32-bit float, in place, input == output format
//   * LockForProcess: configures the injector and maps the shared ring
//   * APOProcess (real-time): mixes ring audio into the microphone buffer
//
// Design rule: whatever goes wrong, fall back to passing the microphone
// through untouched. Failing calls count towards Windows' "disable system
// effects after repeated APO failures" policy, so we avoid returning errors
// for conditions we can tolerate.
#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>

#include <atomic>

#include "somike/common/mapped_file.h"
#include "somike/dsp/injector.h"

namespace somike::apo {

class InjectorApo final : public IAudioProcessingObject,
                          public IAudioProcessingObjectRT,
                          public IAudioProcessingObjectConfiguration,
                          public IAudioSystemEffects2 {
public:
    InjectorApo() noexcept;

    InjectorApo(const InjectorApo&) = delete;
    InjectorApo& operator=(const InjectorApo&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override;
    ULONG STDMETHODCALLTYPE AddRef() noexcept override;
    ULONG STDMETHODCALLTYPE Release() noexcept override;

    // IAudioProcessingObject
    HRESULT STDMETHODCALLTYPE Reset() noexcept override;
    HRESULT STDMETHODCALLTYPE GetLatency(HNSTIME* time) noexcept override;
    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(APO_REG_PROPERTIES** props) noexcept override;
    HRESULT STDMETHODCALLTYPE Initialize(UINT32 data_size, BYTE* data) noexcept override;
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(IAudioMediaType* opposite,
                                                     IAudioMediaType* requested,
                                                     IAudioMediaType** supported) noexcept override;
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(IAudioMediaType* opposite,
                                                      IAudioMediaType* requested,
                                                      IAudioMediaType** supported) noexcept override;
    HRESULT STDMETHODCALLTYPE GetInputChannelCount(UINT32* channels) noexcept override;

    // IAudioProcessingObjectRT (real-time thread)
    void STDMETHODCALLTYPE APOProcess(UINT32 input_count, APO_CONNECTION_PROPERTY** inputs,
                                      UINT32 output_count,
                                      APO_CONNECTION_PROPERTY** outputs) noexcept override;
    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 output_frames) noexcept override;
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 input_frames) noexcept override;

    // IAudioProcessingObjectConfiguration
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 input_count, APO_CONNECTION_DESCRIPTOR** inputs,
                                             UINT32 output_count,
                                             APO_CONNECTION_DESCRIPTOR** outputs) noexcept override;
    HRESULT STDMETHODCALLTYPE UnlockForProcess() noexcept override;

    // IAudioSystemEffects2
    HRESULT STDMETHODCALLTYPE GetEffectsList(LPGUID* effect_ids, UINT* effect_count,
                                             HANDLE event) noexcept override;

private:
    ~InjectorApo();

    void AttachRing() noexcept;

    std::atomic<ULONG> refs_{1};

    bool initialized_ = false;
    bool discovery_only_ = false;
    bool locked_ = false;
    UINT32 channels_ = 0;
    UINT32 sample_rate_ = 0;
    UINT32 max_frames_ = 0;

    win32::MappedFile ring_;
    dsp::Injector injector_;
};

}  // namespace somike::apo
