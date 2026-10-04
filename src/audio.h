#pragma once

class AudioAnalyzer {
public:
    AudioAnalyzer();
    ~AudioAnalyzer();

    AudioAnalyzer(const AudioAnalyzer&) = delete;
    AudioAnalyzer& operator=(const AudioAnalyzer&) = delete;

    // Starts WASAPI loopback capture on the default render device.
    // Returns false on failure; the analyzer stays usable (values read 0).
    bool init();

    // Drains any pending audio, refreshes the spectrum and derived bands.
    // Call once per render frame.
    void update();

    // Fast-reacting bands: good for debug bars / sparkle, too jittery for
    // driving continuous motion directly.
    float bass() const { return bass_; }
    float mid() const { return mid_; }
    float treble() const { return treble_; }
    float volume() const { return volume_; }
    float onsetPulse() const { return onsetPulse_; }

    // Heavily-smoothed bands: safe to drive continuous motion/color with,
    // since they change gradually rather than frame-to-frame.
    float bassSlow() const { return bassSlow_; }
    float midSlow() const { return midSlow_; }
    float trebleSlow() const { return trebleSlow_; }

    // Fires only on unusually large transients (well above the routine-beat
    // onset threshold), decaying smoothly. Meant to drive occasional jolts.
    float bigAccent() const { return bigAccent_; }

    // Log-spaced multi-band spectrum (40 Hz - 12 kHz), each band individually
    // auto-gained and smoothed. For visual spectrum displays.
    static constexpr int kSpectrumBands = 16;
    const float* spectrum() const { return spectrum_; }

    // Smoothed stereo balance: -1 = fully left, 0 = centered, +1 = fully
    // right. Decays toward 0 during near-silence so it doesn't jitter from
    // noise when nothing meaningful is playing.
    float stereoBalance() const { return stereoBalance_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;

    float bass_ = 0.0f;
    float mid_ = 0.0f;
    float treble_ = 0.0f;
    float volume_ = 0.0f;
    float onsetPulse_ = 0.0f;

    float bassSlow_ = 0.0f;
    float midSlow_ = 0.0f;
    float trebleSlow_ = 0.0f;
    float bigAccent_ = 0.0f;

    float spectrum_[kSpectrumBands] = {};
    float stereoBalance_ = 0.0f;
};
