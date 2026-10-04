// PulseAudio monitor-source capture -- the Linux implementation of
// AudioCapture (audio_capture.h). Talks the PulseAudio client protocol,
// which also covers PipeWire desktops (Fedora, Ubuntu 22.10+, etc.) via
// pipewire-pulse, so one backend serves both. Like the WASAPI file, this
// should never need to know anything about FFTs, bands, or smoothing.
//
// Loopback here means recording from the default sink's ".monitor"
// source: every PulseAudio/PipeWire sink exposes one, carrying exactly
// what's being played to it, so no virtual device or extra setup is
// needed.

#include "audio_capture.h"

#include <pulse/pulseaudio.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

class PulseCapture final : public AudioCapture {
public:
    PulseCapture() {
        mainloop_ = pa_threaded_mainloop_new();
        if (!mainloop_ || pa_threaded_mainloop_start(mainloop_) < 0) {
            std::fprintf(stderr, "[audio] could not start PulseAudio mainloop thread\n");
            if (mainloop_) pa_threaded_mainloop_free(mainloop_);
            mainloop_ = nullptr;
        }
    }

    ~PulseCapture() override {
        if (!mainloop_) return;
        pa_threaded_mainloop_lock(mainloop_);
        releaseCaptureObjects();
        pa_threaded_mainloop_unlock(mainloop_);
        pa_threaded_mainloop_stop(mainloop_);
        pa_threaded_mainloop_free(mainloop_);
    }

    bool open() override {
        if (!mainloop_) return false; // never constructed (mainloop start failed above)

        Lock lock(mainloop_);
        releaseCaptureObjects();
        ready_ = false;

        // A fresh context every open() rather than one kept for the
        // process lifetime: the most common reason to land back here is
        // the sound server itself restarting (PipeWire does this on
        // updates/config changes), which kills the old context outright.
        // The mainloop thread is the long-lived piece, like WASAPI's
        // device enumerator.
        context_ = pa_context_new(pa_threaded_mainloop_get_api(mainloop_), "R5");
        if (!context_) { std::fprintf(stderr, "[audio] pa_context_new failed\n"); return false; }
        pa_context_set_state_callback(context_, &PulseCapture::onContextState, this);

        // NOAUTOSPAWN: open() is retried once a second while no server is
        // reachable; it should never be what launches a sound daemon.
        if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            logContextError("pa_context_connect");
            return false;
        }
        for (;;) {
            const pa_context_state_t st = pa_context_get_state(context_);
            if (st == PA_CONTEXT_READY) break;
            if (!PA_CONTEXT_IS_GOOD(st)) { logContextError("context connect"); return false; }
            pa_threaded_mainloop_wait(mainloop_);
        }

        std::string sinkName;
        if (!queryDefaultSink(sinkName) || sinkName.empty()) {
            std::fprintf(stderr, "[audio] no default output sink\n");
            return false;
        }

        // Match the sink's own rate so the server doesn't resample just
        // for us; always ask for stereo, which the server down/up-mixes
        // to from whatever the sink's real layout is -- audio.cpp only
        // ever looks at the first two channels for balance anyway.
        unsigned sinkRate = 0;
        querySinkRate(sinkName, sinkRate);
        sampleRate_ = sinkRate ? sinkRate : 48000;
        numChannels_ = 2;

        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_FLOAT32NE;
        spec.rate = sampleRate_;
        spec.channels = static_cast<uint8_t>(numChannels_);

        stream_ = pa_stream_new(context_, "R5 loopback", &spec, nullptr);
        if (!stream_) { logContextError("pa_stream_new"); return false; }
        pa_stream_set_state_callback(stream_, &PulseCapture::onStreamState, this);

        // Small fragments so data arrives at roughly frame rate instead of
        // in the server's default ~2s chunks; maxlength caps how far
        // behind we can fall (1s, same as the WASAPI buffer) before the
        // server starts dropping the oldest audio for us.
        const uint32_t bytesPerSec = static_cast<uint32_t>(pa_bytes_per_second(&spec));
        pa_buffer_attr attr{};
        attr.maxlength = bytesPerSec;
        attr.fragsize = bytesPerSec / 100; // ~10ms
        attr.tlength = attr.prebuf = attr.minreq = static_cast<uint32_t>(-1);

        // Remembered as the *sink* name, not the monitor's, since that's
        // what the server's default-sink setting is compared against.
        currentSinkName_ = sinkName;
        const std::string monitorName = sinkName + ".monitor";
        const auto flags = static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
        if (pa_stream_connect_record(stream_, monitorName.c_str(), &attr, flags) < 0) {
            logContextError("pa_stream_connect_record");
            return false;
        }
        for (;;) {
            const pa_stream_state_t st = pa_stream_get_state(stream_);
            if (st == PA_STREAM_READY) break;
            if (!PA_STREAM_IS_GOOD(st)) { logContextError("stream connect"); return false; }
            pa_threaded_mainloop_wait(mainloop_);
        }

        std::printf("[audio] monitor capture (re)started on %s: %u Hz, %u ch\n",
                    monitorName.c_str(), sampleRate_, numChannels_);
        std::fflush(stdout);

        ready_ = true;
        return true;
    }

    bool isReady() const override { return ready_; }

    // PA_STREAM_DONT_MOVE above keeps the server from silently migrating
    // our stream when the user switches outputs (it would follow its own
    // rules, not the new default) -- so, same as on Windows, a changed
    // default is detected here by polling and handled by reopening.
    bool defaultDeviceChanged() override {
        if (!mainloop_ || !context_) return false;
        Lock lock(mainloop_);
        if (pa_context_get_state(context_) != PA_CONTEXT_READY) return false;
        std::string sinkName;
        if (!queryDefaultSink(sinkName) || sinkName.empty()) return false;
        return sinkName != currentSinkName_;
    }

    bool pullSamples(std::vector<float>& out) override {
        Lock lock(mainloop_);
        if (!stream_ || pa_stream_get_state(stream_) != PA_STREAM_READY) {
            // Stream killed under us: sink unplugged (DONT_MOVE means it
            // isn't rescued onto another sink), or the server went away.
            std::fprintf(stderr, "[audio] capture stream lost\n");
            ready_ = false;
            return false;
        }

        // Drain everything the mainloop thread has queued locally since
        // the last call. Never blocks: peek returns 0 bytes once empty.
        for (;;) {
            const void* data = nullptr;
            size_t nbytes = 0;
            if (pa_stream_peek(stream_, &data, &nbytes) < 0) {
                logContextError("pa_stream_peek");
                return false;
            }
            if (nbytes == 0) break; // queue empty (and nothing to drop)

            const size_t numFloats = nbytes / sizeof(float);
            const size_t base = out.size();
            out.resize(base + numFloats, 0.0f);
            if (data != nullptr) {
                std::memcpy(out.data() + base, data, numFloats * sizeof(float));
            }
            // else: a hole in the stream -- already zero-filled by the
            // resize() above, per AudioCapture's silence contract.

            pa_stream_drop(stream_);
        }
        return true;
    }

    unsigned sampleRate() const override { return sampleRate_; }
    unsigned numChannels() const override { return numChannels_; }

private:
    // RAII for the threaded-mainloop lock, which must be held for every
    // pa_* call made from outside the mainloop thread.
    struct Lock {
        explicit Lock(pa_threaded_mainloop* m) : m_(m) { pa_threaded_mainloop_lock(m_); }
        ~Lock() { pa_threaded_mainloop_unlock(m_); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
        pa_threaded_mainloop* m_;
    };

    // Lock must be held. Runs `op` to completion, waiting on the mainloop
    // thread; the op's callback is expected to signal.
    void waitFor(pa_operation* op) {
        if (!op) return;
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(mainloop_);
        }
        pa_operation_unref(op);
    }

    // Lock must be held.
    bool queryDefaultSink(std::string& name) {
        struct Ctx { PulseCapture* self; std::string* name; } ctx{this, &name};
        pa_operation* op = pa_context_get_server_info(context_,
            [](pa_context*, const pa_server_info* info, void* userdata) {
                auto* c = static_cast<Ctx*>(userdata);
                if (info && info->default_sink_name) *c->name = info->default_sink_name;
                pa_threaded_mainloop_signal(c->self->mainloop_, 0);
            }, &ctx);
        if (!op) { logContextError("pa_context_get_server_info"); return false; }
        waitFor(op);
        return true;
    }

    // Lock must be held. Leaves `rate` untouched if the sink can't be found.
    void querySinkRate(const std::string& sinkName, unsigned& rate) {
        struct Ctx { PulseCapture* self; unsigned* rate; } ctx{this, &rate};
        pa_operation* op = pa_context_get_sink_info_by_name(context_, sinkName.c_str(),
            [](pa_context*, const pa_sink_info* info, int eol, void* userdata) {
                auto* c = static_cast<Ctx*>(userdata);
                if (eol == 0 && info) *c->rate = info->sample_spec.rate;
                // Called once per match and once more with eol set; only
                // the final call ends the wait.
                if (eol != 0) pa_threaded_mainloop_signal(c->self->mainloop_, 0);
            }, &ctx);
        waitFor(op);
    }

    static void onContextState(pa_context*, void* userdata) {
        pa_threaded_mainloop_signal(static_cast<PulseCapture*>(userdata)->mainloop_, 0);
    }

    static void onStreamState(pa_stream*, void* userdata) {
        pa_threaded_mainloop_signal(static_cast<PulseCapture*>(userdata)->mainloop_, 0);
    }

    void logContextError(const char* what) const {
        const int err = context_ ? pa_context_errno(context_) : PA_ERR_UNKNOWN;
        std::fprintf(stderr, "[audio] %s failed: %s\n", what, pa_strerror(err));
    }

    // Lock must be held. Tears down the stream and context only --
    // `mainloop_` lives for the whole process and is freed only in the
    // destructor.
    void releaseCaptureObjects() {
        if (stream_) {
            pa_stream_set_state_callback(stream_, nullptr, nullptr);
            pa_stream_disconnect(stream_);
            pa_stream_unref(stream_);
            stream_ = nullptr;
        }
        if (context_) {
            pa_context_set_state_callback(context_, nullptr, nullptr);
            pa_context_disconnect(context_);
            pa_context_unref(context_);
            context_ = nullptr;
        }
    }

    pa_threaded_mainloop* mainloop_ = nullptr;
    pa_context* context_ = nullptr;
    pa_stream* stream_ = nullptr;
    bool ready_ = false;

    unsigned sampleRate_ = 48000;
    unsigned numChannels_ = 2;
    std::string currentSinkName_;
};

} // namespace

std::unique_ptr<AudioCapture> createAudioCapture() {
    return std::make_unique<PulseCapture>();
}
