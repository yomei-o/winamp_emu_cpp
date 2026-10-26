// winmm's waveOut: the guest's PCM goes to the host's sound device.
//
// A header the guest writes is "playing" until the device has consumed every
// byte up to its end; then it is marked done and the guest is told the way it
// asked to be (an event, a window message).  The device's own count of played
// bytes is the clock, so the guest refills exactly as fast as the sound goes
// out.  A host with no device (headless runs) plays to the wall clock instead.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

#include "hookutil.h"

namespace x86emu::win32 {

namespace {

constexpr uint32_t kWhdrDone = 0x01, kWhdrPrepared = 0x02, kWhdrInqueue = 0x10;
constexpr uint32_t kCallbackTypeMask = 0x00070000;
constexpr uint32_t kCallbackWindow = 0x00010000, kCallbackThread = 0x00020000,
                   kCallbackFunction = 0x00030000, kCallbackEvent = 0x00050000;
constexpr uint32_t kWomOpen = 0x3BB, kWomClose = 0x3BC, kWomDone = 0x3BD;
constexpr uint32_t kHandle = 0x5A0001;  // one device, one handle

struct Queued {
    uint64_t hdr;
    uint64_t end;  // byte offset in the stream where this buffer finishes
};

struct WaveOut {
    bool open = false;
    bool device = false;  // the host plays it; otherwise the clock does
    uint32_t rate = 44100, channels = 2, bits = 16, block = 4;
    uint32_t flags = 0;
    uint64_t callback = 0, instance = 0;
    std::deque<Queued> queue;
    uint64_t written = 0;  // bytes handed over since open (or the last reset)
    // The clock fallback: bytes played = elapsed time since `start`, less
    // pauses, plus `base`.
    uint64_t start_ms = 0, base = 0;
    bool paused = false;
    uint64_t paused_at = 0;
    uint32_t volume = 0xFFFFFFFF;
};

WaveOut& wo() {
    static WaveOut w;
    return w;
}

uint64_t played(Emulator& e) {
    WaveOut& w = wo();
    Host* h = gui(e).host;
    if (w.device) return w.base + h->audio_played();
    uint64_t now = w.paused ? w.paused_at : h->now_ms();
    uint64_t p = w.base + (now - w.start_ms) * w.rate * w.block / 1000;
    return std::min(p, w.written);
}

void notify(Emulator& e, uint32_t msg, uint64_t p1) {
    WaveOut& w = wo();
    switch (w.flags & kCallbackTypeMask) {
        case kCallbackEvent:
            e.signal_object(w.callback);
            break;
        case kCallbackWindow:
            gui(e).post(static_cast<uint32_t>(w.callback), msg, kHandle, p1);
            break;
        case kCallbackThread:
            gui(e).post(0, msg, kHandle, p1);
            break;
        case kCallbackFunction:
            // (hwo, msg, instance, param1, param2), on whichever thread is
            // running; the real one is a driver thread, so no guest may care.
            e.call_guest(w.callback, {kHandle, msg, w.instance, p1, 0});
            break;
        default:
            break;
    }
}

// Retires every buffer the device has finished; returns the milliseconds until
// the next one will be, or -1 if none is playing.
int update(Emulator& e) {
    WaveOut& w = wo();
    if (!w.open || w.queue.empty()) return -1;
    uint64_t pos = played(e);
    while (!w.queue.empty() && w.queue.front().end <= pos) {
        uint64_t hdr = w.queue.front().hdr;
        w.queue.pop_front();
        uint32_t f = e.mem.read32(hdr + 16);
        e.mem.write32(hdr + 16, (f & ~kWhdrInqueue) | kWhdrDone);
        notify(e, kWomDone, hdr);
    }
    if (w.queue.empty() || w.paused) return -1;
    uint64_t left = w.queue.front().end - pos;
    uint64_t per_ms = std::max<uint64_t>(1, static_cast<uint64_t>(w.rate) * w.block / 1000);
    return static_cast<int>(std::min<uint64_t>(left / per_ms + 1, 1000));
}

void reset(Emulator& e) {
    WaveOut& w = wo();
    if (w.device) gui(e).host->audio_reset();
    for (const Queued& q : w.queue) {
        uint32_t f = e.mem.read32(q.hdr + 16);
        e.mem.write32(q.hdr + 16, (f & ~kWhdrInqueue) | kWhdrDone);
        notify(e, kWomDone, q.hdr);
    }
    w.queue.clear();
    w.written = 0;
    w.base = 0;
    w.start_ms = gui(e).host->now_ms();
    w.paused_at = w.start_ms;
    if (w.device) {
        // The device's count carries on from where it was; start from there.
        w.base = 0 - gui(e).host->audio_played();
    }
}

}  // namespace

void install_winmm(Emulator& e) {
    e.device_poll = [&e] { return update(e); };

    api(e, "waveOutGetNumDevs", 0, [](Emulator& e) { ret(e, 1); });
    // WAVEOUTCAPSA: mid, pid, version, name[32], formats, channels, reserved, support
    api(e, "waveOutGetDevCapsA", 3, [](Emulator& e) {
        uint64_t p = A(e, 1);
        if (!p || U(e, 2) < 52) return ret(e, 11);  // MMSYSERR_INVALPARAM
        std::vector<uint8_t> z(52, 0);
        e.mem.write(p, z.data(), z.size());
        e.mem.write16(p, 1);
        e.mem.write32(p + 4, 0x0100);
        e.mem.write_cstring(p + 8, "Browser sound");
        e.mem.write32(p + 40, 0xFFF);    // every PCM format up to 44.1k/16/stereo
        e.mem.write16(p + 44, 2);
        e.mem.write32(p + 48, 0x4 | 0x8);  // WAVECAPS_VOLUME | WAVECAPS_LRVOLUME
        ret(e, 0);
    });
    api(e, "waveOutGetErrorTextA", 3, [](Emulator& e) {
        if (A(e, 1) && U(e, 2)) {
            std::string t = "waveOut error " + std::to_string(U(e, 0));
            if (t.size() >= U(e, 2)) t.resize(U(e, 2) - 1);
            e.mem.write_cstring(A(e, 1), t);
        }
        ret(e, 0);
    });
    // (phwo, device, format, callback, instance, flags)
    api(e, "waveOutOpen", 6, [](Emulator& e) {
        WaveOut& w = wo();
        uint64_t fmt = A(e, 2);
        if (!fmt) return ret(e, 11);
        uint16_t tag = e.mem.read16(fmt);
        uint32_t ch = e.mem.read16(fmt + 2), rate = e.mem.read32(fmt + 4), bits = e.mem.read16(fmt + 14);
        if (tag != 1 || ch < 1 || ch > 2 || (bits != 8 && bits != 16) || rate < 1000 || rate > 192000)
            return ret(e, 32);  // WAVERR_BADFORMAT
        if (U(e, 5) & 1) return ret(e, 0);  // WAVE_FORMAT_QUERY
        if (w.open) return ret(e, 4);       // MMSYSERR_ALLOCATED
        w = WaveOut{};
        w.open = true;
        w.rate = rate;
        w.channels = ch;
        w.bits = bits;
        w.block = ch * bits / 8;
        w.callback = A(e, 3);
        w.instance = A(e, 4);
        w.flags = U(e, 5);
        Host* h = gui(e).host;
        w.device = h->audio_open(static_cast<int>(rate), static_cast<int>(ch), static_cast<int>(bits));
        w.start_ms = h->now_ms();
        if (std::getenv("WAEMU_TRACE_WAVE"))
            std::fprintf(stderr, "[wave] open %u Hz %u ch %u bit, flags 0x%X, device %d\n", rate, ch, bits, w.flags, w.device);
        if (A(e, 0)) e.mem.write32(A(e, 0), kHandle);
        notify(e, kWomOpen, 0);
        ret(e, 0);
    });
    api(e, "waveOutClose", 1, [](Emulator& e) {
        WaveOut& w = wo();
        if (!w.open) return ret(e, 5);  // MMSYSERR_INVALHANDLE
        if (!w.queue.empty()) return ret(e, 33);  // WAVERR_STILLPLAYING
        if (w.device) gui(e).host->audio_close();
        notify(e, kWomClose, 0);
        w.open = false;
        ret(e, 0);
    });
    api(e, "waveOutPrepareHeader", 3, [](Emulator& e) {
        uint64_t h = A(e, 1);
        e.mem.write32(h + 16, (e.mem.read32(h + 16) & ~kWhdrDone) | kWhdrPrepared);
        ret(e, 0);
    });
    api(e, "waveOutUnprepareHeader", 3, [](Emulator& e) {
        uint64_t h = A(e, 1);
        update(e);
        uint32_t f = e.mem.read32(h + 16);
        if (f & kWhdrInqueue) return ret(e, 33);
        e.mem.write32(h + 16, f & ~kWhdrPrepared);
        ret(e, 0);
    });
    api(e, "waveOutWrite", 3, [](Emulator& e) {
        WaveOut& w = wo();
        uint64_t h = A(e, 1);
        if (!w.open) return ret(e, 5);
        uint32_t f = e.mem.read32(h + 16);
        if (!(f & kWhdrPrepared)) return ret(e, 34);  // WAVERR_UNPREPARED
        uint32_t n = e.mem.read32(h + 4) / w.block * w.block;
        if (w.device && n) {
            std::vector<uint8_t> buf(n);
            e.mem.read(e.mem.read32(h), buf.data(), n);
            gui(e).host->audio_write(buf.data(), n);
        }
        if (w.queue.empty() && !w.device && !w.paused) {
            // The clock only runs while there is something to play: a gap
            // between buffers is silence the guest did not ask to skip.
            uint64_t now = gui(e).host->now_ms();
            w.base = w.written;
            w.start_ms = now;
        }
        w.written += n;
        if (std::getenv("WAEMU_TRACE_WAVE"))
            std::fprintf(stderr, "[wave] write %u bytes at %llu ms, total %llu, played %llu\n", n,
                         (unsigned long long)gui(e).host->now_ms(), (unsigned long long)w.written,
                         (unsigned long long)played(e));
        e.mem.write32(h + 16, (f & ~kWhdrDone) | kWhdrInqueue);
        w.queue.push_back({h, w.written});
        update(e);
        ret(e, 0);
    });
    api(e, "waveOutPause", 1, [](Emulator& e) {
        WaveOut& w = wo();
        if (!w.paused) {
            update(e);
            w.paused = true;
            w.paused_at = gui(e).host->now_ms();
            if (w.device) gui(e).host->audio_pause(true);
        }
        ret(e, 0);
    });
    api(e, "waveOutRestart", 1, [](Emulator& e) {
        WaveOut& w = wo();
        if (w.paused) {
            w.paused = false;
            w.start_ms += gui(e).host->now_ms() - w.paused_at;
            if (w.device) gui(e).host->audio_pause(false);
        }
        ret(e, 0);
    });
    api(e, "waveOutReset", 1, [](Emulator& e) {
        reset(e);
        wo().paused = false;
        ret(e, 0);
    });
    // MMTIME: wType, then the value; only TIME_BYTES (4) and TIME_SAMPLES (2).
    api(e, "waveOutGetPosition", 3, [](Emulator& e) {
        WaveOut& w = wo();
        uint64_t p = A(e, 1);
        uint64_t pos = played(e);
        if (e.mem.read32(p) == 2) {
            e.mem.write32(p + 4, static_cast<uint32_t>(pos / w.block));
        } else {
            e.mem.write32(p, 4);
            e.mem.write32(p + 4, static_cast<uint32_t>(pos));
        }
        ret(e, 0);
    });
    api(e, "waveOutGetVolume", 2, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), wo().volume);
        ret(e, 0);
    });
    api(e, "waveOutSetVolume", 2, [](Emulator& e) {
        wo().volume = U(e, 1);
        gui(e).host->audio_volume(static_cast<int>(U(e, 1) & 0xFFFF), static_cast<int>(U(e, 1) >> 16));
        ret(e, 0);
    });
}

}  // namespace x86emu::win32
