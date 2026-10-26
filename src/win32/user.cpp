// user32: windows, the message queue, input, painting, and composition.
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "font.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

void registry_flush();
RealFont* dc_realfont(Gui& g, uint32_t hdc);
bool dc_text_out(Gui& g, uint32_t hdc, int x, int y, uint32_t options, const Rect* rc,
                 const std::u16string& s);

// ---------------------------------------------------------------------------
// The Gui object
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<Emulator*, std::unique_ptr<Gui>>& gui_table() {
    static std::unordered_map<Emulator*, std::unique_ptr<Gui>> t;
    return t;
}

// A host that never has input: used until a front end installs one, and by
// tests that only drive the guest through its own startup.
class NullHost : public Host {
public:
    bool wait_event(HostEvent&, int) override { return false; }
    void present(const uint32_t*, int, int, const Rect&) override {}
    uint64_t now_ms() override {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
};

const char* msg_name(uint32_t m) {
    switch (m) {
        case WM_CREATE: return "WM_CREATE";
        case WM_DESTROY: return "WM_DESTROY";
        case WM_MOVE: return "WM_MOVE";
        case WM_SIZE: return "WM_SIZE";
        case WM_ACTIVATE: return "WM_ACTIVATE";
        case WM_SETFOCUS: return "WM_SETFOCUS";
        case WM_KILLFOCUS: return "WM_KILLFOCUS";
        case WM_PAINT: return "WM_PAINT";
        case WM_ERASEBKGND: return "WM_ERASEBKGND";
        case WM_SHOWWINDOW: return "WM_SHOWWINDOW";
        case WM_SETTEXT: return "WM_SETTEXT";
        case WM_NCCREATE: return "WM_NCCREATE";
        case WM_NCCALCSIZE: return "WM_NCCALCSIZE";
        case WM_NCPAINT: return "WM_NCPAINT";
        case WM_COMMAND: return "WM_COMMAND";
        case WM_NOTIFY: return "WM_NOTIFY";
        case WM_TIMER: return "WM_TIMER";
        case WM_INITDIALOG: return "WM_INITDIALOG";
        case WM_WINDOWPOSCHANGED: return "WM_WINDOWPOSCHANGED";
        case WM_WINDOWPOSCHANGING: return "WM_WINDOWPOSCHANGING";
        case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
        case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
        case WM_LBUTTONUP: return "WM_LBUTTONUP";
        case WM_KEYDOWN: return "WM_KEYDOWN";
        case WM_CHAR: return "WM_CHAR";
        case WM_SETFONT: return "WM_SETFONT";
        case WM_GETFONT: return "WM_GETFONT";
        case WM_INITMENUPOPUP: return "WM_INITMENUPOPUP";
        case WM_SETCURSOR: return "WM_SETCURSOR";
        case WM_NCHITTEST: return "WM_NCHITTEST";
        case WM_GETMINMAXINFO: return "WM_GETMINMAXINFO";
        case WM_PARENTNOTIFY: return "WM_PARENTNOTIFY";
        case 0x0363: return "WM_IDLEUPDATECMDUI";
        case 0x036A: return "WM_KICKIDLE";
    }
    return nullptr;
}

}  // namespace

Gui& gui(Emulator& e) {
    auto& t = gui_table();
    auto it = t.find(&e);
    if (it != t.end()) return *it->second;
    auto g = std::make_unique<Gui>(e);
    Gui& ref = *g;
    t[&e] = std::move(g);
    return ref;
}

Gui::Gui(Emulator& e) : emu(e) {
    owned_host = std::make_unique<NullHost>();
    host = owned_host.get();
    trace_msgs = std::getenv("JWEMU_TRACE_MSG") != nullptr;
    screen.assign(static_cast<size_t>(screen_w) * screen_h, 0x3A6EA5);
    // The desktop window: the root of the tree, the size of the screen.
    auto d = std::make_unique<Window>();
    d->hwnd = new_hwnd();
    d->style = WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    d->wrect = d->crect = {0, 0, screen_w, screen_h};
    desktop = d->hwnd;
    windows[d->hwnd] = std::move(d);
}

Gui::~Gui() = default;

uint32_t Gui::new_hwnd() {
    uint32_t h = next_hwnd_;
    next_hwnd_ += 2;
    return h;
}

uint32_t Gui::msg_time() const { return static_cast<uint32_t>(host->now_ms()); }

uint32_t Gui::add_atom(const std::u16string& s) {
    std::u16string up = upper(s);
    auto it = atoms.find(up);
    if (it != atoms.end()) return it->second;
    uint32_t a = next_atom++;
    atoms[up] = a;
    atom_names[a] = s;
    return a;
}

uint64_t Gui::scratch_alloc(size_t bytes) {
    uint64_t p = emu.heap_alloc(bytes);
    std::vector<uint8_t> z(bytes, 0);
    emu.mem.write(p, z.data(), z.size());
    return p;
}
void Gui::scratch_free(uint64_t addr) {
    if (addr) emu.heap_free(addr);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

Window* Gui::toplevel_of(Window* w) const {
    while (w && w->parent && w->parent != desktop) w = win(w->parent);
    return w && w->hwnd != desktop ? w : nullptr;
}

static Point client_origin_screen(const Gui& g, const Window* w) {
    Point p{0, 0};
    while (w && w->hwnd != g.desktop) {
        p.x += w->crect.l;
        p.y += w->crect.t;
        w = g.win(w->parent);
    }
    return p;
}

Point Gui::client_to_screen(const Window* w, Point p) const {
    Point o = client_origin_screen(*this, w);
    return {p.x + o.x, p.y + o.y};
}

Rect Gui::window_rect_screen(const Window* w) const {
    if (!w) return {};
    if (w->hwnd == desktop) return w->wrect;
    Point o = client_origin_screen(*this, win(w->parent));
    return w->wrect.offset(o.x, o.y);
}

Rect Gui::client_rect_screen(const Window* w) const {
    if (!w) return {};
    if (w->hwnd == desktop) return w->crect;
    Point o = client_origin_screen(*this, win(w->parent));
    return w->crect.offset(o.x, o.y);
}

Point Gui::window_to_screen(const Window* w, Point p) const {
    Rect r = window_rect_screen(w);
    return {p.x + r.l, p.y + r.t};
}

bool Gui::is_visible(const Window* w) const {
    while (w) {
        if (!(w->style & WS_VISIBLE)) return false;
        if (w->hwnd == desktop) return true;
        w = win(w->parent);
    }
    return false;
}

Point Gui::surface_origin(const Window* w, bool client) const {
    Window* top = toplevel_of(const_cast<Window*>(w));
    if (!top) return {0, 0};
    Rect tr = window_rect_screen(top);
    Rect r = client ? client_rect_screen(w) : window_rect_screen(w);
    return {r.l - tr.l, r.t - tr.t};
}

Region Gui::visible_region(const Window* w, bool client) const {
    Window* top = toplevel_of(const_cast<Window*>(w));
    Region out;
    if (!top || !is_visible(w)) return out;
    Rect tr = window_rect_screen(top);
    auto to_surf = [&](Rect r) { return r.offset(-tr.l, -tr.t); };
    Rect base = client ? client_rect_screen(w) : window_rect_screen(w);
    out.set_rect(to_surf(base));
    // Every ancestor's client area bounds what a descendant can touch.
    const Window* c = w;
    while (c != top) {
        const Window* p = win(c->parent);
        if (!p) break;
        out.intersect(to_surf(client_rect_screen(p)));
        // Siblings above, for a window that clips them.
        if (c->style & WS_CLIPSIBLINGS) {
            for (uint32_t sh : p->children) {
                if (sh == c->hwnd) break;
                const Window* s = win(sh);
                if (s && (s->style & WS_VISIBLE)) out.subtract(to_surf(window_rect_screen(s)));
            }
        }
        c = p;
    }
    if (w->style & WS_CLIPCHILDREN) {
        // Only what a child can show: the part inside this window's client
        // area.  A child poking out over the frame (MFC puts toolbars two
        // pixels into their bar's border) must not stop the frame painting.
        Rect cr = client_rect_screen(w);
        for (uint32_t ch : w->children) {
            const Window* k = win(ch);
            if (k && (k->style & WS_VISIBLE)) out.subtract(to_surf(window_rect_screen(k).intersect(cr)));
        }
    }
    return out;
}

uint32_t Gui::child_from_point(Window* parent, Point cp, bool all) {
    for (uint32_t ch : parent->children) {
        Window* c = win(ch);
        if (!c || !(c->style & WS_VISIBLE)) continue;
        if (!all && (c->style & WS_DISABLED)) continue;
        if (c->wrect.contains(cp.x, cp.y)) return ch;
    }
    return 0;
}

uint32_t Gui::window_from_point(Point sp, bool skip_disabled) {
    Window* d = win(desktop);
    uint32_t hit = 0;
    for (uint32_t th : d->children) {
        Window* t = win(th);
        if (!t || !(t->style & WS_VISIBLE)) continue;
        if (t->wrect.contains(sp.x, sp.y)) {
            hit = th;
            break;
        }
    }
    if (!hit) return 0;
    // Descend into the deepest visible child under the point.
    while (true) {
        Window* w = win(hit);
        Rect cr = client_rect_screen(w);
        if (!cr.contains(sp.x, sp.y)) break;
        Point o = client_origin_screen(*this, w);
        uint32_t ch = 0;
        for (uint32_t c : w->children) {
            Window* k = win(c);
            if (!k || !(k->style & WS_VISIBLE)) continue;
            if (!k->wrect.contains(sp.x - o.x, sp.y - o.y)) continue;
            if (skip_disabled && (k->style & WS_DISABLED)) continue;
            ch = c;
            break;
        }
        if (!ch) break;
        hit = ch;
    }
    return hit;
}

// ---------------------------------------------------------------------------
// Surfaces and the screen
// ---------------------------------------------------------------------------

void Gui::ensure_surface(Window* top) {
    int w = std::max(1, top->wrect.w()), h = std::max(1, top->wrect.h());
    if (top->surface && top->surf_w == w && top->surf_h == h) return;
    auto s = std::make_unique<std::vector<uint32_t>>(static_cast<size_t>(w) * h, sys_color(15));
    if (top->surface) {
        int cw = std::min(w, top->surf_w), ch = std::min(h, top->surf_h);
        for (int y = 0; y < ch; ++y)
            std::memcpy(s->data() + static_cast<size_t>(y) * w, top->surface->data() + static_cast<size_t>(y) * top->surf_w,
                        static_cast<size_t>(cw) * 4);
    }
    top->surface = std::move(s);
    top->surf_w = w;
    top->surf_h = h;
}

// A top-level window the way Windows 11's DWM shows it on this machine,
// measured on the screen next to the real Jw_cad: the outer 7 pixels on the
// left, right and bottom are an invisible resize border, the corners are
// square, the 1-pixel border darkens what lies behind by a third (#ABABAB
// over white), and a soft shadow falls round the window - about 18 % at the
// sides fading over 16 pixels, and a longer, stronger one below (31 %,
// still 20 % twenty pixels down).  An inactive window's are lighter.
void Gui::compose_dwm(Window* t) {
    const int inv = kDwmInvisible;
    int W = std::min(t->wrect.w(), t->surf_w), H = std::min(t->wrect.h(), t->surf_h);
    int vb = H - inv;  // rows [0, vb) are visible
    bool act = t->nc_active >= 0 ? t->nc_active != 0 : active == t->hwnd;
    auto darken = [](uint32_t c, int a256) {
        auto ch = [&](int s) { return ((((c >> s) & 0xFF) * static_cast<uint32_t>(256 - a256)) >> 8) << s; };
        return ch(0) | ch(8) | ch(16);
    };
    // The shadow, in 1/256ths of darkening, by distance from the border.
    constexpr int kSide = 40, kTop = 30, kBelow = 70;
    static int side[kSide + 1], top[kTop + 1], below[kBelow + 1], fade16[kBelow + 1], fade40[kBelow + 1];
    static bool tables = false;
    if (!tables) {
        for (int d = 1; d <= kSide; ++d) side[d] = static_cast<int>(256 * 0.18 * std::exp(-(d - 1) / 16.0));
        for (int d = 1; d <= kTop; ++d) top[d] = static_cast<int>(256 * 0.10 * std::exp(-(d - 1) / 10.0));
        for (int d = 1; d <= kBelow; ++d) below[d] = static_cast<int>(256 * 0.31 * std::exp(-(d - 1) / 40.0));
        for (int d = 0; d <= kBelow; ++d) {
            fade16[d] = static_cast<int>(256 * std::exp(-d / 16.0));
            fade40[d] = static_cast<int>(256 * std::exp(-d / 40.0));
        }
        tables = true;
    }
    int vl = t->wrect.l + inv, vr = t->wrect.l + W - inv, vt = t->wrect.t, vbot = t->wrect.t + vb;  // [vl,vr) x [vt,vbot)
    int strength = act ? 256 : 140;
    for (int y = std::max(0, vt - kTop); y < std::min(screen_h, vbot + kBelow); ++y) {
        int dyt = y < vt ? vt - y : 0, dyb = y >= vbot ? y - vbot + 1 : 0;
        uint32_t* dst = screen.data() + static_cast<size_t>(y) * screen_w;
        for (int x = std::max(0, vl - kSide); x < std::min(screen_w, vr + kSide); ++x) {
            int dx = x < vl ? vl - x : (x >= vr ? x - vr + 1 : 0);
            if (!dx && !dyt && !dyb) {
                x = vr - 1;  // inside: the window itself goes here below
                continue;
            }
            int v;
            if (dyb) v = std::max(below[dyb] * fade16[std::min(dx, kBelow)] >> 8, dx ? side[dx] * fade40[dyb] >> 8 : 0);
            else if (dyt) v = std::max(top[dyt] * fade16[std::min(dx, kBelow)] >> 8, dx ? side[dx] * fade16[dyt] >> 8 : 0);
            else v = side[dx];
            v = v * strength >> 8;
            if (v > 0) dst[x] = darken(dst[x], v);
        }
    }
    int border = act ? 85 : 55;
    for (int sy = 0; sy < vb; ++sy) {
        int y = t->wrect.t + sy;
        if (y < 0 || y >= screen_h) continue;
        uint32_t* dst = screen.data() + static_cast<size_t>(y) * screen_w;
        const uint32_t* src = t->surface->data() + static_cast<size_t>(sy) * t->surf_w;
        bool edge_row = sy == 0 || sy == vb - 1;
        for (int x = inv; x < W - inv; ++x) {
            int sx = t->wrect.l + x;
            if (sx < 0 || sx >= screen_w) continue;
            bool edge = edge_row || x == inv || x == W - inv - 1;
            dst[sx] = edge ? darken(dst[sx], border) : src[x];
        }
    }
}

void Gui::compose(bool) {
    std::fill(screen.begin(), screen.end(), 0x3A6EA5);
    Window* d = win(desktop);
    for (auto it = d->children.rbegin(); it != d->children.rend(); ++it) {
        Window* t = win(*it);
        if (!t || !(t->style & WS_VISIBLE) || !t->surface) continue;
        // A window that took its whole rectangle as client area (Winamp's
        // skinned ones, through WM_NCCALCSIZE) has no frame for DWM to draw
        // round, and no shadow.
        bool frameless = t->crect.l == t->wrect.l && t->crect.t == t->wrect.t &&
                         t->crect.r == t->wrect.r && t->crect.b == t->wrect.b;
        if (dwm_frame(t->style, t->exstyle) && !t->maximized && !frameless) {
            compose_dwm(t);
            continue;
        }
        Rect r = t->wrect.intersect({0, 0, screen_w, screen_h});
        for (int y = r.t; y < r.b; ++y) {
            int sy = y - t->wrect.t;
            if (sy < 0 || sy >= t->surf_h) continue;
            int x0 = r.l, x1 = std::min(r.r, t->wrect.l + t->surf_w);
            if (x1 <= x0) continue;
            std::memcpy(screen.data() + static_cast<size_t>(y) * screen_w + x0,
                        t->surface->data() + static_cast<size_t>(sy) * t->surf_w + (x0 - t->wrect.l),
                        static_cast<size_t>(x1 - x0) * 4);
        }
    }
    // The caret, drawn on top of whatever was composed.
    caret_draw(*this, true);
}

void Gui::present_now() {
    compose(true);
    host->present(screen.data(), screen_w, screen_h, {0, 0, screen_w, screen_h});
    last_present = host->now_ms();
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

void Gui::invalidate(Window* w, const Region* rgn, bool erase, bool children, bool frame) {
    if (!w) return;
    if (w->hwnd == desktop) {
        for (uint32_t ch : w->children) invalidate(win(ch), nullptr, erase, true, true);
        return;
    }
    Rect cr{0, 0, w->crect.w(), w->crect.h()};
    Region add = rgn ? *rgn : Region(cr);
    add.intersect(cr);
    if (!add.empty()) {
        w->update.unite(add);
        if (erase) w->need_erase = true;
    }
    if (frame) w->need_ncpaint = true;
    if (children && !(w->style & WS_CLIPCHILDREN || false)) {
        // Children inside the invalidated area repaint too.
    }
    if (children) {
        for (uint32_t ch : w->children) {
            Window* c = win(ch);
            if (!c || !(c->style & WS_VISIBLE)) continue;
            Rect cw = c->wrect;  // parent-client coordinates
            if (rgn && !rgn->intersects(cw)) continue;
            Region sub;
            if (rgn) {
                sub = *rgn;
                sub.offset(-c->crect.l, -c->crect.t);
            }
            invalidate(c, rgn ? &sub : nullptr, erase, true, true);
        }
    }
}

void Gui::invalidate_rect_screen(const Rect& r) {
    // Expose a screen area: every visible top-level under it repaints there.
    Window* d = win(desktop);
    for (uint32_t th : d->children) {
        Window* t = win(th);
        if (!t || !(t->style & WS_VISIBLE)) continue;
        Rect cr = client_rect_screen(t);
        Rect x = r.intersect(cr);
        if (x.empty()) continue;
        Region rg(x.offset(-cr.l, -cr.t));
        invalidate(t, &rg, true, true, true);
    }
}

void Gui::validate(Window* w, const Region* rgn) {
    if (!w) return;
    if (!rgn) {
        w->update.clear();
        w->need_erase = false;
        w->internal_paint = false;
    } else {
        w->update.subtract(*rgn);
        if (w->update.empty()) w->need_erase = false;
    }
}

Window* Gui::find_paint_window(uint32_t root, uint32_t filter) {
    Window* w = win(root);
    if (!w || !(w->style & WS_VISIBLE)) return nullptr;
    bool want = (!w->update.empty() || w->internal_paint || w->need_ncpaint) && w->hwnd != desktop;
    if (want && (!filter || filter == w->hwnd || is_child_of(*this, w->hwnd, filter)))
        return w;
    for (uint32_t ch : w->children) {
        if (Window* p = find_paint_window(ch, filter)) return p;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Sending, posting, hooks
// ---------------------------------------------------------------------------

uint64_t Gui::call_wndproc(uint64_t proc, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    if (!proc) return 0;
    return emu.call_guest(proc, {hwnd, msg, wp, lp}) & 0xFFFFFFFFull;
}

uint64_t Gui::send(uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = win(hwnd);
    if (!w) return 0;
    if (trace_msgs) {
        const char* n = msg_name(msg);
        if (n) std::fprintf(stderr, "[msg] %08X %s %llX %llX\n", hwnd, n, (unsigned long long)wp, (unsigned long long)lp);
        else std::fprintf(stderr, "[msg] %08X 0x%04X %llX %llX\n", hwnd, msg, (unsigned long long)wp, (unsigned long long)lp);
    }
    uint64_t r = call_wndproc(w->wndproc, hwnd, msg, wp, lp);
    return r;
}

void Gui::post(uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Msg m;
    m.hwnd = hwnd;
    m.message = msg;
    m.wparam = wp;
    m.lparam = lp;
    m.time = msg_time();
    m.pt = cursor_pos;
    posted.push_back(m);
}

uint64_t Gui::call_hook(int type, int code, uint64_t wp, uint64_t lp) {
    for (auto it = hooks.rbegin(); it != hooks.rend(); ++it) {
        if (it->type != type) continue;
        return emu.call_guest(it->proc, {static_cast<uint64_t>(static_cast<uint32_t>(code)), wp, lp}) & 0xFFFFFFFFull;
    }
    return 0;
}

void Gui::write_msg(uint64_t addr, const Msg& m) {
    emu.mem.write32(addr, m.hwnd);
    emu.mem.write32(addr + 4, m.message);
    emu.mem.write32(addr + 8, static_cast<uint32_t>(m.wparam));
    emu.mem.write32(addr + 12, static_cast<uint32_t>(m.lparam));
    emu.mem.write32(addr + 16, m.time);
    emu.mem.write32(addr + 20, static_cast<uint32_t>(m.pt.x));
    emu.mem.write32(addr + 24, static_cast<uint32_t>(m.pt.y));
}

Msg Gui::read_msg(uint64_t addr) {
    Msg m;
    m.hwnd = emu.mem.read32(addr);
    m.message = emu.mem.read32(addr + 4);
    m.wparam = emu.mem.read32(addr + 8);
    m.lparam = emu.mem.read32(addr + 12);
    m.time = emu.mem.read32(addr + 16);
    m.pt = {static_cast<int32_t>(emu.mem.read32(addr + 20)), static_cast<int32_t>(emu.mem.read32(addr + 24))};
    return m;
}

static bool msg_matches(const Gui& g, const Msg& m, uint32_t hwnd, uint32_t min, uint32_t max) {
    if (hwnd && hwnd != 0xFFFFFFFF && m.hwnd != hwnd && !is_child_of(const_cast<Gui&>(g), m.hwnd, hwnd)) return false;
    if (hwnd == 0xFFFFFFFF && m.hwnd != 0) return false;
    if (min == 0 && max == 0) return true;
    return m.message >= min && m.message <= max;
}

bool Gui::peek_posted(Msg& out, uint32_t hwnd, uint32_t min, uint32_t max, bool remove) {
    for (size_t i = 0; i < posted.size(); ++i) {
        if (!msg_matches(*this, posted[i], hwnd, min, max)) continue;
        out = posted[i];
        if (remove) posted.erase(posted.begin() + static_cast<ptrdiff_t>(i));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static uint32_t key_flags(const Gui& g) {
    uint32_t f = 0;
    if (g.key_state[0x01] & 0x80) f |= 0x0001;  // MK_LBUTTON
    if (g.key_state[0x02] & 0x80) f |= 0x0002;  // MK_RBUTTON
    if (g.key_state[0x10] & 0x80) f |= 0x0004;  // MK_SHIFT
    if (g.key_state[0x11] & 0x80) f |= 0x0008;  // MK_CONTROL
    if (g.key_state[0x04] & 0x80) f |= 0x0010;  // MK_MBUTTON
    return f;
}

void Gui::idle_wait(int ms) {
    uint64_t now = host->now_ms();
    if (now - last_present > 33) present_now();
    int timeout = ms;
    for (auto& t : timers) {
        int left = t.due > now ? static_cast<int>(t.due - now) : 0;
        if (timeout < 0 || left < timeout) timeout = left;
    }
    pump_host(timeout);
}

bool Gui::message_ready() {
    pump_host(0);
    if (!posted.empty() || !input.empty() || quit_posted) return true;
    uint64_t now = host->now_ms();
    if (now - last_present > 33) present_now();
    for (auto& t : timers)
        if (t.due <= now) return true;
    return find_paint_window(desktop, 0) != nullptr;
}

void Gui::pump_host(int timeout_ms) {
    HostEvent ev;
    bool first = true;
    while (host->wait_event(ev, first ? timeout_ms : 0)) {
        first = false;
        switch (ev.type) {
            case HostEvent::MouseDown:
            case HostEvent::MouseUp: {
                static const int vk[3] = {0x01, 0x02, 0x04};
                async_down[vk[ev.button % 3]] = ev.type == HostEvent::MouseDown;
                break;
            }
            case HostEvent::KeyDown:
                async_down[ev.key & 0xFF] = true;
                break;
            case HostEvent::KeyUp:
                async_down[ev.key & 0xFF] = false;
                break;
            case HostEvent::Quit:
                quit_posted = true;
                break;
            default:
                break;
        }
        input.push_back(ev);
    }
}

// Turns one host event into a window message.  Returns false if it produced
// nothing (or was consumed here: a resize, a click on a disabled window).
bool Gui::translate_input(const HostEvent& ev, Msg& out) {
    out = Msg{};
    out.time = msg_time();
    if (ev.type == HostEvent::Resize) {
        if (ev.w > 0 && ev.h > 0 && (ev.w != screen_w || ev.h != screen_h)) {
            screen_w = ev.w;
            screen_h = ev.h;
            screen.assign(static_cast<size_t>(screen_w) * screen_h, 0);
            Window* d = win(desktop);
            d->wrect = d->crect = {0, 0, screen_w, screen_h};
            ++geom_ver;
            // Maximised windows follow the screen.
            for (uint32_t th : d->children) {
                Window* t = win(th);
                if (t && t->maximized) {
                    Rect r = nc_insets(t->style, t->exstyle, false, 0);
                    set_window_pos(th, 0, -r.l, -r.t, screen_w + r.l + r.r, screen_h + r.t + r.b,
                                   SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
        }
        return false;
    }
    if (ev.type == HostEvent::Quit) return false;
    if (ev.type == HostEvent::MouseMove || ev.type == HostEvent::MouseDown ||
        ev.type == HostEvent::MouseUp || ev.type == HostEvent::Wheel) {
        cursor_pos = {ev.x, ev.y};
        out.pt = cursor_pos;
        static const int vk[3] = {0x01, 0x02, 0x04};
        if (ev.type == HostEvent::MouseDown) key_state[vk[ev.button % 3]] = 0x80 | (key_state[vk[ev.button % 3]] ^ 1);
        if (ev.type == HostEvent::MouseUp) key_state[vk[ev.button % 3]] &= 0x7F;
        key_state[0x10] = (ev.mods & 1) ? 0x80 : 0;
        key_state[0x11] = (ev.mods & 2) ? 0x80 : 0;
        key_state[0x12] = (ev.mods & 4) ? 0x80 : 0;
        if (ev.type == HostEvent::Wheel) {
            uint32_t target = window_from_point(cursor_pos, true);
            if (!target) target = focus;
            if (!target) return false;
            out.hwnd = target;
            out.message = WM_MOUSEWHEEL;
            out.wparam = (static_cast<uint32_t>(static_cast<uint16_t>(ev.delta)) << 16) | key_flags(*this);
            out.lparam = make_lparam(ev.x, ev.y);
            return true;
        }
        uint32_t target = capture;
        int ht = HTCLIENT;
        if (!target) {
            target = window_from_point(cursor_pos, false);
            if (!target) {
                nc_set_hot(*this, 0, 0, false);
                return false;
            }
            // A disabled window gets nothing; the modal dialog it owns flashes.
            Window* w = win(target);
            while (w && (w->style & WS_DISABLED) && (w->style & WS_CHILD)) {
                target = w->parent;
                w = win(target);
            }
            if (w && (w->style & WS_DISABLED)) {
                return false;
            }
            ht = static_cast<int16_t>(send(target, WM_NCHITTEST, 0, make_lparam(ev.x, ev.y)));
            while (ht == HTTRANSPARENT) {
                Window* tw = win(target);
                if (!tw || !(tw->style & WS_CHILD)) break;
                target = tw->parent;
                ht = static_cast<int16_t>(send(target, WM_NCHITTEST, 0, make_lparam(ev.x, ev.y)));
            }
        }
        Window* tw = win(target);
        if (!tw) return false;
        uint32_t base = 0;
        if (ev.type == HostEvent::MouseMove) {
            base = WM_MOUSEMOVE;
        } else {
            static const uint32_t down[3] = {WM_LBUTTONDOWN, WM_RBUTTONDOWN, WM_MBUTTONDOWN};
            static const uint32_t up[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
            base = ev.type == HostEvent::MouseDown ? down[ev.button % 3] : up[ev.button % 3];
        }
        if (ev.type == HostEvent::MouseDown) {
            // Activation: a click on an inactive top-level brings it forward.
            Window* top = toplevel_of(tw);
            if (top && top->hwnd != active && !capture) {
                uint64_t ma = send(target, WM_MOUSEACTIVATE, top->hwnd, make_lparam(ht, base));
                if (ma != 3 && ma != 4) set_active(top->hwnd, true);  // MA_NOACTIVATE(ANDEAT)
                if (ma == 2 || ma == 4) return false;  // MA_ACTIVATEANDEAT / NOACTIVATEANDEAT
            }
            // Double clicks, for classes that ask for them.
            uint64_t now = host->now_ms();
            bool dbl = last_click_button == ev.button && last_click_hwnd == target &&
                       now - last_click_time <= 500 && std::abs(ev.x - last_click_pt.x) <= 4 &&
                       std::abs(ev.y - last_click_pt.y) <= 4;
            bool wants = tw->cls && (tw->cls->style & 0x0008);  // CS_DBLCLKS
            if (ht != HTCLIENT) wants = true;  // non-client double clicks always exist
            if (dbl && wants) {
                base += 2;  // xBUTTONDBLCLK
                last_click_button = -1;
            } else {
                last_click_button = ev.button;
                last_click_time = now;
                last_click_pt = {ev.x, ev.y};
                last_click_hwnd = target;
            }
        }
        if (!capture) {
            send(target, WM_SETCURSOR, target, make_lparam(ht, base));
        }
        out.hwnd = target;
        if (ht == HTCLIENT || capture) {
            Point o = client_origin_screen(*this, tw);
            out.message = base;
            out.wparam = key_flags(*this);
            out.lparam = make_lparam(ev.x - o.x, ev.y - o.y);
        } else {
            out.message = base - WM_MOUSEMOVE + WM_NCMOUSEMOVE;
            out.wparam = static_cast<uint32_t>(ht);
            out.lparam = make_lparam(ev.x, ev.y);
        }
        if (ev.type == HostEvent::MouseMove && !capture) nc_set_hot(*this, target, ht, false);
        hover_hwnd = target;
        return true;
    }
    if (ev.type == HostEvent::KeyDown || ev.type == HostEvent::KeyUp) {
        int vk = ev.key & 0xFF;
        bool down = ev.type == HostEvent::KeyDown;
        bool was = (key_state[vk] & 0x80) != 0;
        if (down) key_state[vk] = static_cast<uint8_t>(0x80 | ((key_state[vk] & 1) ^ (was ? 0 : 1)));
        else key_state[vk] &= 0x7F;
        if (vk == 0x10 || vk == 0xA0 || vk == 0xA1) key_state[0x10] = down ? 0x80 : 0;
        if (vk == 0x11 || vk == 0xA2 || vk == 0xA3) key_state[0x11] = down ? 0x80 : 0;
        if (vk == 0x12 || vk == 0xA4 || vk == 0xA5) key_state[0x12] = down ? 0x80 : 0;
        if (down && (vk == 0x12 || vk == 0xA4 || vk == 0xA5 || vk == 0x79)) menu_keyboard = true;  // keyboard cues from now on
        bool alt = (key_state[0x12] & 0x80) != 0;
        uint32_t target = focus ? focus : active;
        if (!target) return false;
        bool sys = alt || vk == 0x79;  // F10
        if (alt && (key_state[0x11] & 0x80)) sys = false;
        out.hwnd = target;
        out.message = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
        out.wparam = static_cast<uint32_t>(vk);
        static const uint8_t scan[256] = {
            0, 0, 0, 0x46, 0, 0, 0, 0, 0x0E, 0x0F, 0, 0, 0x4C, 0x1C, 0, 0,
            0x2A, 0x1D, 0x38, 0, 0x3A, 0x70, 0, 0, 0, 0x29, 0, 0x01, 0x79, 0x7B, 0, 0,
            0x39, 0x49, 0x51, 0x4F, 0x47, 0x4B, 0x48, 0x4D, 0x50, 0, 0, 0, 0x54, 0x52, 0x53, 0,
            0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0, 0, 0, 0, 0, 0,
            0, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
            0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, 0x5B, 0x5C, 0x5D, 0, 0,
            0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x37, 0x4E, 0, 0x4A, 0x53, 0x35,
            0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
        };
        uint32_t sc = scan[vk];
        uint32_t lp = 1 | (sc << 16);
        if (vk >= 0x21 && vk <= 0x2E) lp |= 1u << 24;  // extended keys
        if (alt) lp |= 1u << 29;
        if (was) lp |= 1u << 30;
        if (!down) lp |= (1u << 30) | (1u << 31);
        out.lparam = lp;
        out.pt = cursor_pos;
        return true;
    }
    if (ev.type == HostEvent::Char) {
        uint32_t target = focus ? focus : active;
        if (!target) return false;
        out.hwnd = target;
        out.message = (key_state[0x12] & 0x80) && !(key_state[0x11] & 0x80) ? WM_SYSCHAR : WM_CHAR;
        out.wparam = ev.ch;
        out.lparam = 1;
        out.pt = cursor_pos;
        if (ev.ch > 0xFFFF) {
            // A surrogate pair is two WM_CHARs.
            uint32_t c = ev.ch - 0x10000;
            out.wparam = 0xD800 + (c >> 10);
            Msg second = out;
            second.wparam = 0xDC00 + (c & 0x3FF);
            posted.insert(posted.begin(), second);
        }
        return true;
    }
    return false;
}

bool Gui::get_message(Msg& out, uint32_t hwnd, uint32_t min, uint32_t max, bool remove, bool wait) {
    while (true) {
        // 1. posted messages
        if (peek_posted(out, hwnd, min, max, remove)) return true;
        // 2. input
        pump_host(0);
        bool want_input = min == 0 && max == 0;
        if (!want_input) {
            want_input = (min <= WM_MOUSEMOVE + 0x0F && max >= WM_MOUSEMOVE) ||
                         (min <= WM_KEYUP + 6 && max >= WM_KEYDOWN) ||
                         (min <= WM_NCMBUTTONUP && max >= WM_NCMOUSEMOVE);
        }
        while (want_input && !input.empty()) {
            HostEvent ev = input.front();
            if (!remove) {
                // Peeking at input means translating it without consuming the
                // event.  Mouse moves are cheap to re-translate; the rest are
                // left in place and translated for real on the removing call.
                Msg m;
                if (translate_input(ev, m)) {
                    input.erase(input.begin());
                    if (msg_matches(*this, m, hwnd, min, max)) {
                        posted.insert(posted.begin(), m);
                        out = m;
                        return true;
                    }
                    posted.push_back(m);
                    continue;
                }
                input.erase(input.begin());
                continue;
            }
            input.erase(input.begin());
            Msg m;
            if (!translate_input(ev, m)) continue;
            if (msg_matches(*this, m, hwnd, min, max)) {
                out = m;
                return true;
            }
            posted.push_back(m);
        }
        if (peek_posted(out, hwnd, min, max, remove)) return true;
        // 3. quit
        if (quit_posted && (min == 0 && max == 0)) {
            out = Msg{};
            out.message = WM_QUIT;
            out.wparam = static_cast<uint32_t>(quit_code);
            if (remove) quit_posted = false;
            return true;
        }
        // 4. paint
        if (min == 0 && max == 0 || (min <= WM_PAINT && max >= WM_PAINT)) {
            if (Window* p = find_paint_window(desktop, hwnd == 0xFFFFFFFF ? 0 : hwnd)) {
                if (p->need_ncpaint && p->update.empty() && !p->internal_paint) {
                    // Only the frame needs drawing; do it here, it has no client message.
                    p->need_ncpaint = false;
                    send(p->hwnd, WM_NCPAINT, 1, 0);
                    continue;
                }
                out = Msg{};
                out.hwnd = p->hwnd;
                out.message = WM_PAINT;
                out.time = msg_time();
                out.pt = cursor_pos;
                return true;
            }
        }
        // 5. timers
        uint64_t now = host->now_ms();
        uint64_t next_due = 0;
        for (auto& t : timers) {
            if (hwnd && hwnd != 0xFFFFFFFF && t.hwnd != hwnd && !is_child_of(*this, t.hwnd, hwnd)) continue;
            if (!(min == 0 && max == 0) && !(min <= WM_TIMER && max >= WM_TIMER)) continue;
            if (t.due <= now) {
                out = Msg{};
                out.hwnd = t.hwnd;
                out.message = WM_TIMER;
                out.wparam = t.id;
                out.lparam = t.proc;
                out.time = static_cast<uint32_t>(now);
                out.pt = cursor_pos;
                if (remove) t.due = now + std::max<uint32_t>(t.interval, 10);
                return true;
            }
            if (!next_due || t.due < next_due) next_due = t.due;
        }
        if (!wait) {
            if (now - last_present > 33) present_now();
            return false;
        }
        // Nothing to do: show the screen and wait for the outside world.
        present_now();
        if (std::getenv("JWEMU_EXIT_AT_IDLE")) {
            // Measuring startup: how long until the program first waits for input.
            static uint64_t t0 = host->now_ms();
            std::fprintf(stderr, "[jwemu] idle after %llu instructions\n",
                         (unsigned long long)emu.cpu().instructions_executed);
            (void)t0;
            std::exit(0);
        }
        registry_flush();
        if (emu.threads().size() > 1) {
            // Other threads have work (decoding, playing): run them until
            // something arrives for this one.
            emu.run_others_while([this] { return !message_ready(); });
            continue;
        }
        int timeout = -1;
        if (next_due) timeout = static_cast<int>(next_due > now ? next_due - now : 0);
        pump_host(timeout);
        if (input.empty() && !next_due && !quit_posted && posted.empty()) {
            // A host that has nothing and never will: the program is waiting
            // for input that cannot come.  Report it rather than spin.
            HostEvent probe;
            if (!host->wait_event(probe, 50)) {
                static int idle = 0;
                if (++idle > 20 && std::getenv("JWEMU_EXIT_IDLE")) {
                    out = Msg{};
                    out.message = WM_QUIT;
                    return true;
                }
            } else {
                input.push_back(probe);
            }
        }
    }
}

void Gui::dispatch(const Msg& m) {
    if (m.message == WM_TIMER && m.lparam) {
        call_wndproc(m.lparam, m.hwnd, WM_TIMER, m.wparam, m.time);
        return;
    }
    Window* w = win(m.hwnd);
    if (!w) return;
    if (m.message == WM_PAINT) {
        bool had = !w->update.empty() || w->internal_paint || w->need_ncpaint;
        send(m.hwnd, WM_PAINT, 0, 0);
        // A window procedure that does not validate (no BeginPaint) would
        // otherwise get WM_PAINT forever; Windows validates for it.
        if (had && win(m.hwnd)) {
            Window* w2 = win(m.hwnd);
            if (!w2->update.empty() || w2->internal_paint) {
                w2->update.clear();
                w2->internal_paint = false;
                w2->need_erase = false;
            }
            w2->need_ncpaint = false;
        }
        return;
    }
    send(m.hwnd, m.message, m.wparam, m.lparam);
}

void Gui::modal_loop(const std::function<bool()>& done, uint32_t filter_hwnd) {
    (void)filter_hwnd;
    Msg m;
    while (!done()) {
        if (!get_message(m, 0, 0, 0, true, true)) continue;
        if (m.message == WM_QUIT) {
            // Re-post so the outer loop sees it.
            quit_posted = true;
            quit_code = static_cast<int>(m.wparam);
            break;
        }
        // Dialog keyboard handling for whichever dialog holds the focus.
        Window* f = win(m.hwnd);
        Window* dlg = nullptr;
        for (Window* p = f; p; p = win(p->parent)) {
            if (p->is_dialog) {
                dlg = p;
                break;
            }
            if (!(p->style & WS_CHILD)) break;
        }
        if (dlg && is_dialog_message(*this, dlg->hwnd, m)) continue;
        if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            // TranslateMessage
            int vk = static_cast<int>(m.wparam);
            uint32_t ch = 0;
            if (vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B) ch = static_cast<uint32_t>(vk);
            if (ch) post(m.hwnd, WM_CHAR, ch, m.lparam);
        }
        dispatch(m);
    }
}

// ---------------------------------------------------------------------------
// Window creation and placement
// ---------------------------------------------------------------------------

void Gui::dump_windows(std::FILE* f, uint32_t hwnd, int depth) const {
    auto it = windows.find(hwnd ? hwnd : desktop);
    if (it == windows.end()) return;
    const Window* w = it->second.get();
    Rect r = window_rect_screen(w);
    std::fprintf(f, "%*s%08X %-24s %4d,%4d %4dx%-4d st=%08X ex=%08X %s \"%s\"\n", depth * 2, "", w->hwnd,
                 w->cls ? to_utf8(w->cls->display_name).c_str() : "?", r.l, r.t, r.w(), r.h(), w->style, w->exstyle,
                 (w->style & WS_VISIBLE) ? "vis" : "hid", to_utf8(w->text).c_str());
    for (uint32_t c : w->children) dump_windows(f, c, depth + 1);
}

Rect Gui::nc_insets(uint32_t style, uint32_t exstyle, bool menu, int width) const {
    (void)width;
    Rect r;
    int b = frame_for_style(style, exstyle);
    if (exstyle & WS_EX_CLIENTEDGE) b += 2;
    if (exstyle & WS_EX_STATICEDGE) b += 1;
    r.l = r.r = r.b = b;
    r.t = b;
    if ((style & WS_CAPTION) == WS_CAPTION) r.t += (exstyle & WS_EX_TOOLWINDOW) ? kSmCaptionH : kCaptionH;
    if (menu) r.t += kMenuH;
    return r;
}

void Gui::recalc_client(Window* w) {
    Rect client = w->wrect;
    nc_calc(*this, w, client);
    w->crect = client;
}

void Gui::unlink(Window* w) {
    Window* p = win(w->parent);
    if (!p) return;
    auto& v = p->children;
    v.erase(std::remove(v.begin(), v.end(), w->hwnd), v.end());
}

void Gui::link(Window* w, uint32_t parent, uint32_t after) {
    Window* p = win(parent);
    if (!p) return;
    w->parent = parent;
    auto& v = p->children;
    // HWND_TOP 0, HWND_BOTTOM 1, HWND_TOPMOST -1, HWND_NOTOPMOST -2
    if (after == 1) {
        v.push_back(w->hwnd);
    } else if (after == 0 || after == 0xFFFFFFFF || after == 0xFFFFFFFE) {
        // Top-most windows stay above the rest.
        size_t pos = 0;
        if (!(w->exstyle & WS_EX_TOPMOST))
            while (pos < v.size() && win(v[pos]) && (win(v[pos])->exstyle & WS_EX_TOPMOST)) ++pos;
        v.insert(v.begin() + static_cast<ptrdiff_t>(pos), w->hwnd);
    } else {
        auto it = std::find(v.begin(), v.end(), after);
        if (it == v.end()) v.insert(v.begin(), w->hwnd);
        else v.insert(it + 1, w->hwnd);
    }
}

uint32_t Gui::create_window(uint32_t exstyle, const std::u16string& cls_name, uint32_t atom,
                            const std::u16string& title, uint32_t style, int x, int y, int w,
                            int h, uint32_t parent, uint64_t menu_or_id, uint64_t hinst,
                            uint64_t param, bool ansi) {
    WndClass* cls = atom ? find_class_atom(atom) : find_class(cls_name, hinst);
    if (!cls) {
        emu.set_last_error(1407);  // ERROR_CANNOT_FIND_WND_CLASS
        if (trace_msgs)
            std::fprintf(stderr, "[gui] CreateWindow: no class %s\n", to_utf8(cls_name).c_str());
        return 0;
    }
    constexpr int kUseDefault = static_cast<int>(0x80000000);
    bool child = (style & WS_CHILD) != 0;
    uint32_t owner = 0;
    if (!child) {
        // A top-level window's "parent" argument is its owner (the top-level
        // window that contains it).
        if (parent && parent != desktop) {
            Window* pw = win(parent);
            Window* t = pw ? toplevel_of(pw) : nullptr;
            owner = t ? t->hwnd : 0;
        }
        parent = desktop;
    } else if (!win(parent)) {
        emu.set_last_error(1406);
        return 0;
    }
    if (!child && !(style & WS_POPUP)) style |= WS_CLIPSIBLINGS | WS_CAPTION;  // WS_OVERLAPPED
    if (!child && (style & WS_POPUP) == 0) exstyle |= WS_EX_WINDOWEDGE;

    auto wnd = std::make_unique<Window>();
    Window* wp = wnd.get();
    wp->hwnd = new_hwnd();
    wp->cls = cls;
    wp->style = style & ~WS_VISIBLE;
    wp->exstyle = exstyle;
    wp->owner = owner;
    wp->wndproc = cls->wndproc;
    wp->hinstance = hinst;
    wp->extra.assign(static_cast<size_t>(std::max(0, cls->wnd_extra)), 0);
    wp->ansi = ansi || cls->ansi;
    if (child) wp->id = menu_or_id;
    else wp->menu = static_cast<uint32_t>(menu_or_id);
    if (!child && !wp->menu && cls->menu_name) {
        // The class menu (MAKEINTRESOURCE or a name) loads from the class's module.
        extern uint32_t load_menu_resource(Gui&, uint64_t, uint64_t);
        wp->menu = load_menu_resource(*this, cls->hinstance, cls->menu_name);
    }

    // Default placement.
    if (!child) {
        if (x == kUseDefault) {
            static int cascade = 0;
            x = 26 * (cascade % 8);
            if (y == kUseDefault || !(style & WS_POPUP)) y = 26 * (cascade % 8);
            ++cascade;
        }
        if (w == kUseDefault) {
            w = screen_w * 3 / 4;
            h = screen_h * 3 / 4;
        }
    } else {
        if (x == kUseDefault) x = y = 0;
        if (w == kUseDefault) w = h = 0;
    }
    if (y == kUseDefault) y = 0;
    if (h == kUseDefault) h = 0;
    wp->wrect = {x, y, x + w, y + h};
    wp->text = title;
    windows[wp->hwnd] = std::move(wnd);
    link(wp, parent, child ? 1 : 0);  // children are created at the bottom
    wp->crect = wp->wrect;
    ++geom_ver;

    uint32_t hwnd = wp->hwnd;
    // CREATESTRUCTW, then the CBT hook gets a chance at it (this is where MFC
    // subclasses every window it creates).
    uint64_t cs = scratch_alloc(48 + 8);
    auto fill_cs = [&]() {
        Window* ww = win(hwnd);
        emu.mem.write32(cs + 0, static_cast<uint32_t>(param));
        emu.mem.write32(cs + 4, static_cast<uint32_t>(hinst));
        emu.mem.write32(cs + 8, static_cast<uint32_t>(menu_or_id));
        emu.mem.write32(cs + 12, child ? parent : owner);
        emu.mem.write32(cs + 16, static_cast<uint32_t>(ww ? ww->wrect.h() : h));
        emu.mem.write32(cs + 20, static_cast<uint32_t>(ww ? ww->wrect.w() : w));
        emu.mem.write32(cs + 24, static_cast<uint32_t>(ww ? ww->wrect.t : y));
        emu.mem.write32(cs + 28, static_cast<uint32_t>(ww ? ww->wrect.l : x));
        emu.mem.write32(cs + 32, style);
        emu.mem.write32(cs + 44, exstyle);
    };
    // The strings the struct points at.
    uint64_t name_buf = scratch_alloc((title.size() + 1) * 2);
    write_wstr(emu, name_buf, title, static_cast<int>(title.size() + 1));
    uint64_t cls_buf = scratch_alloc((cls->display_name.size() + 1) * 2);
    write_wstr(emu, cls_buf, cls->display_name, static_cast<int>(cls->display_name.size() + 1));
    fill_cs();
    emu.mem.write32(cs + 36, static_cast<uint32_t>(name_buf));
    emu.mem.write32(cs + 40, atom ? atom : static_cast<uint32_t>(cls_buf));
    auto cleanup = [&]() {
        scratch_free(cs);
        scratch_free(name_buf);
        scratch_free(cls_buf);
    };

    {
        uint64_t cbt = scratch_alloc(8);
        emu.mem.write32(cbt, static_cast<uint32_t>(cs));
        emu.mem.write32(cbt + 4, 0);
        uint64_t veto = call_hook(5, 3, hwnd, cbt);  // WH_CBT, HCBT_CREATEWND
        scratch_free(cbt);
        if (veto) {
            Window* ww = win(hwnd);
            if (ww) unlink(ww);
            windows.erase(hwnd);
            cleanup();
            return 0;
        }
    }

    // Top-level overlapped windows get a say in their size.
    if (!child && (style & (WS_THICKFRAME | WS_CAPTION))) {
        uint64_t mmi = scratch_alloc(40);
        Rect ins = nc_insets(style, exstyle, false, 0);
        int vals[10] = {0, 0, screen_w + ins.l + ins.r, screen_h + ins.t + ins.b, -ins.l, -ins.t,
                        112, 27, screen_w + ins.l + ins.r, screen_h + ins.t + ins.b};
        for (int i = 0; i < 10; ++i) emu.mem.write32(mmi + i * 4, static_cast<uint32_t>(vals[i]));
        send(hwnd, WM_GETMINMAXINFO, 0, mmi);
        scratch_free(mmi);
    }

    if (!send(hwnd, WM_NCCREATE, 0, cs)) {
        if (trace_msgs) std::fprintf(stderr, "[gui] WM_NCCREATE refused for %s\n", to_utf8(cls->display_name).c_str());
        destroy_window(hwnd);
        cleanup();
        return 0;
    }
    Window* ww = win(hwnd);
    if (!ww) {
        cleanup();
        return 0;
    }
    // WM_NCCALCSIZE through the window procedure.
    {
        uint64_t ncp = scratch_alloc(68);
        write_rect(emu, ncp, ww->wrect);
        send(hwnd, WM_NCCALCSIZE, 0, ncp);
        ww = win(hwnd);
        if (!ww) {
            scratch_free(ncp);
            cleanup();
            return 0;
        }
        ww->crect = read_rect(emu, ncp);
        scratch_free(ncp);
    }
    ++geom_ver;
    if (static_cast<int64_t>(static_cast<int32_t>(send(hwnd, WM_CREATE, 0, cs))) == -1) {
        destroy_window(hwnd);
        cleanup();
        return 0;
    }
    cleanup();
    ww = win(hwnd);
    if (!ww) return 0;
    ww->created = true;
    send(hwnd, WM_SIZE, 0, make_lparam(ww->crect.w(), ww->crect.h()));
    if (win(hwnd)) send(hwnd, WM_MOVE, 0, make_lparam(ww->crect.l, ww->crect.t));
    ww = win(hwnd);
    if (!ww) return 0;
    if (child && !(exstyle & WS_EX_NOPARENTNOTIFY)) {
        // WM_PARENTNOTIFY up the chain.
        Window* p = win(parent);
        while (p && p->hwnd != desktop) {
            send(p->hwnd, WM_PARENTNOTIFY, make_lparam(WM_CREATE, static_cast<int>(ww->id)), hwnd);
            if (!(p->style & WS_CHILD) || (p->exstyle & WS_EX_NOPARENTNOTIFY)) break;
            p = win(p->parent);
        }
    }
    ww = win(hwnd);
    if (!ww) return 0;
    if (style & WS_MAXIMIZE) {
        show_window(hwnd, 3);  // SW_SHOWMAXIMIZED
    } else if (style & WS_VISIBLE) {
        show_window(hwnd, 5);  // SW_SHOW
    }
    return hwnd;
}

bool Gui::destroy_window(uint32_t hwnd) {
    Window* w = win(hwnd);
    if (!w || hwnd == desktop) return false;
    if (w->destroying) return true;
    w->destroying = true;
    call_hook(5, 4, hwnd, 0);  // HCBT_DESTROYWND
    // Owned windows go first.
    std::vector<uint32_t> owned;
    for (auto& [h, o] : windows)
        if (o->owner == hwnd && h != hwnd) owned.push_back(h);
    for (uint32_t h : owned) destroy_window(h);
    w = win(hwnd);
    if (!w) return true;
    if (w->style & WS_VISIBLE) {
        set_window_pos(hwnd, 0, 0, 0, 0, 0,
                       SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        w = win(hwnd);
        if (!w) return true;
    }
    if ((w->style & WS_CHILD) && !(w->exstyle & WS_EX_NOPARENTNOTIFY)) {
        Window* p = win(w->parent);
        if (p && p->hwnd != desktop) send(p->hwnd, WM_PARENTNOTIFY, make_lparam(WM_DESTROY, static_cast<int>(w->id)), hwnd);
    }
    if (capture == hwnd) capture = 0;
    send(hwnd, WM_DESTROY, 0, 0);
    // Children: WM_DESTROY after the parent's, WM_NCDESTROY before it.
    w = win(hwnd);
    if (w) {
        std::vector<uint32_t> kids = w->children;
        for (uint32_t k : kids) destroy_window(k);
    }
    if (focus == hwnd || (focus && is_child_of(*this, focus, hwnd))) focus = 0;
    send(hwnd, WM_NCDESTROY, 0, 0);
    w = win(hwnd);
    if (!w) return true;
    timers.erase(std::remove_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.hwnd == hwnd; }),
                 timers.end());
    posted.erase(std::remove_if(posted.begin(), posted.end(), [&](const Msg& m) { return m.hwnd == hwnd; }),
                 posted.end());
    if (caret_hwnd == hwnd) caret_hwnd = 0;
    bool was_active = active == hwnd;
    unlink(w);
    // A DC still referring to the window paints nowhere from now on.
    windows.erase(hwnd);
    ++geom_ver;
    if (was_active) {
        active = 0;
        // Activate the next visible top-level window.
        Window* d = win(desktop);
        for (uint32_t th : d->children) {
            Window* t = win(th);
            if (t && (t->style & WS_VISIBLE) && !(t->style & WS_DISABLED)) {
                set_active(th);
                break;
            }
        }
    }
    if (hover_hwnd == hwnd) hover_hwnd = 0;
    return true;
}

bool Gui::set_window_pos(uint32_t hwnd, uint32_t after, int x, int y, int cx, int cy, uint32_t flags) {
    Window* w = win(hwnd);
    if (!w || hwnd == desktop) return false;
    // WINDOWPOS for WM_WINDOWPOSCHANGING, which may edit it.
    Rect old_w = w->wrect, old_c = w->crect;
    bool was_visible = (w->style & WS_VISIBLE) != 0;
    if (flags & SWP_NOMOVE) {
        x = old_w.l;
        y = old_w.t;
    }
    if (flags & SWP_NOSIZE) {
        cx = old_w.w();
        cy = old_w.h();
    }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    uint64_t wpos = scratch_alloc(28);
    auto write_wp = [&]() {
        emu.mem.write32(wpos, hwnd);
        emu.mem.write32(wpos + 4, after);
        emu.mem.write32(wpos + 8, static_cast<uint32_t>(x));
        emu.mem.write32(wpos + 12, static_cast<uint32_t>(y));
        emu.mem.write32(wpos + 16, static_cast<uint32_t>(cx));
        emu.mem.write32(wpos + 20, static_cast<uint32_t>(cy));
        emu.mem.write32(wpos + 24, flags);
    };
    write_wp();
    if (!(flags & SWP_NOSENDCHANGING) && w->created) {
        send(hwnd, WM_WINDOWPOSCHANGING, 0, wpos);
        w = win(hwnd);
        if (!w) {
            scratch_free(wpos);
            return false;
        }
        after = emu.mem.read32(wpos + 4);
        x = static_cast<int32_t>(emu.mem.read32(wpos + 8));
        y = static_cast<int32_t>(emu.mem.read32(wpos + 12));
        cx = static_cast<int32_t>(emu.mem.read32(wpos + 16));
        cy = static_cast<int32_t>(emu.mem.read32(wpos + 20));
        flags = emu.mem.read32(wpos + 24);
    }
    Rect nw{x, y, x + cx, y + cy};
    bool moved = nw.l != old_w.l || nw.t != old_w.t;
    bool sized = nw.w() != old_w.w() || nw.h() != old_w.h();
    w->wrect = nw;
    // Client area through WM_NCCALCSIZE when the frame may have changed.
    if (sized || moved || (flags & SWP_FRAMECHANGED)) {
        uint64_t ncp = scratch_alloc(68);
        write_rect(emu, ncp, nw);
        write_rect(emu, ncp + 16, old_w);
        write_rect(emu, ncp + 32, old_c);
        emu.mem.write32(ncp + 48, static_cast<uint32_t>(wpos));
        send(hwnd, WM_NCCALCSIZE, 1, ncp);
        w = win(hwnd);
        if (!w) {
            scratch_free(ncp);
            scratch_free(wpos);
            return false;
        }
        w->crect = read_rect(emu, ncp);
        scratch_free(ncp);
    }
    if (!(flags & SWP_NOZORDER) && w->parent) {
        if (after != hwnd) {
            unlink(w);
            if (after == 0xFFFFFFFF) w->exstyle |= WS_EX_TOPMOST;
            if (after == 0xFFFFFFFE) w->exstyle &= ~WS_EX_TOPMOST;
            link(w, w->parent, after);
        }
    }
    if (flags & SWP_SHOWWINDOW) w->style |= WS_VISIBLE;
    if (flags & SWP_HIDEWINDOW) w->style &= ~WS_VISIBLE;
    bool now_visible = (w->style & WS_VISIBLE) != 0;
    ++geom_ver;

    // What needs repainting.
    bool top = !(w->style & WS_CHILD);
    if (!(flags & SWP_NOREDRAW)) {
        if (top) {
            if (sized || (flags & SWP_FRAMECHANGED) || (now_visible && !was_visible)) {
                ensure_surface(w);
                invalidate(w, nullptr, true, true, true);
            }
        } else {
            Window* p = win(w->parent);
            bool geometry = moved || sized || (flags & SWP_FRAMECHANGED) || was_visible != now_visible;
            if (p && geometry) {
                // The parent repaints where the window was, and the window
                // (and its children) where it is now.
                if (was_visible) {
                    Region rg(old_w);
                    invalidate(p, &rg, true, true, false);
                }
                if (now_visible) {
                    Region rg(nw);
                    invalidate(p, &rg, true, true, false);
                    invalidate(w, nullptr, true, true, true);
                }
            } else if (now_visible && !(flags & SWP_NOZORDER)) {
                invalidate(w, nullptr, true, true, true);
            }
        }
    }
    write_wp();
    emu.mem.write32(wpos + 4, after);
    emu.mem.write32(wpos + 24, flags | (moved ? 0 : SWP_NOMOVE) | (sized ? 0 : SWP_NOSIZE));
    if (w->created) send(hwnd, WM_WINDOWPOSCHANGED, 0, wpos);
    scratch_free(wpos);
    w = win(hwnd);
    if (!w) return true;
    if (now_visible && !was_visible && top && !(flags & SWP_NOACTIVATE)) set_active(hwnd);
    if (!now_visible && was_visible) {
        if (active == hwnd) {
            active = 0;
            // Activate the owner, or the next top-level window.
            uint32_t next = w->owner;
            if (!next) {
                for (uint32_t th : win(desktop)->children) {
                    Window* t = win(th);
                    if (t && th != hwnd && (t->style & WS_VISIBLE) && !(t->style & WS_DISABLED)) {
                        next = th;
                        break;
                    }
                }
            }
            if (next && win(next)) set_active(next);
        }
        if (focus == hwnd || (focus && is_child_of(*this, focus, hwnd))) {
            Window* p = win(w->parent);
            if (p && p->hwnd != desktop) set_focus(p->hwnd);
            else if (focus == hwnd) set_focus(0);
        }
        if (capture == hwnd) capture = 0;
    }
    return true;
}

bool Gui::show_window(uint32_t hwnd, int cmd) {
    Window* w = win(hwnd);
    if (!w) return false;
    bool was = (w->style & WS_VISIBLE) != 0;
    if (cmd == 0) {  // SW_HIDE
        if (!was) return false;
        send(hwnd, WM_SHOWWINDOW, 0, 0);
        set_window_pos(hwnd, 0, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return was;
    }
    bool child = (w->style & WS_CHILD) != 0;
    if (!was) {
        send(hwnd, WM_SHOWWINDOW, 1, 0);
        w = win(hwnd);
        if (!w) return false;
    }
    if (cmd == 3 && !child) {  // SW_SHOWMAXIMIZED / SW_MAXIMIZE
        if (!w->maximized) w->normal_rect = w->wrect;
        w->maximized = true;
        w->minimized = false;
        w->style |= WS_MAXIMIZE;
        w->style &= ~WS_MINIMIZE;
        // Maximised: the frame hangs off the screen edges, as on Windows.
        Rect ins = nc_insets(w->style, w->exstyle, false, 0);
        int b = ins.l;
        set_window_pos(hwnd, 0, -b, -b, screen_w + 2 * b, screen_h + 2 * b,
                       SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOZORDER);
        return was;
    }
    if ((cmd == 1 || cmd == 9) && w->maximized && !child) {  // SW_SHOWNORMAL / SW_RESTORE
        w->maximized = false;
        w->style &= ~WS_MAXIMIZE;
        Rect r = w->normal_rect.empty() ? w->wrect : w->normal_rect;
        set_window_pos(hwnd, 0, r.l, r.t, r.w(), r.h(), SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOZORDER);
        return was;
    }
    if (cmd == 6 || cmd == 2 || cmd == 7) {  // minimise: keep it simple, just hide
        w->minimized = true;
        w->style |= WS_MINIMIZE;
    }
    uint32_t flags = SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE;
    if (cmd == 4 || cmd == 8 || cmd == 7 || child) flags |= SWP_NOACTIVATE;  // SW_SHOWNOACTIVATE / SW_SHOWNA
    if (child || cmd == 4 || cmd == 8) flags |= SWP_NOZORDER;
    if (!was || !child) set_window_pos(hwnd, 0, 0, 0, 0, 0, flags);
    return was;
}

void Gui::set_focus(uint32_t hwnd) {
    if (hwnd == focus) return;
    if (hwnd) {
        Window* w = win(hwnd);
        if (!w) return;
        // Focus moves into the window's top-level, which becomes active.
        Window* top = toplevel_of(w);
        if (top && top->hwnd != active) set_active(top->hwnd);
        if (call_hook(5, 9, hwnd, focus)) return;  // HCBT_SETFOCUS
    }
    uint32_t old = focus;
    focus = hwnd;
    if (old && win(old)) send(old, WM_KILLFOCUS, hwnd, 0);
    if (focus == hwnd && hwnd && win(hwnd)) send(hwnd, WM_SETFOCUS, old, 0);
}

void Gui::set_active(uint32_t hwnd, bool by_mouse) {
    if (hwnd == active) return;
    Window* w = win(hwnd);
    if (hwnd && (!w || (w->style & WS_CHILD))) return;
    if (hwnd && call_hook(5, 5, hwnd, 0)) return;  // HCBT_ACTIVATE
    uint32_t old = active;
    if (old && win(old)) {
        send(old, WM_NCACTIVATE, 0, 0);
        if (win(old)) send(old, WM_ACTIVATE, 0, hwnd);
    }
    active = hwnd;
    foreground = hwnd;
    if (!hwnd) return;
    // Bring it (and the windows it owns) to the top of the z-order.
    w = win(hwnd);
    if (!w) return;
    unlink(w);
    link(w, desktop, 0);
    for (auto& [h, o] : windows)
        if (o->owner == hwnd && (o->style & WS_VISIBLE) && h != hwnd) {
            unlink(o.get());
            link(o.get(), desktop, 0);
        }
    ++geom_ver;
    if (!old) send(hwnd, WM_ACTIVATEAPP, 1, 0);
    if (win(hwnd)) send(hwnd, WM_NCACTIVATE, 1, 0);
    if (win(hwnd)) send(hwnd, WM_ACTIVATE, by_mouse ? 2 : 1, old);
}

// ---------------------------------------------------------------------------
// Painting entry points
// ---------------------------------------------------------------------------

uint32_t get_dc(Gui& g, uint32_t hwnd, bool window, const Region* clip, uint32_t flags) {
    (void)flags;
    if (!hwnd) hwnd = g.desktop;
    Window* w = g.win(hwnd);
    if (!w) return 0;
    uint32_t hdc = g.create_dc(DC::Kind::Window, hwnd, window);
    if (clip) {
        DC* d = g.dc(hdc);
        // clip is in screen coordinates for GetDCEx; convert to surface.
        Window* top = g.toplevel_of(w);
        if (top) {
            Rect tr = g.window_rect_screen(top);
            Region c = *clip;
            c.offset(-tr.l, -tr.t);
            d->paint_clip = c;
            d->has_paint_clip = true;
        }
    }
    // Class/parent DC styles: the window's font is not selected automatically.
    return hdc;
}

void release_dc(Gui& g, uint32_t hdc) {
    DC* d = g.dc(hdc);
    if (d && d->kind == DC::Kind::Window) g.delete_dc(hdc);
}

uint32_t begin_paint(Gui& g, uint32_t hwnd, Rect& rc, bool& erase) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    if (w->need_ncpaint) {
        w->need_ncpaint = false;
        g.send(hwnd, WM_NCPAINT, 1, 0);
        w = g.win(hwnd);
        if (!w) return 0;
    }
    Region upd = w->update;
    Rect cr{0, 0, w->crect.w(), w->crect.h()};
    if (upd.empty() && w->internal_paint) upd.clear();
    bool need_erase = w->need_erase;
    w->update.clear();
    w->need_erase = false;
    w->internal_paint = false;
    uint32_t hdc = g.create_dc(DC::Kind::Window, hwnd, false);
    DC* d = g.dc(hdc);
    d->from_paint = true;
    Point o = g.surface_origin(w, true);
    Region pc = upd;
    pc.offset(o.x, o.y);
    d->paint_clip = pc;
    d->has_paint_clip = true;
    rc = upd.bounds().intersect(cr);
    caret_draw(g, false);
    erase = need_erase;
    if (need_erase) {
        erase = g.send(hwnd, WM_ERASEBKGND, hdc, 0) == 0;
    }
    return hdc;
}

void end_paint(Gui& g, uint32_t, uint32_t hdc) { release_dc(g, hdc); }

uint32_t get_sys_color_brush(Gui& g, int idx) {
    if (idx < 0 || idx >= 31) return 0;
    return g.sys_color_brush[idx];
}

uint32_t ctl_color(Gui& g, uint32_t hwnd, uint32_t hdc, uint32_t msg) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    Window* p = g.win(w->parent);
    uint64_t br = 0;
    if (p && p->hwnd != g.desktop) br = g.send(p->hwnd, msg, hdc, hwnd);
    else br = def_window_proc(g, hwnd, msg, hdc, hwnd);
    if (!br) br = def_window_proc(g, hwnd, msg, hdc, hwnd);
    return static_cast<uint32_t>(br);
}

// ---------------------------------------------------------------------------
// Helpers shared with the other files
// ---------------------------------------------------------------------------

uint32_t get_parent_or_owner(Gui& g, Window* w) {
    if (!w) return 0;
    if (w->style & WS_CHILD) return w->parent;
    (void)g;
    return w->owner;
}

bool is_child_of(Gui& g, uint32_t child, uint32_t parent) {
    Window* w = g.win(child);
    while (w && (w->style & WS_CHILD)) {
        if (w->parent == parent) return true;
        w = g.win(w->parent);
    }
    return false;
}

std::u16string window_text(Gui& g, uint32_t hwnd) {
    Window* w = g.win(hwnd);
    return w ? w->text : std::u16string();
}

uint32_t control_id(Window* w) { return w ? static_cast<uint32_t>(w->id) : 0; }

void notify_parent(Gui& g, Window* w, uint16_t code) {
    if (!w) return;
    uint32_t id = control_id(w);
    uint32_t hwnd = w->hwnd;
    Window* p = g.win(w->parent);
    if (!p || p->hwnd == g.desktop) return;
    g.send(p->hwnd, WM_COMMAND, (static_cast<uint32_t>(code) << 16) | (id & 0xFFFF), hwnd);
}

uint32_t window_font(Gui& g, Window* w) {
    if (w && w->font) return static_cast<uint32_t>(w->font);
    return default_gui_font(g);
}

void caret_draw(Gui& g, bool on) {
    if (!on || !g.caret_hwnd || g.caret_hide > 0) return;
    Window* w = g.win(g.caret_hwnd);
    if (!w || !g.is_visible(w)) return;
    // Blink at 530 ms like Windows.
    if ((g.host->now_ms() / 530) % 2) return;
    Point o = g.client_to_screen(w, g.caret_pos);
    Rect cr = g.client_rect_screen(w);
    for (int y = o.y; y < o.y + g.caret_h; ++y)
        for (int x = o.x; x < o.x + g.caret_w; ++x) {
            if (!cr.contains(x, y) || x < 0 || y < 0 || x >= g.screen_w || y >= g.screen_h) continue;
            uint32_t& p = g.screen[static_cast<size_t>(y) * g.screen_w + x];
            p = ~p & 0xFFFFFF;
        }
}

WndClass* Gui::find_class(const std::u16string& name, uint64_t hinst) const {
    std::u16string up = upper(name);
    WndClass* any = nullptr;
    for (const auto& c : classes) {
        if (c->name != up) continue;
        if (c->builtin || c->hinstance == hinst || (c->style & 0x4000)) return c.get();  // CS_GLOBALCLASS
        if (!any) any = c.get();
    }
    return any;
}

WndClass* Gui::find_class_atom(uint32_t atom) const {
    for (const auto& c : classes)
        if (c->atom == atom) return c.get();
    return nullptr;
}

void register_builtin_class(Emulator& e, const char16_t* name, uint32_t style, int wnd_extra,
                            uint64_t cursor_id, uint64_t brush, HostProc proc) {
    Gui& g = gui(e);
    auto c = std::make_unique<WndClass>();
    c->name = upper(name);
    c->display_name = name;
    c->style = style | 0x4000;
    c->wnd_extra = wnd_extra;
    c->builtin = true;
    c->background = brush;
    c->cursor = cursor_id ? system_cursor(g, static_cast<int>(cursor_id)) : 0;
    std::string hook_name = "__wndproc_" + to_utf8(name);
    c->wndproc = e.add_hook(hook_name, e.is64() ? 0 : 16, [proc](Emulator& e) {
        Gui& g = gui(e);
        ret(e, proc(g, U(e, 0), U(e, 1), A(e, 2), A(e, 3)));
    });
    c->atom = g.add_atom(name);
    if (std::u16string(name) == u"#32770") c->atom = 32770;
    if (std::u16string(name) == u"#32768") c->atom = 32768;
    g.builtin_procs[c->name] = c->wndproc;
    g.classes.push_back(std::move(c));
}

// ---------------------------------------------------------------------------
// DrawText
// ---------------------------------------------------------------------------

int draw_text_impl(Gui& g, uint32_t hdc, std::u16string s, Rect& rc, uint32_t fmt, int tab) {
    constexpr uint32_t DT_CENTER = 0x1, DT_RIGHT = 0x2, DT_VCENTER = 0x4, DT_BOTTOM = 0x8,
                       DT_WORDBREAK = 0x10, DT_SINGLELINE = 0x20, DT_EXPANDTABS = 0x40,
                       DT_NOCLIP = 0x100, DT_CALCRECT = 0x400, DT_NOPREFIX = 0x800,
                       DT_PATH_ELLIPSIS = 0x4000, DT_END_ELLIPSIS = 0x8000,
                       DT_HIDEPREFIX = 0x100000, DT_WORD_ELLIPSIS = 0x40000;
    RealFont* f = dc_realfont(g, hdc);
    DC* d = g.dc(hdc);
    int line_h = f->height;
    auto width = [&](const std::u16string& t) {
        int w = 0;
        for (char16_t c : t) w += f->advance(c);
        return w;
    };
    // Prefix processing: '&' underlines the next character, "&&" is '&'.
    struct Line {
        std::u16string text;
        int ul = -1;  // index of the underlined character
    };
    auto strip_prefix = [&](const std::u16string& t, int& ul) {
        std::u16string out;
        ul = -1;
        if (fmt & DT_NOPREFIX) return t;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == u'&') {
                if (i + 1 < t.size() && t[i + 1] == u'&') {
                    out.push_back(u'&');
                    ++i;
                } else if (i + 1 < t.size()) {
                    ul = static_cast<int>(out.size());
                }
                continue;
            }
            out.push_back(t[i]);
        }
        return out;
    };
    if (fmt & DT_EXPANDTABS) {
        std::u16string o;
        for (char16_t c : s) {
            if (c == u'\t') o.append(static_cast<size_t>(tab), u' ');
            else o.push_back(c);
        }
        s = o;
    }
    std::vector<Line> lines;
    int maxw = rc.w();
    auto push = [&](const std::u16string& raw) {
        Line l;
        l.text = strip_prefix(raw, l.ul);
        lines.push_back(l);
    };
    if (fmt & DT_SINGLELINE) {
        std::u16string t;
        for (char16_t c : s)
            if (c != u'\r' && c != u'\n') t.push_back(c);
        push(t);
    } else {
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t nl = s.find(u'\n', pos);
            std::u16string para = s.substr(pos, nl == std::u16string::npos ? std::u16string::npos : nl - pos);
            if (!para.empty() && para.back() == u'\r') para.pop_back();
            if ((fmt & DT_WORDBREAK) && maxw > 0) {
                // Break at spaces when they exist, or between any two
                // characters (Japanese has no spaces).
                std::u16string cur;
                size_t i = 0;
                while (i < para.size()) {
                    cur.push_back(para[i]);
                    int dummy;
                    if (width(strip_prefix(cur, dummy)) > maxw && cur.size() > 1) {
                        size_t sp = cur.find_last_of(u' ');
                        if (sp != std::u16string::npos && sp > 0) {
                            push(cur.substr(0, sp));
                            cur = cur.substr(sp + 1);
                        } else {
                            cur.pop_back();
                            push(cur);
                            cur.clear();
                            continue;
                        }
                    }
                    ++i;
                }
                push(cur);
            } else {
                push(para);
            }
            if (nl == std::u16string::npos) break;
            pos = nl + 1;
        }
    }
    int total_h = line_h * static_cast<int>(lines.size());
    int widest = 0;
    for (auto& l : lines) widest = std::max(widest, width(l.text));
    if (fmt & DT_CALCRECT) {
        if (fmt & DT_SINGLELINE) {
            rc.r = rc.l + widest;
            rc.b = rc.t + line_h;
        } else {
            rc.r = rc.l + std::max(widest, (fmt & DT_WORDBREAK) ? 0 : 0);
            if (!(fmt & DT_WORDBREAK)) rc.r = rc.l + widest;
            rc.b = rc.t + total_h;
        }
        return total_h;
    }
    if (!d) return 0;
    int y = rc.t;
    if (fmt & DT_SINGLELINE) {
        if (fmt & DT_VCENTER) y = rc.t + (rc.h() - line_h) / 2;
        else if (fmt & DT_BOTTOM) y = rc.b - line_h;
    }
    DCState save = d->s;
    if (!(fmt & DT_NOCLIP)) {
        // Clip to the rectangle, within whatever clip the DC already has.
        Rect dr = rc;
        Point a{dr.l - d->s.win_org.x + d->s.vp_org.x, dr.t - d->s.win_org.y + d->s.vp_org.y};
        Region clip(Rect{a.x, a.y, a.x + dr.w(), a.y + dr.h()});
        if (d->s.has_clip) clip.intersect(d->s.clip);
        d->s.clip = clip;
        d->s.has_clip = true;
    }
    d->s.text_align = 0;
    for (auto& l : lines) {
        std::u16string t = l.text;
        int w = width(t);
        if ((fmt & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)) && w > rc.w()) {
            while (!t.empty() && width(t + u"...") > rc.w()) t.pop_back();
            t += u"...";
            w = width(t);
        }
        int x = rc.l;
        if (fmt & DT_CENTER) x = rc.l + (rc.w() - w) / 2;
        else if (fmt & DT_RIGHT) x = rc.r - w;
        dc_text_out(g, hdc, x, y, 0, nullptr, t);
        if (l.ul >= 0 && l.ul < static_cast<int>(t.size()) && !(fmt & DT_HIDEPREFIX)) {
            int ux = x + width(t.substr(0, static_cast<size_t>(l.ul)));
            int uw = f->advance(t[static_cast<size_t>(l.ul)]);
            draw::fill_rect(g, hdc, {ux, y + f->ascent + 1, ux + uw, y + f->ascent + 2}, d->s.text_color);
        }
        y += line_h;
    }
    d->s.clip = save.clip;
    d->s.has_clip = save.has_clip;
    d->s.text_align = save.text_align;
    return total_h;
}

namespace draw {
int draw_text(Gui& g, uint32_t hdc, const std::u16string& s, Rect& r, uint32_t fmt) {
    return draw_text_impl(g, hdc, s, r, fmt);
}
}  // namespace draw

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

void install_user(Emulator& e) {
    Gui& g = gui(e);
    g.defwndproc_hook = e.add_hook("DefWindowProcW", e.is64() ? 0 : 16, [](Emulator& e) {
        ret(e, def_window_proc(gui(e), U(e, 0), U(e, 1), A(e, 2), A(e, 3)));
    });
    e.alias_hook("DefWindowProcW", "DefWindowProcA");
    // No MDI here: the frame and child defaults are the plain window default.
    api(e, "DefFrameProcW", 5, [](Emulator& e) {
        ret(e, def_window_proc(gui(e), U(e, 0), U(e, 2), A(e, 3), A(e, 4)));
    });
    api(e, "DefMDIChildProcW", 4, [](Emulator& e) {
        ret(e, def_window_proc(gui(e), U(e, 0), U(e, 1), A(e, 2), A(e, 3)));
    });

    // ---- classes ----------------------------------------------------------------
    auto reg_class = [](Emulator& e, uint64_t p, bool ex, bool ansi) -> uint32_t {
        Gui& g = gui(e);
        uint64_t q = ex ? p + 4 : p;
        auto c = std::make_unique<WndClass>();
        c->style = e.mem.read32(q);
        c->wndproc = e.mem.read32(q + 4);
        c->cls_extra = static_cast<int32_t>(e.mem.read32(q + 8));
        c->wnd_extra = static_cast<int32_t>(e.mem.read32(q + 12));
        c->hinstance = e.mem.read32(q + 16);
        c->icon = e.mem.read32(q + 20);
        c->cursor = e.mem.read32(q + 24);
        c->background = e.mem.read32(q + 28);
        c->menu_name = e.mem.read32(q + 32);
        uint64_t name = e.mem.read32(q + 36);
        if (ex) c->icon_sm = e.mem.read32(q + 40);
        c->ansi = ansi;
        if (name < 0x10000) {
            auto it = g.atom_names.find(static_cast<uint32_t>(name));
            c->display_name = it != g.atom_names.end() ? it->second : u"#" + from_utf8(std::to_string(name));
        } else {
            c->display_name = ansi ? cp932_to_utf16(e.mem.read_cstring(name)) : read_wstr(e, name);
        }
        c->name = upper(c->display_name);
        // Re-registering in the same module fails, as it does on Windows.
        for (const auto& old : g.classes) {
            if (old->name == c->name && !old->builtin && old->hinstance == c->hinstance) {
                e.set_last_error(1410);  // ERROR_CLASS_ALREADY_EXISTS
                return 0;
            }
        }
        c->extra.assign(static_cast<size_t>(std::max(0, c->cls_extra)), 0);
        c->atom = g.add_atom(c->display_name);
        uint32_t atom = c->atom;
        g.classes.push_back(std::move(c));
        return atom;
    };
    api(e, "RegisterClassW", 1, [reg_class](Emulator& e) { ret(e, reg_class(e, A(e, 0), false, false)); });
    api(e, "RegisterClassA", 1, [reg_class](Emulator& e) { ret(e, reg_class(e, A(e, 0), false, true)); });
    api(e, "RegisterClassExW", 1, [reg_class](Emulator& e) { ret(e, reg_class(e, A(e, 0), true, false)); });
    api(e, "RegisterClassExA", 1, [reg_class](Emulator& e) { ret(e, reg_class(e, A(e, 0), true, true)); });
    api(e, "UnregisterClassW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string n = A(e, 0) < 0x10000 ? (g.atom_names.count(U(e, 0)) ? g.atom_names[U(e, 0)] : u"") : read_wstr(e, A(e, 0));
        std::u16string up = upper(n);
        for (auto it = g.classes.begin(); it != g.classes.end(); ++it) {
            if ((*it)->name == up && !(*it)->builtin) {
                // Windows refuses while windows of the class exist.
                for (auto& [h, w] : g.windows)
                    if (w->cls == it->get()) return ret(e, 0);
                g.classes.erase(it);
                return ret(e, 1);
            }
        }
        ret(e, 0);
    });
    auto class_info = [](Emulator& e, bool ex) {
        Gui& g = gui(e);
        uint64_t name = A(e, 1), out = A(e, 2);
        WndClass* c = nullptr;
        if (name < 0x10000) c = g.find_class_atom(static_cast<uint32_t>(name));
        else c = g.find_class(read_wstr(e, name), A(e, 0));
        e.log_call("GetClassInfo(%s) -> %s", name < 0x10000 ? "atom" : to_utf8(read_wstr(e, name)).c_str(),
                   c ? "found" : "none");
        if (!c) {
            e.set_last_error(1411);  // ERROR_CLASS_DOES_NOT_EXIST
            return ret(e, 0);
        }
        uint64_t q = ex ? out + 4 : out;
        if (ex) e.mem.write32(out, 48);
        e.mem.write32(q, c->style);
        e.mem.write32(q + 4, static_cast<uint32_t>(c->wndproc));
        e.mem.write32(q + 8, static_cast<uint32_t>(c->cls_extra));
        e.mem.write32(q + 12, static_cast<uint32_t>(c->wnd_extra));
        e.mem.write32(q + 16, static_cast<uint32_t>(c->builtin ? A(e, 0) : c->hinstance));
        e.mem.write32(q + 20, static_cast<uint32_t>(c->icon));
        e.mem.write32(q + 24, static_cast<uint32_t>(c->cursor));
        e.mem.write32(q + 28, static_cast<uint32_t>(c->background));
        e.mem.write32(q + 32, static_cast<uint32_t>(c->menu_name));
        e.mem.write32(q + 36, static_cast<uint32_t>(name));
        if (ex) e.mem.write32(q + 40, static_cast<uint32_t>(c->icon_sm));
        ret(e, c->atom);
    };
    api(e, "GetClassInfoW", 3, [class_info](Emulator& e) { class_info(e, false); });
    api(e, "GetClassInfoExW", 3, [class_info](Emulator& e) { class_info(e, true); });
    api(e, "GetClassNameW", 3, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (!w || !w->cls) return ret(e, 0);
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 1), w->cls->display_name, I(e, 2))));
    });
    api(e, "GetClassNameA", 3, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (!w || !w->cls) return ret(e, 0);
        std::string a = utf16_to_cp932(w->cls->display_name);
        int n = std::min(static_cast<int>(a.size()), I(e, 2) - 1);
        if (n < 0) return ret(e, 0);
        e.mem.write(A(e, 1), a.data(), static_cast<uint64_t>(n));
        e.mem.write8(A(e, 1) + n, 0);
        ret(e, static_cast<uint64_t>(n));
    });
    auto class_long = [](Emulator& e, bool set) {
        Window* w = gui(e).win(A(e, 0));
        if (!w || !w->cls) return ret(e, 0);
        WndClass* c = w->cls;
        int idx = I(e, 1);
        uint64_t v = set ? A(e, 2) : 0;
        auto rw = [&](uint64_t& field) {
            uint64_t old = field;
            if (set) field = v;
            ret(e, old & 0xFFFFFFFF);
        };
        uint64_t style = c->style;
        switch (idx) {
            case -10: return rw(c->background);
            case -12: return rw(c->cursor);
            case -14: return rw(c->icon);
            case -34: return rw(c->icon_sm);
            case -16: return rw(c->hinstance);
            case -24: return rw(c->wndproc);
            case -8: return rw(c->menu_name);
            case -26: {
                rw(style);
                if (set) c->style = static_cast<uint32_t>(v);
                return;
            }
            case -18: return ret(e, static_cast<uint64_t>(c->wnd_extra));
            case -20: return ret(e, static_cast<uint64_t>(c->cls_extra));
            case -32: return ret(e, c->atom);
        }
        if (idx >= 0 && idx + 4 <= static_cast<int>(c->extra.size())) {
            uint32_t old;
            std::memcpy(&old, c->extra.data() + idx, 4);
            if (set) {
                uint32_t nv = static_cast<uint32_t>(v);
                std::memcpy(c->extra.data() + idx, &nv, 4);
            }
            return ret(e, old);
        }
        ret(e, 0);
    };
    api(e, "GetClassLongW", 2, [class_long](Emulator& e) { class_long(e, false); });
    api(e, "SetClassLongW", 3, [class_long](Emulator& e) { class_long(e, true); });
    e.alias_hook("GetClassLongW", "GetClassLongA");
    e.alias_hook("SetClassLongW", "SetClassLongA");
    e.alias_hook("GetClassLongW", "GetClassLongPtrW");
    e.alias_hook("SetClassLongW", "SetClassLongPtrW");

    // ---- creating and destroying ----------------------------------------------------
    auto create = [](Emulator& e, bool ansi) {
        Gui& g = gui(e);
        uint64_t cls = A(e, 1), title = A(e, 2);
        std::u16string cname, t;
        uint32_t atom = 0;
        if (cls < 0x10000) atom = static_cast<uint32_t>(cls);
        else cname = ansi ? cp932_to_utf16(e.mem.read_cstring(cls)) : read_wstr(e, cls);
        if (title) t = ansi ? cp932_to_utf16(e.mem.read_cstring(title)) : read_wstr(e, title);
        uint32_t h = g.create_window(U(e, 0), cname, atom, t, U(e, 3), I(e, 4), I(e, 5), I(e, 6),
                                     I(e, 7), U(e, 8), A(e, 9), A(e, 10), A(e, 11), ansi);
        if (g.trace_msgs)
            std::fprintf(stderr, "[gui] CreateWindowEx(%s, \"%s\", style %08X) = %08X\n",
                         atom ? "atom" : to_utf8(cname).c_str(), to_utf8(t).c_str(), U(e, 3), h);
        ret(e, h);
    };
    api(e, "CreateWindowExW", 12, [create](Emulator& e) { create(e, false); });
    api(e, "CreateWindowExA", 12, [create](Emulator& e) { create(e, true); });
    api(e, "DestroyWindow", 1, [](Emulator& e) { ret(e, gui(e).destroy_window(U(e, 0)) ? 1 : 0); });
    api(e, "IsWindow", 1, [](Emulator& e) { ret(e, gui(e).win(A(e, 0)) ? 1 : 0); });
    api(e, "IsWindowVisible", 1, [](Emulator& e) {
        Gui& g = gui(e);
        ret(e, g.is_visible(g.win(A(e, 0))) ? 1 : 0);
    });
    api(e, "IsWindowEnabled", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w && !(w->style & WS_DISABLED) ? 1 : 0);
    });
    api(e, "EnableWindow", 2, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        Window* w = g.win(h);
        if (!w) return ret(e, 0);
        bool was_disabled = (w->style & WS_DISABLED) != 0;
        bool enable = U(e, 1) != 0;
        if (enable == !was_disabled) return ret(e, was_disabled ? 1 : 0);
        if (enable) {
            w->style &= ~WS_DISABLED;
        } else {
            w->style |= WS_DISABLED;
            if (g.focus == h || is_child_of(g, g.focus, h)) g.set_focus(0);
            if (g.capture == h) g.capture = 0;
        }
        g.send(h, WM_ENABLE, enable ? 1 : 0, 0);
        if ((w = g.win(h))) g.invalidate(w, nullptr, true, false, true);
        ret(e, was_disabled ? 1 : 0);
    });
    api(e, "IsIconic", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w && w->minimized ? 1 : 0);
    });
    api(e, "IsZoomed", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w && w->maximized ? 1 : 0);
    });
    api(e, "IsChild", 2, [](Emulator& e) { ret(e, is_child_of(gui(e), U(e, 1), U(e, 0)) ? 1 : 0); });
    api(e, "ShowWindow", 2, [](Emulator& e) { ret(e, gui(e).show_window(U(e, 0), I(e, 1)) ? 1 : 0); });
    api(e, "ShowWindowAsync", 2, [](Emulator& e) { ret(e, gui(e).show_window(U(e, 0), I(e, 1)) ? 1 : 0); });
    api(e, "ShowOwnedPopups", 2, [](Emulator& e) {
        Gui& g = gui(e);
        std::vector<uint32_t> owned;
        for (auto& [h, w] : g.windows)
            if (w->owner == U(e, 0)) owned.push_back(h);
        for (uint32_t h : owned) {
            Window* w = g.win(h);
            if (!w) continue;
            if (U(e, 1)) {
                if (!(w->style & WS_VISIBLE) && w->created) g.show_window(h, 8);
            } else if (w->style & WS_VISIBLE) {
                g.show_window(h, 0);
            }
        }
        ret(e, 1);
    });
    api(e, "SetWindowPos", 7, [](Emulator& e) {
        ret(e, gui(e).set_window_pos(U(e, 0), U(e, 1), I(e, 2), I(e, 3), I(e, 4), I(e, 5), U(e, 6)) ? 1 : 0);
    });
    api(e, "MoveWindow", 6, [](Emulator& e) {
        uint32_t flags = SWP_NOZORDER | SWP_NOACTIVATE | (U(e, 5) ? 0 : SWP_NOREDRAW);
        ret(e, gui(e).set_window_pos(U(e, 0), 0, I(e, 1), I(e, 2), I(e, 3), I(e, 4), flags) ? 1 : 0);
    });
    // DeferWindowPos: applied at once, which is a correct (if eager) reading.
    api(e, "BeginDeferWindowPos", 1, [](Emulator& e) { ret(e, 0x00DEF000); });
    api(e, "DeferWindowPos", 8, [](Emulator& e) {
        gui(e).set_window_pos(U(e, 1), U(e, 2), I(e, 3), I(e, 4), I(e, 5), I(e, 6), U(e, 7));
        ret(e, A(e, 0));
    });
    api(e, "EndDeferWindowPos", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "BringWindowToTop", 1, [](Emulator& e) {
        ret(e, gui(e).set_window_pos(U(e, 0), 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE) ? 1 : 0);
    });
    api(e, "SetParent", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        uint32_t np = U(e, 1) ? U(e, 1) : g.desktop;
        if (!g.win(np)) return ret(e, 0);
        uint32_t old = w->parent;
        bool vis = (w->style & WS_VISIBLE) != 0;
        if (vis) g.show_window(w->hwnd, 0);
        g.unlink(w);
        g.link(w, np, 0);
        ++g.geom_ver;
        if (np == g.desktop) {
            w->surface.reset();
        }
        if (vis) g.show_window(w->hwnd, 5);
        ret(e, old == g.desktop ? 0 : old);
    });
    api(e, "GetParent", 1, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        if (w->style & WS_CHILD) return ret(e, w->parent);
        ret(e, (w->style & WS_POPUP) ? w->owner : 0);
    });
    api(e, "GetAncestor", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w || w->hwnd == g.desktop) return ret(e, 0);
        switch (U(e, 1)) {
            case 1: return ret(e, w->parent);  // GA_PARENT
            case 2: {                          // GA_ROOT
                Window* t = g.toplevel_of(w);
                return ret(e, t ? t->hwnd : 0);
            }
            case 3: {  // GA_ROOTOWNER
                Window* t = g.toplevel_of(w);
                while (t && t->owner && g.win(t->owner)) t = g.win(t->owner);
                return ret(e, t ? t->hwnd : 0);
            }
        }
        ret(e, 0);
    });
    api(e, "GetDesktopWindow", 0, [](Emulator& e) { ret(e, gui(e).desktop); });
    api(e, "GetShellWindow", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "GetWindow", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        Window* p = g.win(w->parent);
        uint32_t cmd = U(e, 1);
        switch (cmd) {
            case 0:  // GW_HWNDFIRST
                return ret(e, p && !p->children.empty() ? p->children.front() : 0);
            case 1:  // GW_HWNDLAST
                return ret(e, p && !p->children.empty() ? p->children.back() : 0);
            case 2:    // GW_HWNDNEXT
            case 3: {  // GW_HWNDPREV
                if (!p) return ret(e, 0);
                auto& v = p->children;
                auto it = std::find(v.begin(), v.end(), w->hwnd);
                if (it == v.end()) return ret(e, 0);
                if (cmd == 2) return ret(e, it + 1 != v.end() ? *(it + 1) : 0);
                return ret(e, it != v.begin() ? *(it - 1) : 0);
            }
            case 4: return ret(e, w->owner);  // GW_OWNER
            case 5: return ret(e, w->children.empty() ? 0 : w->children.front());  // GW_CHILD
            case 6: {  // GW_ENABLEDPOPUP
                for (auto& [h, o] : g.windows)
                    if (o->owner == w->hwnd && (o->style & WS_VISIBLE) && !(o->style & WS_DISABLED)) return ret(e, h);
                return ret(e, 0);
            }
        }
        ret(e, 0);
    });
    api(e, "GetTopWindow", 1, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0) ? A(e, 0) : g.desktop);
        ret(e, w && !w->children.empty() ? w->children.front() : 0);
    });
    api(e, "GetNextWindow", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        Window* p = g.win(w->parent);
        if (!p) return ret(e, 0);
        auto& v = p->children;
        auto it = std::find(v.begin(), v.end(), w->hwnd);
        if (U(e, 1) == 2) return ret(e, it != v.end() && it + 1 != v.end() ? *(it + 1) : 0);
        ret(e, it != v.begin() && it != v.end() ? *(it - 1) : 0);
    });
    api(e, "GetLastActivePopup", 1, [](Emulator& e) {
        Gui& g = gui(e);
        for (auto& [h, o] : g.windows)
            if (o->owner == U(e, 0) && (o->style & WS_VISIBLE) && h == g.active) return ret(e, h);
        ret(e, A(e, 0));
    });
    auto enum_windows = [](Emulator& e, uint32_t parent, uint64_t proc, uint64_t lp, bool recurse) {
        Gui& g = gui(e);
        std::vector<uint32_t> list;
        std::function<void(uint32_t)> collect = [&](uint32_t h) {
            Window* w = g.win(h);
            if (!w) return;
            for (uint32_t c : w->children) {
                list.push_back(c);
                if (recurse) collect(c);
            }
        };
        collect(parent);
        for (uint32_t h : list) {
            if (!g.win(h)) continue;
            if (!g.call_wndproc(proc, h, static_cast<uint32_t>(lp), 0, 0) && false) break;
        }
    };
    (void)enum_windows;
    api(e, "EnumChildWindows", 3, [](Emulator& e) {
        Gui& g = gui(e);
        std::vector<uint32_t> list;
        std::function<void(uint32_t)> collect = [&](uint32_t h) {
            Window* w = g.win(h);
            if (!w) return;
            for (uint32_t c : w->children) {
                list.push_back(c);
                collect(c);
            }
        };
        collect(U(e, 0) ? U(e, 0) : g.desktop);
        for (uint32_t h : list) {
            if (!g.win(h)) continue;
            if (!(e.call_guest(A(e, 1), {h, A(e, 2)}) & 0xFFFFFFFF)) break;
        }
        ret(e, 1);
    });
    api(e, "EnumWindows", 2, [](Emulator& e) {
        Gui& g = gui(e);
        std::vector<uint32_t> list = g.win(g.desktop)->children;
        for (uint32_t h : list) {
            if (!g.win(h)) continue;
            if (!(e.call_guest(A(e, 0), {h, A(e, 1)}) & 0xFFFFFFFF)) break;
        }
        ret(e, 1);
    });
    api(e, "EnumThreadWindows", 3, [](Emulator& e) {
        Gui& g = gui(e);
        std::vector<uint32_t> list = g.win(g.desktop)->children;
        for (uint32_t h : list) {
            if (!g.win(h)) continue;
            if (!(e.call_guest(A(e, 1), {h, A(e, 2)}) & 0xFFFFFFFF)) break;
        }
        ret(e, 1);
    });
    api(e, "FindWindowW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string cls = A(e, 0) >= 0x10000 ? upper(read_wstr(e, A(e, 0))) : u"";
        std::u16string title = A(e, 1) ? read_wstr(e, A(e, 1)) : u"";
        for (uint32_t h : g.win(g.desktop)->children) {
            Window* w = g.win(h);
            if (!w) continue;
            if (!cls.empty() && (!w->cls || w->cls->name != cls)) continue;
            if (A(e, 1) && w->text != title) continue;
            return ret(e, h);
        }
        ret(e, 0);
    });
    api(e, "FindWindowExW", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "GetWindowThreadProcessId", 2, [](Emulator& e) {
        if (A(e, 1)) e.mem.write32(A(e, 1), static_cast<uint32_t>(e.pid()));
        ret(e, gui(e).win(A(e, 0)) ? 1234 : 0);
    });

    // ---- window data ----------------------------------------------------------------------
    auto window_long = [](Emulator& e, bool set) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        Window* w = g.win(h);
        if (!w) {
            e.set_last_error(1400);
            return ret(e, 0);
        }
        int idx = I(e, 1);
        uint64_t v = set ? (A(e, 2) & 0xFFFFFFFF) : 0;
        switch (idx) {
            case -4: {  // GWL_WNDPROC
                uint64_t old = w->wndproc;
                if (set) w->wndproc = v;
                return ret(e, old);
            }
            case -6: {
                uint64_t old = w->hinstance;
                if (set) w->hinstance = v;
                return ret(e, old);
            }
            case -8: {
                uint64_t old = (w->style & WS_CHILD) ? w->parent : w->owner;
                if (set && !(w->style & WS_CHILD)) w->owner = static_cast<uint32_t>(v);
                return ret(e, old);
            }
            case -12: {
                uint64_t old = w->id;
                if (set) w->id = v;
                return ret(e, old);
            }
            case -16: {  // GWL_STYLE
                uint32_t old = w->style;
                if (set) {
                    uint64_t ss = g.scratch_alloc(8);
                    e.mem.write32(ss, old);
                    e.mem.write32(ss + 4, static_cast<uint32_t>(v));
                    g.send(h, WM_STYLECHANGING, static_cast<uint64_t>(-16) & 0xFFFFFFFF, ss);
                    uint32_t nv = e.mem.read32(ss + 4);
                    if ((w = g.win(h))) {
                        // WS_VISIBLE through SetWindowLong changes visibility
                        // without the usual messages.
                        w->style = nv;
                        ++g.geom_ver;
                        g.send(h, WM_STYLECHANGED, static_cast<uint64_t>(-16) & 0xFFFFFFFF, ss);
                    }
                    g.scratch_free(ss);
                }
                return ret(e, old);
            }
            case -20: {  // GWL_EXSTYLE
                uint32_t old = w->exstyle;
                if (set) {
                    uint64_t ss = g.scratch_alloc(8);
                    e.mem.write32(ss, old);
                    e.mem.write32(ss + 4, static_cast<uint32_t>(v));
                    g.send(h, WM_STYLECHANGING, static_cast<uint64_t>(-20) & 0xFFFFFFFF, ss);
                    if ((w = g.win(h))) {
                        w->exstyle = e.mem.read32(ss + 4);
                        g.send(h, WM_STYLECHANGED, static_cast<uint64_t>(-20) & 0xFFFFFFFF, ss);
                    }
                    g.scratch_free(ss);
                }
                return ret(e, old);
            }
            case -21: {
                uint64_t old = w->userdata;
                if (set) w->userdata = v;
                return ret(e, old);
            }
        }
        if (idx >= 0 && idx + 4 <= static_cast<int>(w->extra.size())) {
            uint32_t old;
            std::memcpy(&old, w->extra.data() + idx, 4);
            if (set) {
                uint32_t nv = static_cast<uint32_t>(v);
                std::memcpy(w->extra.data() + idx, &nv, 4);
            }
            return ret(e, old);
        }
        e.set_last_error(1413);  // ERROR_INVALID_INDEX
        ret(e, 0);
    };
    api(e, "GetWindowLongW", 2, [window_long](Emulator& e) { window_long(e, false); });
    api(e, "SetWindowLongW", 3, [window_long](Emulator& e) { window_long(e, true); });
    for (const char* n : {"GetWindowLongA", "GetWindowLongPtrW", "GetWindowLongPtrA"}) e.alias_hook("GetWindowLongW", n);
    for (const char* n : {"SetWindowLongA", "SetWindowLongPtrW", "SetWindowLongPtrA"}) e.alias_hook("SetWindowLongW", n);
    api(e, "GetWindowWord", 2, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        int idx = I(e, 1);
        if (!w || idx < 0 || idx + 2 > static_cast<int>(w->extra.size())) return ret(e, 0);
        uint16_t v;
        std::memcpy(&v, w->extra.data() + idx, 2);
        ret(e, v);
    });
    api(e, "SetWindowWord", 3, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        int idx = I(e, 1);
        if (!w || idx < 0 || idx + 2 > static_cast<int>(w->extra.size())) return ret(e, 0);
        uint16_t old, v = static_cast<uint16_t>(U(e, 2));
        std::memcpy(&old, w->extra.data() + idx, 2);
        std::memcpy(w->extra.data() + idx, &v, 2);
        ret(e, old);
    });
    auto prop_key = [](Emulator& e, uint64_t p) -> std::u16string {
        if (p < 0x10000) return u"#atom" + from_utf8(std::to_string(p));
        return upper(read_wstr(e, p));
    };
    api(e, "SetPropW", 3, [prop_key](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (!w) return ret(e, 0);
        std::u16string k = prop_key(e, A(e, 1));
        for (auto& p : w->props)
            if (p.first == k) {
                p.second = A(e, 2);
                return ret(e, 1);
            }
        w->props.push_back({k, A(e, 2)});
        ret(e, 1);
    });
    api(e, "GetPropW", 2, [prop_key](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (!w) return ret(e, 0);
        std::u16string k = prop_key(e, A(e, 1));
        for (auto& p : w->props)
            if (p.first == k) return ret(e, p.second);
        ret(e, 0);
    });
    api(e, "RemovePropW", 2, [prop_key](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (!w) return ret(e, 0);
        std::u16string k = prop_key(e, A(e, 1));
        for (auto it = w->props.begin(); it != w->props.end(); ++it)
            if (it->first == k) {
                uint64_t v = it->second;
                w->props.erase(it);
                return ret(e, v);
            }
        ret(e, 0);
    });
    api(e, "SetWindowTextW", 2, [](Emulator& e) {
        ret(e, gui(e).send(U(e, 0), WM_SETTEXT, 0, A(e, 1)) ? 1 : 0);
    });
    api(e, "SetWindowTextA", 2, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string s = A(e, 1) ? cp932_to_utf16(e.mem.read_cstring(A(e, 1))) : u"";
        uint64_t buf = g.scratch_alloc((s.size() + 1) * 2);
        write_wstr(e, buf, s, static_cast<int>(s.size() + 1));
        uint64_t r = g.send(U(e, 0), WM_SETTEXT, 0, buf);
        g.scratch_free(buf);
        ret(e, r ? 1 : 0);
    });
    api(e, "GetWindowTextW", 3, [](Emulator& e) {
        if (I(e, 2) <= 0) return ret(e, 0);
        ret(e, gui(e).send(U(e, 0), WM_GETTEXT, U(e, 2), A(e, 1)));
    });
    api(e, "GetWindowTextA", 3, [](Emulator& e) {
        Gui& g = gui(e);
        int cap = I(e, 2);
        if (cap <= 0) return ret(e, 0);
        uint64_t buf = g.scratch_alloc(static_cast<size_t>(cap) * 2 + 2);
        g.send(U(e, 0), WM_GETTEXT, static_cast<uint64_t>(cap), buf);
        std::string a = utf16_to_cp932(read_wstr(e, buf));
        g.scratch_free(buf);
        int n = std::min(static_cast<int>(a.size()), cap - 1);
        e.mem.write(A(e, 1), a.data(), static_cast<uint64_t>(n));
        e.mem.write8(A(e, 1) + n, 0);
        ret(e, static_cast<uint64_t>(n));
    });
    api(e, "GetWindowTextLengthW", 1, [](Emulator& e) {
        ret(e, gui(e).send(U(e, 0), WM_GETTEXTLENGTH, 0, 0));
    });
    e.alias_hook("GetWindowTextLengthW", "GetWindowTextLengthA");
    api(e, "InternalGetWindowText", 3, [](Emulator& e) {
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 1), window_text(gui(e), U(e, 0)), I(e, 2))));
    });
    api(e, "GetDlgCtrlID", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w && (w->style & WS_CHILD) ? static_cast<uint32_t>(w->id) : 0);
    });
    api(e, "SetWindowContextHelpId", 2, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (w) w->context_help = A(e, 1);
        ret(e, w ? 1 : 0);
    });
    api(e, "GetWindowContextHelpId", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w ? w->context_help : 0);
    });

    // ---- geometry ----------------------------------------------------------------------------
    api(e, "GetWindowRect", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        write_rect(e, A(e, 1), g.window_rect_screen(w));
        ret(e, 1);
    });
    api(e, "GetClientRect", 2, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        if (!w) return ret(e, 0);
        write_rect(e, A(e, 1), {0, 0, std::max(0, w->crect.w()), std::max(0, w->crect.h())});
        ret(e, 1);
    });
    api(e, "ClientToScreen", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        write_point(e, A(e, 1), g.client_to_screen(w, read_point(e, A(e, 1))));
        ret(e, 1);
    });
    api(e, "ScreenToClient", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        Point o = g.client_to_screen(w, {0, 0});
        Point p = read_point(e, A(e, 1));
        write_point(e, A(e, 1), {p.x - o.x, p.y - o.y});
        ret(e, 1);
    });
    api(e, "MapWindowPoints", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Window* from = g.win(A(e, 0) ? A(e, 0) : g.desktop);
        Window* to = g.win(A(e, 1) ? A(e, 1) : g.desktop);
        if (!from || !to) return ret(e, 0);
        Point a = g.client_to_screen(from, {0, 0}), b = g.client_to_screen(to, {0, 0});
        int dx = a.x - b.x, dy = a.y - b.y;
        for (uint32_t i = 0; i < U(e, 3); ++i) {
            uint64_t p = A(e, 2) + i * 8;
            Point q = read_point(e, p);
            write_point(e, p, {q.x + dx, q.y + dy});
        }
        ret(e, make_lparam(dx, dy));
    });
    api(e, "AdjustWindowRectEx", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Rect r = read_rect(e, A(e, 0));
        Rect ins = g.nc_insets(U(e, 1), U(e, 3), U(e, 2) != 0, r.w());
        write_rect(e, A(e, 0), {r.l - ins.l, r.t - ins.t, r.r + ins.r, r.b + ins.b});
        ret(e, 1);
    });
    api(e, "AdjustWindowRect", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Rect r = read_rect(e, A(e, 0));
        Rect ins = g.nc_insets(U(e, 1), 0, U(e, 2) != 0, r.w());
        write_rect(e, A(e, 0), {r.l - ins.l, r.t - ins.t, r.r + ins.r, r.b + ins.b});
        ret(e, 1);
    });
    api(e, "WindowFromPoint", 2, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = g.window_from_point({I(e, 0), I(e, 1)});
        ret(e, h ? h : g.desktop);
    });
    api(e, "ChildWindowFromPoint", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        Point p{I(e, 1), I(e, 2)};
        if (!Rect{0, 0, w->crect.w(), w->crect.h()}.contains(p.x, p.y)) return ret(e, 0);
        uint32_t c = g.child_from_point(w, p, true);
        ret(e, c ? c : w->hwnd);
    });
    api(e, "ChildWindowFromPointEx", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        Point p{I(e, 1), I(e, 2)};
        if (!Rect{0, 0, w->crect.w(), w->crect.h()}.contains(p.x, p.y)) return ret(e, 0);
        uint32_t c = g.child_from_point(w, p, !(U(e, 3) & 2));
        ret(e, c ? c : w->hwnd);
    });
    api(e, "RealChildWindowFromPoint", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        uint32_t c = g.child_from_point(w, {I(e, 1), I(e, 2)}, true);
        ret(e, c ? c : w->hwnd);
    });
    api(e, "GetWindowPlacement", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        uint64_t p = A(e, 1);
        e.mem.write32(p, 44);
        e.mem.write32(p + 4, 0);
        e.mem.write32(p + 8, w->maximized ? 3 : (w->minimized ? 2 : 1));
        write_point(e, p + 12, {-1, -1});
        write_point(e, p + 20, {-1, -1});
        write_rect(e, p + 28, w->maximized && !w->normal_rect.empty() ? w->normal_rect : w->wrect);
        ret(e, 1);
    });
    api(e, "SetWindowPlacement", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        uint64_t p = A(e, 1);
        uint32_t show = e.mem.read32(p + 8);
        Rect r = read_rect(e, p + 28);
        w->normal_rect = r;
        if (show == 3) {
            g.show_window(w->hwnd, 3);
        } else {
            g.set_window_pos(w->hwnd, 0, r.l, r.t, r.w(), r.h(), SWP_NOZORDER | SWP_NOACTIVATE);
            if (show != 0) g.show_window(w->hwnd, static_cast<int>(show));
        }
        ret(e, 1);
    });
    api(e, "GetWindowRgn", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "SetWindowRgn", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "GetTitleBarInfo", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "GetWindowInfo", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        uint64_t p = A(e, 1);
        write_rect(e, p + 4, g.window_rect_screen(w));
        write_rect(e, p + 20, g.client_rect_screen(w));
        e.mem.write32(p + 36, w->style);
        e.mem.write32(p + 40, w->exstyle);
        e.mem.write32(p + 44, g.active == w->hwnd ? 1 : 0);
        ret(e, 1);
    });

    // ---- focus, activation, capture -----------------------------------------------
    api(e, "SetFocus", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t old = g.focus;
        uint32_t h = U(e, 0);
        if (h && !g.win(h)) return ret(e, 0);
        if (h) {
            Window* w = g.win(h);
            if (w->style & WS_DISABLED) return ret(e, 0);
        }
        g.set_focus(h);
        ret(e, old);
    });
    api(e, "GetFocus", 0, [](Emulator& e) { ret(e, gui(e).focus); });
    api(e, "SetActiveWindow", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t old = g.active;
        g.set_active(U(e, 0));
        ret(e, old);
    });
    api(e, "GetActiveWindow", 0, [](Emulator& e) { ret(e, gui(e).active); });
    api(e, "GetForegroundWindow", 0, [](Emulator& e) { ret(e, gui(e).active); });
    api(e, "SetForegroundWindow", 1, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (w) {
            Window* t = g.toplevel_of(w);
            if (t) g.set_active(t->hwnd);
        }
        ret(e, w ? 1 : 0);
    });
    api(e, "LockSetForegroundWindow", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "AllowSetForegroundWindow", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "SetCapture", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t old = g.capture;
        g.capture = U(e, 0);
        if (old && old != g.capture && g.win(old)) g.send(old, WM_CAPTURECHANGED, 0, g.capture);
        ret(e, old);
    });
    api(e, "ReleaseCapture", 0, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t old = g.capture;
        g.capture = 0;
        if (old && g.win(old)) g.send(old, WM_CAPTURECHANGED, 0, 0);
        ret(e, 1);
    });
    api(e, "GetCapture", 0, [](Emulator& e) { ret(e, gui(e).capture); });
    api(e, "DragDetect", 3, [](Emulator& e) { ret(e, 0); });

    // ---- messages ----------------------------------------------------------------------------
    api(e, "GetMessageW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Msg m;
        g.get_message(m, U(e, 1), U(e, 2), U(e, 3), true, true);
        g.last_msg = m;
        if (m.message == WM_QUIT) g.quit_posted = false;
        g.write_msg(A(e, 0), m);
        ret(e, m.message == WM_QUIT ? 0 : 1);
    });
    e.alias_hook("GetMessageW", "GetMessageA");
    api(e, "PeekMessageW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        Msg m;
        bool remove = (U(e, 4) & 1) != 0;
        if (!g.get_message(m, U(e, 1), U(e, 2), U(e, 3), remove, false)) return ret(e, 0);
        if (remove) g.last_msg = m;
        if (A(e, 0)) g.write_msg(A(e, 0), m);
        ret(e, 1);
    });
    e.alias_hook("PeekMessageW", "PeekMessageA");
    api(e, "WaitMessage", 0, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.posted.empty() && g.input.empty()) {
            g.present_now();
            uint64_t now = g.host->now_ms(), next = 0;
            for (auto& t : g.timers)
                if (!next || t.due < next) next = t.due;
            bool paint = g.find_paint_window(g.desktop, 0) != nullptr;
            if (!paint) g.pump_host(next ? static_cast<int>(next > now ? next - now : 0) : 100);
        }
        ret(e, 1);
    });
    api(e, "MsgWaitForMultipleObjects", 5, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.posted.empty() && g.input.empty()) {
            g.present_now();
            uint32_t to = U(e, 3);
            g.pump_host(to == 0xFFFFFFFF ? 50 : static_cast<int>(std::min<uint32_t>(to, 50)));
        }
        ret(e, U(e, 0));  // WAIT_OBJECT_0 + count: input is available
    });
    api(e, "MsgWaitForMultipleObjectsEx", 5, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.posted.empty() && g.input.empty()) {
            g.present_now();
            g.pump_host(std::min<int>(I(e, 2) < 0 ? 50 : I(e, 2), 50));
        }
        ret(e, U(e, 0));
    });
    api(e, "GetQueueStatus", 1, [](Emulator& e) {
        Gui& g = gui(e);
        g.pump_host(0);
        uint32_t s = 0;
        if (!g.posted.empty()) s |= 0x08;  // QS_POSTMESSAGE
        if (!g.input.empty()) s |= 0x07;
        if (g.find_paint_window(g.desktop, 0)) s |= 0x20;
        ret(e, (s << 16) | s);
    });
    api(e, "GetInputState", 0, [](Emulator& e) {
        Gui& g = gui(e);
        g.pump_host(0);
        ret(e, g.input.empty() ? 0 : 1);
    });
    api(e, "TranslateMessage", 1, [](Emulator& e) {
        Gui& g = gui(e);
        Msg m = g.read_msg(A(e, 0));
        if (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN) return ret(e, 0);
        int vk = static_cast<int>(m.wparam);
        uint32_t ch = 0;
        bool ctrl = (g.key_state[0x11] & 0x80) != 0;
        // Printable characters arrive from the host as their own events;
        // these are the ones only a key produces.
        if (vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B) ch = static_cast<uint32_t>(vk);
        else if (ctrl && vk >= 'A' && vk <= 'Z') ch = static_cast<uint32_t>(vk - 'A' + 1);
        if (ch) g.post(m.hwnd, m.message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, m.lparam);
        ret(e, ch ? 1 : 0);
    });
    api(e, "DispatchMessageW", 1, [](Emulator& e) {
        Gui& g = gui(e);
        Msg m = g.read_msg(A(e, 0));
        if (m.message == WM_TIMER && m.lparam) {
            g.call_wndproc(m.lparam, m.hwnd, WM_TIMER, m.wparam, m.time);
            return ret(e, 0);
        }
        Window* w = g.win(m.hwnd);
        if (!w) return ret(e, 0);
        if (m.message == WM_PAINT) {
            bool had = !w->update.empty() || w->internal_paint || w->need_ncpaint;
            uint64_t r = g.send(m.hwnd, WM_PAINT, 0, 0);
            if (had && (w = g.win(m.hwnd))) {
                if (!w->update.empty() || w->internal_paint) {
                    w->update.clear();
                    w->internal_paint = false;
                    w->need_erase = false;
                }
                w->need_ncpaint = false;
            }
            return ret(e, r);
        }
        ret(e, g.send(m.hwnd, m.message, m.wparam, m.lparam));
    });
    e.alias_hook("DispatchMessageW", "DispatchMessageA");
    api(e, "SendMessageW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        if (h == 0xFFFF) {  // HWND_BROADCAST
            for (uint32_t th : g.win(g.desktop)->children) g.send(th, U(e, 1), A(e, 2), A(e, 3));
            return ret(e, 1);
        }
        ret(e, g.send(h, U(e, 1), A(e, 2), A(e, 3)));
    });
    api(e, "SendMessageTimeoutW", 7, [](Emulator& e) {
        uint64_t r = gui(e).send(U(e, 0), U(e, 1), A(e, 2), A(e, 3));
        if (A(e, 6)) e.mem.write32(A(e, 6), static_cast<uint32_t>(r));
        ret(e, 1);
    });
    api(e, "SendNotifyMessageW", 4, [](Emulator& e) {
        ret(e, gui(e).send(U(e, 0), U(e, 1), A(e, 2), A(e, 3)) | 1);
    });
    api(e, "PostMessageW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        if (h && h != 0xFFFF && !g.win(h)) return ret(e, 0);
        g.post(h, U(e, 1), A(e, 2), A(e, 3));
        ret(e, 1);
    });
    e.alias_hook("PostMessageW", "PostMessageA");
    api(e, "PostThreadMessageW", 4, [](Emulator& e) {
        gui(e).post(0, U(e, 1), A(e, 2), A(e, 3));
        ret(e, 1);
    });
    api(e, "PostQuitMessage", 1, [](Emulator& e) {
        Gui& g = gui(e);
        g.quit_posted = true;
        g.quit_code = I(e, 0);
        ret(e, 0);
    });
    api(e, "CallWindowProcW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        ret(e, g.call_wndproc(A(e, 0), U(e, 1), U(e, 2), A(e, 3), A(e, 4)));
    });
    e.alias_hook("CallWindowProcW", "CallWindowProcA");
    api(e, "InSendMessage", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "InSendMessageEx", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "ReplyMessage", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GetMessageTime", 0, [](Emulator& e) { ret(e, gui(e).last_msg.time); });
    api(e, "GetMessagePos", 0, [](Emulator& e) {
        Gui& g = gui(e);
        ret(e, make_lparam(g.last_msg.pt.x, g.last_msg.pt.y));
    });
    api(e, "GetMessageExtraInfo", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "RegisterWindowMessageW", 1, [](Emulator& e) {
        ret(e, gui(e).add_atom(read_wstr(e, A(e, 0))));
    });
    api(e, "RegisterWindowMessageA", 1, [](Emulator& e) {
        ret(e, gui(e).add_atom(cp932_to_utf16(e.mem.read_cstring(A(e, 0)))));
    });
    api(e, "SetWindowsHookExW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        HookEntry h;
        h.type = I(e, 0);
        h.proc = A(e, 1);
        h.handle = g.next_hook;
        g.next_hook += 4;
        g.hooks.push_back(h);
        ret(e, h.handle);
    });
    e.alias_hook("SetWindowsHookExW", "SetWindowsHookExA");
    api(e, "SetWindowsHookW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        HookEntry h;
        h.type = I(e, 0);
        h.proc = A(e, 1);
        h.handle = g.next_hook;
        g.next_hook += 4;
        g.hooks.push_back(h);
        ret(e, h.handle);
    });
    api(e, "UnhookWindowsHookEx", 1, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = std::find_if(g.hooks.begin(), g.hooks.end(), [&](const HookEntry& h) { return h.handle == U(e, 0); });
        if (it == g.hooks.end()) return ret(e, 0);
        g.hooks.erase(it);
        ret(e, 1);
    });
    api(e, "CallNextHookEx", 4, [](Emulator& e) {
        Gui& g = gui(e);
        // The next hook of the same type below this one in the chain.
        uint32_t self = U(e, 0);
        int type = -100;
        size_t pos = g.hooks.size();
        for (size_t i = 0; i < g.hooks.size(); ++i)
            if (g.hooks[i].handle == self) {
                type = g.hooks[i].type;
                pos = i;
            }
        if (type == -100) return ret(e, 0);
        for (size_t i = pos; i-- > 0;) {
            if (g.hooks[i].type != type) continue;
            return ret(e, e.call_guest(g.hooks[i].proc, {A(e, 1), A(e, 2), A(e, 3)}) & 0xFFFFFFFF);
        }
        ret(e, 0);
    });
    api(e, "CallMsgFilterW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.call_hook(6, I(e, 1), 0, A(e, 0))) return ret(e, 1);   // WH_SYSMSGFILTER
        ret(e, g.call_hook(-1, I(e, 1), 0, A(e, 0)) ? 1 : 0);          // WH_MSGFILTER
    });
    e.alias_hook("CallMsgFilterW", "CallMsgFilterA");

    // ---- timers ------------------------------------------------------------------------------
    api(e, "SetTimer", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        uint64_t id = A(e, 1);
        uint32_t interval = std::max<uint32_t>(U(e, 2), 10);
        if (!h && !id) {
            static uint64_t next_id = 0x7F00;
            id = next_id++;
        }
        for (auto& t : g.timers)
            if (t.hwnd == h && t.id == id) {
                t.interval = interval;
                t.due = g.host->now_ms() + interval;
                t.proc = A(e, 3);
                return ret(e, id ? id : 1);
            }
        Timer t;
        t.hwnd = h;
        t.id = id;
        t.interval = interval;
        t.due = g.host->now_ms() + interval;
        t.proc = A(e, 3);
        g.timers.push_back(t);
        ret(e, id ? id : 1);
    });
    api(e, "KillTimer", 2, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = std::find_if(g.timers.begin(), g.timers.end(),
                               [&](const Timer& t) { return t.hwnd == U(e, 0) && t.id == A(e, 1); });
        if (it == g.timers.end()) return ret(e, 0);
        g.timers.erase(it);
        ret(e, 1);
    });

    // ---- painting -------------------------------------------------------------------------------
    api(e, "BeginPaint", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Rect rc;
        bool erase = false;
        uint32_t hdc = begin_paint(g, U(e, 0), rc, erase);
        uint64_t ps = A(e, 1);
        e.mem.write32(ps, hdc);
        e.mem.write32(ps + 4, erase ? 1 : 0);
        write_rect(e, ps + 8, rc);
        for (int i = 24; i < 64; i += 4) e.mem.write32(ps + i, 0);
        ret(e, hdc);
    });
    api(e, "EndPaint", 2, [](Emulator& e) {
        Gui& g = gui(e);
        end_paint(g, U(e, 0), e.mem.read32(A(e, 1)));
        ret(e, 1);
    });
    api(e, "GetDC", 1, [](Emulator& e) { ret(e, get_dc(gui(e), U(e, 0), false, nullptr)); });
    api(e, "GetWindowDC", 1, [](Emulator& e) { ret(e, get_dc(gui(e), U(e, 0), true, nullptr)); });
    api(e, "GetDCEx", 3, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t flags = U(e, 2);
        GdiObject* r = (flags & 0x80) ? g.obj(A(e, 1), ObjKind::Region) : nullptr;  // DCX_INTERSECTRGN
        Region rg;
        if (r) {
            // GetDCEx region is in screen coordinates... except for
            // DCX_WINDOW/CLIPxx combos; treat as client-relative when small.
            rg = r->region;
            Window* w = g.win(A(e, 0));
            if (w) {
                Rect sr = (flags & 1) ? g.window_rect_screen(w) : g.client_rect_screen(w);
                (void)sr;
            }
        }
        ret(e, get_dc(g, U(e, 0), (flags & 1) != 0, r ? &rg : nullptr, flags));  // DCX_WINDOW
    });
    api(e, "ReleaseDC", 2, [](Emulator& e) {
        release_dc(gui(e), U(e, 1));
        ret(e, 1);
    });
    api(e, "WindowFromDC", 1, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        ret(e, d && d->kind == DC::Kind::Window ? d->hwnd : 0);
    });
    api(e, "InvalidateRect", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!A(e, 0)) {
            g.invalidate(g.win(g.desktop), nullptr, U(e, 2) != 0, true);
            return ret(e, 1);
        }
        if (!w) return ret(e, 0);
        if (A(e, 1)) {
            Rect r = read_rect(e, A(e, 1));
            Region rg(r);
            g.invalidate(w, &rg, U(e, 2) != 0, !(w->style & WS_CLIPCHILDREN));
        } else {
            g.invalidate(w, nullptr, U(e, 2) != 0, !(w->style & WS_CLIPCHILDREN));
        }
        ret(e, 1);
    });
    api(e, "InvalidateRgn", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        g.invalidate(w, o ? &o->region : nullptr, U(e, 2) != 0, !(w->style & WS_CLIPCHILDREN));
        ret(e, 1);
    });
    api(e, "ValidateRect", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        if (A(e, 1)) {
            Region rg(read_rect(e, A(e, 1)));
            g.validate(w, &rg);
        } else {
            g.validate(w, nullptr);
        }
        ret(e, 1);
    });
    api(e, "ValidateRgn", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        g.validate(w, o ? &o->region : nullptr);
        ret(e, 1);
    });
    api(e, "GetUpdateRect", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        Rect b = w->update.bounds();
        if (A(e, 1)) write_rect(e, A(e, 1), b);
        if (U(e, 2) && !w->update.empty() && w->need_erase) {
            uint32_t hdc = get_dc(g, w->hwnd, false, nullptr);
            DC* d = g.dc(hdc);
            Point o = g.surface_origin(w, true);
            Region pc = w->update;
            pc.offset(o.x, o.y);
            d->paint_clip = pc;
            d->has_paint_clip = true;
            g.send(w->hwnd, WM_ERASEBKGND, hdc, 0);
            release_dc(g, hdc);
            w->need_erase = false;
        }
        ret(e, b.empty() ? 0 : 1);
    });
    api(e, "GetUpdateRgn", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!w || !o) return ret(e, 0);
        o->region = w->update;
        ret(e, w->update.empty() ? 1 : (w->update.is_rect() ? 2 : 3));
    });
    api(e, "UpdateWindow", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        // Paint this window and its children now, synchronously.
        for (int guard = 0; guard < 1000; ++guard) {
            Window* p = g.find_paint_window(h, h);
            if (!p) break;
            uint32_t ph = p->hwnd;
            if (p->need_ncpaint && p->update.empty() && !p->internal_paint) {
                p->need_ncpaint = false;
                g.send(ph, WM_NCPAINT, 1, 0);
                continue;
            }
            Msg m;
            m.hwnd = ph;
            m.message = WM_PAINT;
            g.dispatch(m);
        }
        ret(e, 1);
    });
    api(e, "RedrawWindow", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0) ? U(e, 0) : g.desktop;
        Window* w = g.win(h);
        if (!w) return ret(e, 0);
        uint32_t f = U(e, 3);
        Region rg;
        const Region* prg = nullptr;
        if (A(e, 1)) {
            rg.set_rect(read_rect(e, A(e, 1)));
            prg = &rg;
        } else if (A(e, 2)) {
            GdiObject* o = g.obj(A(e, 2), ObjKind::Region);
            if (o) {
                rg = o->region;
                prg = &rg;
            }
        }
        bool children = (f & 0x80) || (!(f & 0x40) && !(w->style & WS_CLIPCHILDREN));  // ALLCHILDREN / NOCHILDREN
        if (f & 0x1) g.invalidate(w, prg, (f & 0x4) != 0, children, (f & 0x400) != 0);  // RDW_INVALIDATE
        if (f & 0x2) w->internal_paint = true;  // RDW_INTERNALPAINT
        if (f & 0x8) {  // RDW_VALIDATE
            g.validate(w, prg);
        }
        if (f & 0x400) w->need_ncpaint = true;  // RDW_FRAME
        if (f & 0x100) {  // RDW_UPDATENOW
            for (int guard = 0; guard < 1000; ++guard) {
                Window* p = g.find_paint_window(h, h);
                if (!p) break;
                if (p->need_ncpaint && p->update.empty() && !p->internal_paint) {
                    p->need_ncpaint = false;
                    g.send(p->hwnd, WM_NCPAINT, 1, 0);
                    continue;
                }
                Msg m;
                m.hwnd = p->hwnd;
                m.message = WM_PAINT;
                g.dispatch(m);
            }
        } else if (f & 0x200) {  // RDW_ERASENOW
            Window* p = g.win(h);
            if (p && p->need_erase && !p->update.empty()) {
                uint32_t hdc = get_dc(g, h, false, nullptr);
                g.send(h, WM_ERASEBKGND, hdc, 0);
                release_dc(g, hdc);
                p->need_erase = false;
            }
        }
        ret(e, 1);
    });
    api(e, "LockWindowUpdate", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "ScrollWindow", 5, [](Emulator& e) {
        // Scrolling by repainting: correct output, without the blit.
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        // Children move with the contents.
        int dx = I(e, 1), dy = I(e, 2);
        if (!A(e, 3)) {
            for (uint32_t c : w->children) {
                Window* k = g.win(c);
                if (!k) continue;
                k->wrect = k->wrect.offset(dx, dy);
                k->crect = k->crect.offset(dx, dy);
            }
            ++g.geom_ver;
        }
        g.invalidate(w, nullptr, true, true);
        ret(e, 1);
    });
    api(e, "ScrollWindowEx", 8, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        int dx = I(e, 1), dy = I(e, 2);
        if (U(e, 7) & 1) {  // SW_SCROLLCHILDREN
            for (uint32_t c : w->children) {
                Window* k = g.win(c);
                if (!k) continue;
                k->wrect = k->wrect.offset(dx, dy);
                k->crect = k->crect.offset(dx, dy);
            }
            ++g.geom_ver;
        }
        if (U(e, 7) & 2) g.invalidate(w, nullptr, (U(e, 7) & 4) != 0, true);  // SW_INVALIDATE
        if (A(e, 6)) write_rect(e, A(e, 6), {0, 0, w->crect.w(), w->crect.h()});
        ret(e, 3);
    });
    api(e, "ScrollDC", 7, [](Emulator& e) {
        if (A(e, 6)) write_rect(e, A(e, 6), {});
        ret(e, 1);
    });
    api(e, "ExcludeUpdateRgn", 2, [](Emulator& e) { ret(e, 2); });
    api(e, "GdiFlush", 0, [](Emulator& e) { ret(e, 1); });

    // ---- rectangles ----------------------------------------------------------------------
    api(e, "SetRect", 5, [](Emulator& e) {
        write_rect(e, A(e, 0), {I(e, 1), I(e, 2), I(e, 3), I(e, 4)});
        ret(e, 1);
    });
    api(e, "SetRectEmpty", 1, [](Emulator& e) {
        write_rect(e, A(e, 0), {});
        ret(e, 1);
    });
    api(e, "CopyRect", 2, [](Emulator& e) {
        if (!A(e, 0) || !A(e, 1)) return ret(e, 0);
        write_rect(e, A(e, 0), read_rect(e, A(e, 1)));
        ret(e, 1);
    });
    api(e, "IsRectEmpty", 1, [](Emulator& e) { ret(e, !A(e, 0) || read_rect(e, A(e, 0)).empty() ? 1 : 0); });
    api(e, "EqualRect", 2, [](Emulator& e) { ret(e, read_rect(e, A(e, 0)) == read_rect(e, A(e, 1)) ? 1 : 0); });
    api(e, "PtInRect", 3, [](Emulator& e) { ret(e, read_rect(e, A(e, 0)).contains(I(e, 1), I(e, 2)) ? 1 : 0); });
    api(e, "OffsetRect", 3, [](Emulator& e) {
        write_rect(e, A(e, 0), read_rect(e, A(e, 0)).offset(I(e, 1), I(e, 2)));
        ret(e, 1);
    });
    api(e, "InflateRect", 3, [](Emulator& e) {
        Rect r = read_rect(e, A(e, 0));
        write_rect(e, A(e, 0), {r.l - I(e, 1), r.t - I(e, 2), r.r + I(e, 1), r.b + I(e, 2)});
        ret(e, 1);
    });
    api(e, "IntersectRect", 3, [](Emulator& e) {
        Rect a = read_rect(e, A(e, 1)), b = read_rect(e, A(e, 2));
        Rect r = a.empty() || b.empty() ? Rect{} : a.intersect(b);
        write_rect(e, A(e, 0), r);
        ret(e, r.empty() ? 0 : 1);
    });
    api(e, "UnionRect", 3, [](Emulator& e) {
        Rect a = read_rect(e, A(e, 1)), b = read_rect(e, A(e, 2));
        Rect r = a.unite(b);
        write_rect(e, A(e, 0), r);
        ret(e, r.empty() ? 0 : 1);
    });
    api(e, "SubtractRect", 3, [](Emulator& e) {
        Rect a = read_rect(e, A(e, 1)), b = read_rect(e, A(e, 2));
        Rect r = a;
        Rect x = a.intersect(b);
        if (!x.empty()) {
            if (x.t == a.t && x.b == a.b) {
                if (x.l == a.l) r.l = x.r;
                else if (x.r == a.r) r.r = x.l;
            } else if (x.l == a.l && x.r == a.r) {
                if (x.t == a.t) r.t = x.b;
                else if (x.b == a.b) r.b = x.t;
            }
        }
        write_rect(e, A(e, 0), r);
        ret(e, r.empty() ? 0 : 1);
    });

    // ---- drawing helpers in user32 -----------------------------------------------------------
    api(e, "FillRect", 3, [](Emulator& e) {
        draw::fill_rect_brush(gui(e), U(e, 0), read_rect(e, A(e, 1)), U(e, 2));
        ret(e, 1);
    });
    api(e, "FrameRect", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Rect r = read_rect(e, A(e, 1));
        uint32_t b = U(e, 2);
        draw::fill_rect_brush(g, U(e, 0), {r.l, r.t, r.r, r.t + 1}, b);
        draw::fill_rect_brush(g, U(e, 0), {r.l, r.b - 1, r.r, r.b}, b);
        draw::fill_rect_brush(g, U(e, 0), {r.l, r.t, r.l + 1, r.b}, b);
        draw::fill_rect_brush(g, U(e, 0), {r.r - 1, r.t, r.r, r.b}, b);
        ret(e, 1);
    });
    api(e, "InvertRect", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        int old = d->s.rop2;
        d->s.rop2 = 6;
        draw::fill_rect(g, U(e, 0), read_rect(e, A(e, 1)), 0);
        d->s.rop2 = old;
        ret(e, 1);
    });
    api(e, "DrawFocusRect", 2, [](Emulator& e) {
        draw::focus_rect(gui(e), U(e, 0), read_rect(e, A(e, 1)));
        ret(e, 1);
    });
    api(e, "DrawTextW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string s = read_wstr(e, A(e, 1), I(e, 2) < 0 ? -1 : I(e, 2));
        Rect r = read_rect(e, A(e, 3));
        int h = draw_text_impl(g, U(e, 0), s, r, U(e, 4));
        if (U(e, 4) & 0x400) write_rect(e, A(e, 3), r);
        ret(e, static_cast<uint64_t>(h));
    });
    api(e, "DrawTextA", 5, [](Emulator& e) {
        Gui& g = gui(e);
        std::string a;
        if (I(e, 2) < 0) a = e.mem.read_cstring(A(e, 1));
        else {
            a.resize(static_cast<size_t>(I(e, 2)));
            e.mem.read(A(e, 1), a.data(), a.size());
        }
        Rect r = read_rect(e, A(e, 3));
        int h = draw_text_impl(g, U(e, 0), cp932_to_utf16(a), r, U(e, 4));
        if (U(e, 4) & 0x400) write_rect(e, A(e, 3), r);
        ret(e, static_cast<uint64_t>(h));
    });
    api(e, "DrawTextExW", 6, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string s = read_wstr(e, A(e, 1), I(e, 2) < 0 ? -1 : I(e, 2));
        Rect r = read_rect(e, A(e, 3));
        int tab = 8;
        if (A(e, 5)) tab = static_cast<int>(e.mem.read32(A(e, 5) + 4));
        int h = draw_text_impl(g, U(e, 0), s, r, U(e, 4), tab ? tab : 8);
        if (U(e, 4) & 0x400) write_rect(e, A(e, 3), r);
        ret(e, static_cast<uint64_t>(h));
    });
    api(e, "TabbedTextOutW", 8, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string s = read_wstr(e, A(e, 3), I(e, 4));
        int tabw = 8 * dc_realfont(g, U(e, 0))->ave_width;
        if (I(e, 5) > 0 && A(e, 6)) tabw = static_cast<int32_t>(e.mem.read32(A(e, 6)));
        int x = I(e, 1), start = I(e, 7);
        std::u16string run;
        RealFont* f = dc_realfont(g, U(e, 0));
        auto flush = [&]() {
            if (!run.empty()) {
                dc_text_out(g, U(e, 0), x, I(e, 2), 0, nullptr, run);
                for (char16_t c : run) x += f->advance(c);
                run.clear();
            }
        };
        for (char16_t c : s) {
            if (c == u'\t') {
                flush();
                if (tabw > 0) x = start + ((x - start) / tabw + 1) * tabw;
            } else {
                run.push_back(c);
            }
        }
        flush();
        ret(e, make_lparam(x - I(e, 1), f->height));
    });
    api(e, "GetTabbedTextExtentW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        RealFont* f = dc_realfont(g, U(e, 0));
        std::u16string s = read_wstr(e, A(e, 1), I(e, 2));
        int tabw = 8 * f->ave_width;
        if (I(e, 3) > 0 && A(e, 4)) tabw = static_cast<int32_t>(e.mem.read32(A(e, 4)));
        int x = 0;
        for (char16_t c : s) {
            if (c == u'\t') {
                if (tabw > 0) x = (x / tabw + 1) * tabw;
            } else {
                x += f->advance(c);
            }
        }
        ret(e, make_lparam(x, f->height));
    });
    api(e, "GrayStringW", 9, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        if (A(e, 2)) {
            // A callback draws it; give it the DC directly.
            return ret(e, e.call_guest(A(e, 2), {A(e, 0), A(e, 3), A(e, 4)}) ? 1 : 0);
        }
        std::u16string s = read_wstr(e, A(e, 3), I(e, 4) > 0 ? I(e, 4) : -1);
        uint32_t old = d->s.text_color;
        d->s.text_color = g.sys_color(17);  // COLOR_GRAYTEXT
        dc_text_out(g, U(e, 0), I(e, 5), I(e, 6), 0, nullptr, s);
        d->s.text_color = old;
        ret(e, 1);
    });

    // ---- system metrics, colours, parameters ---------------------------------------------
    api(e, "GetSystemMetrics", 1, [](Emulator& e) {
        Gui& g = gui(e);
        switch (I(e, 0)) {
            case 0: return ret(e, static_cast<uint64_t>(g.screen_w));   // SM_CXSCREEN
            case 1: return ret(e, static_cast<uint64_t>(g.screen_h));   // SM_CYSCREEN
            case 2: return ret(e, kScrollW);   // SM_CXVSCROLL
            case 3: return ret(e, kScrollW);   // SM_CYHSCROLL
            case 4: return ret(e, kCaptionH);  // SM_CYCAPTION
            case 5: case 6: return ret(e, 1);  // SM_CXBORDER
            case 7: case 8: return ret(e, kDlgFrame);  // SM_CXDLGFRAME
            case 9: return ret(e, kScrollW);   // SM_CYVTHUMB
            case 10: return ret(e, kScrollW);  // SM_CXHTHUMB
            case 11: case 12: return ret(e, 32);  // SM_CXICON
            case 13: case 14: return ret(e, 32);  // SM_CXCURSOR
            case 15: return ret(e, kMenuH);    // SM_CYMENU
            case 16: return ret(e, static_cast<uint64_t>(g.screen_w));  // SM_CXFULLSCREEN
            case 17: return ret(e, static_cast<uint64_t>(g.screen_h - kCaptionH));
            case 19: return ret(e, 1);   // SM_MOUSEPRESENT
            case 20: return ret(e, kScrollW);  // SM_CYVSCROLL
            case 21: return ret(e, kScrollW);  // SM_CXHSCROLL
            case 22: return ret(e, 0);   // SM_DEBUG
            case 23: return ret(e, 0);   // SM_SWAPBUTTON
            case 28: return ret(e, 112);  // SM_CXMIN
            case 29: return ret(e, 27);   // SM_CYMIN
            case 30: case 31: return ret(e, 36);  // SM_CXSIZE / SM_CYSIZE...
            case 32: case 33: return ret(e, 4);   // SM_CXFRAME
            case 34: return ret(e, 112);
            case 35: return ret(e, 27);
            case 36: case 37: return ret(e, 4);   // SM_CXDOUBLECLK
            case 38: return ret(e, 75);   // SM_CXICONSPACING
            case 39: return ret(e, 75);
            case 40: return ret(e, 0);    // SM_MENUDROPALIGNMENT
            case 42: return ret(e, 1);    // SM_DBCSENABLED
            case 43: return ret(e, 5);    // SM_CMOUSEBUTTONS
            case 45: case 46: return ret(e, 2);   // SM_CXEDGE
            case 49: case 50: return ret(e, 16);  // SM_CXSMICON
            case 51: return ret(e, kSmCaptionH);  // SM_CYSMCAPTION
            case 52: case 53: return ret(e, 22);  // SM_CXSMSIZE
            case 54: case 55: return ret(e, 19);  // SM_CXMENUSIZE
            case 56: return ret(e, 8);    // SM_ARRANGE
            case 57: return ret(e, 160);  // SM_CXMINIMIZED
            case 58: return ret(e, 28);
            case 59: return ret(e, static_cast<uint64_t>(g.screen_w + 16));  // SM_CXMAXTRACK
            case 60: return ret(e, static_cast<uint64_t>(g.screen_h + 16));
            case 61: return ret(e, static_cast<uint64_t>(g.screen_w + 16));  // SM_CXMAXIMIZED
            case 62: return ret(e, static_cast<uint64_t>(g.screen_h + 16));
            case 63: return ret(e, 3);    // SM_NETWORK
            case 67: return ret(e, 0);    // SM_CLEANBOOT
            case 68: case 69: return ret(e, 4);   // SM_CXDRAG
            case 70: return ret(e, 0);    // SM_SHOWSOUNDS
            case 71: case 72: return ret(e, 13);  // SM_CXMENUCHECK
            case 73: return ret(e, 0);    // SM_SLOWMACHINE
            case 74: return ret(e, 0);    // SM_MIDEASTENABLED
            case 75: return ret(e, 1);    // SM_MOUSEWHEELPRESENT
            case 76: case 77: return ret(e, 0);   // SM_XVIRTUALSCREEN
            case 78: return ret(e, static_cast<uint64_t>(g.screen_w));  // SM_CXVIRTUALSCREEN
            case 79: return ret(e, static_cast<uint64_t>(g.screen_h));
            case 80: return ret(e, 1);    // SM_CMONITORS
            case 81: return ret(e, 1);    // SM_SAMEDISPLAYFORMAT
            case 83: return ret(e, 0);    // SM_CXFOCUSBORDER
            case 92: return ret(e, 4);    // SM_CXPADDEDBORDER
            case 0x1000: return ret(e, 0);  // SM_REMOTESESSION
        }
        ret(e, 0);
    });
    api(e, "GetSystemMetricsForDpi", 2, [](Emulator& e) {
        uint64_t saved = e.cpu().regs[RSP];
        (void)saved;
        ret(e, 0);
    });
    api(e, "GetSysColor", 1, [](Emulator& e) { ret(e, gui(e).sys_color(I(e, 0))); });
    api(e, "GetSysColorBrush", 1, [](Emulator& e) { ret(e, get_sys_color_brush(gui(e), I(e, 0))); });
    api(e, "SetSysColors", 3, [](Emulator& e) { ret(e, 1); });
    api(e, "SystemParametersInfoW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t action = U(e, 0);
        uint64_t p = A(e, 2);
        auto write_lf = [&](uint64_t at, const char16_t* face, int h, int weight) {
            for (int i = 0; i < 92; i += 4) e.mem.write32(at + i, 0);
            e.mem.write32(at, static_cast<uint32_t>(h));
            e.mem.write32(at + 16, static_cast<uint32_t>(weight));
            e.mem.write8(at + 23, 128);  // SHIFTJIS_CHARSET
            write_wstr(e, at + 28, face, 32);
        };
        switch (action) {
            case 0x0029: {  // SPI_GETNONCLIENTMETRICS
                if (!p) return ret(e, 0);
                uint32_t size = e.mem.read32(p);
                e.mem.write32(p + 4, 1);          // iBorderWidth
                e.mem.write32(p + 8, kScrollW);   // iScrollWidth
                e.mem.write32(p + 12, kScrollW);  // iScrollHeight
                e.mem.write32(p + 16, 36);        // iCaptionWidth
                e.mem.write32(p + 20, kCaptionH - 1);
                write_lf(p + 24, u"Yu Gothic UI", -12, 400);   // lfCaptionFont
                e.mem.write32(p + 116, 22);
                e.mem.write32(p + 120, kSmCaptionH - 1);
                write_lf(p + 124, u"Yu Gothic UI", -12, 400);  // lfSmCaptionFont
                e.mem.write32(p + 216, 19);
                e.mem.write32(p + 220, kMenuH - 1);
                write_lf(p + 224, u"Yu Gothic UI", -12, 400);  // lfMenuFont
                write_lf(p + 316, u"Yu Gothic UI", -12, 400);  // lfStatusFont
                write_lf(p + 408, u"Yu Gothic UI", -12, 400);  // lfMessageFont
                if (size >= 504) e.mem.write32(p + 500, 4);    // iPaddedBorderWidth
                return ret(e, 1);
            }
            case 0x001F: {  // SPI_GETICONTITLELOGFONT
                if (p) write_lf(p, u"Yu Gothic UI", -12, 400);
                return ret(e, 1);
            }
            case 0x0030: {  // SPI_GETWORKAREA
                if (p) write_rect(e, p, {0, 0, g.screen_w, g.screen_h});
                return ret(e, 1);
            }
            case 0x0026:  // SPI_GETICONMETRICS
            case 0x002C:  // SPI_GETMINIMIZEDMETRICS
                return ret(e, 1);
            case 0x0068:  // SPI_GETWHEELSCROLLLINES
                if (p) e.mem.write32(p, 3);
                return ret(e, 1);
            case 0x006C:  // SPI_GETWHEELSCROLLCHARS
                if (p) e.mem.write32(p, 3);
                return ret(e, 1);
            case 0x0016:  // SPI_GETKEYBOARDDELAY
                if (p) e.mem.write32(p, 1);
                return ret(e, 1);
            case 0x000A:  // SPI_GETKEYBOARDSPEED
                if (p) e.mem.write32(p, 31);
                return ret(e, 1);
            case 0x1002:  // SPI_GETMENUANIMATION
            case 0x1004:  // SPI_GETCOMBOBOXANIMATION
            case 0x1006:  // SPI_GETLISTBOXSMOOTHSCROLLING
            case 0x1008:  // SPI_GETGRADIENTCAPTIONS
            case 0x100A:  // SPI_GETKEYBOARDCUES
            case 0x1012:  // SPI_GETMENUFADE
            case 0x1014:  // SPI_GETSELECTIONFADE
            case 0x1016:  // SPI_GETTOOLTIPANIMATION
            case 0x1018:  // SPI_GETTOOLTIPFADE
            case 0x101A:  // SPI_GETCURSORSHADOW
            case 0x1022:  // SPI_GETFLATMENU
            case 0x1024:  // SPI_GETDROPSHADOW
            case 0x103E:  // SPI_GETCLIENTAREAANIMATION
            case 0x0048:  // SPI_GETANIMATION
            case 0x0026 + 0x1000:
                if (p) e.mem.write32(p, action == 0x100A ? 1 : 0);
                return ret(e, 1);
            case 0x0025:  // SPI_GETDRAGFULLWINDOWS
                if (p) e.mem.write32(p, 1);
                return ret(e, 1);
            case 0x004A:  // SPI_GETFONTSMOOTHING
                if (p) e.mem.write32(p, 1);
                return ret(e, 1);
            case 0x200A:  // SPI_GETFONTSMOOTHINGTYPE
                if (p) e.mem.write32(p, 1);
                return ret(e, 1);
            case 0x0042:  // SPI_GETHIGHCONTRAST
                if (p) {
                    e.mem.write32(p + 4, 0);
                    e.mem.write32(p + 8, 0);
                }
                return ret(e, 1);
            case 0x0059:  // SPI_GETDEFAULTINPUTLANG
                if (p) e.mem.write32(p, 0x04110411);
                return ret(e, 1);
            case 0x0070:  // SPI_GETMOUSEHOVERWIDTH
            case 0x0064:  // SPI_GETMOUSEHOVERHEIGHT
                if (p) e.mem.write32(p, 4);
                return ret(e, 1);
            case 0x0066:  // SPI_GETMOUSEHOVERTIME
                if (p) e.mem.write32(p, 400);
                return ret(e, 1);
            case 0x0076 - 0x1:  // SPI_GETMENUSHOWDELAY 0x6A
            case 0x006A:
                if (p) e.mem.write32(p, 400);
                return ret(e, 1);
            case 0x200C:  // SPI_GETFOCUSBORDERWIDTH
            case 0x200E:
                if (p) e.mem.write32(p, 1);
                return ret(e, 1);
            case 0x2000:  // SPI_GETFOREGROUNDLOCKTIMEOUT
                if (p) e.mem.write32(p, 0);
                return ret(e, 1);
        }
        if (p && (action & 0xF000) == 0x1000) e.mem.write32(p, 0);
        ret(e, 1);
    });
    api(e, "GetDoubleClickTime", 0, [](Emulator& e) { ret(e, 500); });
    api(e, "GetCaretBlinkTime", 0, [](Emulator& e) { ret(e, 530); });
    api(e, "SetCaretBlinkTime", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "MessageBeep", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "GetProcessDefaultLayout", 1, [](Emulator& e) {
        if (A(e, 0)) e.mem.write32(A(e, 0), 0);
        ret(e, 1);
    });
    api(e, "SetProcessDefaultLayout", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "SetProcessDPIAware", 0, [](Emulator& e) { ret(e, 1); });
    api(e, "IsProcessDPIAware", 0, [](Emulator& e) { ret(e, 1); });
    api(e, "SetProcessDpiAwarenessContext", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "GetDpiForWindow", 1, [](Emulator& e) { ret(e, 96); });
    api(e, "GetDpiForSystem", 0, [](Emulator& e) { ret(e, 96); });
    api(e, "SetThreadDpiAwarenessContext", 1, [](Emulator& e) { ret(e, 0x11); });
    api(e, "GetThreadDpiAwarenessContext", 0, [](Emulator& e) { ret(e, 0x11); });
    api(e, "GetAwarenessFromDpiAwarenessContext", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "IsWindowUnicode", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w && !w->ansi ? 1 : 0);
    });
    api(e, "GetGUIThreadInfo", 2, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t p = A(e, 1);
        if (!p) return ret(e, 0);
        e.mem.write32(p + 4, 0);
        e.mem.write32(p + 8, g.active);
        e.mem.write32(p + 12, g.focus);
        e.mem.write32(p + 16, g.capture);
        e.mem.write32(p + 20, 0);
        e.mem.write32(p + 24, 0);
        e.mem.write32(p + 28, g.caret_hwnd);
        ret(e, 1);
    });
    api(e, "NotifyWinEvent", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "SetWinEventHook", 7, [](Emulator& e) { ret(e, 0x00EE0001); });
    api(e, "UnhookWinEvent", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "SetLayeredWindowAttributes", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "UpdateLayeredWindow", 9, [](Emulator& e) { ret(e, 1); });
    api(e, "FlashWindow", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "FlashWindowEx", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "WinHelpW", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "IsHungAppWindow", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "SetLastErrorEx", 2, [](Emulator& e) {
        e.set_last_error(U(e, 0));
        ret(e, 0);
    });
    api(e, "LockWorkStation", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "ExitWindowsEx", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "ReuseDDElParam", 5, [](Emulator& e) { ret(e, make_lparam(static_cast<int>(A(e, 3)), static_cast<int>(A(e, 4)))); });
    api(e, "UnpackDDElParam", 4, [](Emulator& e) {
        if (A(e, 2)) e.mem.write32(A(e, 2), static_cast<uint32_t>(A(e, 1) & 0xFFFF));
        if (A(e, 3)) e.mem.write32(A(e, 3), static_cast<uint32_t>(A(e, 1) >> 16));
        ret(e, 1);
    });
    api(e, "PackDDElParam", 3, [](Emulator& e) { ret(e, make_lparam(static_cast<int>(A(e, 1)), static_cast<int>(A(e, 2)))); });
    api(e, "FreeDDElParam", 2, [](Emulator& e) { ret(e, 1); });

    // Monitors: one.
    api(e, "MonitorFromWindow", 2, [](Emulator& e) { ret(e, 0x00010001); });
    api(e, "MonitorFromPoint", 3, [](Emulator& e) { ret(e, 0x00010001); });
    api(e, "MonitorFromRect", 2, [](Emulator& e) { ret(e, 0x00010001); });
    api(e, "GetMonitorInfoW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t p = A(e, 1);
        if (!p) return ret(e, 0);
        uint32_t size = e.mem.read32(p);
        write_rect(e, p + 4, {0, 0, g.screen_w, g.screen_h});
        write_rect(e, p + 20, {0, 0, g.screen_w, g.screen_h});
        e.mem.write32(p + 36, 1);  // MONITORINFOF_PRIMARY
        if (size >= 104) write_wstr(e, p + 40, u"\\\\.\\DISPLAY1", 32);
        ret(e, 1);
    });
    api(e, "EnumDisplayMonitors", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t rc = g.scratch_alloc(16);
        write_rect(e, rc, {0, 0, g.screen_w, g.screen_h});
        e.call_guest(A(e, 2), {0x00010001, A(e, 0), rc, A(e, 3)});
        g.scratch_free(rc);
        ret(e, 1);
    });
    api(e, "EnumDisplaySettingsW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t p = A(e, 2);
        if (!p) return ret(e, 0);
        e.mem.write32(p + 104, 32);  // dmBitsPerPel
        e.mem.write32(p + 108, static_cast<uint32_t>(g.screen_w));
        e.mem.write32(p + 112, static_cast<uint32_t>(g.screen_h));
        e.mem.write32(p + 120, 60);
        ret(e, 1);
    });
    api(e, "EnumDisplayDevicesW", 4, [](Emulator& e) { ret(e, 0); });

    // ---- keyboard ------------------------------------------------------------------
    api(e, "GetKeyState", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint8_t s = g.key_state[U(e, 0) & 0xFF];
        ret(e, static_cast<uint64_t>(static_cast<int16_t>((s & 0x80) ? 0xFF80 : 0) | (s & 1)) & 0xFFFF);
    });
    api(e, "GetAsyncKeyState", 1, [](Emulator& e) {
        Gui& g = gui(e);
        g.pump_host(0);
        ret(e, g.async_down[U(e, 0) & 0xFF] ? 0x8000 : 0);
    });
    api(e, "GetKeyboardState", 1, [](Emulator& e) {
        Gui& g = gui(e);
        e.mem.write(A(e, 0), g.key_state, 256);
        ret(e, 1);
    });
    api(e, "SetKeyboardState", 1, [](Emulator& e) {
        Gui& g = gui(e);
        e.mem.read(A(e, 0), g.key_state, 256);
        ret(e, 1);
    });
    api(e, "MapVirtualKeyW", 2, [](Emulator& e) {
        uint32_t code = U(e, 0), type = U(e, 1);
        if (type == 2) {  // MAPVK_VK_TO_CHAR
            if (code >= 'A' && code <= 'Z') return ret(e, code);
            if (code >= '0' && code <= '9') return ret(e, code);
            return ret(e, 0);
        }
        if (type == 0) {  // VK to scan
            static const uint8_t scan[] = {0, 0, 0, 0x46, 0, 0, 0, 0, 0x0E, 0x0F, 0, 0, 0x4C, 0x1C};
            if (code < sizeof scan) return ret(e, scan[code]);
            return ret(e, 0);
        }
        ret(e, 0);
    });
    e.alias_hook("MapVirtualKeyW", "MapVirtualKeyExW");
    e.alias_hook("MapVirtualKeyW", "MapVirtualKeyA");
    api(e, "GetKeyNameTextW", 3, [](Emulator& e) {
        uint32_t sc = (U(e, 0) >> 16) & 0xFF;
        std::u16string n;
        switch (sc) {
            case 0x1D: n = u"Ctrl"; break;
            case 0x2A: n = u"Shift"; break;
            case 0x38: n = u"Alt"; break;
            case 0x1C: n = u"Enter"; break;
            case 0x0E: n = u"Backspace"; break;
            case 0x01: n = u"Esc"; break;
            case 0x39: n = u"Space"; break;
            case 0x0F: n = u"Tab"; break;
            case 0x53: n = u"Del"; break;
            case 0x52: n = u"Ins"; break;
            default:
                if (sc >= 0x3B && sc <= 0x44) n = u"F" + from_utf8(std::to_string(sc - 0x3A));
                else n = u"?";
        }
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 1), n, I(e, 2))));
    });
    api(e, "ToUnicodeEx", 7, [](Emulator& e) {
        uint32_t vk = U(e, 0);
        char16_t c = 0;
        if (vk >= 'A' && vk <= 'Z') c = static_cast<char16_t>(vk + 32);
        else if (vk >= '0' && vk <= '9') c = static_cast<char16_t>(vk);
        else if (vk == 0x20) c = u' ';
        if (!c || !A(e, 3) || I(e, 4) < 1) return ret(e, 0);
        e.mem.write16(A(e, 3), c);
        ret(e, 1);
    });
    e.alias_hook("ToUnicodeEx", "ToUnicode");
    api(e, "VkKeyScanW", 1, [](Emulator& e) {
        uint32_t c = U(e, 0) & 0xFFFF;
        if (c >= 'a' && c <= 'z') return ret(e, c - 32);
        if (c >= 'A' && c <= 'Z') return ret(e, c | 0x100);
        if (c >= '0' && c <= '9') return ret(e, c);
        ret(e, 0xFFFF);
    });
    e.alias_hook("VkKeyScanW", "VkKeyScanExW");
    api(e, "GetKeyboardLayoutList", 2, [](Emulator& e) {
        if (I(e, 0) > 0 && A(e, 1)) e.mem.write32(A(e, 1), 0x04110411);
        ret(e, 1);
    });
    api(e, "GetKeyboardType", 1, [](Emulator& e) {
        switch (I(e, 0)) {
            case 0: return ret(e, 7);  // Japanese keyboard
            case 1: return ret(e, 2);
            case 2: return ret(e, 12);
        }
        ret(e, 0);
    });
    api(e, "ActivateKeyboardLayout", 2, [](Emulator& e) { ret(e, 0x04110411); });
    api(e, "BlockInput", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "keybd_event", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "SendInput", 3, [](Emulator& e) { ret(e, 0); });

    // ---- cursor and mouse ------------------------------------------------------------------
    api(e, "GetCursorPos", 1, [](Emulator& e) {
        Gui& g = gui(e);
        g.pump_host(0);
        // The latest position the host reported, even if not yet dispatched.
        Point p = g.cursor_pos;
        for (const auto& ev : g.input)
            if (ev.type == HostEvent::MouseMove || ev.type == HostEvent::MouseDown || ev.type == HostEvent::MouseUp)
                p = {ev.x, ev.y};
        write_point(e, A(e, 0), p);
        ret(e, 1);
    });
    e.alias_hook("GetCursorPos", "GetPhysicalCursorPos");
    api(e, "SetCursorPos", 2, [](Emulator& e) {
        gui(e).cursor_pos = {I(e, 0), I(e, 1)};
        ret(e, 1);
    });
    api(e, "ClipCursor", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "GetClipCursor", 1, [](Emulator& e) {
        Gui& g = gui(e);
        write_rect(e, A(e, 0), {0, 0, g.screen_w, g.screen_h});
        ret(e, 1);
    });
    api(e, "TrackMouseEvent", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "_TrackMouseEvent", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "SetCursor", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t old = g.current_cursor;
        g.current_cursor = U(e, 0);
        auto it = g.cursors.find(g.current_cursor);
        g.host->set_cursor(it != g.cursors.end() ? it->second->system_id : 0);
        ret(e, old);
    });
    api(e, "GetCursor", 0, [](Emulator& e) { ret(e, gui(e).current_cursor); });
    api(e, "ShowCursor", 1, [](Emulator& e) {
        Gui& g = gui(e);
        g.cursor_show += U(e, 0) ? 1 : -1;
        ret(e, static_cast<uint32_t>(g.cursor_show));
    });
    api(e, "GetCursorInfo", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t p = A(e, 0);
        e.mem.write32(p + 4, 1);
        e.mem.write32(p + 8, g.current_cursor);
        write_point(e, p + 12, g.cursor_pos);
        ret(e, 1);
    });

    // ---- caret -------------------------------------------------------------------------------
    api(e, "CreateCaret", 4, [](Emulator& e) {
        Gui& g = gui(e);
        g.caret_hwnd = U(e, 0);
        g.caret_w = I(e, 2) > 0 ? I(e, 2) : 1;
        g.caret_h = I(e, 3) > 0 ? I(e, 3) : 16;
        g.caret_hide = 1;
        ret(e, 1);
    });
    api(e, "DestroyCaret", 0, [](Emulator& e) {
        gui(e).caret_hwnd = 0;
        ret(e, 1);
    });
    api(e, "SetCaretPos", 2, [](Emulator& e) {
        gui(e).caret_pos = {I(e, 0), I(e, 1)};
        ret(e, 1);
    });
    api(e, "GetCaretPos", 1, [](Emulator& e) {
        write_point(e, A(e, 0), gui(e).caret_pos);
        ret(e, 1);
    });
    api(e, "ShowCaret", 1, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.caret_hide > 0) --g.caret_hide;
        ret(e, 1);
    });
    api(e, "HideCaret", 1, [](Emulator& e) {
        ++gui(e).caret_hide;
        ret(e, 1);
    });

    // ---- clipboard -------------------------------------------------------------------------
    api(e, "OpenClipboard", 1, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.clipboard_open) return ret(e, 0);
        g.clipboard_open = true;
        ret(e, 1);
    });
    api(e, "CloseClipboard", 0, [](Emulator& e) {
        Gui& g = gui(e);
        g.clipboard_open = false;
        auto it = g.clipboard.find(13);  // CF_UNICODETEXT
        if (it != g.clipboard.end()) {
            std::u16string s;
            for (size_t i = 0; i + 1 < it->second.size(); i += 2) {
                char16_t c = static_cast<char16_t>(it->second[i] | it->second[i + 1] << 8);
                if (!c) break;
                s.push_back(c);
            }
            g.host->clipboard_text(to_utf8(s));
        }
        ret(e, 1);
    });
    api(e, "EmptyClipboard", 0, [](Emulator& e) {
        Gui& g = gui(e);
        g.clipboard.clear();
        ret(e, 1);
    });
    api(e, "SetClipboardData", 2, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t h = A(e, 1);
        std::vector<uint8_t> data;
        if (h) {
            uint64_t n = e.heap_block_size(h);
            data.resize(static_cast<size_t>(n));
            if (n) e.mem.read(h, data.data(), n);
            e.heap_free(h);  // the clipboard owns it now
        }
        g.clipboard[U(e, 0)] = data;
        ret(e, h ? h : 1);
    });
    api(e, "GetClipboardData", 1, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.clipboard.find(U(e, 0));
        if (it == g.clipboard.end()) {
            // CF_TEXT from CF_UNICODETEXT.
            if (U(e, 0) == 1) {
                auto u = g.clipboard.find(13);
                if (u != g.clipboard.end()) {
                    std::u16string s;
                    for (size_t i = 0; i + 1 < u->second.size(); i += 2) {
                        char16_t c = static_cast<char16_t>(u->second[i] | u->second[i + 1] << 8);
                        if (!c) break;
                        s.push_back(c);
                    }
                    std::string a = utf16_to_cp932(s);
                    uint64_t p = e.heap_alloc(a.size() + 1);
                    e.mem.write_cstring(p, a);
                    return ret(e, p);
                }
            }
            return ret(e, 0);
        }
        uint64_t p = e.heap_alloc(it->second.size() ? it->second.size() : 1);
        if (!it->second.empty()) e.mem.write(p, it->second.data(), it->second.size());
        ret(e, p);
    });
    api(e, "IsClipboardFormatAvailable", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t f = U(e, 0);
        ret(e, g.clipboard.count(f) || (f == 1 && g.clipboard.count(13)) ? 1 : 0);
    });
    api(e, "CountClipboardFormats", 0, [](Emulator& e) { ret(e, gui(e).clipboard.size()); });
    api(e, "EnumClipboardFormats", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t prev = U(e, 0);
        for (auto& [f, d] : g.clipboard)
            if (f > prev) return ret(e, f);
        ret(e, 0);
    });
    api(e, "RegisterClipboardFormatW", 1, [](Emulator& e) { ret(e, gui(e).add_atom(read_wstr(e, A(e, 0)))); });
    api(e, "RegisterClipboardFormatA", 1, [](Emulator& e) {
        ret(e, gui(e).add_atom(cp932_to_utf16(e.mem.read_cstring(A(e, 0)))));
    });
    api(e, "GetClipboardFormatNameW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.atom_names.find(U(e, 0));
        if (it == g.atom_names.end()) return ret(e, 0);
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 1), it->second, I(e, 2))));
    });
    api(e, "GetClipboardOwner", 0, [](Emulator& e) { ret(e, gui(e).clipboard_owner); });
    api(e, "GetOpenClipboardWindow", 0, [](Emulator& e) { ret(e, 0); });
    api(e, "SetClipboardViewer", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "ChangeClipboardChain", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "AddClipboardFormatListener", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "RemoveClipboardFormatListener", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "GetClipboardSequenceNumber", 0, [](Emulator& e) { ret(e, 1); });
}

}  // namespace x86emu::win32
