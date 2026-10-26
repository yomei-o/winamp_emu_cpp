// Menus: the object API, menu templates, the menu bar, popup menus and the
// modal loop that tracks them, and keyboard accelerators.
#include <algorithm>
#include <cstring>

#include "font.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

RealFont* dc_realfont(Gui& g, uint32_t hdc);

namespace {

constexpr uint32_t MF_BYPOSITION = 0x400, MF_POPUP = 0x10, MF_SEPARATOR = 0x800,
                   MF_GRAYED = 0x1, MF_DISABLED = 0x2, MF_CHECKED = 0x8, MF_BITMAP = 0x4,
                   MF_OWNERDRAW = 0x100, MF_MENUBARBREAK = 0x20, MF_MENUBREAK = 0x40,
                   MF_HILITE = 0x80, MF_DEFAULT = 0x1000, MF_RIGHTJUSTIFY = 0x4000;
constexpr uint32_t MFT_SEPARATOR = 0x800, MFT_OWNERDRAW = 0x100, MFT_BITMAP = 0x4,
                   MFT_RADIOCHECK = 0x200;
constexpr int kItemH = 22, kSepH = 9, kCheckW = 28, kArrowW = 20;

uint32_t new_menu(Gui& g, bool popup) {
    auto m = std::make_unique<Menu>();
    m->handle = g.new_user_handle(0x0B);
    m->popup = popup;
    uint32_t h = m->handle;
    g.menus[h] = std::move(m);
    return h;
}

// Finds an item by command (recursively, as Windows does) or by position.
MenuItem* find_item(Gui& g, uint32_t hmenu, uint32_t id, uint32_t flags, Menu** owner = nullptr,
                    int* index = nullptr) {
    Menu* m = g.menu(hmenu);
    if (!m) return nullptr;
    if (flags & MF_BYPOSITION) {
        if (id >= m->items.size()) return nullptr;
        if (owner) *owner = m;
        if (index) *index = static_cast<int>(id);
        return &m->items[id];
    }
    for (size_t i = 0; i < m->items.size(); ++i) {
        MenuItem& it = m->items[i];
        if (it.submenu) {
            if (MenuItem* r = find_item(g, it.submenu, id, flags, owner, index)) return r;
            if (it.id == id && id != 0xFFFFFFFF && id != 0) {
                if (owner) *owner = m;
                if (index) *index = static_cast<int>(i);
                return &it;
            }
            continue;
        }
        if (it.id == id && !(it.type & MFT_SEPARATOR)) {
            if (owner) *owner = m;
            if (index) *index = static_cast<int>(i);
            return &it;
        }
    }
    return nullptr;
}

// Legacy MF_* flags into an item.
void apply_flags(Gui& g, Emulator& e, MenuItem& it, uint32_t flags, uint64_t id, uint64_t data) {
    it.type = flags & (MF_SEPARATOR | MF_BITMAP | MF_OWNERDRAW | MF_MENUBARBREAK | MF_MENUBREAK | MF_RIGHTJUSTIFY);
    it.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT);
    if (flags & MF_POPUP) {
        it.submenu = static_cast<uint32_t>(id);
        it.id = static_cast<uint32_t>(id);
        if (Menu* sm = g.menu(id)) sm->popup = true;
    } else {
        it.submenu = 0;
        it.id = static_cast<uint32_t>(id);
    }
    if (flags & (MF_BITMAP)) {
        it.bmp_item = static_cast<uint32_t>(data);
        it.text.clear();
    } else if (flags & MF_OWNERDRAW) {
        it.data = data;
    } else if (!(flags & MF_SEPARATOR) && data) {
        it.text = read_wstr(e, data);
    }
}

void read_item_info(Gui& g, Emulator& e, uint64_t p, MenuItem& it) {
    uint32_t mask = e.mem.read32(p + 4);
    if (mask & 0x10) {  // MIIM_TYPE
        it.type = e.mem.read32(p + 8);
        uint64_t s = e.mem.read32(p + 36);
        if (!(it.type & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW)))
            it.text = s ? read_wstr(e, s) : u"";
        else if (it.type & MFT_BITMAP)
            it.bmp_item = static_cast<uint32_t>(s);
        else if (it.type & MFT_OWNERDRAW)
            it.data = s;
    }
    if (mask & 0x100) it.type = e.mem.read32(p + 8);  // MIIM_FTYPE
    if (mask & 0x1) it.state = e.mem.read32(p + 12);  // MIIM_STATE
    if (mask & 0x2) it.id = e.mem.read32(p + 16);      // MIIM_ID
    if (mask & 0x4) {                                  // MIIM_SUBMENU
        it.submenu = e.mem.read32(p + 20);
        if (Menu* sm = g.menu(it.submenu)) sm->popup = true;
    }
    if (mask & 0x8) {  // MIIM_CHECKMARKS
        it.bmp_checked = e.mem.read32(p + 24);
        it.bmp_unchecked = e.mem.read32(p + 28);
    }
    if (mask & 0x20) it.data = e.mem.read32(p + 32);  // MIIM_DATA
    if (mask & 0x40) {                                 // MIIM_STRING
        uint64_t s = e.mem.read32(p + 36);
        it.text = s ? read_wstr(e, s) : u"";
    }
    if (mask & 0x80) it.bmp_item = e.mem.read32(p + 44);  // MIIM_BITMAP
}

void write_item_info(Emulator& e, uint64_t p, const MenuItem& it) {
    uint32_t mask = e.mem.read32(p + 4);
    if (mask & (0x10 | 0x100)) e.mem.write32(p + 8, it.type);
    if (mask & 0x1) e.mem.write32(p + 12, it.state);
    if (mask & 0x2) e.mem.write32(p + 16, it.id);
    if (mask & 0x4) e.mem.write32(p + 20, it.submenu);
    if (mask & 0x8) {
        e.mem.write32(p + 24, it.bmp_checked);
        e.mem.write32(p + 28, it.bmp_unchecked);
    }
    if (mask & 0x20) e.mem.write32(p + 32, static_cast<uint32_t>(it.data));
    if (mask & (0x40 | 0x10)) {
        uint64_t buf = e.mem.read32(p + 36);
        uint32_t cch = e.mem.read32(p + 40);
        if (!(it.type & (MFT_SEPARATOR | MFT_BITMAP | MFT_OWNERDRAW)) || (mask & 0x40)) {
            if (buf && cch) write_wstr(e, buf, it.text, static_cast<int>(cch));
            e.mem.write32(p + 40, static_cast<uint32_t>(it.text.size()));
        }
    }
    if (mask & 0x80) e.mem.write32(p + 44, it.bmp_item);
}

// ---- templates -----------------------------------------------------------------

uint64_t parse_std(Gui& g, Emulator& e, uint64_t p, uint32_t hmenu) {
    while (true) {
        uint16_t flags = e.mem.read16(p);
        p += 2;
        MenuItem it;
        uint32_t id = 0;
        if (!(flags & MF_POPUP)) {
            id = e.mem.read16(p);
            p += 2;
        }
        std::u16string text = read_wstr(e, p);
        p += (text.size() + 1) * 2;
        it.text = text;
        it.state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_DEFAULT);
        it.type = flags & (MF_MENUBARBREAK | MF_MENUBREAK | MF_RIGHTJUSTIFY | MF_OWNERDRAW | MF_BITMAP);
        if (flags & MF_POPUP) {
            uint32_t sub = new_menu(g, true);
            p = parse_std(g, e, p, sub);
            it.submenu = sub;
            it.id = sub;
        } else {
            it.id = id;
            if (id == 0 && text.empty()) it.type |= MFT_SEPARATOR;
        }
        g.menu(hmenu)->items.push_back(it);
        if (flags & 0x80) break;  // MF_END
    }
    return p;
}

uint64_t parse_ex(Gui& g, Emulator& e, uint64_t p, uint32_t hmenu) {
    while (true) {
        uint32_t type = e.mem.read32(p), state = e.mem.read32(p + 4), id = e.mem.read32(p + 8);
        uint16_t res = e.mem.read16(p + 12);
        p += 14;
        std::u16string text = read_wstr(e, p);
        p += (text.size() + 1) * 2;
        p = (p + 3) & ~3ull;
        MenuItem it;
        it.type = type;
        it.state = state;
        it.id = id;
        it.text = text;
        if (res & 1) {
            p += 4;  // dwHelpId
            uint32_t sub = new_menu(g, true);
            p = parse_ex(g, e, p, sub);
            it.submenu = sub;
        }
        g.menu(hmenu)->items.push_back(it);
        if (res & 0x80) break;
    }
    return p;
}

}  // namespace

uint32_t load_menu_template(Gui& g, uint64_t addr) {
    Emulator& e = g.emu;
    uint16_t version = e.mem.read16(addr), offset = e.mem.read16(addr + 2);
    uint32_t h = new_menu(g, false);
    if (version == 1) parse_ex(g, e, addr + 4 + offset, h);
    else parse_std(g, e, addr + 4 + offset, h);
    return h;
}

uint32_t load_menu_resource(Gui& g, uint64_t hinst, uint64_t name) {
    uint64_t addr;
    uint32_t size;
    if (!find_resource(g.emu, hinst, 4, name, addr, size)) return 0;  // RT_MENU
    return load_menu_template(g, addr);
}

// ---------------------------------------------------------------------------
// Measuring and drawing
// ---------------------------------------------------------------------------

namespace {

uint32_t menu_font(Gui& g) { return g.ui_font ? g.ui_font : default_gui_font(g); }

std::pair<std::u16string, std::u16string> split_accel(const std::u16string& s) {
    size_t t = s.find(u'\t');
    if (t == std::u16string::npos) return {s, u""};
    return {s.substr(0, t), s.substr(t + 1)};
}

int text_w(Gui& g, const std::u16string& s) {
    RealFont* f = realize_font(g.obj(menu_font(g))->font);
    int w = 0;
    bool amp = false;
    for (char16_t c : s) {
        if (c == u'&' && !amp) {
            amp = true;
            continue;
        }
        amp = false;
        w += f->advance(c);
    }
    return w;
}

struct PopupLayout {
    std::vector<Rect> items;  // client coordinates
    int w = 0, h = 0;
};

PopupLayout layout_popup(Gui& g, Menu* m, uint32_t owner) {
    PopupLayout L;
    int text_max = 0, acc_max = 0;
    for (auto& it : m->items) {
        if (it.type & MFT_SEPARATOR) continue;
        auto [t, a] = split_accel(it.text);
        text_max = std::max(text_max, text_w(g, t));
        acc_max = std::max(acc_max, text_w(g, a));
    }
    int width = kCheckW + text_max + (acc_max ? acc_max + 24 : 0) + kArrowW;
    width = std::max(width, 80);
    int y = 3;
    for (auto& it : m->items) {
        int h = (it.type & MFT_SEPARATOR) ? kSepH : kItemH;
        if ((it.type & MFT_OWNERDRAW) && owner) {
            uint64_t mis = g.scratch_alloc(24);
            g.emu.mem.write32(mis, 1);  // ODT_MENU
            g.emu.mem.write32(mis + 8, it.id);
            g.emu.mem.write32(mis + 12, 100);
            g.emu.mem.write32(mis + 16, kItemH);
            g.emu.mem.write32(mis + 20, static_cast<uint32_t>(it.data));
            g.send(owner, WM_MEASUREITEM, 0, mis);
            width = std::max(width, static_cast<int>(g.emu.mem.read32(mis + 12)) + kCheckW + kArrowW);
            h = static_cast<int>(g.emu.mem.read32(mis + 16));
            g.scratch_free(mis);
        }
        L.items.push_back({3, y, 0, y + h});
        y += h;
    }
    L.w = width + 6;
    for (auto& r : L.items) r.r = L.w - 3;
    L.h = y + 3;
    return L;
}

void draw_check(Gui& g, uint32_t hdc, int cx, int cy, uint32_t c, bool radio) {
    if (radio) {
        for (int dy = -3; dy <= 3; ++dy) {
            int dx = dy == -3 || dy == 3 ? 1 : (dy == -2 || dy == 2 ? 2 : 3);
            draw::fill_rect(g, hdc, {cx - dx, cy + dy, cx + dx + 1, cy + dy + 1}, c);
        }
        return;
    }
    for (int i = 0; i < 3; ++i) draw::fill_rect(g, hdc, {cx - 4 + i, cy - 1 + i, cx - 3 + i, cy + 2 + i}, c);
    for (int i = 0; i < 5; ++i) draw::fill_rect(g, hdc, {cx - 1 + i, cy + 1 - i, cx + i, cy + 4 - i}, c);
}

void draw_item_text(Gui& g, uint32_t hdc, const Rect& r, const std::u16string& text, uint32_t color, uint32_t fmt) {
    DC* d = g.dc(hdc);
    if (!d) return;
    d->s.text_color = color;
    d->s.bk_mode = 1;
    Rect rr = r;
    draw_text_impl(g, hdc, text, rr, fmt);
}

}  // namespace

int menu_bar_layout(Gui& g, Window* w, int width) {
    Menu* m = g.menu(w->menu);
    if (!m) return 0;
    // Rows of SM_CYMENU - 1 pixels and a line under the last: 20 for one row,
    // as on Windows.
    const int rh = kMenuH - 1;
    int x = 0, y = 0, rows = 1;
    for (auto& it : m->items) {
        int iw = (it.type & MFT_SEPARATOR) ? 8 : text_w(g, it.text) + 16;
        if (x + iw > width && x > 0) {
            x = 0;
            y += rh;
            ++rows;
        }
        it.rect = {x, y, x + iw, y + rh};
        x += iw;
    }
    return rows * rh + 1;
}

void menu_bar_draw(Gui& g, Window* w, uint32_t hdc, const Rect& bar) {
    Menu* m = g.menu(w->menu);
    // The themed menu bar of Windows 10/11: white, a faint line, then the
    // 3D-face line the client starts under.
    draw::fill_rect(g, hdc, bar, 0xFFFFFF);
    draw::fill_rect(g, hdc, {bar.l, bar.b - 2, bar.r, bar.b - 1}, 0xF2F2F2);
    draw::fill_rect(g, hdc, {bar.l, bar.b - 1, bar.r, bar.b}, g.sys_color(15));
    if (!m) return;
    draw::select_font(g, hdc, menu_font(g));
    int sel = g.tracking ? -2 : -1;
    extern int menubar_selected(Gui&, Window*);
    sel = menubar_selected(g, w);
    for (size_t i = 0; i < m->items.size(); ++i) {
        auto& it = m->items[i];
        Rect r = it.rect.offset(bar.l, bar.t);
        if (it.type & MFT_SEPARATOR) continue;
        bool grayed = (it.state & (MF_GRAYED | MF_DISABLED)) != 0;
        uint32_t tc = grayed ? g.sys_color(17) : g.sys_color(7);
        if (static_cast<int>(i) == sel) {
            // The themed selection: pale blue with a blue edge (COLORREFs).
            draw::fill_rect(g, hdc, r, 0xFFE8CC);
            draw::frame_rect(g, hdc, r, 0xFFD199);
        }
        // Access-key underlines stay hidden until the keyboard is used, as
        // Windows has them by default.
        uint32_t hide = g.menu_keyboard ? 0 : 0x100000;  // DT_HIDEPREFIX
        draw_item_text(g, hdc, r, it.text, tc, 0x1 | 0x20 | 0x4 | hide);  // CENTER|SINGLELINE|VCENTER
    }
}

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------

struct MenuTracking {
    struct Level {
        uint32_t hmenu = 0;
        uint32_t hwnd = 0;  // the popup window
        int sel = -1;
        PopupLayout layout;
    };
    std::vector<Level> levels;
    uint32_t owner = 0;
    uint32_t bar_hwnd = 0;  // the window whose menu bar started it, if any
    int bar_sel = -1;
    uint32_t result = 0;
    bool done = false;
    bool keyboard = false;
};

int menubar_selected(Gui& g, Window* w) {
    if (!g.tracking || g.tracking->bar_hwnd != w->hwnd) return -1;
    return g.tracking->bar_sel;
}

namespace {

uint64_t popup_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);

uint32_t popup_class_registered = 0;

void ensure_popup_class(Emulator& e) {
    if (popup_class_registered) return;
    register_builtin_class(e, u"#32768", 0x0800 | 0x0008, 0, 32512, 0, popup_proc);  // CS_SAVEBITS|CS_DBLCLKS
    popup_class_registered = 1;
}

MenuTracking::Level* level_of(Gui& g, uint32_t hwnd) {
    if (!g.tracking) return nullptr;
    for (auto& l : g.tracking->levels)
        if (l.hwnd == hwnd) return &l;
    return nullptr;
}

void paint_popup(Gui& g, uint32_t hwnd, uint32_t hdc) {
    MenuTracking::Level* L = level_of(g, hwnd);
    Window* w = g.win(hwnd);
    if (!w || !L) return;
    Menu* m = g.menu(L->hmenu);
    Rect all{0, 0, w->crect.w(), w->crect.h()};
    draw::fill_rect(g, hdc, all, g.sys_color(4));
    draw::frame_rect(g, hdc, all, g.sys_color(16));
    if (!m) return;
    draw::select_font(g, hdc, menu_font(g));
    for (size_t i = 0; i < m->items.size() && i < L->layout.items.size(); ++i) {
        const MenuItem& it = m->items[i];
        Rect r = L->layout.items[i];
        if (it.type & MFT_SEPARATOR) {
            int y = (r.t + r.b) / 2;
            draw::fill_rect(g, hdc, {r.l + kCheckW - 4, y, r.r - 2, y + 1}, g.sys_color(16));
            draw::fill_rect(g, hdc, {r.l + kCheckW - 4, y + 1, r.r - 2, y + 2}, g.sys_color(20));
            continue;
        }
        bool grayed = (it.state & (MF_GRAYED | MF_DISABLED)) != 0;
        bool hi = static_cast<int>(i) == L->sel;
        uint32_t tc = grayed ? g.sys_color(17) : g.sys_color(7);
        if (it.type & MFT_OWNERDRAW) {
            uint64_t dis = g.scratch_alloc(48);
            Emulator& e = g.emu;
            e.mem.write32(dis, 1);  // ODT_MENU
            e.mem.write32(dis + 8, it.id);
            e.mem.write32(dis + 12, 2);  // ODA_DRAWENTIRE
            e.mem.write32(dis + 16, (hi ? 1u : 0u) | (grayed ? 6u : 0u) | ((it.state & MF_CHECKED) ? 8u : 0u));
            e.mem.write32(dis + 20, L->hmenu);
            e.mem.write32(dis + 24, hdc);
            write_rect(e, dis + 28, r);
            e.mem.write32(dis + 44, static_cast<uint32_t>(it.data));
            g.send(g.tracking->owner, WM_DRAWITEM, 0, dis);
            g.scratch_free(dis);
            continue;
        }
        if (hi) {
            draw::fill_rect(g, hdc, r, g.sys_color(13));
            if (!grayed) tc = g.sys_color(14);
        }
        if (it.state & MF_CHECKED) draw_check(g, hdc, r.l + 12, (r.t + r.b) / 2, tc, (it.type & MFT_RADIOCHECK) != 0);
        auto [text, acc] = split_accel(it.text);
        Rect tr{r.l + kCheckW, r.t, r.r - kArrowW, r.b};
        draw_item_text(g, hdc, tr, text, tc, 0x20 | 0x4 | (g.menu_keyboard ? 0 : 0x100000));
        if (!acc.empty()) draw_item_text(g, hdc, tr, acc, tc, 0x20 | 0x4 | 0x2 | 0x800);
        if (it.submenu) {
            int cx = r.r - 10, cy = (r.t + r.b) / 2;
            for (int k = 0; k < 4; ++k) draw::fill_rect(g, hdc, {cx - 2 + k - 2, cy - 3 + k, cx - 1 + k - 2, cy + 4 - k}, tc);
        }
    }
}

uint64_t popup_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    switch (msg) {
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            paint_popup(g, hwnd, hdc);
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            return 3;  // MA_NOACTIVATE
        case WM_NCCALCSIZE: {
            Rect r = read_rect(g.emu, lp);
            write_rect(g.emu, lp, r);
            return 0;
        }
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

void repaint_level(Gui& g, MenuTracking::Level& L) {
    Window* w = g.win(L.hwnd);
    if (!w) return;
    uint32_t hdc = get_dc(g, L.hwnd, false, nullptr);
    paint_popup(g, L.hwnd, hdc);
    release_dc(g, hdc);
}

void repaint_bar(Gui& g) {
    if (!g.tracking || !g.tracking->bar_hwnd) return;
    Window* w = g.win(g.tracking->bar_hwnd);
    if (w) nc_paint(g, w, nullptr);
}

void close_levels_from(Gui& g, size_t from) {
    MenuTracking* t = g.tracking;
    while (t->levels.size() > from) {
        MenuTracking::Level L = t->levels.back();
        t->levels.pop_back();
        if (L.hwnd) g.destroy_window(L.hwnd);
        if (t->owner && g.win(t->owner)) g.send(t->owner, WM_UNINITMENUPOPUP, L.hmenu, 0);
    }
}

// Opens `hmenu` as a popup at screen point (x, y); `exclude` is the item it
// drops from, which it should not cover.
void open_level(Gui& g, uint32_t hmenu, int x, int y, const Rect* exclude, int index_in_parent, bool sys) {
    MenuTracking* t = g.tracking;
    if (t->owner) g.send(t->owner, WM_INITMENUPOPUP, hmenu, make_lparam(index_in_parent, sys ? 1 : 0));
    Menu* m = g.menu(hmenu);
    if (!m) return;
    MenuTracking::Level L;
    L.hmenu = hmenu;
    L.layout = layout_popup(g, m, t->owner);
    int w = L.layout.w, h = L.layout.h;
    if (x + w > g.screen_w) x = exclude ? std::max(0, exclude->l - w) : std::max(0, g.screen_w - w);
    if (y + h > g.screen_h) y = exclude && exclude->b - exclude->t < 40 ? std::max(0, exclude->t - h) : std::max(0, g.screen_h - h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    t->levels.push_back(L);
    size_t idx = t->levels.size() - 1;
    uint32_t hwnd = g.create_window(0x8 | 0x80, u"#32768", 32768, u"", WS_POPUP | WS_CLIPSIBLINGS, x, y, w, h, 0, 0, 0, 0, false);
    t->levels[idx].hwnd = hwnd;
    if (Window* pw = g.win(hwnd)) pw->exstyle |= WS_EX_TOPMOST;
    g.set_window_pos(hwnd, 0xFFFFFFFF, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    repaint_level(g, t->levels[idx]);
}

void select_item(Gui& g, size_t level, int sel) {
    MenuTracking* t = g.tracking;
    if (level >= t->levels.size()) return;
    MenuTracking::Level& L = t->levels[level];
    if (L.sel == sel) return;
    L.sel = sel;
    Menu* m = g.menu(L.hmenu);
    if (m && sel >= 0 && sel < static_cast<int>(m->items.size())) {
        const MenuItem& it = m->items[static_cast<size_t>(sel)];
        uint32_t flags = (it.state & 0x3) | (it.submenu ? MF_POPUP : 0) | MF_HILITE | (it.state & MF_CHECKED);
        g.send(t->owner, WM_MENUSELECT, (flags << 16) | (it.submenu ? static_cast<uint32_t>(sel) : (it.id & 0xFFFF)), L.hmenu);
    }
    repaint_level(g, t->levels[level]);
}

void open_submenu(Gui& g, size_t level) {
    MenuTracking* t = g.tracking;
    MenuTracking::Level& L = t->levels[level];
    Menu* m = g.menu(L.hmenu);
    if (!m || L.sel < 0 || L.sel >= static_cast<int>(m->items.size())) return;
    const MenuItem& it = m->items[static_cast<size_t>(L.sel)];
    if (!it.submenu || (it.state & (MF_GRAYED | MF_DISABLED))) return;
    if (t->levels.size() > level + 1 && t->levels[level + 1].hmenu == it.submenu) return;
    close_levels_from(g, level + 1);
    Window* pw = g.win(t->levels[level].hwnd);
    if (!pw) return;
    Rect ir = t->levels[level].layout.items[static_cast<size_t>(L.sel)];
    Rect sr = g.window_rect_screen(pw);
    Rect ex = ir.offset(sr.l, sr.t);
    open_level(g, it.submenu, sr.r - 3, ex.t - 3, &ex, L.sel, false);
}

// Which level and item lie under a screen point.
bool hit(Gui& g, Point sp, size_t& level, int& item) {
    MenuTracking* t = g.tracking;
    for (size_t i = t->levels.size(); i-- > 0;) {
        Window* w = g.win(t->levels[i].hwnd);
        if (!w) continue;
        Rect r = g.window_rect_screen(w);
        if (!r.contains(sp.x, sp.y)) continue;
        level = i;
        item = -1;
        for (size_t k = 0; k < t->levels[i].layout.items.size(); ++k)
            if (t->levels[i].layout.items[k].offset(r.l, r.t).contains(sp.x, sp.y)) item = static_cast<int>(k);
        return true;
    }
    return false;
}

int bar_item_at(Gui& g, Window* w, Point sp) {
    Menu* m = g.menu(w->menu);
    if (!m) return -1;
    Rect wr = g.window_rect_screen(w);
    // The bar starts after the frame and caption; menu_bar_layout put the
    // item rectangles relative to the bar.
    int f = frame_for_style(w->style, w->exstyle);
    int top = f + (((w->style & WS_CAPTION) == WS_CAPTION) ? kCaptionH : 0);
    for (size_t i = 0; i < m->items.size(); ++i)
        if (m->items[i].rect.offset(wr.l + f, wr.t + top).contains(sp.x, sp.y)) return static_cast<int>(i);
    return -1;
}

Rect bar_item_screen(Gui& g, Window* w, int idx) {
    Menu* m = g.menu(w->menu);
    Rect wr = g.window_rect_screen(w);
    int f = frame_for_style(w->style, w->exstyle);
    int top = f + (((w->style & WS_CAPTION) == WS_CAPTION) ? kCaptionH : 0);
    return m->items[static_cast<size_t>(idx)].rect.offset(wr.l + f, wr.t + top);
}

void open_bar_item(Gui& g, Window* w, int idx) {
    MenuTracking* t = g.tracking;
    close_levels_from(g, 0);
    t->bar_sel = idx;
    repaint_bar(g);
    Menu* m = g.menu(w->menu);
    if (!m || idx < 0 || idx >= static_cast<int>(m->items.size())) return;
    const MenuItem& it = m->items[static_cast<size_t>(idx)];
    uint32_t flags = (it.state & 0x3) | (it.submenu ? MF_POPUP : 0) | MF_HILITE;
    g.send(t->owner, WM_MENUSELECT, (flags << 16) | static_cast<uint32_t>(idx), w->menu);
    if (it.submenu && !(it.state & (MF_GRAYED | MF_DISABLED))) {
        Rect r = bar_item_screen(g, w, idx);
        open_level(g, it.submenu, r.l, r.b, &r, idx, false);
    }
}

void choose(Gui& g, const MenuItem& it) {
    MenuTracking* t = g.tracking;
    if (it.state & (MF_GRAYED | MF_DISABLED)) return;
    if (it.submenu || (it.type & MFT_SEPARATOR)) return;
    t->result = it.id;
    t->done = true;
}

void paint_everything(Gui& g) {
    for (int k = 0; k < 200; ++k) {
        Window* p = g.find_paint_window(g.desktop, 0);
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
}

// The loop itself.
void run_tracking(Gui& g) {
    MenuTracking* t = g.tracking;
    bool button_was_down = (g.key_state[1] & 0x80) != 0;
    uint64_t opened_at = g.host->now_ms();
    while (!t->done) {
        // Posted messages for other windows still flow (timers excluded).
        Msg pm;
        while (g.peek_posted(pm, 0, 0, 0, true)) {
            if (pm.message == WM_QUIT) {
                g.quit_posted = true;
                t->done = true;
                break;
            }
            g.dispatch(pm);
        }
        paint_everything(g);
        g.present_now();
        g.pump_host(200);
        while (!g.input.empty() && !t->done) {
            HostEvent ev = g.input.front();
            g.input.erase(g.input.begin());
            if (ev.type == HostEvent::Resize) {
                Msg dummy;
                g.translate_input(ev, dummy);
                t->done = true;
                break;
            }
            if (ev.type == HostEvent::MouseMove || ev.type == HostEvent::MouseDown || ev.type == HostEvent::MouseUp) {
                g.cursor_pos = {ev.x, ev.y};
                Point sp{ev.x, ev.y};
                size_t lvl;
                int item;
                bool over = hit(g, sp, lvl, item);
                if (ev.type == HostEvent::MouseDown) g.key_state[ev.button == 1 ? 2 : 1] |= 0x80;
                if (ev.type == HostEvent::MouseUp) g.key_state[ev.button == 1 ? 2 : 1] &= 0x7F;
                if (over) {
                    select_item(g, lvl, item);
                    close_levels_from(g, lvl + 1 + (item >= 0 && g.menu(t->levels[lvl].hmenu) &&
                                                    g.menu(t->levels[lvl].hmenu)->items[static_cast<size_t>(item)].submenu
                                                ? 1 : 0));
                    if (item >= 0) open_submenu(g, lvl);
                    if (ev.type == HostEvent::MouseUp && item >= 0) {
                        // A release just after opening (the press that opened it) does not choose.
                        if (!(button_was_down && g.host->now_ms() - opened_at < 300 && lvl == 0 && false)) {
                            Menu* m = g.menu(t->levels[lvl].hmenu);
                            if (m) choose(g, m->items[static_cast<size_t>(item)]);
                        }
                    }
                    continue;
                }
                // Over the menu bar: switch to another top-level item.
                if (t->bar_hwnd) {
                    Window* bw = g.win(t->bar_hwnd);
                    int bi = bw ? bar_item_at(g, bw, sp) : -1;
                    if (bi >= 0) {
                        if (bi != t->bar_sel && (ev.type == HostEvent::MouseMove || ev.type == HostEvent::MouseDown))
                            open_bar_item(g, bw, bi);
                        else if (ev.type == HostEvent::MouseDown && bi == t->bar_sel)
                            t->done = true;  // clicking the open item closes it
                        continue;
                    }
                }
                if (ev.type == HostEvent::MouseDown) {
                    t->done = true;  // a click elsewhere cancels
                }
                continue;
            }
            if (ev.type == HostEvent::KeyDown) {
                if (ev.key == 0x12 || ev.key == 0x10 || ev.key == 0x11) continue;  // modifiers
                size_t lvl = t->levels.empty() ? 0 : t->levels.size() - 1;
                MenuTracking::Level* L = t->levels.empty() ? nullptr : &t->levels[lvl];
                Menu* m = L ? g.menu(L->hmenu) : nullptr;
                auto step = [&](int dir) {
                    if (!m || m->items.empty()) return;
                    int n = static_cast<int>(m->items.size());
                    int s = L->sel;
                    for (int k = 0; k < n; ++k) {
                        s = (s + dir + n) % n;
                        if (!(m->items[static_cast<size_t>(s)].type & MFT_SEPARATOR)) break;
                    }
                    select_item(g, lvl, s);
                };
                switch (ev.key) {
                    case 0x1B:  // Esc
                        if (t->levels.size() > 1) close_levels_from(g, t->levels.size() - 1);
                        else if (t->bar_hwnd && !t->levels.empty()) {
                            close_levels_from(g, 0);
                            repaint_bar(g);
                        } else t->done = true;
                        break;
                    case 0x26: step(-1); break;  // up
                    case 0x28:                   // down
                        if (!L && t->bar_hwnd) open_bar_item(g, g.win(t->bar_hwnd), t->bar_sel < 0 ? 0 : t->bar_sel);
                        else step(1);
                        break;
                    case 0x27:  // right
                        if (m && L->sel >= 0 && m->items[static_cast<size_t>(L->sel)].submenu) {
                            open_submenu(g, lvl);
                            if (t->levels.size() > lvl + 1) select_item(g, lvl + 1, 0);
                        } else if (t->bar_hwnd) {
                            Menu* bm = g.menu(g.win(t->bar_hwnd)->menu);
                            int n = bm ? static_cast<int>(bm->items.size()) : 0;
                            if (n) open_bar_item(g, g.win(t->bar_hwnd), (t->bar_sel + 1) % n);
                        }
                        break;
                    case 0x25:  // left
                        if (t->levels.size() > 1) close_levels_from(g, t->levels.size() - 1);
                        else if (t->bar_hwnd) {
                            Menu* bm = g.menu(g.win(t->bar_hwnd)->menu);
                            int n = bm ? static_cast<int>(bm->items.size()) : 0;
                            if (n) open_bar_item(g, g.win(t->bar_hwnd), (t->bar_sel - 1 + n) % n);
                        }
                        break;
                    case 0x0D:  // Enter
                        if (m && L->sel >= 0) {
                            const MenuItem& it = m->items[static_cast<size_t>(L->sel)];
                            if (it.submenu) {
                                open_submenu(g, lvl);
                                if (t->levels.size() > lvl + 1) select_item(g, lvl + 1, 0);
                            } else {
                                choose(g, it);
                            }
                        } else if (!L && t->bar_hwnd) {
                            open_bar_item(g, g.win(t->bar_hwnd), t->bar_sel < 0 ? 0 : t->bar_sel);
                        }
                        break;
                }
                continue;
            }
            if (ev.type == HostEvent::Char) {
                // Mnemonics: the character after '&'.
                char16_t c = static_cast<char16_t>(ev.ch);
                auto up = [](char16_t x) { return x >= 'a' && x <= 'z' ? static_cast<char16_t>(x - 32) : x; };
                if (!t->levels.empty()) {
                    size_t lvl = t->levels.size() - 1;
                    Menu* m = g.menu(t->levels[lvl].hmenu);
                    if (!m) continue;
                    for (size_t k = 0; k < m->items.size(); ++k) {
                        const std::u16string& s = m->items[k].text;
                        size_t a = s.find(u'&');
                        if (a != std::u16string::npos && a + 1 < s.size() && up(s[a + 1]) == up(c)) {
                            select_item(g, lvl, static_cast<int>(k));
                            if (m->items[k].submenu) {
                                open_submenu(g, lvl);
                                if (t->levels.size() > lvl + 1) select_item(g, lvl + 1, 0);
                            } else {
                                choose(g, m->items[k]);
                            }
                            break;
                        }
                    }
                } else if (t->bar_hwnd) {
                    Window* bw = g.win(t->bar_hwnd);
                    Menu* bm = bw ? g.menu(bw->menu) : nullptr;
                    if (!bm) continue;
                    for (size_t k = 0; k < bm->items.size(); ++k) {
                        const std::u16string& s = bm->items[k].text;
                        size_t a = s.find(u'&');
                        if (a != std::u16string::npos && a + 1 < s.size() && up(s[a + 1]) == up(c)) {
                            open_bar_item(g, bw, static_cast<int>(k));
                            if (!t->levels.empty()) select_item(g, 0, 0);
                            break;
                        }
                    }
                }
            }
        }
    }
    close_levels_from(g, 0);
}

}  // namespace

uint32_t track_popup_menu(Gui& g, uint32_t hmenu, uint32_t flags, int x, int y, uint32_t owner,
                          const Rect* exclude) {
    if (g.tracking || !g.menu(hmenu)) return 0;
    ensure_popup_class(g.emu);
    MenuTracking t;
    t.owner = owner;
    g.tracking = &t;
    g.send(owner, WM_ENTERMENULOOP, 1, 0);
    g.send(owner, WM_INITMENU, hmenu, 0);
    // Alignment flags.
    Menu* m = g.menu(hmenu);
    PopupLayout L = layout_popup(g, m, owner);
    if (flags & 0x8) x -= L.w;            // TPM_RIGHTALIGN
    else if (flags & 0x4) x -= L.w / 2;   // TPM_CENTERALIGN
    if (flags & 0x20) y -= L.h;           // TPM_BOTTOMALIGN
    else if (flags & 0x10) y -= L.h / 2;  // TPM_VCENTERALIGN
    open_level(g, hmenu, x, y, exclude, 0, false);
    run_tracking(g);
    g.tracking = nullptr;
    if (g.win(owner)) {
        g.send(owner, WM_MENUSELECT, 0xFFFF0000, 0);
        g.send(owner, WM_EXITMENULOOP, 1, 0);
    }
    uint32_t r = t.result;
    if (flags & 0x100) return r;  // TPM_RETURNCMD
    if (r && !(flags & 0x80) && g.win(owner)) g.post(owner, WM_COMMAND, r & 0xFFFF, 0);  // TPM_NONOTIFY
    return r ? 1 : 0;
}

void menu_bar_track(Gui& g, Window* w, Point sp, bool keyboard, int key) {
    if (g.tracking || !g.menu(w->menu)) return;
    ensure_popup_class(g.emu);
    MenuTracking t;
    t.owner = w->hwnd;
    t.bar_hwnd = w->hwnd;
    t.keyboard = keyboard;
    g.tracking = &t;
    uint32_t hwnd = w->hwnd;
    g.send(hwnd, WM_ENTERMENULOOP, 0, 0);
    g.send(hwnd, WM_INITMENU, w->menu, 0);
    w = g.win(hwnd);
    if (w) {
        if (keyboard) {
            t.bar_sel = 0;
            repaint_bar(g);
            if (key) {
                HostEvent ev;
                ev.type = HostEvent::Char;
                ev.ch = static_cast<uint32_t>(key);
                g.input.insert(g.input.begin(), ev);
            }
        } else {
            int bi = bar_item_at(g, w, sp);
            if (bi >= 0) open_bar_item(g, w, bi);
            else t.done = true;
        }
        run_tracking(g);
    }
    g.tracking = nullptr;
    if ((w = g.win(hwnd))) {
        nc_paint(g, w, nullptr);
        g.send(hwnd, WM_MENUSELECT, 0xFFFF0000, 0);
        g.send(hwnd, WM_EXITMENULOOP, 0, 0);
        if (t.result) g.post(hwnd, WM_COMMAND, t.result & 0xFFFF, 0);
    }
}

// ---------------------------------------------------------------------------
// Accelerators
// ---------------------------------------------------------------------------

bool translate_accelerator(Gui& g, uint32_t hwnd, uint32_t haccel, const Msg& m) {
    auto it = g.accels.find(haccel);
    if (it == g.accels.end() || !g.win(hwnd)) return false;
    bool key = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
    bool chr = m.message == WM_CHAR || m.message == WM_SYSCHAR;
    if (!key && !chr) return false;
    bool shift = (g.key_state[0x10] & 0x80) != 0, ctrl = (g.key_state[0x11] & 0x80) != 0,
         alt = (g.key_state[0x12] & 0x80) != 0;
    for (const AccelEntry& a : it->second) {
        bool virt = (a.flags & 1) != 0;
        if (virt != key) continue;
        if (virt) {
            if (a.key != (m.wparam & 0xFFFF)) continue;
            if (((a.flags & 4) != 0) != shift || ((a.flags & 8) != 0) != ctrl || ((a.flags & 0x10) != 0) != alt) continue;
        } else {
            if (a.key != (m.wparam & 0xFFFF)) continue;
            if ((a.flags & 0x10) && !alt) continue;
        }
        // A disabled menu command is not executed.
        Window* w = g.win(hwnd);
        Window* top = g.toplevel_of(w);
        if (top && top->menu) {
            g.send(top->hwnd, WM_INITMENU, top->menu, 0);
            MenuItem* mi = find_item(g, top->menu, a.cmd, 0);
            if (mi && (mi->state & (MF_GRAYED | MF_DISABLED))) return true;
        }
        // HIWORD 1 marks the command as coming from an accelerator.
        g.send(hwnd, WM_COMMAND, (1u << 16) | a.cmd, 0);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The API
// ---------------------------------------------------------------------------

void install_menu(Emulator& e) {
    ensure_popup_class(e);
    api(e, "CreateMenu", 0, [](Emulator& e) { ret(e, new_menu(gui(e), false)); });
    api(e, "CreatePopupMenu", 0, [](Emulator& e) { ret(e, new_menu(gui(e), true)); });
    api(e, "DestroyMenu", 1, [](Emulator& e) {
        Gui& g = gui(e);
        std::function<void(uint32_t)> kill = [&](uint32_t h) {
            Menu* m = g.menu(h);
            if (!m) return;
            std::vector<uint32_t> subs;
            for (auto& it : m->items)
                if (it.submenu) subs.push_back(it.submenu);
            g.menus.erase(h);
            for (uint32_t s : subs) kill(s);
        };
        if (!g.menu(A(e, 0))) return ret(e, 0);
        kill(U(e, 0));
        ret(e, 1);
    });
    api(e, "IsMenu", 1, [](Emulator& e) { ret(e, gui(e).menu(A(e, 0)) ? 1 : 0); });
    api(e, "LoadMenuW", 2, [](Emulator& e) { ret(e, load_menu_resource(gui(e), A(e, 0), A(e, 1))); });
    api(e, "LoadMenuA", 2, [](Emulator& e) { ret(e, load_menu_resource(gui(e), A(e, 0), A(e, 1))); });
    api(e, "LoadMenuIndirectW", 1, [](Emulator& e) { ret(e, load_menu_template(gui(e), A(e, 0))); });
    api(e, "GetMenu", 1, [](Emulator& e) {
        Window* w = gui(e).win(A(e, 0));
        ret(e, w && !(w->style & WS_CHILD) ? w->menu : 0);
    });
    api(e, "SetMenu", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w || (w->style & WS_CHILD)) return ret(e, 0);
        w->menu = U(e, 1);
        g.set_window_pos(w->hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ret(e, 1);
    });
    api(e, "DrawMenuBar", 1, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        g.set_window_pos(w->hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ret(e, 1);
    });
    api(e, "GetSubMenu", 2, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        int i = I(e, 1);
        if (!m || i < 0 || i >= static_cast<int>(m->items.size())) return ret(e, 0);
        ret(e, m->items[static_cast<size_t>(i)].submenu);
    });
    api(e, "GetMenuItemCount", 1, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        ret(e, m ? m->items.size() : 0xFFFFFFFF);
    });
    api(e, "GetMenuItemID", 2, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        int i = I(e, 1);
        if (!m || i < 0 || i >= static_cast<int>(m->items.size())) return ret(e, 0xFFFFFFFF);
        const MenuItem& it = m->items[static_cast<size_t>(i)];
        ret(e, it.submenu ? 0xFFFFFFFF : it.id);
    });
    api(e, "GetMenuState", 3, [](Emulator& e) {
        Gui& g = gui(e);
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2));
        if (!it) return ret(e, 0xFFFFFFFF);
        uint32_t s = it->state | it->type;
        if (it->submenu) {
            Menu* sm = g.menu(it->submenu);
            s = (static_cast<uint32_t>(sm ? sm->items.size() : 0) << 8) | (s & 0xFF) | MF_POPUP;
        }
        ret(e, s);
    });
    api(e, "GetMenuStringW", 5, [](Emulator& e) {
        MenuItem* it = find_item(gui(e), U(e, 0), U(e, 1), U(e, 4));
        if (!it) return ret(e, 0);
        if (!A(e, 2) || I(e, 3) <= 0) return ret(e, it->text.size());
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 2), it->text, I(e, 3))));
    });
    auto insert = [](Emulator& e, uint32_t hmenu, uint32_t pos, uint32_t flags, uint64_t id, uint64_t data, bool append) {
        Gui& g = gui(e);
        Menu* m = g.menu(hmenu);
        if (!m) return false;
        MenuItem it;
        apply_flags(g, e, it, flags, id, data);
        if (append) {
            m->items.push_back(it);
            return true;
        }
        size_t at = m->items.size();
        if (flags & MF_BYPOSITION) {
            if (pos < m->items.size()) at = pos;
        } else {
            Menu* owner = nullptr;
            int idx = -1;
            if (find_item(g, hmenu, pos, 0, &owner, &idx) && owner) {
                owner->items.insert(owner->items.begin() + idx, it);
                return true;
            }
        }
        m->items.insert(m->items.begin() + static_cast<ptrdiff_t>(at), it);
        return true;
    };
    api(e, "AppendMenuW", 4, [insert](Emulator& e) { ret(e, insert(e, U(e, 0), 0, U(e, 1), A(e, 2), A(e, 3), true) ? 1 : 0); });
    api(e, "InsertMenuW", 5, [insert](Emulator& e) {
        ret(e, insert(e, U(e, 0), U(e, 1), U(e, 2), A(e, 3), A(e, 4), false) ? 1 : 0);
    });
    api(e, "ModifyMenuW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2));
        if (!it) return ret(e, 0);
        apply_flags(g, e, *it, U(e, 2), A(e, 3), A(e, 4));
        ret(e, 1);
    });
    auto remove = [](Emulator& e, bool destroy) {
        Gui& g = gui(e);
        Menu* owner = nullptr;
        int idx = -1;
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2), &owner, &idx);
        if (!it || !owner) return ret(e, 0);
        uint32_t sub = it->submenu;
        owner->items.erase(owner->items.begin() + idx);
        if (destroy && sub) g.menus.erase(sub);
        ret(e, 1);
    };
    api(e, "RemoveMenu", 3, [remove](Emulator& e) { remove(e, false); });
    api(e, "DeleteMenu", 3, [remove](Emulator& e) { remove(e, true); });
    api(e, "InsertMenuItemW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Menu* m = g.menu(A(e, 0));
        if (!m) return ret(e, 0);
        MenuItem it;
        read_item_info(g, e, A(e, 3), it);
        size_t at = m->items.size();
        if (U(e, 2)) {
            if (U(e, 1) < m->items.size()) at = U(e, 1);
        } else {
            Menu* owner = nullptr;
            int idx = -1;
            if (find_item(g, U(e, 0), U(e, 1), 0, &owner, &idx) && owner) {
                owner->items.insert(owner->items.begin() + idx, it);
                return ret(e, 1);
            }
        }
        m->items.insert(m->items.begin() + static_cast<ptrdiff_t>(at), it);
        ret(e, 1);
    });
    api(e, "GetMenuItemInfoW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2) ? MF_BYPOSITION : 0);
        if (!it) return ret(e, 0);
        write_item_info(e, A(e, 3), *it);
        ret(e, 1);
    });
    api(e, "SetMenuItemInfoW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2) ? MF_BYPOSITION : 0);
        if (!it) return ret(e, 0);
        read_item_info(g, e, A(e, 3), *it);
        ret(e, 1);
    });
    api(e, "EnableMenuItem", 3, [](Emulator& e) {
        Gui& g = gui(e);
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2));
        if (!it) return ret(e, 0xFFFFFFFF);
        uint32_t old = it->state & 0x3;
        it->state = (it->state & ~0x3u) | (U(e, 2) & 0x3);
        ret(e, old);
    });
    api(e, "CheckMenuItem", 3, [](Emulator& e) {
        Gui& g = gui(e);
        MenuItem* it = find_item(g, U(e, 0), U(e, 1), U(e, 2));
        if (!it) return ret(e, 0xFFFFFFFF);
        uint32_t old = it->state & MF_CHECKED;
        it->state = (it->state & ~MF_CHECKED) | (U(e, 2) & MF_CHECKED);
        ret(e, old);
    });
    api(e, "CheckMenuRadioItem", 5, [](Emulator& e) {
        Gui& g = gui(e);
        Menu* m = g.menu(A(e, 0));
        if (!m) return ret(e, 0);
        bool bypos = (U(e, 4) & MF_BYPOSITION) != 0;
        for (size_t i = 0; i < m->items.size(); ++i) {
            MenuItem& it = m->items[i];
            uint32_t key = bypos ? static_cast<uint32_t>(i) : it.id;
            if (key < U(e, 1) || key > U(e, 2)) continue;
            if (key == U(e, 3)) {
                it.state |= MF_CHECKED;
                it.type |= MFT_RADIOCHECK;
            } else {
                it.state &= ~MF_CHECKED;
            }
        }
        ret(e, 1);
    });
    api(e, "HiliteMenuItem", 4, [](Emulator& e) { ret(e, 1); });
    api(e, "SetMenuItemBitmaps", 5, [](Emulator& e) {
        MenuItem* it = find_item(gui(e), U(e, 0), U(e, 1), U(e, 2));
        if (!it) return ret(e, 0);
        it->bmp_unchecked = U(e, 3);
        it->bmp_checked = U(e, 4);
        ret(e, 1);
    });
    api(e, "GetMenuCheckMarkDimensions", 0, [](Emulator& e) { ret(e, make_lparam(13, 13)); });
    api(e, "GetMenuDefaultItem", 3, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        if (!m) return ret(e, 0xFFFFFFFF);
        for (size_t i = 0; i < m->items.size(); ++i)
            if (m->items[i].state & MF_DEFAULT) return ret(e, U(e, 1) ? static_cast<uint32_t>(i) : m->items[i].id);
        ret(e, 0xFFFFFFFF);
    });
    api(e, "SetMenuDefaultItem", 3, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        if (!m) return ret(e, 0);
        for (size_t i = 0; i < m->items.size(); ++i) {
            uint32_t key = U(e, 2) ? static_cast<uint32_t>(i) : m->items[i].id;
            if (key == U(e, 1)) m->items[i].state |= MF_DEFAULT;
            else m->items[i].state &= ~MF_DEFAULT;
        }
        ret(e, 1);
    });
    api(e, "GetMenuInfo", 2, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        if (!m) return ret(e, 0);
        uint64_t p = A(e, 1);
        e.mem.write32(p + 8, m->style);
        e.mem.write32(p + 12, m->max_height);
        e.mem.write32(p + 16, 0);
        e.mem.write32(p + 20, static_cast<uint32_t>(m->context_help));
        e.mem.write32(p + 24, static_cast<uint32_t>(m->menu_data));
        ret(e, 1);
    });
    api(e, "SetMenuInfo", 2, [](Emulator& e) {
        Menu* m = gui(e).menu(A(e, 0));
        if (!m) return ret(e, 0);
        uint64_t p = A(e, 1);
        uint32_t mask = e.mem.read32(p + 4);
        if (mask & 0x10) m->style = e.mem.read32(p + 8);
        if (mask & 0x1) m->max_height = e.mem.read32(p + 12);
        if (mask & 0x4) m->context_help = e.mem.read32(p + 20);
        if (mask & 0x8) m->menu_data = e.mem.read32(p + 24);
        ret(e, 1);
    });
    api(e, "SetMenuContextHelpId", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "GetMenuContextHelpId", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GetSystemMenu", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        if (U(e, 1)) {
            w->sys_menu = 0;
            return ret(e, 0);
        }
        if (!w->sys_menu) {
            uint32_t h = new_menu(g, true);
            Menu* m = g.menu(h);
            auto add = [&](uint32_t id, const char16_t* text) {
                MenuItem it;
                it.id = id;
                it.text = text;
                if (!id) it.type = MFT_SEPARATOR;
                m->items.push_back(it);
            };
            add(0xF120, u"元のサイズに戻す(&R)");
            add(0xF010, u"移動(&M)");
            add(0xF000, u"サイズ変更(&S)");
            add(0xF020, u"最小化(&N)");
            add(0xF030, u"最大化(&X)");
            add(0, u"");
            add(0xF060, u"閉じる(&C)\tAlt+F4");
            w->sys_menu = h;
        }
        ret(e, w->sys_menu);
    });
    api(e, "TrackPopupMenu", 7, [](Emulator& e) {
        Gui& g = gui(e);
        ret(e, track_popup_menu(g, U(e, 0), U(e, 1), I(e, 2), I(e, 3), U(e, 5), nullptr));
    });
    api(e, "TrackPopupMenuEx", 6, [](Emulator& e) {
        Gui& g = gui(e);
        Rect ex;
        const Rect* pex = nullptr;
        if (A(e, 5)) {
            ex = read_rect(e, A(e, 5) + 4);
            pex = &ex;
        }
        ret(e, track_popup_menu(g, U(e, 0), U(e, 1), I(e, 2), I(e, 3), U(e, 4), pex));
    });
    api(e, "EndMenu", 0, [](Emulator& e) {
        Gui& g = gui(e);
        if (g.tracking) g.tracking->done = true;
        ret(e, 1);
    });
    api(e, "GetMenuItemRect", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        Menu* m = g.menu(A(e, 1));
        uint32_t i = U(e, 2);
        if (!w || !m || i >= m->items.size()) return ret(e, 0);
        write_rect(e, A(e, 3), bar_item_screen(g, w, static_cast<int>(i)));
        ret(e, 1);
    });
    api(e, "MenuItemFromPoint", 4, [](Emulator& e) { ret(e, 0xFFFFFFFF); });

    // ---- accelerators -------------------------------------------------------------
    auto make_accel = [](Gui& g, std::vector<AccelEntry> v) {
        uint32_t h = g.new_user_handle(0x0C);
        g.accels[h] = std::move(v);
        return h;
    };
    api(e, "LoadAcceleratorsW", 2, [make_accel](Emulator& e) {
        Gui& g = gui(e);
        uint64_t addr;
        uint32_t size;
        if (!find_resource(e, A(e, 0), 9, A(e, 1), addr, size)) return ret(e, 0);
        std::vector<AccelEntry> v;
        for (uint32_t off = 0; off + 8 <= size; off += 8) {
            AccelEntry a;
            uint16_t flags = e.mem.read16(addr + off);
            a.flags = static_cast<uint8_t>(flags & 0x7F);
            a.key = e.mem.read16(addr + off + 2);
            a.cmd = e.mem.read16(addr + off + 4);
            v.push_back(a);
            if (flags & 0x80) break;
        }
        ret(e, make_accel(g, v));
    });
    e.alias_hook("LoadAcceleratorsW", "LoadAcceleratorsA");
    api(e, "CreateAcceleratorTableW", 2, [make_accel](Emulator& e) {
        Gui& g = gui(e);
        std::vector<AccelEntry> v;
        for (int i = 0; i < I(e, 1); ++i) {
            uint64_t p = A(e, 0) + static_cast<uint64_t>(i) * 6;
            AccelEntry a;
            a.flags = e.mem.read8(p);
            a.key = e.mem.read16(p + 2);
            a.cmd = e.mem.read16(p + 4);
            v.push_back(a);
        }
        ret(e, make_accel(g, v));
    });
    api(e, "DestroyAcceleratorTable", 1, [](Emulator& e) {
        gui(e).accels.erase(U(e, 0));
        ret(e, 1);
    });
    api(e, "CopyAcceleratorTableW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.accels.find(U(e, 0));
        if (it == g.accels.end()) return ret(e, 0);
        if (!A(e, 1)) return ret(e, it->second.size());
        int n = std::min<int>(I(e, 2), static_cast<int>(it->second.size()));
        for (int i = 0; i < n; ++i) {
            uint64_t p = A(e, 1) + static_cast<uint64_t>(i) * 6;
            e.mem.write8(p, it->second[i].flags);
            e.mem.write8(p + 1, 0);
            e.mem.write16(p + 2, it->second[i].key);
            e.mem.write16(p + 4, it->second[i].cmd);
        }
        ret(e, static_cast<uint64_t>(n));
    });
    api(e, "TranslateAcceleratorW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        Msg m = g.read_msg(A(e, 2));
        ret(e, translate_accelerator(g, U(e, 0), U(e, 1), m) ? 1 : 0);
    });
    e.alias_hook("TranslateAcceleratorW", "TranslateAcceleratorA");
    api(e, "TranslateMDISysAccel", 2, [](Emulator& e) { ret(e, 0); });
}

}  // namespace x86emu::win32
