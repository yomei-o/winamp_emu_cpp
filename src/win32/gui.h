// The Win32 GUI subsystem: user32, gdi32 and the pieces of comctl32 a classic
// MFC program reaches for, implemented on the host over a software framebuffer.
//
// Nothing here is a real Windows: there is one "screen" (a 32-bit pixel buffer
// the front end shows), each top-level window paints into a surface of its
// own, and the screen is those surfaces composed in z-order.  Child windows
// paint into their top-level window's surface through a clip region, which is
// how Windows 3.x worked and is enough for an MFC frame with docked bars.
//
// Window procedures are guest code reached through Emulator::call_guest.  The
// built-in classes (BUTTON, EDIT, ToolbarWindow32...) have window procedures
// too, but theirs are hook addresses: calling one from the guest - which is
// what CallWindowProc on a subclassed control does - lands in host code.
#pragma once

#include <cstdio>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace x86emu {
class Emulator;
}

namespace x86emu::win32 {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Point {
    int x = 0, y = 0;
};

struct Rect {
    int l = 0, t = 0, r = 0, b = 0;
    int w() const { return r - l; }
    int h() const { return b - t; }
    bool empty() const { return r <= l || b <= t; }
    bool contains(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
    Rect offset(int dx, int dy) const { return {l + dx, t + dy, r + dx, b + dy}; }
    Rect intersect(const Rect& o) const {
        Rect x{l > o.l ? l : o.l, t > o.t ? t : o.t, r < o.r ? r : o.r, b < o.b ? b : o.b};
        if (x.empty()) return {};
        return x;
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return {l < o.l ? l : o.l, t < o.t ? t : o.t, r > o.r ? r : o.r, b > o.b ? b : o.b};
    }
    bool operator==(const Rect& o) const {
        return l == o.l && t == o.t && r == o.r && b == o.b;
    }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

// A set of pixels, kept as y-bands of x-spans: the representation GDI itself
// uses, and the one that makes intersection with a scanline cheap.
class Region {
public:
    struct Band {
        int top, bottom;
        std::vector<std::pair<int, int>> spans;  // [x0, x1), sorted, disjoint
    };

    Region() = default;
    explicit Region(const Rect& r) { set_rect(r); }

    void clear() { bands_.clear(); }
    void set_rect(const Rect& r);
    bool empty() const { return bands_.empty(); }
    bool is_rect() const {
        return bands_.size() == 1 && bands_[0].spans.size() == 1;
    }
    Rect bounds() const;
    bool contains(int x, int y) const;
    bool intersects(const Rect& r) const;
    void offset(int dx, int dy);
    // The rectangles that make up the region, top to bottom, left to right.
    std::vector<Rect> rects() const;
    const std::vector<Band>& bands() const { return bands_; }
    // The band containing row y, or null.
    const Band* band_at(int y) const;

    enum class Op { And = 1, Or = 2, Xor = 3, Diff = 4, Copy = 5 };
    static Region combine(const Region& a, const Region& b, Op op);
    void intersect(const Region& o) { *this = combine(*this, o, Op::And); }
    void intersect(const Rect& r) { intersect(Region(r)); }
    void unite(const Region& o) { *this = combine(*this, o, Op::Or); }
    void unite(const Rect& r) { unite(Region(r)); }
    void subtract(const Region& o) { *this = combine(*this, o, Op::Diff); }
    void subtract(const Rect& r) { subtract(Region(r)); }
    bool operator==(const Region& o) const;

    // Builds a region from a polygon's interior, using GDI's fill convention.
    static Region polygon(const std::vector<Point>& pts, const std::vector<int>& counts,
                          bool winding);
    static Region ellipse(const Rect& r);
    static Region round_rect(const Rect& r, int ew, int eh);

    // Appends a band; the caller guarantees order.  Used by the builders.
    void push_band(int top, int bottom, std::vector<std::pair<int, int>> spans);
    void normalise();

private:
    std::vector<Band> bands_;
};

// ---------------------------------------------------------------------------
// Pixels
// ---------------------------------------------------------------------------

// A COLORREF is 0x00BBGGRR; a pixel in a 32-bit buffer is 0x00RRGGBB (the
// little-endian B,G,R,X byte order of a Windows DIB).
inline uint32_t colorref_to_pixel(uint32_t c) {
    return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}
inline uint32_t pixel_to_colorref(uint32_t p) {
    return ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF);
}

// A bitmap's pixels wherever they live: a host vector for device-dependent
// bitmaps and window surfaces, guest memory for DIB sections.
struct PixelBuf {
    uint8_t* bits = nullptr;  // row 0 is the *top* row
    int w = 0, h = 0;
    int stride = 0;           // bytes from one row to the next (may be negative)
    int bpp = 32;             // 1, 4, 8, 16, 24 or 32
    std::vector<uint32_t> palette;  // 0x00RRGGBB, for bpp <= 8
    // 16 bpp masks (5-5-5 unless the DIB said otherwise).
    uint32_t rmask = 0x7C00, gmask = 0x03E0, bmask = 0x001F;

    uint32_t get(int x, int y) const;           // as 0x00RRGGBB
    void put(int x, int y, uint32_t rgb);
    uint32_t* row32(int y) const {
        return reinterpret_cast<uint32_t*>(bits + static_cast<ptrdiff_t>(y) * stride);
    }
    int nearest_index(uint32_t rgb) const;
};

// ---------------------------------------------------------------------------
// GDI objects
// ---------------------------------------------------------------------------

enum class ObjKind : uint8_t {
    None = 0, Pen = 1, Brush = 2, Font = 3, Bitmap = 4, Region = 5, Palette = 6,
    DC = 7, ExtPen = 8, MetaFile = 9,
};

struct LogFont {
    int height = 0, width = 0, escapement = 0, orientation = 0, weight = 400;
    uint8_t italic = 0, underline = 0, strikeout = 0, charset = 1, out_precision = 0,
            clip_precision = 0, quality = 0, pitch_family = 0;
    std::u16string face;
};

struct Bitmap {
    PixelBuf px;
    std::vector<uint8_t> host;   // storage for a device-dependent bitmap
    bool dib = false;            // a DIB section: px.bits points into guest memory
    uint64_t guest_bits = 0;     // ...at this guest address
    bool mono_ddb = false;       // a 1-bpp DDB: colours come from the DC
    uint32_t selected_into = 0;  // the DC holding it, if any
    uint64_t dib_header = 0;     // copy of the BITMAPINFOHEADER the DIB was made from
    std::vector<uint8_t> dib_info;
};

struct GdiObject {
    ObjKind kind = ObjKind::None;
    bool stock = false;
    // Pen
    int pen_style = 0, pen_width = 1;
    uint32_t color = 0;  // COLORREF, for pens and brushes
    std::vector<uint32_t> pen_dashes;  // PS_USERSTYLE
    // Brush
    int brush_style = 0, hatch = 0;
    std::shared_ptr<Bitmap> pattern;   // BS_PATTERN / BS_DIBPATTERN (a copy)
    // Font
    LogFont font;
    // Bitmap
    std::shared_ptr<Bitmap> bitmap;
    // Region
    Region region;
    // Palette
    std::vector<uint32_t> pal;  // PALETTEENTRY as 0x00BBGGRR (peRed in the low byte)
};

struct XForm {
    double m11 = 1, m12 = 0, m21 = 0, m22 = 1, dx = 0, dy = 0;
};

struct DCState {
    uint32_t pen = 0, brush = 0, font = 0, bitmap = 0, palette = 0;
    uint32_t text_color = 0, bk_color = 0xFFFFFF;
    int bk_mode = 2, rop2 = 13, poly_fill = 1, stretch_mode = 1, text_align = 0;
    int map_mode = 1, graphics_mode = 1, arc_direction = 1, layout = 0;
    int char_extra = 0;
    Point cur;
    Point win_org, vp_org;
    Point win_ext{1, 1}, vp_ext{1, 1};
    Point brush_org;
    XForm world;
    bool has_clip = false;
    Region clip;  // in device coordinates (relative to the DC's origin)
};

struct DC {
    enum class Kind { Window, Memory, Info, Printer };
    Kind kind = Kind::Memory;
    uint32_t hwnd = 0;
    bool whole_window = false;  // GetWindowDC: origin at the window, not the client
    bool from_paint = false;    // BeginPaint: clipped to the update region
    Region paint_clip;          // that region, in surface coordinates
    bool has_paint_clip = false;
    uint32_t default_bitmap = 0;  // the 1x1 monochrome bitmap a memory DC starts with
    DCState s;
    std::vector<DCState> saved;
    Rect bounds;  // SetBoundsRect accumulation (unused, but answered)
    // The window part of the clip, cached against Gui::geom_ver.
    uint64_t vis_ver = ~0ull;
    Region vis_cache;
    // A printer DC (print.cpp): the page being drawn, between StartPage and
    // EndPage, and the paper it is on.
    std::shared_ptr<Bitmap> page;
    int prn_paper = 0;
    bool prn_landscape = false, prn_color = true;
};

// ---------------------------------------------------------------------------
// USER objects
// ---------------------------------------------------------------------------

struct MenuItem {
    uint32_t type = 0;   // MFT_*
    uint32_t state = 0;  // MFS_*
    uint32_t id = 0;
    uint32_t submenu = 0;
    std::u16string text;
    uint64_t data = 0;   // dwItemData
    uint32_t bmp_checked = 0, bmp_unchecked = 0, bmp_item = 0;
    Rect rect;           // where the menu bar or popup drew it, window-relative
};

struct Menu {
    uint32_t handle = 0;
    bool popup = false;
    std::vector<MenuItem> items;
    uint64_t context_help = 0;
    uint32_t style = 0;
    uint32_t max_height = 0;
    uint64_t menu_data = 0;
    uint32_t default_item = 0xFFFFFFFF;
};

struct Cursor {
    uint32_t handle = 0;
    bool icon = false;
    int w = 32, h = 32;
    int hot_x = 0, hot_y = 0;
    std::vector<uint32_t> color;  // 0xAARRGGBB
    int system_id = 0;            // IDC_* / IDI_* for the stock ones
};

struct AccelEntry {
    uint8_t flags = 0;  // FVIRTKEY, FSHIFT, FCONTROL, FALT
    uint16_t key = 0;
    uint16_t cmd = 0;
};

struct WndClass {
    uint32_t atom = 0;
    std::u16string name;  // upper-cased for lookup
    std::u16string display_name;
    uint32_t style = 0;
    uint64_t wndproc = 0;
    int cls_extra = 0, wnd_extra = 0;
    uint64_t hinstance = 0, icon = 0, cursor = 0, background = 0, menu_name = 0, icon_sm = 0;
    std::vector<uint8_t> extra;
    bool builtin = false;
    bool ansi = false;
};

struct ScrollInfo {
    int min = 0, max = 100, page = 0, pos = 0, track = 0;
    bool enabled = true;
    bool shown = true;
};

// Host-side state of a built-in control.  Each control kind derives its own.
struct ControlState {
    virtual ~ControlState() = default;
};

struct Window {
    uint32_t hwnd = 0;
    WndClass* cls = nullptr;
    uint32_t style = 0, exstyle = 0;
    uint32_t parent = 0;  // 0 only for the desktop
    uint32_t owner = 0;
    std::vector<uint32_t> children;  // z-order, topmost first
    Rect wrect;  // the window, relative to the parent's client area
    Rect crect;  // the client area, relative to the parent's client area
    uint64_t wndproc = 0;
    uint64_t hinstance = 0;
    uint64_t id = 0;      // a child's control id (or a top-level's HMENU)
    uint32_t menu = 0;    // a top-level window's menu bar
    uint64_t userdata = 0;
    std::vector<uint8_t> extra;
    std::u16string text;
    std::vector<std::pair<std::u16string, uint64_t>> props;
    bool ansi = false;

    // Painting
    Region update;  // client coordinates
    bool need_erase = false;
    bool need_ncpaint = false;
    bool internal_paint = false;
    bool redraw_off = false;  // WM_SETREDRAW FALSE

    ScrollInfo hscroll, vscroll;
    uint64_t context_help = 0;
    uint32_t sys_menu = 0;
    uint64_t font = 0;        // WM_SETFONT on a built-in control or dialog
    uint32_t icon_big = 0, icon_small = 0;

    // Top-level windows own the pixels everything inside them draws into.
    std::unique_ptr<std::vector<uint32_t>> surface;
    int surf_w = 0, surf_h = 0;

    // Dialogs
    bool is_dialog = false;
    bool dlg_ended = false;
    int64_t dlg_result = 0;
    int dlg_bux = 8, dlg_buy = 16;  // dialog base units
    uint32_t dlg_def_id = 1;
    uint32_t dlg_focus = 0;
    bool owns_font = false;

    // Before maximising, so that SC_RESTORE can put it back.
    Rect normal_rect;
    bool maximized = false, minimized = false;
    int nc_active = -1;  // the last WM_NCACTIVATE state (-1: none yet, follow the active window)

    bool destroying = false;
    bool created = false;  // WM_CREATE has returned

    std::unique_ptr<ControlState> ctl;
};

// ---------------------------------------------------------------------------
// The front end
// ---------------------------------------------------------------------------

// What the outside world feeds in.  Coordinates are screen pixels.
struct HostEvent {
    enum Type {
        None, MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, Char, Resize, Quit,
    };
    Type type = None;
    int x = 0, y = 0;
    int button = 0;       // 0 left, 1 right, 2 middle
    int key = 0;          // a Windows virtual-key code
    int mods = 0;         // 1 shift, 2 ctrl, 4 alt
    int delta = 0;        // wheel, in WHEEL_DELTA units of 120
    uint32_t ch = 0;      // UTF-32 for Char
    int w = 0, h = 0;     // Resize
};

class Host {
public:
    virtual ~Host() = default;
    // Waits up to `timeout_ms` (-1: forever, 0: just poll) for an event.
    virtual bool wait_event(HostEvent& ev, int timeout_ms) = 0;
    // The screen changed inside `dirty`.
    virtual void present(const uint32_t* pixels, int w, int h, const Rect& dirty) = 0;
    virtual uint64_t now_ms() = 0;
    virtual void set_cursor(int /*idc*/) {}
    virtual void clipboard_text(const std::string& /*utf8*/) {}

    // Sound, for waveOut.  A host without a device returns false from
    // audio_open, and waveOut then plays to the clock instead.  PCM arrives in
    // the format given to audio_open; audio_played() counts the bytes the
    // device has actually consumed since then.
    virtual bool audio_open(int /*rate*/, int /*channels*/, int /*bits*/) { return false; }
    virtual void audio_write(const uint8_t* /*data*/, size_t /*bytes*/) {}
    virtual uint64_t audio_played() { return 0; }
    virtual void audio_pause(bool /*paused*/) {}
    virtual void audio_reset() {}  // drop everything not yet played
    virtual void audio_close() {}
    virtual void audio_volume(int /*left*/, int /*right*/) {}  // 0..0xFFFF
};

// ---------------------------------------------------------------------------
// The subsystem
// ---------------------------------------------------------------------------

struct Msg {
    uint32_t hwnd = 0, message = 0;
    uint64_t wparam = 0, lparam = 0;
    uint32_t time = 0;
    Point pt;
};

struct Timer {
    uint32_t hwnd = 0;
    uint64_t id = 0;
    uint32_t interval = 0;
    uint64_t due = 0;
    uint64_t proc = 0;
};

struct HookEntry {
    int type = 0;
    uint64_t proc = 0;
    uint32_t handle = 0;
};

class Gui {
public:
    explicit Gui(Emulator& e);
    ~Gui();

    Emulator& emu;
    Host* host = nullptr;
    std::unique_ptr<Host> owned_host;

    // The screen
    int screen_w = 1280, screen_h = 800;
    std::vector<uint32_t> screen;
    Rect screen_dirty;
    void compose_dwm(Window* t);
    void compose(bool force = false);
    void present_now();
    uint64_t last_present = 0;

    // ---- windows -----------------------------------------------------------
    uint32_t desktop = 0;
    // Bumped whenever anything that decides a window's visible pixels changes.
    uint64_t geom_ver = 1;
    std::unordered_map<uint32_t, std::unique_ptr<Window>> windows;
    Window* win(uint64_t hwnd) const {
        auto it = windows.find(static_cast<uint32_t>(hwnd));
        return it == windows.end() ? nullptr : it->second.get();
    }
    uint32_t new_hwnd();
    uint32_t next_hwnd_ = 0x00010010;

    std::vector<std::unique_ptr<WndClass>> classes;
    WndClass* find_class(const std::u16string& name, uint64_t hinst) const;
    WndClass* find_class_atom(uint32_t atom) const;
    uint32_t next_atom = 0xC000;

    uint32_t focus = 0, active = 0, capture = 0, foreground = 0;
    int capture_ht = 0;  // capture from a non-client drag
    uint32_t hover_hwnd = 0;
    // The caption button under the mouse (HTCLOSE / HTMAXBUTTON / HTMINBUTTON)
    // and whether it is held down, for the Windows 11 hover and press colours.
    uint32_t nc_hot_hwnd = 0;
    int nc_hot_ht = 0;
    bool nc_hot_pressed = false;
    uint32_t ui_font = 0;  // Yu Gothic UI 9pt: captions and menus
    bool menu_keyboard = false;  // Alt / F10 was used: show access-key underlines

    // Geometry helpers
    Window* toplevel_of(Window* w) const;
    Point client_to_screen(const Window* w, Point p) const;
    Point window_to_screen(const Window* w, Point p) const;  // relative to w's window rect
    Rect window_rect_screen(const Window* w) const;
    // The window tree with screen rectangles, for debugging layout.
    void dump_windows(std::FILE* f, uint32_t hwnd = 0, int depth = 0) const;
    Rect client_rect_screen(const Window* w) const;
    bool is_visible(const Window* w) const;  // this and every ancestor
    // Offset of a window's client (or window) origin inside its top-level's surface.
    Point surface_origin(const Window* w, bool client) const;
    // The pixels a DC on this window may touch, in surface coordinates.
    Region visible_region(const Window* w, bool client) const;
    uint32_t window_from_point(Point screen_pt, bool skip_disabled = false);
    uint32_t child_from_point(Window* parent, Point client_pt, bool all = false);

    // Painting
    void invalidate(Window* w, const Region* rgn_client, bool erase, bool children,
                    bool frame = false);
    void invalidate_rect_screen(const Rect& r);
    void validate(Window* w, const Region* rgn_client);
    Window* find_paint_window(uint32_t root, uint32_t filter);
    void ensure_surface(Window* top);

    // Messages
    std::vector<Msg> posted;
    std::vector<HostEvent> input;
    std::vector<Timer> timers;
    bool quit_posted = false;
    int quit_code = 0;
    Msg last_msg;
    uint32_t msg_time() const;
    Point cursor_pos;
    uint8_t key_state[256] = {};
    bool async_down[256] = {};
    uint64_t last_click_time = 0;
    Point last_click_pt;
    int last_click_button = -1;
    uint32_t last_click_hwnd = 0;

    uint64_t send(uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
    uint64_t call_wndproc(uint64_t proc, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
    void post(uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
    // GetMessage/PeekMessage.  Returns false if nothing was found (and, when
    // `wait` is set, only after the host had nothing to offer either).
    bool get_message(Msg& out, uint32_t hwnd, uint32_t min, uint32_t max, bool remove,
                     bool wait);
    void pump_host(int timeout_ms);
    // The emulator's host_idle: no guest thread can run, so wait for input,
    // a timer, or `ms` (-1: no limit of its own).
    void idle_wait(int ms);
    // Whether a message is waiting for a GetMessage on the GUI thread.
    bool message_ready();
    bool translate_input(const HostEvent& ev, Msg& out);
    bool peek_posted(Msg& out, uint32_t hwnd, uint32_t min, uint32_t max, bool remove);
    void write_msg(uint64_t addr, const Msg& m);
    Msg read_msg(uint64_t addr);
    // Runs a modal loop until `done` says so: the loop dialogs, menus and
    // window drags use, dispatching everything else meanwhile.
    void modal_loop(const std::function<bool()>& done, uint32_t filter_hwnd = 0);
    void dispatch(const Msg& m);

    // Window lifetime and placement
    uint32_t create_window(uint32_t exstyle, const std::u16string& cls_name, uint32_t atom,
                           const std::u16string& title, uint32_t style, int x, int y, int w,
                           int h, uint32_t parent, uint64_t menu, uint64_t hinst,
                           uint64_t param, bool ansi);
    bool destroy_window(uint32_t hwnd);
    bool set_window_pos(uint32_t hwnd, uint32_t after, int x, int y, int cx, int cy,
                        uint32_t flags);
    bool show_window(uint32_t hwnd, int cmd);
    void set_focus(uint32_t hwnd);
    void set_active(uint32_t hwnd, bool by_mouse = false);
    void unlink(Window* w);
    void link(Window* w, uint32_t parent, uint32_t after);
    // Non-client sizes for a style, the inverse of AdjustWindowRectEx.
    Rect nc_insets(uint32_t style, uint32_t exstyle, bool menu, int width) const;
    int menu_bar_height(Window* w, int width);
    void recalc_client(Window* w);

    // Hooks (SetWindowsHookEx)
    std::vector<HookEntry> hooks;
    uint32_t next_hook = 0x00700010;
    uint64_t call_hook(int type, int code, uint64_t wp, uint64_t lp);

    // ---- GDI -----------------------------------------------------------------
    std::unordered_map<uint32_t, std::unique_ptr<GdiObject>> objs;
    std::unordered_map<uint32_t, std::unique_ptr<DC>> dcs;
    uint32_t next_obj = 1;
    uint32_t new_obj(ObjKind kind, std::unique_ptr<GdiObject> o);
    GdiObject* obj(uint64_t h, ObjKind kind = ObjKind::None) const;
    ObjKind kind_of(uint64_t h) const;
    DC* dc(uint64_t h) const {
        auto it = dcs.find(static_cast<uint32_t>(h));
        return it == dcs.end() ? nullptr : it->second.get();
    }
    uint32_t stock[20] = {};
    uint32_t sys_color_brush[32] = {};
    uint32_t create_dc(DC::Kind kind, uint32_t hwnd, bool whole_window);
    void delete_dc(uint32_t hdc);
    uint32_t sys_color(int idx) const;

    // ---- USER objects --------------------------------------------------------
    std::unordered_map<uint32_t, std::unique_ptr<Menu>> menus;
    std::unordered_map<uint32_t, std::unique_ptr<Cursor>> cursors;
    std::unordered_map<uint32_t, std::vector<AccelEntry>> accels;
    uint32_t next_user = 1;
    uint32_t new_user_handle(uint32_t kind) { return (kind << 24) | (next_user++ << 2); }
    Menu* menu(uint64_t h) const {
        auto it = menus.find(static_cast<uint32_t>(h));
        return it == menus.end() ? nullptr : it->second.get();
    }
    uint32_t current_cursor = 0;
    int cursor_show = 0;

    // Atoms and registered messages
    std::map<std::u16string, uint32_t> atoms;  // upper-cased
    std::map<uint32_t, std::u16string> atom_names;
    uint32_t add_atom(const std::u16string& s);

    // Clipboard
    std::map<uint32_t, std::vector<uint8_t>> clipboard;
    uint32_t clipboard_owner = 0;
    bool clipboard_open = false;

    // Caret
    uint32_t caret_hwnd = 0;
    int caret_w = 1, caret_h = 16;
    Point caret_pos;
    int caret_hide = 0;

    // Modal menu state (menu.cpp)
    struct MenuTracking* tracking = nullptr;

    // Built-in classes register their window procedure here.
    std::map<std::u16string, uint64_t> builtin_procs;
    uint64_t defwndproc_hook = 0, defdlgproc_hook = 0;

    // Scratch guest memory for structures passed in messages.
    uint64_t scratch_alloc(size_t bytes);
    void scratch_free(uint64_t addr);

    // A trace of window messages, for debugging (JWEMU_TRACE_MSG=1).
    bool trace_msgs = false;
};

// The single Gui for the process (one per emulator; created on demand).
Gui& gui(Emulator& e);

// Install the hooks.  Each file registers its own share.
void install_kernel_extra(Emulator& e);
void install_registry(Emulator& e);
void install_user(Emulator& e);
void install_gdi(Emulator& e);
void install_menu(Emulator& e);
void install_dialog(Emulator& e);
void install_controls(Emulator& e);
void install_comctl(Emulator& e);
void install_shell_ole(Emulator& e);
void install_all(Emulator& e);

// String helpers shared by the hook files.
std::u16string read_wstr(Emulator& e, uint64_t p, int max_units = -1);
std::string to_utf8(const std::u16string& s);
std::u16string from_utf8(const std::string& s);
std::u16string upper(const std::u16string& s);
// Copies a string into a guest buffer of `cap` units with NUL; returns units
// written excluding NUL (or required length when cap == 0).
int write_wstr(Emulator& e, uint64_t dst, const std::u16string& s, int cap);
std::u16string cp932_to_utf16(const std::string& s);
std::string utf16_to_cp932(const std::u16string& s);
// A resource id: either a small integer or a pointer to a name.
std::u16string res_name(Emulator& e, uint64_t id);
bool is_intresource(uint64_t p);

// Resource lookup in a loaded module.  Returns the guest address and size.
bool find_resource(Emulator& e, uint64_t hmod, uint64_t type, uint64_t name, uint64_t& addr,
                   uint32_t& size);

// Drawing API used by the built-in controls and non-client painting, so that
// host code paints through exactly the same paths as the guest.
namespace draw {
void fill_rect(Gui& g, uint32_t hdc, const Rect& r, uint32_t colorref);
void fill_rect_brush(Gui& g, uint32_t hdc, const Rect& r, uint32_t hbrush);
void frame_rect(Gui& g, uint32_t hdc, const Rect& r, uint32_t colorref);
void line(Gui& g, uint32_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref);
void edge(Gui& g, uint32_t hdc, Rect r, uint32_t edge, uint32_t flags);
void text(Gui& g, uint32_t hdc, int x, int y, const std::u16string& s, uint32_t colorref,
          int bk = -1);
int draw_text(Gui& g, uint32_t hdc, const std::u16string& s, Rect& r, uint32_t fmt);
Point text_extent(Gui& g, uint32_t hdc, const std::u16string& s);
void select_font(Gui& g, uint32_t hdc, uint32_t font);
int font_height(Gui& g, uint32_t font);
void focus_rect(Gui& g, uint32_t hdc, const Rect& r);
void blit_bitmap(Gui& g, uint32_t hdc, int x, int y, uint32_t hbmp, int sx, int sy, int w,
                 int h, uint32_t rop);
void draw_icon(Gui& g, uint32_t hdc, int x, int y, uint32_t hicon, int w, int h);
void frame_control(Gui& g, uint32_t hdc, Rect r, uint32_t type, uint32_t state);
}  // namespace draw

uint32_t default_gui_font(Gui& g);
// GetClipBox: the bounds of what drawing on the DC can touch, logical units.
Rect dc_clip_box(Gui& g, uint32_t hdc, int* complexity = nullptr);
uint32_t text_metrics_height(Gui& g, uint32_t font);

// Called after the registry file is rewritten, so a front end without a
// persistent filesystem (the browser) can keep a copy elsewhere.
extern std::function<void(const std::string&)> registry_saved_hook;

// Printing (print.cpp): a printer DC for CreateDC, and what GetDeviceCaps
// says about one.  print_done_hook is called with the guest path of each PDF
// a print job wrote.
uint32_t create_printer_dc(Gui& g, uint64_t devmode);
bool is_printer_name(const std::u16string& name);
uint64_t printer_device_caps(const DC& d, int index);
void install_print(Emulator& e);
// uxtheme (theme.cpp): a theme handle for the classes that have one, else 0.
uint32_t theme_open(const std::u16string& class_list);
void install_theme(Emulator& e);
void install_winmm(Emulator& e);
extern std::function<void(const std::string&)> print_done_hook;

// The print / printer-setup dialog (dialog.cpp).  Returns false on cancel.
bool print_dialog(Gui& g, uint32_t owner, bool setup, const std::vector<std::u16string>& papers,
                  int& paper, bool& landscape, bool& color);

}  // namespace x86emu::win32
