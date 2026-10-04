#pragma once

#include <cstddef>
#include <memory>
#include <vector>

// Grabs raw interleaved float PCM from the system's current audio output
// (loopback capture) and reports basic device status. Implemented once per
// platform (audio_capture_win.cpp, audio_capture_linux.cpp; audio_capture_mac.mm later, for
// whichever of BlackHole+CoreAudio or ScreenCaptureKit gets chosen) --
// the shared analysis pipeline in audio.cpp only ever talks to this
// interface, never to a platform audio API directly. Keeping this surface
// small is what keeps the two platforms' capture files independent of one
// another: every future tuning change (band splitting, smoothing, onset
// detection) lives once in audio.cpp and never needs touching per platform.
class AudioCapture {
public:
    AudioCapture() = default;
    virtual ~AudioCapture() = default;

    // Every backend owns raw OS handles (COM interfaces, a PulseAudio
    // mainloop thread) released in its destructor, so a copy would
    // double-free them. Deleted here once rather than in each backend.
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // (Re)opens capture on the current default output device. Safe to call
    // repeatedly -- always tears down any prior capture first. Returns
    // false on failure; isReady() stays false and the caller should retry
    // later (e.g. on a timer) rather than treat this as permanent, since
    // the failure may be transient (no output device yet, etc.).
    virtual bool open() = 0;

    // True once open() has succeeded and capture is actively running.
    virtual bool isReady() const = 0;

    // True if the system's default output device is no longer the one
    // open() connected to -- signals the caller to reopen. Only meaningful
    // once isReady() is true.
    virtual bool defaultDeviceChanged() = 0;

    // Appends every currently-available frame as interleaved float samples
    // (numChannels() per frame) to the end of `out`. Silence/gap packets
    // are appended as real zeros rather than skipped, so the sample count
    // (and the FFT window built from it) stays continuous in time. Returns
    // false on an unrecoverable capture error (caller should call open()
    // again); returning true having appended nothing is normal.
    virtual bool pullSamples(std::vector<float>& out) = 0;

    virtual unsigned sampleRate() const = 0;
    virtual unsigned numChannels() const = 0;
};

// Constructs the platform's capture backend. Implemented once per platform
// (audio_capture_win.cpp / audio_capture_linux.cpp / audio_capture_mac.mm),
// selected by CMakeLists.txt.
std::unique_ptr<AudioCapture> createAudioCapture();
