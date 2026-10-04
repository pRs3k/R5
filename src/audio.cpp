// Shared audio analysis pipeline: FFT, band splitting, gain-normalized
// smoothing, onset/accent detection, spectrum bands, stereo balance. Talks
// only to the AudioCapture interface (audio_capture.h) for actual samples,
// never to a platform audio API directly -- this file is identical on
// every platform; only the capture backend it's built against differs
// (see audio_capture_win.cpp / audio_capture_linux.cpp, selected by
// CMakeLists.txt).

#include "audio.h"
#include "audio_capture.h"

#include <kiss_fftr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kFftSize = 2048;
constexpr int kNumBins = kFftSize / 2 + 1;

constexpr float kBassLoHz = 20.0f;
constexpr float kBassHiHz = 250.0f;
constexpr float kMidHiHz = 2000.0f;
constexpr float kTrebleHiHz = 8000.0f;

constexpr float kGainDecay = 0.999f;
constexpr float kBandSmoothing = 0.6f;
constexpr float kSlowSmoothing = 0.04f; // ~1/15 as reactive as the fast bands
constexpr float kFluxDecay = 0.98f;
constexpr float kOnsetDecay = 0.90f;
constexpr float kOnsetThresholdMul = 1.6f;
constexpr float kMinOnsetGap = 0.10f; // seconds

constexpr float kBigAccentThresholdMul = 3.2f; // well above routine-beat onsets
constexpr float kBigAccentDecay = 0.90f;
constexpr float kMinBigAccentGap = 0.45f; // seconds

constexpr float kSpectrumLoHz = 40.0f;
constexpr float kSpectrumHiHz = 12000.0f;
constexpr float kSpectrumGainDecay = 0.998f;
constexpr float kSpectrumSmoothing = 0.45f;

constexpr float kStereoSmoothing = 0.12f;
constexpr float kStereoSilenceGate = 0.0005f; // combined RMS below this decays balance to 0

int spectrumBandForFreq(float freq) {
    if (freq < kSpectrumLoHz) return 0;
    const float t = std::log(freq / kSpectrumLoHz) / std::log(kSpectrumHiHz / kSpectrumLoHz);
    const int band = static_cast<int>(t * AudioAnalyzer::kSpectrumBands);
    return std::clamp(band, 0, AudioAnalyzer::kSpectrumBands - 1);
}

} // namespace

struct AudioAnalyzer::Impl {
    std::unique_ptr<AudioCapture> capture;

    std::vector<float> interleavedBuf; // scratch, refilled by pullSamples() each update()
    std::vector<float> ring;
    size_t windowSamples = kFftSize;

    kiss_fftr_cfg fftCfg = nullptr;
    std::vector<float> fftIn;
    std::vector<kiss_fft_cpx> fftOut;
    std::vector<float> window;
    std::vector<float> prevMag;

    float bassGain = 1e-4f;
    float midGain = 1e-4f;
    float trebleGain = 1e-4f;

    float spectrumGain[AudioAnalyzer::kSpectrumBands];

    float fluxAvg = 0.0f;
    float lastOnsetTime = -1.0f;
    float lastBigAccentTime = -1.0f;
    float clock = 0.0f;

    // How often to poll whether the system default output device changed
    // out from under us (see AudioCapture::defaultDeviceChanged) -- cheap
    // but not free, so this is checked a few times a second rather than
    // every frame.
    float lastDeviceCheckTime = -1000.0f;
    static constexpr float kDeviceCheckInterval = 1.0f;

    Impl() : capture(createAudioCapture()) {
        std::fill(std::begin(spectrumGain), std::end(spectrumGain), 1e-4f);
    }

    ~Impl() {
        if (fftCfg) kiss_fftr_free(fftCfg);
    }
};

AudioAnalyzer::AudioAnalyzer() : impl_(new Impl()) {}

AudioAnalyzer::~AudioAnalyzer() { delete impl_; }

bool AudioAnalyzer::init() {
    Impl& s = *impl_;

    s.fftCfg = kiss_fftr_alloc(kFftSize, 0, nullptr, nullptr);
    s.fftIn.assign(kFftSize, 0.0f);
    s.fftOut.assign(kNumBins, kiss_fft_cpx{0.0f, 0.0f});
    s.prevMag.assign(kNumBins, 0.0f);

    s.window.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i) {
        s.window[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * i / (kFftSize - 1));
    }

    s.ring.reserve(kFftSize * 4);

    return s.capture->open();
}

void AudioAnalyzer::update() {
    Impl& s = *impl_;

    const float frameDt = 1.0f / 60.0f;
    s.clock += frameDt;

    // If we're not ready at all (initial device-open failed, or a previous
    // reopen attempt failed), keep retrying here rather than giving up
    // permanently -- the capture backend owns whatever internal state
    // never gets torn down (e.g. WASAPI's device enumerator, PulseAudio's
    // mainloop thread), so there's
    // always something to retry against.
    if (s.clock - s.lastDeviceCheckTime > Impl::kDeviceCheckInterval) {
        s.lastDeviceCheckTime = s.clock;
        if (!s.capture->isReady()) {
            s.capture->open();
        } else if (s.capture->defaultDeviceChanged()) {
            std::printf("[audio] default output device changed, reopening capture\n");
            std::fflush(stdout);
            s.capture->open();
        }
    }

    if (!s.capture->isReady()) return;

    s.interleavedBuf.clear();
    if (!s.capture->pullSamples(s.interleavedBuf)) {
        s.capture->open(); // device likely invalidated outright; try to recover
        return;
    }

    const unsigned numChannels = s.capture->numChannels();
    const unsigned sampleRate = s.capture->sampleRate();
    if (numChannels == 0) return;

    float leftAccum = 0.0f, rightAccum = 0.0f;
    int stereoSampleCount = 0;

    // Silence/gap frames come back zero-filled (see AudioCapture's
    // contract) rather than omitted, so they do end up diluting this
    // frame's stereo RMS average slightly during a capture batch that's
    // partly silent -- a small, deliberate simplification over the old
    // WASAPI-specific code, which skipped silence at the packet level.
    // stereoBalance_ is already gated on combined RMS below, so this
    // never shows up as anything perceptible.
    const size_t numFrames = s.interleavedBuf.size() / numChannels;
    for (size_t f = 0; f < numFrames; ++f) {
        float mono = 0.0f;
        for (unsigned c = 0; c < numChannels; ++c) {
            mono += s.interleavedBuf[f * numChannels + c];
        }
        mono /= static_cast<float>(numChannels);

        if (numChannels >= 2) {
            const float l = s.interleavedBuf[f * numChannels + 0];
            const float r = s.interleavedBuf[f * numChannels + 1];
            leftAccum += l * l;
            rightAccum += r * r;
            ++stereoSampleCount;
        }
        s.ring.push_back(mono);
    }

    if (stereoSampleCount > 0) {
        const float leftRms = std::sqrt(leftAccum / stereoSampleCount);
        const float rightRms = std::sqrt(rightAccum / stereoSampleCount);
        const float combined = leftRms + rightRms;
        const float targetBalance = combined > kStereoSilenceGate
            ? std::clamp((rightRms - leftRms) / combined, -1.0f, 1.0f)
            : 0.0f;
        stereoBalance_ = stereoBalance_ * (1.0f - kStereoSmoothing) + targetBalance * kStereoSmoothing;
    }

    if (s.ring.size() < s.windowSamples) return;

    const size_t excess = s.ring.size() - s.windowSamples;
    if (excess > 0 && s.ring.size() > s.windowSamples * 4) {
        s.ring.erase(s.ring.begin(), s.ring.begin() + static_cast<long>(s.ring.size() - s.windowSamples));
    }

    const size_t start = s.ring.size() - s.windowSamples;
    for (int i = 0; i < kFftSize; ++i) {
        s.fftIn[i] = s.ring[start + i] * s.window[i];
    }

    kiss_fftr(s.fftCfg, s.fftIn.data(), s.fftOut.data());

    float rmsAccum = 0.0f;
    float bassSum = 0.0f, midSum = 0.0f, trebleSum = 0.0f;
    int bassCount = 0, midCount = 0, trebleCount = 0;
    float flux = 0.0f;

    float spectrumSum[AudioAnalyzer::kSpectrumBands] = {};
    int spectrumCount[AudioAnalyzer::kSpectrumBands] = {};

    for (int i = 0; i < kNumBins; ++i) {
        const float re = s.fftOut[i].r;
        const float im = s.fftOut[i].i;
        const float mag = std::sqrt(re * re + im * im);
        const float freq = static_cast<float>(i) * static_cast<float>(sampleRate) / kFftSize;

        flux += std::max(0.0f, mag - s.prevMag[i]);
        s.prevMag[i] = mag;

        if (freq >= kBassLoHz && freq < kBassHiHz) { bassSum += mag; ++bassCount; }
        else if (freq >= kBassHiHz && freq < kMidHiHz) { midSum += mag; ++midCount; }
        else if (freq >= kMidHiHz && freq < kTrebleHiHz) { trebleSum += mag; ++trebleCount; }

        if (freq >= kSpectrumLoHz) {
            const int band = spectrumBandForFreq(freq);
            spectrumSum[band] += mag;
            ++spectrumCount[band];
        }
    }

    for (int i = 0; i < kFftSize; ++i) rmsAccum += s.fftIn[i] * s.fftIn[i];
    const float rms = std::sqrt(rmsAccum / kFftSize);

    const float bassRaw = bassCount ? bassSum / bassCount : 0.0f;
    const float midRaw = midCount ? midSum / midCount : 0.0f;
    const float trebleRaw = trebleCount ? trebleSum / trebleCount : 0.0f;

    s.bassGain = std::max(bassRaw, s.bassGain * kGainDecay);
    s.midGain = std::max(midRaw, s.midGain * kGainDecay);
    s.trebleGain = std::max(trebleRaw, s.trebleGain * kGainDecay);

    const float bassNorm = s.bassGain > 1e-6f ? bassRaw / s.bassGain : 0.0f;
    const float midNorm = s.midGain > 1e-6f ? midRaw / s.midGain : 0.0f;
    const float trebleNorm = s.trebleGain > 1e-6f ? trebleRaw / s.trebleGain : 0.0f;

    bass_ = bass_ * (1.0f - kBandSmoothing) + bassNorm * kBandSmoothing;
    mid_ = mid_ * (1.0f - kBandSmoothing) + midNorm * kBandSmoothing;
    treble_ = treble_ * (1.0f - kBandSmoothing) + trebleNorm * kBandSmoothing;
    volume_ = rms;

    bassSlow_ = bassSlow_ * (1.0f - kSlowSmoothing) + bassNorm * kSlowSmoothing;
    midSlow_ = midSlow_ * (1.0f - kSlowSmoothing) + midNorm * kSlowSmoothing;
    trebleSlow_ = trebleSlow_ * (1.0f - kSlowSmoothing) + trebleNorm * kSlowSmoothing;

    for (int b = 0; b < AudioAnalyzer::kSpectrumBands; ++b) {
        const float raw = spectrumCount[b] ? spectrumSum[b] / spectrumCount[b] : 0.0f;
        s.spectrumGain[b] = std::max(raw, s.spectrumGain[b] * kSpectrumGainDecay);
        const float norm = s.spectrumGain[b] > 1e-6f ? raw / s.spectrumGain[b] : 0.0f;
        spectrum_[b] = spectrum_[b] * (1.0f - kSpectrumSmoothing) + norm * kSpectrumSmoothing;
    }

    s.fluxAvg = s.fluxAvg * kFluxDecay + flux * (1.0f - kFluxDecay);

    const bool cooldownOver = (s.clock - s.lastOnsetTime) > kMinOnsetGap;
    if (cooldownOver && flux > s.fluxAvg * kOnsetThresholdMul && flux > 0.001f) {
        onsetPulse_ = 1.0f;
        s.lastOnsetTime = s.clock;
    } else {
        onsetPulse_ *= kOnsetDecay;
    }

    // Big accents use a much higher bar and a longer cooldown than routine
    // beat onsets above, so they only fire on the largest transients.
    const bool bigCooldownOver = (s.clock - s.lastBigAccentTime) > kMinBigAccentGap;
    if (bigCooldownOver && flux > s.fluxAvg * kBigAccentThresholdMul && flux > 0.001f) {
        const float magnitude = std::min(2.0f, flux / (s.fluxAvg * kBigAccentThresholdMul));
        bigAccent_ = std::max(bigAccent_, magnitude);
        s.lastBigAccentTime = s.clock;
    } else {
        bigAccent_ *= kBigAccentDecay;
    }
}
