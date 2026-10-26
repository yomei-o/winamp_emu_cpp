// jwemu: runs a Windows GUI program in the emulator.
//
//   jwemu [options] program.exe [args...]
//
//   --size WxH        screen size (default 1280x800)
//   --headless        no window; use with --script / --shot
//   --script FILE     timed input for the headless host (see below)
//   --shot FILE       write the screen as PNG when the guest goes idle at
//                     the end of the script (or after --time ms)
//   --time MS         headless: how long to run before the final shot
//   --dlls DIR        where real DLLs (comctl32.dll) are found (default ./dlls)
//   --fonts DIR       where the TrueType files are (default ./fonts)
//   --drive-c DIR     the guest's C: drive is this folder (as in the browser)
//   -c / --trace-calls, --max-insns N   as in x86emu
//
// Script lines: "<ms> move X Y", "<ms> down X Y [button]", "<ms> up X Y [button]",
// "<ms> key VK", "<ms> char TEXT", "<ms> wheel X Y DELTA", "<ms> shot FILE",
// "<ms> quit".  Times are milliseconds since start.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "emulator.h"
#include "files.h"
#include "win32/font.h"
#include "win32/gui.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace x86emu;
using namespace x86emu::win32;

namespace {

// ---- PNG -------------------------------------------------------------------------------

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

bool write_png(const std::string& path, const uint32_t* px, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(w * 3 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        for (int x = 0; x < w; ++x) {
            uint32_t v = px[static_cast<size_t>(y) * w + x];
            raw.push_back(static_cast<uint8_t>(v >> 16));
            raw.push_back(static_cast<uint8_t>(v >> 8));
            raw.push_back(static_cast<uint8_t>(v));
        }
    }
    // zlib stream of stored blocks.
    std::vector<uint8_t> z{0x78, 0x01};
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<ptrdiff_t>(pos), raw.begin() + static_cast<ptrdiff_t>(pos + n));
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(static_cast<uint8_t>(adler >> (i * 8)));
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        uint8_t len[4] = {static_cast<uint8_t>(data.size() >> 24), static_cast<uint8_t>(data.size() >> 16),
                          static_cast<uint8_t>(data.size() >> 8), static_cast<uint8_t>(data.size())};
        std::fwrite(len, 1, 4, fp);
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        std::fwrite(td.data(), 1, td.size(), fp);
        uint32_t c = crc32(td.data(), td.size());
        uint8_t cb[4] = {static_cast<uint8_t>(c >> 24), static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c)};
        std::fwrite(cb, 1, 4, fp);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, fp);
    std::vector<uint8_t> ihdr = {static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8),
                                 static_cast<uint8_t>(w), static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16),
                                 static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(fp);
    return true;
}

uint64_t wall_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---- headless ---------------------------------------------------------------------------

struct ScriptLine {
    uint64_t at = 0;
    std::string cmd;
    std::vector<std::string> args;
};

Gui* g_dump_gui = nullptr;  // for the script's "dump"

class HeadlessHost : public Host {
public:
    HeadlessHost(std::vector<ScriptLine> script, std::string final_shot, uint64_t run_ms)
        : script_(std::move(script)), final_shot_(std::move(final_shot)), run_ms_(run_ms) {
        start_ = wall_ms();
    }
    uint64_t now_ms() override { return wall_ms(); }
    void present(const uint32_t* px, int w, int h, const Rect&) override {
        last_.assign(px, px + static_cast<size_t>(w) * h);
        w_ = w;
        h_ = h;
    }
    // Script times are on the program's clock, not the wall's: time passes
    // freely while it waits for input, but a stretch of computation between
    // two looks at the queue counts for at most 20 ms.  A slow machine, or
    // several runs at once, then delivers the same events at the same points
    // in the program - a click is never sent before the window it is meant
    // for has been drawn.
    bool wait_event(HostEvent& ev, int timeout_ms) override {
        auto tick = [&] {
            uint64_t now = wall_ms();
            vclock_ += std::min<uint64_t>(now - last_call_, 20);
            last_call_ = now;
        };
        tick();
        uint64_t deadline = timeout_ms < 0 ? ~0ull : wall_ms() + static_cast<uint64_t>(timeout_ms);
        while (true) {
            uint64_t t = vclock_;
            if (next_ < script_.size() && script_[next_].at <= t) {
                ScriptLine& l = script_[next_++];
                if (translate(l, ev)) {
                    last_call_ = wall_ms();
                    return true;
                }
                continue;
            }
            if (next_ >= script_.size() && run_ms_ && t >= run_ms_) {
                finish();
            }
            uint64_t now = wall_ms();
            if (now >= deadline) {
                last_call_ = now;
                return false;
            }
            uint64_t wake = deadline - now;
            if (next_ < script_.size()) wake = std::min<uint64_t>(wake, script_[next_].at - t);
            if (next_ >= script_.size() && run_ms_) wake = std::min<uint64_t>(wake, run_ms_ - t);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint64_t>(std::max<uint64_t>(wake, 1), 20)));
            tick();
        }
    }
    void finish() {
        if (!final_shot_.empty() && !last_.empty()) {
            write_png(final_shot_, last_.data(), w_, h_);
            std::fprintf(stderr, "[jwemu] wrote %s\n", final_shot_.c_str());
        }
        std::fflush(stderr);
        std::_Exit(0);
    }

private:
    bool translate(const ScriptLine& l, HostEvent& ev) {
        auto num = [&](size_t i, int def = 0) { return i < l.args.size() ? std::atoi(l.args[i].c_str()) : def; };
        ev = HostEvent{};
        if (l.cmd == "move") {
            ev.type = HostEvent::MouseMove;
        } else if (l.cmd == "down") {
            ev.type = HostEvent::MouseDown;
            ev.button = num(2);
        } else if (l.cmd == "up") {
            ev.type = HostEvent::MouseUp;
            ev.button = num(2);
        } else if (l.cmd == "wheel") {
            ev.type = HostEvent::Wheel;
            ev.delta = num(2, 120);
        } else if (l.cmd == "key") {
            ev.type = HostEvent::KeyDown;
            ev.key = num(0);
            pending_up_ = ev.key;
            ev.x = ev.y = 0;
            return true;
        } else if (l.cmd == "keyup") {
            ev.type = HostEvent::KeyUp;
            ev.key = num(0);
            return true;
        } else if (l.cmd == "char") {
            ev.type = HostEvent::Char;
            std::u16string s = from_utf8(l.args.empty() ? "" : l.args[0]);
            ev.ch = s.empty() ? 0 : s[0];
            return true;
        } else if (l.cmd == "shot") {
            if (!last_.empty() && !l.args.empty()) {
                write_png(l.args[0], last_.data(), w_, h_);
                std::fprintf(stderr, "[jwemu] wrote %s\n", l.args[0].c_str());
            }
            return false;
        } else if (l.cmd == "dump") {
            if (g_dump_gui) {
                g_dump_gui->dump_windows(stderr);
                for (const auto& name : g_dump_gui->emu.unimplemented_imports()) std::fprintf(stderr, "[unimplemented] %s\n", name.c_str());
            }
            return false;
        } else if (l.cmd == "quit") {
            finish();
        } else {
            return false;
        }
        ev.x = num(0);
        ev.y = num(1);
        return true;
    }

    std::vector<ScriptLine> script_;
    size_t next_ = 0;
    std::string final_shot_;
    uint64_t run_ms_;
    uint64_t start_;
    uint64_t vclock_ = 0;            // the script's clock (see wait_event)
    uint64_t last_call_ = wall_ms();
    std::vector<uint32_t> last_;
    int w_ = 0, h_ = 0;
    int pending_up_ = 0;
};

#if defined(_WIN32)
// ---- a native window ------------------------------------------------------------------------

class WindowHost : public Host {
public:
    std::mutex mu;
    std::condition_variable cv;
    std::deque<HostEvent> events;
    std::vector<uint32_t> frame;
    int fw = 0, fh = 0;
    HWND hwnd = nullptr;
    int cursor_id = 0;

    uint64_t now_ms() override { return wall_ms(); }
    bool wait_event(HostEvent& ev, int timeout_ms) override {
        std::unique_lock<std::mutex> lk(mu);
        if (events.empty() && timeout_ms != 0) {
            if (timeout_ms < 0) cv.wait(lk, [&] { return !events.empty(); });
            else cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !events.empty(); });
        }
        if (events.empty()) return false;
        ev = events.front();
        events.pop_front();
        return true;
    }
    void present(const uint32_t* px, int w, int h, const Rect&) override {
        {
            std::lock_guard<std::mutex> lk(mu);
            frame.assign(px, px + static_cast<size_t>(w) * h);
            fw = w;
            fh = h;
        }
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }
    void set_cursor(int idc) override { cursor_id = idc; }
    void push(const HostEvent& ev) {
        {
            std::lock_guard<std::mutex> lk(mu);
            events.push_back(ev);
        }
        cv.notify_one();
    }
};

WindowHost* g_whost = nullptr;

int mods_now() {
    return (GetKeyState(VK_SHIFT) < 0 ? 1 : 0) | (GetKeyState(VK_CONTROL) < 0 ? 2 : 0) | (GetKeyState(VK_MENU) < 0 ? 4 : 0);
}

LRESULT CALLBACK viewer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WindowHost* h = g_whost;
    auto mouse = [&](HostEvent::Type t, int button) {
        HostEvent ev;
        ev.type = t;
        ev.x = static_cast<short>(LOWORD(lp));
        ev.y = static_cast<short>(HIWORD(lp));
        ev.button = button;
        ev.mods = mods_now();
        h->push(ev);
    };
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            std::lock_guard<std::mutex> lk(h->mu);
            if (!h->frame.empty()) {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = h->fw;
                bi.bmiHeader.biHeight = -h->fh;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                SetDIBitsToDevice(dc, 0, 0, h->fw, h->fh, 0, 0, 0, h->fh, h->frame.data(), &bi, DIB_RGB_COLORS);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: mouse(HostEvent::MouseMove, 0); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetCapture(hwnd); mouse(HostEvent::MouseDown, 0); return 0;
        case WM_LBUTTONUP: ReleaseCapture(); mouse(HostEvent::MouseUp, 0); return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: mouse(HostEvent::MouseDown, 1); return 0;
        case WM_RBUTTONUP: mouse(HostEvent::MouseUp, 1); return 0;
        case WM_MBUTTONDOWN: mouse(HostEvent::MouseDown, 2); return 0;
        case WM_MBUTTONUP: mouse(HostEvent::MouseUp, 2); return 0;
        case WM_MOUSEWHEEL: {
            POINT p{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            ScreenToClient(hwnd, &p);
            HostEvent ev;
            ev.type = HostEvent::Wheel;
            ev.x = p.x;
            ev.y = p.y;
            ev.delta = static_cast<short>(HIWORD(wp));
            ev.mods = mods_now();
            h->push(ev);
            return 0;
        }
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            HostEvent ev;
            ev.type = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? HostEvent::KeyDown : HostEvent::KeyUp;
            ev.key = static_cast<int>(wp);
            ev.mods = mods_now();
            h->push(ev);
            if (wp == VK_F10 || wp == VK_MENU) return 0;
            break;
        }
        case WM_CHAR: case WM_SYSCHAR: {
            if (wp >= 0x20 && wp != 0x7F) {
                HostEvent ev;
                ev.type = HostEvent::Char;
                ev.ch = static_cast<uint32_t>(wp);
                ev.mods = mods_now();
                h->push(ev);
            }
            return 0;
        }
        case WM_SIZE: {
            HostEvent ev;
            ev.type = HostEvent::Resize;
            ev.w = LOWORD(lp);
            ev.h = HIWORD(lp);
            if (ev.w > 0 && ev.h > 0) h->push(ev);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                int id = h->cursor_id;
                LPCWSTR c = IDC_ARROW;
                switch (id) {
                    case 32513: c = IDC_IBEAM; break;
                    case 32514: c = IDC_WAIT; break;
                    case 32515: c = IDC_CROSS; break;
                    case 32642: c = IDC_SIZENWSE; break;
                    case 32643: c = IDC_SIZENESW; break;
                    case 32644: c = IDC_SIZEWE; break;
                    case 32645: c = IDC_SIZENS; break;
                    case 32646: c = IDC_SIZEALL; break;
                    case 32649: c = IDC_HAND; break;
                    case 0: c = IDC_CROSS; break;  // an application cursor: Jw_cad's are crosshairs
                }
                SetCursor(LoadCursorW(nullptr, c));
                return TRUE;
            }
            break;
        case WM_CLOSE: {
            HostEvent ev;
            ev.type = HostEvent::Quit;
            h->push(ev);
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
#endif

std::string exe_dir(const char* argv0) {
    std::string s = argv0;
    size_t p = s.find_last_of("/\\");
    return p == std::string::npos ? "." : s.substr(0, p);
}

bool dir_exists(const std::string& d) {
    FILE* fp = std::fopen((d + "/jwsubgothic.ttf").c_str(), "rb");
    if (fp) std::fclose(fp);
    return fp != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // The narrow argv is in the ANSI code page; everything here is UTF-8, so a
    // Japanese file name on the command line has to come from the wide one.
    std::vector<std::string> utf8_args;
    std::vector<char*> utf8_argv;
    {
        int wargc = 0;
        if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
            for (int k = 0; k < wargc; ++k) {
                int n = WideCharToMultiByte(CP_UTF8, 0, wargv[k], -1, nullptr, 0, nullptr, nullptr);
                std::string u(n > 0 ? n - 1 : 0, '\0');
                if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[k], -1, u.data(), n, nullptr, nullptr);
                utf8_args.push_back(u);
            }
            LocalFree(wargv);
            for (auto& u : utf8_args) utf8_argv.push_back(u.data());
            utf8_argv.push_back(nullptr);
            argc = wargc;
            argv = utf8_argv.data();
        }
    }
#endif
    Emulator::Options opt;
    int sw = 1280, sh = 800;
    bool headless = false;
    std::string script_path, shot_path, dlls, fonts;
    uint64_t run_ms = 0;
    std::vector<std::string> preload;  // --imports: DLLs the program loads at run time
    size_t history = 0;  // --history N: keep the last N instruction addresses
    int i = 1;
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a.empty() || a[0] != '-') break;
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &sw, &sh);
        else if (a == "--headless") headless = true;
        else if (a == "--script" && i + 1 < argc) script_path = argv[++i];
        else if (a == "--shot" && i + 1 < argc) shot_path = argv[++i];
        else if (a == "--time" && i + 1 < argc) run_ms = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--dlls" && i + 1 < argc) dlls = argv[++i];
        else if (a == "--fonts" && i + 1 < argc) fonts = argv[++i];
        else if (a == "-c" || a == "--trace-calls") opt.trace_calls = true;
        else if (a == "--imports") opt.imports_only = true;
        else if (a == "--drive-c" && i + 1 < argc) FileTable::set_drive_c(argv[++i]);
        else if (a == "--load" && i + 1 < argc) preload.push_back(argv[++i]);
        else if (a == "--history" && i + 1 < argc) history = std::strtoull(argv[++i], nullptr, 0);
        else if (a == "--max-insns" && i + 1 < argc) opt.max_instructions = std::strtoull(argv[++i], nullptr, 0);
        else {
            std::fprintf(stderr, "jwemu: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (i >= argc) {
        std::fprintf(stderr, "usage: jwemu [--size WxH] [--headless] [--script F] [--shot F] [--time MS] program.exe [args]\n");
        return 2;
    }
    std::string here = exe_dir(argv[0]);
    if (dlls.empty()) dlls = here + "/dlls";
    opt.library_paths.push_back(dlls);
    if (fonts.empty()) {
        for (const std::string& c : {here + "/fonts", here + "/../fonts", std::string("fonts")})
            if (dir_exists(c)) {
                fonts = c;
                break;
            }
    }
    if (!fonts.empty()) set_font_dir(fonts);

    std::vector<std::string> args(argv + i, argv + argc);
    Emulator emu(opt);

    std::vector<ScriptLine> script;
    if (!script_path.empty()) {
        std::ifstream in(script_path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            ScriptLine l;
            ls >> l.at >> l.cmd;
            std::string w;
            while (ls >> w) l.args.push_back(w);
            script.push_back(l);
        }
    }

#if defined(_WIN32)
    if (!headless) {
        static WindowHost wh;
        g_whost = &wh;
        Gui& g = gui(emu);
        g.host = &wh;
        HostEvent rs;
        rs.type = HostEvent::Resize;
        rs.w = sw;
        rs.h = sh;
        Msg dummy;
        g.translate_input(rs, dummy);
        WNDCLASSW wc{};
        wc.lpfnWndProc = viewer_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"jwemu";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DBLCLKS;
        RegisterClassW(&wc);
        RECT r{0, 0, sw, sh};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        wh.hwnd = CreateWindowW(L"jwemu", L"jwemu", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
        int code = 0;
        std::thread worker([&] {
            try {
                emu.load(args[0], args);
                if (history) emu.cpu().enable_history(history);
                code = emu.run();
            } catch (const std::exception& err) {
                std::fprintf(stderr, "jwemu: %s\n", err.what());
                code = 1;
            }
            PostMessageW(wh.hwnd, WM_CLOSE, 0, 0);
        });
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        worker.detach();
        std::_Exit(code);
    }
#endif
    if (opt.imports_only) {
        emu.load(args[0], args);
        if (history) emu.cpu().enable_history(history);
        for (const auto& dll : preload) emu.load_library_for_guest(dll);
        for (const auto& name : emu.unimplemented_imports()) std::printf("%s\n", name.c_str());
        return 0;
    }
    HeadlessHost hh(std::move(script), shot_path, run_ms ? run_ms : (shot_path.empty() ? 0 : 5000));
    Gui& g = gui(emu);
    g.host = &hh;
    g_dump_gui = &g;
    HostEvent rs;
    rs.type = HostEvent::Resize;
    rs.w = sw;
    rs.h = sh;
    Msg dummy;
    g.translate_input(rs, dummy);
    try {
        emu.load(args[0], args);
        if (history) emu.cpu().enable_history(history);
        int code = emu.run();
        hh.finish();
        return code;
    } catch (const std::exception& err) {
        uint64_t eip = emu.has_cpu() ? emu.cpu().rip : 0;
        std::fprintf(stderr, "jwemu: %s\n  eip=%08llX %s\n%s", err.what(), (unsigned long long)eip,
                     emu.describe_address(eip).c_str(), emu.has_cpu() ? emu.stack_trace(16).c_str() : "");
        if (emu.has_cpu())
            for (uint64_t a : emu.cpu().history()) std::fprintf(stderr, "    %08llX\n", (unsigned long long)a);
        if (!shot_path.empty()) {
            g.present_now();
            write_png(shot_path, g.screen.data(), g.screen_w, g.screen_h);
        }
        return 1;
    }
}
