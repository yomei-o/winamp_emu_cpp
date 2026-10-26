// The browser front end.  It runs inside a Web Worker: the emulator owns the
// worker's thread and blocks in Atomics.wait while the guest waits for input,
// which is what lets a guest's modal loops (dialogs, menus, drags) nest on the
// host stack exactly as they do natively.
//
//   main thread  --events-->  SharedArrayBuffer ring  --Atomics-->  worker
//   worker       --frames-->  ImageBitmap by postMessage  -->  page canvas
//
// worker.js sets Module.jwEvents (an Int32Array over the ring) before
// calling jwemu_main.
#include <emscripten.h>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "emulator.h"
#include "files.h"
#include "win32/font.h"
#include "win32/gui.h"

using namespace x86emu;
using namespace x86emu::win32;

// Ring layout (Int32 slots): [0] events written, [1] events read,
// [16 + 8*i ...] event i: type, x, y, button, key, mods, delta/ch, w|h.
// Event types 100 and up are file operations from the page (the file panel):
// they are carried out here, in JS, while the guest waits, and never reach the
// emulator.  worker.js supplies Module.jwFileOp.
EM_JS(int, js_wait_event, (int timeout_ms, int* out), {
    const ev = Module.jwEvents;
    const SLOTS = 8, BASE = 16, N = ((ev.length - BASE) / SLOTS) | 0;
    const deadline = timeout_ms < 0 ? Infinity : Date.now() + timeout_ms;
    for (;;) {
        let w = Atomics.load(ev, 0), r = Atomics.load(ev, 1);
        if (w === r) {
            const left = deadline - Date.now();
            if (timeout_ms === 0 || left <= 0) return 0;
            Atomics.wait(ev, 0, w, left === Infinity ? Infinity : left);
            w = Atomics.load(ev, 0);
            if (w === r) {
                if (Date.now() >= deadline) return 0;
                continue;
            }
        }
        const at = BASE + (r % N) * SLOTS;
        const type = ev[at];
        if (type >= 100) {
            const slot = Array.from(ev.subarray(at, at + SLOTS));
            Atomics.store(ev, 1, r + 1);
            let deliver = false;
            try { if (Module.jwFileOp) deliver = Module.jwFileOp(slot); } catch (e) { console.log('file op failed', e); }
            if (deliver) {  // one for the emulator too (play a file): path in Module.waOpenPath
                for (let i = 0; i < SLOTS; i++) HEAP32[(out >> 2) + i] = slot[i];
                return 1;
            }
            continue;
        }
        for (let i = 0; i < SLOTS; i++) HEAP32[(out >> 2) + i] = ev[at + i];
        Atomics.store(ev, 1, r + 1);
        return 1;
    }
});

// The path the page asked to play (file op 105/106), as UTF-8.
EM_JS(int, js_open_path, (char* buf, int cap), {
    const b = new TextEncoder().encode(Module.waOpenPath || '').slice(0, cap - 1);
    HEAPU8.set(b, buf);
    HEAPU8[buf + b.length] = 0;
    return b.length;
});

EM_JS(void, js_present, (const uint8_t* rgba, int w, int h), {
    // The worker never returns to its event loop, so an OffscreenCanvas it
    // owned would never be committed to the page.  It draws into one of its
    // own, cuts an ImageBitmap off it (synchronous), and posts that: a message
    // leaves at once even from a blocked worker.
    if (typeof OffscreenCanvas === 'undefined') {
        // Headless (node): keep the latest frame for the test to look at.
        Module.jwFrame = {w: w, h: h, rgba: HEAPU8.slice(rgba, rgba + w * h * 4)};
        return;
    }
    if (!Module.jwOff || Module.jwOff.width !== w || Module.jwOff.height !== h) {
        Module.jwOff = new OffscreenCanvas(w, h);
        Module.jwCtx = Module.jwOff.getContext('2d', {alpha: false});
    }
    const img = new ImageData(new Uint8ClampedArray(HEAPU8.buffer, rgba, w * h * 4).slice(), w, h);
    Module.jwCtx.putImageData(img, 0, 0);
    const bmp = Module.jwOff.transferToImageBitmap();
    postMessage({type: 'frame', bitmap: bmp}, [bmp]);
});

EM_JS(void, js_set_cursor, (int id), { if (typeof postMessage === 'function') postMessage({type: 'cursor', id: id}); });

EM_JS(void, js_clipboard, (const char* s), { if (typeof postMessage === 'function') postMessage({type: 'clipboard', text: UTF8ToString(s)}); });

// The registry file, after each save: the page keeps it in localStorage,
// because the worker never yields to the event loop IndexedDB would need.
EM_JS(void, js_persist, (const char* path), {
    const p = UTF8ToString(path);
    try {
        if (typeof postMessage === 'function') postMessage({type: 'persist', path: p, text: FS.readFile(p, {encoding: 'utf8'})});
    } catch (e) {}
});

// A print job's PDF, sent straight to the page so it can be downloaded.
EM_JS(void, js_printed, (const char* path), {
    const p = UTF8ToString(path);
    try {
        if (typeof postMessage === 'function') postMessage({type: 'printed', path: p, data: FS.readFile(p)});
    } catch (e) {}
});

EM_JS(void, js_status, (const char* s), { if (typeof postMessage === 'function') postMessage({type: 'status', text: UTF8ToString(s)}); });

// Sound: a ring of 16-bit stereo frames in a SharedArrayBuffer that the page's
// AudioWorklet drains (audio-worklet.js has the same layout).  Int32 header:
// [0] frames written  [1] frames played  [2] paused  [3] sample rate
// [4] [5] volume left/right (0..65535)  [6] drop everything before this frame
// The counters are wrapping 32-bit frame counts.
EM_JS(int, js_audio_ok, (), { return Module.waAudio ? 1 : 0; });
EM_JS(void, js_audio_set, (int slot, int value), {
    Atomics.store(Module.waAudio, slot, value);
});
EM_JS(int, js_audio_get, (int slot), { return Atomics.load(Module.waAudio, slot); });
EM_JS(void, js_audio_push, (const int16_t* frames, int count), {
    const h = Module.waAudio, pcm = Module.waPcm;
    const cap = (pcm.length / 2) | 0;
    let w = Atomics.load(h, 0) >>> 0;
    const src = HEAP16.subarray(frames >> 1, (frames >> 1) + count * 2);
    for (let i = 0; i < count; i++, w++) {
        const at = (w % cap) * 2;
        pcm[at] = src[i * 2];
        pcm[at + 1] = src[i * 2 + 1];
    }
    Atomics.store(h, 0, w | 0);
});

namespace {

// Hands a file to Winamp the way another program would: WM_COPYDATA with
// IPC_PLAYFILE (100) adds it to the playlist; to play it now, the playlist
// is cleared first (IPC_DELETE, 101) and IPC_STARTPLAY (102) follows.
Emulator* g_emu = nullptr;

void winamp_open(const char* fs_path, bool play) {
    if (!g_emu) return;
    Gui& g = gui(*g_emu);
    uint32_t wa = 0;
    for (uint32_t h : g.win(g.desktop)->children) {
        Window* w = g.win(h);
        if (w && w->cls && w->cls->name == u"WINAMP V1.X") wa = h;
    }
    if (!wa) return;
    // "/C:/Music/a.mp3" is C:\Music\a.mp3 to the guest.
    std::string path = fs_path;
    if (path.size() > 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
    for (char& c : path)
        if (c == '/') c = '\\';
    std::string ansi = utf16_to_cp932(from_utf8(path));
    Emulator& e = *g_emu;
    uint64_t str = e.heap_alloc(ansi.size() + 1);
    e.mem.write_cstring(str, ansi);
    uint64_t cds = e.heap_alloc(12);
    e.mem.write32(cds, 100);
    e.mem.write32(cds + 4, static_cast<uint32_t>(ansi.size() + 1));
    e.mem.write32(cds + 8, static_cast<uint32_t>(str));
    constexpr uint32_t kWmUser = 0x400, kWmCopyData = 0x4A;
    if (play) g.send(wa, kWmUser, 0, 101);
    g.send(wa, kWmCopyData, 0, cds);
    if (play) g.send(wa, kWmUser, 0, 102);
    e.heap_free(cds);
    e.heap_free(str);
}

class WasmHost : public Host {
public:
    bool wait_event(HostEvent& ev, int timeout_ms) override {
        // A timed run (the node test): stop once the time is up.
        if (const char* ms = std::getenv("WAEMU_RUN_MS")) {
            static double start = emscripten_get_now();
            if (emscripten_get_now() - start > std::atof(ms)) std::exit(0);
        }
        int raw[8];
        if (!js_wait_event(timeout_ms, raw)) return false;
        if (raw[0] == 105 || raw[0] == 106) {
            char p[1024];
            js_open_path(p, sizeof p);
            winamp_open(p, raw[0] == 105);
            return wait_event(ev, 0);
        }
        ev = HostEvent{};
        ev.type = static_cast<HostEvent::Type>(raw[0]);
        ev.x = raw[1];
        ev.y = raw[2];
        ev.button = raw[3];
        ev.key = raw[4];
        ev.mods = raw[5];
        ev.delta = raw[6];
        ev.ch = static_cast<uint32_t>(raw[6]);
        ev.w = raw[7] & 0xFFFF;
        ev.h = (raw[7] >> 16) & 0xFFFF;
        return true;
    }
    void present(const uint32_t* px, int w, int h, const Rect&) override {
        // 0x00RRGGBB words (B, G, R, X bytes) to the canvas's R, G, B, A.
        rgba_.resize(static_cast<size_t>(w) * h * 4);
        uint8_t* d = rgba_.data();
        for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
            uint32_t v = px[i];
            d[i * 4 + 0] = static_cast<uint8_t>(v >> 16);
            d[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
            d[i * 4 + 2] = static_cast<uint8_t>(v);
            d[i * 4 + 3] = 255;
        }
        js_present(d, w, h);
    }
    uint64_t now_ms() override { return static_cast<uint64_t>(emscripten_get_now()); }
    void set_cursor(int id) override {
        if (id != last_cursor_) {
            last_cursor_ = id;
            js_set_cursor(id);
        }
    }
    void clipboard_text(const std::string& s) override { js_clipboard(s.c_str()); }

    bool audio_open(int rate, int channels, int bits) override {
        if (!js_audio_ok()) return false;
        channels_ = channels;
        bits_ = bits;
        js_audio_set(3, rate);
        js_audio_set(2, 0);
        // Start from wherever the counters are: the ring outlives a waveOut.
        written_ = static_cast<uint32_t>(js_audio_get(0));
        js_audio_set(6, static_cast<int>(written_));
        js_audio_set(1, static_cast<int>(written_));
        origin_ = written_;
        dropped_ = written_;
        return true;
    }
    void audio_write(const uint8_t* data, size_t bytes) override {
        size_t block = static_cast<size_t>(channels_ * bits_ / 8);
        size_t n = bytes / block;
        frames_.resize(n * 2);
        for (size_t i = 0; i < n; ++i) {
            int16_t l, r;
            const uint8_t* f = data + i * block;
            if (bits_ == 16) {
                l = static_cast<int16_t>(f[0] | f[1] << 8);
                r = channels_ == 2 ? static_cast<int16_t>(f[2] | f[3] << 8) : l;
            } else {
                l = static_cast<int16_t>((f[0] - 128) << 8);
                r = channels_ == 2 ? static_cast<int16_t>((f[1] - 128) << 8) : l;
            }
            frames_[i * 2] = l;
            frames_[i * 2 + 1] = r;
        }
        if (n) js_audio_push(frames_.data(), static_cast<int>(n));
        written_ += n;
    }
    uint64_t audio_played() override {
        // Played frames, extended past the 32-bit wrap: never more than written.
        uint32_t read = static_cast<uint32_t>(js_audio_get(1));
        uint64_t played = written_ - static_cast<uint32_t>(static_cast<uint32_t>(written_) - read);
        played = std::max(played, dropped_);
        return (played - origin_) * static_cast<uint64_t>(channels_ * bits_ / 8);
    }
    void audio_pause(bool paused) override { js_audio_set(2, paused ? 1 : 0); }
    void audio_reset() override {
        // Everything written counts as played from now on; the worklet skips it.
        dropped_ = written_;
        js_audio_set(6, static_cast<int>(static_cast<uint32_t>(written_)));
    }
    void audio_close() override { audio_reset(); }
    void audio_volume(int left, int right) override {
        if (!js_audio_ok()) return;
        js_audio_set(4, left);
        js_audio_set(5, right);
    }

private:
    int channels_ = 2, bits_ = 16;
    uint64_t written_ = 0, origin_ = 0, dropped_ = 0;
    std::vector<int16_t> frames_;
    std::vector<uint8_t> rgba_;
    int last_cursor_ = -1;
};

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE int waemu_main(int w, int h) {
    static WasmHost host;
    set_font_dir("/fonts");
    Emulator::Options opt;
    opt.library_paths.push_back("/dlls");
    opt.trace_calls = std::getenv("JWEMU_TRACE_CALLS") != nullptr;
    static Emulator emu(opt);
    g_emu = &emu;
    Gui& g = gui(emu);
    g.host = &host;
    HostEvent rs;
    rs.type = HostEvent::Resize;
    rs.w = w;
    rs.h = h;
    Msg dummy;
    g.translate_input(rs, dummy);
    setenv("JWEMU_REGISTRY", "/persist/registry.txt", 1);
    registry_saved_hook = [](const std::string& path) { js_persist(path.c_str()); };
    print_done_hook = [](const std::string& guest) { js_printed(FileTable::host_path(guest).c_str()); };
    std::vector<std::string> args = {"C:\\Winamp\\winamp.exe"};
    if (const char* open = std::getenv("WAEMU_OPEN")) args.push_back(open);
    try {
        js_status("starting");
        emu.load("C:/Winamp/winamp.exe", args);
        int code = emu.run();
        std::fprintf(stderr, "jwemu: exited with %d\n", code);
        js_status("exited");
        return code;
    } catch (const std::exception& err) {
        std::string msg = std::string("jwemu: ") + err.what() + "\n" + (emu.has_cpu() ? emu.stack_trace(12) : std::string());
        std::fprintf(stderr, "%s\n", msg.c_str());
        js_status(msg.c_str());
        return 1;
    }
}
