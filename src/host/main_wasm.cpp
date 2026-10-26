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
            try { if (Module.jwFileOp) Module.jwFileOp(slot); } catch (e) { console.log('file op failed', e); }
            continue;
        }
        for (let i = 0; i < SLOTS; i++) HEAP32[(out >> 2) + i] = ev[at + i];
        Atomics.store(ev, 1, r + 1);
        return 1;
    }
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

namespace {

class WasmHost : public Host {
public:
    bool wait_event(HostEvent& ev, int timeout_ms) override {
        int raw[8];
        if (!js_wait_event(timeout_ms, raw)) return false;
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

private:
    std::vector<uint8_t> rgba_;
    int last_cursor_ = -1;
};

}  // namespace

extern "C" EMSCRIPTEN_KEEPALIVE int jwemu_main(int w, int h) {
    static WasmHost host;
    set_font_dir("/fonts");
    Emulator::Options opt;
    opt.library_paths.push_back("/dlls");
    opt.trace_calls = std::getenv("JWEMU_TRACE_CALLS") != nullptr;
    static Emulator emu(opt);
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
    std::vector<std::string> args = {"C:\\jww\\Jw_win.exe"};
    try {
        js_status("starting");
        emu.load("C:/jww/Jw_win.exe", args);
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
