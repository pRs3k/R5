// WASAPI loopback capture -- the Windows implementation of AudioCapture
// (audio_capture.h). This file should never need to know anything about
// FFTs, bands, or smoothing; that all lives once, shared, in audio.cpp.

#include "audio_capture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

void logHr(const char* what, HRESULT hr) {
    std::fprintf(stderr, "[audio] %s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
}

class WasapiCapture final : public AudioCapture {
public:
    WasapiCapture() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (hr == RPC_E_CHANGED_MODE) {
            // Another part of the process already set apartment threading; fine either way.
        } else if (FAILED(hr)) {
            logHr("CoInitializeEx", hr);
        } else {
            comInitialized_ = true;
        }

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_));
        if (FAILED(hr)) logHr("CoCreateInstance(MMDeviceEnumerator)", hr);
    }

    ~WasapiCapture() override {
        releaseCaptureObjects();
        if (enumerator_) enumerator_->Release();
        if (comInitialized_) CoUninitialize();
    }

    bool open() override {
        releaseCaptureObjects();
        ready_ = false;

        if (!enumerator_) return false; // never constructed (CoCreateInstance failed above)

        HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr)) { logHr("GetDefaultAudioEndpoint", hr); return false; }

        LPWSTR idStr = nullptr;
        if (SUCCEEDED(device_->GetId(&idStr)) && idStr) {
            currentDeviceId_ = idStr;
            CoTaskMemFree(idStr);
        }

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&audioClient_));
        if (FAILED(hr)) { logHr("IMMDevice::Activate", hr); return false; }

        hr = audioClient_->GetMixFormat(&mixFormat_);
        if (FAILED(hr)) { logHr("GetMixFormat", hr); return false; }

        sampleRate_ = mixFormat_->nSamplesPerSec;
        numChannels_ = mixFormat_->nChannels;

        const REFERENCE_TIME bufferDuration = 10'000'000; // 1 second, in 100ns units
        hr = audioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                       bufferDuration, 0, mixFormat_, nullptr);
        if (FAILED(hr)) { logHr("IAudioClient::Initialize", hr); return false; }

        hr = audioClient_->GetService(__uuidof(IAudioCaptureClient),
                                       reinterpret_cast<void**>(&captureClient_));
        if (FAILED(hr)) { logHr("GetService(IAudioCaptureClient)", hr); return false; }

        hr = audioClient_->Start();
        if (FAILED(hr)) { logHr("IAudioClient::Start", hr); return false; }

        std::printf("[audio] loopback capture (re)started: %u Hz, %u ch\n", sampleRate_, numChannels_);
        std::fflush(stdout);

        ready_ = true;
        return true;
    }

    bool isReady() const override { return ready_; }

    // Windows doesn't tear down a loopback stream just because the default
    // output changed (e.g. switching a TV's HDMI input re-negotiates the
    // audio endpoint) -- it silently keeps capturing from a device nothing
    // is being routed to anymore, so this has to be polled and recovered
    // from explicitly by the caller.
    bool defaultDeviceChanged() override {
        IMMDevice* current = nullptr;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &current)) || !current) {
            return false;
        }
        LPWSTR idStr = nullptr;
        bool changed = false;
        if (SUCCEEDED(current->GetId(&idStr)) && idStr) {
            changed = currentDeviceId_ != idStr;
            CoTaskMemFree(idStr);
        }
        current->Release();
        return changed;
    }

    bool pullSamples(std::vector<float>& out) override {
        UINT32 packetLength = 0;
        HRESULT hr = captureClient_->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) { logHr("GetNextPacketSize", hr); return false; }

        while (packetLength != 0) {
            BYTE* data = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = captureClient_->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) { logHr("GetBuffer", hr); return false; }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const size_t base = out.size();
            out.resize(base + static_cast<size_t>(numFrames) * numChannels_, 0.0f);
            if (!silent && data != nullptr) {
                const float* samples = reinterpret_cast<const float*>(data);
                std::copy(samples, samples + static_cast<size_t>(numFrames) * numChannels_,
                          out.begin() + static_cast<std::vector<float>::difference_type>(base));
            }
            // else: already zero-filled by the resize() above.

            hr = captureClient_->ReleaseBuffer(numFrames);
            if (FAILED(hr)) { logHr("ReleaseBuffer", hr); return false; }

            hr = captureClient_->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) { logHr("GetNextPacketSize", hr); return false; }
        }
        return true;
    }

    unsigned sampleRate() const override { return sampleRate_; }
    unsigned numChannels() const override { return numChannels_; }

private:
    // Tears down the WASAPI objects only -- `enumerator_` is reusable for
    // the lifetime of the process and is released only in the destructor.
    void releaseCaptureObjects() {
        if (captureClient_) { captureClient_->Release(); captureClient_ = nullptr; }
        if (audioClient_) {
            audioClient_->Stop();
            audioClient_->Release();
            audioClient_ = nullptr;
        }
        if (mixFormat_) { CoTaskMemFree(mixFormat_); mixFormat_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
    }

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* audioClient_ = nullptr;
    IAudioCaptureClient* captureClient_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;
    bool comInitialized_ = false;
    bool ready_ = false;

    UINT32 sampleRate_ = 48000;
    UINT32 numChannels_ = 2;
    std::wstring currentDeviceId_;
};

} // namespace

std::unique_ptr<AudioCapture> createAudioCapture() {
    return std::make_unique<WasapiCapture>();
}
