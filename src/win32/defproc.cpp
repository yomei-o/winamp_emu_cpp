// DefWindowProc and the non-client area: frames, captions, caption buttons,
// window-level scroll bars, and the modal loops that move and size windows.
#include <algorithm>
#include <cstring>

#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

namespace {

constexpr int kBtnW = 36;  // caption button width (classic-ish, narrower than Win10's 46)

bool has_caption(const Window* w) { return (w->style & WS_CAPTION) == WS_CAPTION; }

int frame_width(const Window* w) { return frame_for_style(w->style, w->exstyle); }

// The Windows 11 caption: flat, the full height of the top inset.
// flat(): drawn in the Windows 11 style.  modern(): also framed by DWM's
// invisible border, so the caption starts at the top edge (a maximised
// window has its frame pushed off the screen instead).
bool flat(const Window* w) { return dwm_frame(w->style, w->exstyle); }
bool modern(const Window* w) { return flat(w) && !w->maximized; }
constexpr uint32_t kCaptionBg = 0xF3F3F3;

int edge_width(const Window* w) {
    int e = 0;
    if (w->exstyle & WS_EX_CLIENTEDGE) e += 2;
    if (w->exstyle & WS_EX_STATICEDGE) e += 1;
    return e;
}

int caption_height(const Window* w) {
    if (!has_caption(w)) return 0;
    return (w->exstyle & WS_EX_TOOLWINDOW) ? kSmCaptionH : kCaptionH;
}

bool menu_bar_shown(Gui& g, const Window* w) {
    return !(w->style & WS_CHILD) && w->menu && g.menu(w->menu);
}

// Pieces of the non-client area, in window coordinates.
struct NcLayout {
    Rect caption, menubar, client_edge, vscroll, hscroll, sizebox;
    Rect close_btn, max_btn, min_btn, sys_btn;
};

NcLayout layout(Gui& g, Window* w) {
    NcLayout L;
    int W = w->wrect.w(), H = w->wrect.h();
    int f = frame_width(w);
    int top = f;
    int ch = caption_height(w);
    if (ch && flat(w)) {
        // Measured on Windows 11: the caption runs from the top edge to the
        // client, buttons are 46 wide (45 for maximise and minimise), and a
        // window with only a close button gets a narrower one.
        int ct = modern(w) ? 0 : f, cb = f + ch;
        L.caption = {f, ct, W - f, cb};
        if (w->style & WS_SYSMENU) {
            bool boxes = (w->style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) && !(w->exstyle & WS_EX_TOOLWINDOW);
            int cw = boxes ? 46 : 32;
            L.close_btn = {W - f - cw, ct, W - f, cb};
            if (boxes) {
                L.max_btn = {L.close_btn.l - 45, ct, L.close_btn.l, cb};
                L.min_btn = {L.max_btn.l - 45, ct, L.max_btn.l, cb};
            }
            if (!(w->exstyle & (WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME)))
                L.sys_btn = {f, ct, f + 30, cb};
        }
        top += ch;
    } else if (ch) {
        L.caption = {f, f, W - f, f + ch};
        int bh = ch - 2;
        int x = W - f - 1;
        if (w->style & WS_SYSMENU) {
            L.close_btn = {x - kBtnW, f + 1, x, f + 1 + bh};
            x -= kBtnW;
            if (w->style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) {
                if (!(w->exstyle & WS_EX_TOOLWINDOW)) {
                    L.max_btn = {x - kBtnW, f + 1, x, f + 1 + bh};
                    x -= kBtnW;
                    L.min_btn = {x - kBtnW, f + 1, x, f + 1 + bh};
                }
            }
            if (!(w->exstyle & (WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME)))
                L.sys_btn = {f + 2, f + 1, f + 2 + bh, f + 1 + bh};
        }
        top += ch;
    }
    if (menu_bar_shown(g, w)) {
        int mh = menu_bar_layout(g, w, W - 2 * f);
        L.menubar = {f, top, W - f, top + mh};
        top += mh;
    }
    int e = edge_width(w);
    Rect inner{f, top, W - f, H - f};
    L.client_edge = inner;
    inner = {inner.l + e, inner.t + e, inner.r - e, inner.b - e};
    bool vs = (w->style & WS_VSCROLL) && w->vscroll.shown;
    bool hs = (w->style & WS_HSCROLL) && w->hscroll.shown;
    if (vs) L.vscroll = {inner.r - kScrollW, inner.t, inner.r, inner.b - (hs ? kScrollW : 0)};
    if (hs) L.hscroll = {inner.l, inner.b - kScrollW, inner.r - (vs ? kScrollW : 0), inner.b};
    if (vs && hs) L.sizebox = {inner.r - kScrollW, inner.b - kScrollW, inner.r, inner.b};
    return L;
}

void edge_rect(Gui& g, uint32_t hdc, Rect r, uint32_t tl, uint32_t br) {
    draw::fill_rect(g, hdc, {r.l, r.t, r.r - 1, r.t + 1}, tl);
    draw::fill_rect(g, hdc, {r.l, r.t, r.l + 1, r.b - 1}, tl);
    draw::fill_rect(g, hdc, {r.l, r.b - 1, r.r, r.b}, br);
    draw::fill_rect(g, hdc, {r.r - 1, r.t, r.r, r.b}, br);
}

void draw_caption_button(Gui& g, uint32_t hdc, const Rect& r, int kind, bool active, bool pressed, bool maximized) {
    uint32_t bg = g.sys_color(15);  // COLOR_3DFACE
    draw::fill_rect(g, hdc, r, bg);
    if (pressed) {
        edge_rect(g, hdc, r, g.sys_color(16), g.sys_color(20));
    } else {
        edge_rect(g, hdc, r, g.sys_color(20), g.sys_color(21));
        edge_rect(g, hdc, {r.l + 1, r.t + 1, r.r - 1, r.b - 1}, g.sys_color(22), g.sys_color(16));
    }
    uint32_t c = active ? 0 : g.sys_color(17);
    int cx = (r.l + r.r) / 2 + (pressed ? 1 : 0), cy = (r.t + r.b) / 2 + (pressed ? 1 : 0);
    switch (kind) {
        case 0:  // close: an X
            for (int i = -4; i <= 4; ++i) {
                draw::fill_rect(g, hdc, {cx + i, cy + i, cx + i + 2, cy + i + 1}, c);
                draw::fill_rect(g, hdc, {cx + i, cy - i, cx + i + 2, cy - i + 1}, c);
            }
            break;
        case 1:  // maximise / restore
            if (maximized) {
                draw::frame_rect(g, hdc, {cx - 2, cy - 5, cx + 6, cy + 2}, c);
                draw::fill_rect(g, hdc, {cx - 2, cy - 4, cx + 6, cy - 3}, c);
                draw::fill_rect(g, hdc, {cx - 5, cy - 2, cx + 3, cy + 5}, bg);
                draw::frame_rect(g, hdc, {cx - 5, cy - 2, cx + 3, cy + 5}, c);
                draw::fill_rect(g, hdc, {cx - 5, cy - 1, cx + 3, cy}, c);
            } else {
                draw::frame_rect(g, hdc, {cx - 5, cy - 5, cx + 5, cy + 4}, c);
                draw::fill_rect(g, hdc, {cx - 5, cy - 4, cx + 5, cy - 3}, c);
            }
            break;
        case 2:  // minimise
            draw::fill_rect(g, hdc, {cx - 4, cy + 2, cx + 3, cy + 4}, c);
            break;
    }
}

// The Windows 11 caption glyphs, 10x10 from the top-left of the glyph box,
// as measured: '#' full ink, '+' half (the antialiased edge).
const char* const kGlyphClose[10] = {"#+      +#", "+#+    +#+", " +#+  +#+ ", "  +#++#+  ", "   +##+   ",
                                     "   +##+   ", "  +#++#+  ", " +#+  +#+ ", "+#+    +#+", "#+      +#"};
const char* const kGlyphMax[10] = {"+########+", "#        #", "#        #", "#        #", "#        #",
                                   "#        #", "#        #", "#        #", "#        #", "+########+"};
const char* const kGlyphRestore[10] = {"  +######+", "  #      #", "+#######+#", "#       ##", "#       ##",
                                       "#       ##", "#       #+", "#       # ", "#       # ", "+#######+ "};
const char* const kGlyphMin[10] = {"", "", "", "", "", "##########", "", "", "", ""};

uint32_t mix(uint32_t bg, uint32_t fg, int a256) {
    auto ch = [&](int s) {
        uint32_t b = (bg >> s) & 0xFF, f = (fg >> s) & 0xFF;
        return ((b * static_cast<uint32_t>(256 - a256) + f * static_cast<uint32_t>(a256)) >> 8) << s;
    };
    return ch(0) | ch(8) | ch(16);
}

// state: 0 normal, 1 under the mouse, 2 held down.  Colours are COLORREFs.
void draw_flat_button(Gui& g, uint32_t hdc, const Rect& r, int kind, bool active, bool enabled, int state,
                      bool maximized) {
    uint32_t bg = kCaptionBg, ink = active ? 0x171818 : 0x999999;
    if (!enabled) ink = 0xC0C0C0;
    if (state && enabled) {
        if (kind == 0) {
            bg = state == 2 ? 0x323FC8 : 0x1C2BC4;  // #C83F32 held, #C42B1C hot
            ink = 0xFFFFFF;
        } else {
            bg = state == 2 ? 0xCCCCCC : 0xE5E5E5;
        }
    }
    draw::fill_rect(g, hdc, r, bg);
    const char* const* glyph = kind == 0 ? kGlyphClose : kind == 2 ? kGlyphMin : (maximized ? kGlyphRestore : kGlyphMax);
    int x0 = r.l + (r.w() == 32 ? 12 : 18), y0 = r.t + (r.h() >= 31 ? 10 : (r.h() - 10) / 2);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; glyph[y][x]; ++x) {
            if (glyph[y][x] == ' ') continue;
            draw::fill_rect(g, hdc, {x0 + x, y0 + y, x0 + x + 1, y0 + y + 1}, mix(bg, ink, glyph[y][x] == '#' ? 256 : 120));
        }
}

int button_state(Gui& g, const Window* w, int ht) {
    if (g.nc_hot_hwnd != w->hwnd || g.nc_hot_ht != ht) return 0;
    return g.nc_hot_pressed ? 2 : 1;
}

}  // namespace

void nc_set_hot(Gui& g, uint32_t hwnd, int ht, bool pressed) {
    if (ht != HTCLOSE && ht != HTMAXBUTTON && ht != HTMINBUTTON) {
        hwnd = 0;
        ht = 0;
        pressed = false;
    }
    if (hwnd == g.nc_hot_hwnd && ht == g.nc_hot_ht && pressed == g.nc_hot_pressed) return;
    uint32_t old = g.nc_hot_hwnd;
    g.nc_hot_hwnd = hwnd;
    g.nc_hot_ht = ht;
    g.nc_hot_pressed = pressed;
    if (Window* ow = g.win(old)) nc_paint(g, ow, nullptr);
    if (hwnd != old)
        if (Window* nw = g.win(hwnd)) nc_paint(g, nw, nullptr);
}

void nc_calc(Gui& g, Window* w, Rect& client) {
    // `client` comes in as the window rectangle (parent coordinates).
    Rect wr = client;
    int f = frame_width(w);
    int top = f + caption_height(w);
    if (menu_bar_shown(g, w)) top += menu_bar_layout(g, w, wr.w() - 2 * f);
    int e = edge_width(w);
    client = {wr.l + f + e, wr.t + top + e, wr.r - f - e, wr.b - f - e};
    if ((w->style & WS_VSCROLL) && w->vscroll.shown) client.r -= kScrollW;
    if ((w->style & WS_HSCROLL) && w->hscroll.shown) client.b -= kScrollW;
    if (client.r < client.l) client.r = client.l;
    if (client.b < client.t) client.b = client.t;
}

int nc_hittest(Gui& g, Window* w, Point sp) {
    Rect wr = g.window_rect_screen(w);
    if (!wr.contains(sp.x, sp.y)) return HTNOWHERE;
    Rect cr = g.client_rect_screen(w);
    if (cr.contains(sp.x, sp.y)) return HTCLIENT;
    Point p{sp.x - wr.l, sp.y - wr.t};
    int W = wr.w(), H = wr.h();
    NcLayout L = layout(g, w);
    if ((w->style & WS_THICKFRAME) && !w->maximized) {
        int f = kThickFrame, corner = 16;
        // With the Windows 11 caption reaching the top edge, only its first
        // few rows resize.
        bool left = p.x < f, right = p.x >= W - f, top = p.y < (modern(w) ? 5 : f), bottom = p.y >= H - f;
        if (top) return p.x < corner ? HTTOPLEFT : (p.x >= W - corner ? HTTOPRIGHT : HTTOP);
        if (bottom) return p.x < corner ? HTBOTTOMLEFT : (p.x >= W - corner ? HTBOTTOMRIGHT : HTBOTTOM);
        if (left) return p.y < corner ? HTTOPLEFT : (p.y >= H - corner ? HTBOTTOMLEFT : HTLEFT);
        if (right) return p.y < corner ? HTTOPRIGHT : (p.y >= H - corner ? HTBOTTOMRIGHT : HTRIGHT);
    }
    if (!L.caption.empty() && L.caption.contains(p.x, p.y)) {
        if (L.close_btn.contains(p.x, p.y)) return HTCLOSE;
        if (L.max_btn.contains(p.x, p.y)) return HTMAXBUTTON;
        if (L.min_btn.contains(p.x, p.y)) return HTMINBUTTON;
        if (L.sys_btn.contains(p.x, p.y)) return HTSYSMENU;
        return HTCAPTION;
    }
    if (!L.menubar.empty() && L.menubar.contains(p.x, p.y)) return HTMENU;
    if (!L.vscroll.empty() && L.vscroll.contains(p.x, p.y)) return HTVSCROLL;
    if (!L.hscroll.empty() && L.hscroll.contains(p.x, p.y)) return HTHSCROLL;
    if (!L.sizebox.empty() && L.sizebox.contains(p.x, p.y))
        return (w->style & WS_THICKFRAME) ? HTBOTTOMRIGHT : HTBORDER;
    return HTBORDER;
}

void nc_paint(Gui& g, Window* w, const Region*) {
    if (!w || !g.is_visible(w)) return;
    if (frame_width(w) == 0 && !has_caption(w) && edge_width(w) == 0 && !menu_bar_shown(g, w) &&
        !(w->style & (WS_VSCROLL | WS_HSCROLL)))
        return;
    uint32_t hdc = get_dc(g, w->hwnd, true, nullptr);
    DC* d = g.dc(hdc);
    // Everything but the client area.
    Rect wr = g.window_rect_screen(w), cr = g.client_rect_screen(w);
    Region excl(cr.offset(-wr.l, -wr.t));
    Region all(Rect{0, 0, wr.w(), wr.h()});
    all.subtract(excl);
    d->s.clip = all;
    d->s.has_clip = true;
    NcLayout L = layout(g, w);
    int W = wr.w(), H = wr.h();
    bool active = g.active == w->hwnd || (g.toplevel_of(w) && g.toplevel_of(w)->hwnd == g.active);
    if (w->nc_active >= 0) active = w->nc_active != 0;
    int f = frame_width(w);
    if (flat(w)) {
        // DWM's frame: everything outside the client is caption-coloured; the
        // compositor leaves out the invisible edges and draws the border.
        draw::fill_rect(g, hdc, {0, 0, W, H}, kCaptionBg);
    } else if (f) {
        Rect outer{0, 0, W, H};
        if (f == 1) {
            draw::frame_rect(g, hdc, outer, g.sys_color(6));  // COLOR_WINDOWFRAME
        } else {
            draw::fill_rect(g, hdc, outer, g.sys_color(15));
            edge_rect(g, hdc, outer, g.sys_color(22), g.sys_color(21));
            edge_rect(g, hdc, {1, 1, W - 1, H - 1}, g.sys_color(20), g.sys_color(16));
        }
    }
    if (!L.caption.empty() && flat(w)) {
        int tx = L.caption.l + 10;  // a dialog's title starts 10 in
        if (!L.sys_btn.empty()) {
            uint32_t icon = w->icon_small ? w->icon_small : (w->cls ? static_cast<uint32_t>(w->cls->icon_sm ? w->cls->icon_sm : w->cls->icon) : 0);
            int iy = L.caption.t + (L.caption.h() - 16) / 2;
            if (icon) draw::draw_icon(g, hdc, L.caption.l + 8, iy, icon, 16, 16);
            tx = L.caption.l + 30;
        }
        draw::select_font(g, hdc, g.ui_font ? g.ui_font : default_gui_font(g));
        int right = L.min_btn.empty() ? (L.close_btn.empty() ? L.caption.r : L.close_btn.l) : L.min_btn.l;
        Rect tr{tx, L.caption.t + 1, right - 4, L.caption.b};
        d->s.text_color = active ? 0x000000 : 0x999999;
        d->s.bk_mode = 1;
        draw_text_impl(g, hdc, w->text, tr, 0x20 | 0x4 | 0x800 | 0x8000);  // SINGLELINE|VCENTER|NOPREFIX|END_ELLIPSIS
        if (!L.close_btn.empty()) draw_flat_button(g, hdc, L.close_btn, 0, active, true, button_state(g, w, HTCLOSE), false);
        if (!L.max_btn.empty())
            draw_flat_button(g, hdc, L.max_btn, 1, active, (w->style & WS_MAXIMIZEBOX) != 0, button_state(g, w, HTMAXBUTTON), w->maximized);
        if (!L.min_btn.empty())
            draw_flat_button(g, hdc, L.min_btn, 2, active, (w->style & WS_MINIMIZEBOX) != 0, button_state(g, w, HTMINBUTTON), false);
    } else if (!L.caption.empty()) {
        uint32_t cap = active ? g.sys_color(2) : g.sys_color(3);  // COLOR_(IN)ACTIVECAPTION
        uint32_t txt = active ? g.sys_color(9) : g.sys_color(19);
        draw::fill_rect(g, hdc, L.caption, cap);
        int tx = L.caption.l + 6;
        if (!L.sys_btn.empty()) {
            uint32_t icon = w->icon_small ? w->icon_small : (w->cls ? static_cast<uint32_t>(w->cls->icon_sm ? w->cls->icon_sm : w->cls->icon) : 0);
            if (icon) draw::draw_icon(g, hdc, L.sys_btn.l, L.sys_btn.t + (L.sys_btn.h() - 16) / 2, icon, 16, 16);
            tx = L.sys_btn.r + 4;
        }
        uint32_t font = default_gui_font(g);
        draw::select_font(g, hdc, font);
        Rect tr{tx, L.caption.t, (L.min_btn.empty() ? (L.close_btn.empty() ? L.caption.r : L.close_btn.l) : L.min_btn.l) - 4, L.caption.b};
        d->s.text_color = txt;
        d->s.bk_mode = 1;
        draw_text_impl(g, hdc, w->text, tr, 0x20 | 0x4 | 0x800 | 0x8000);  // SINGLELINE|VCENTER|NOPREFIX|END_ELLIPSIS
        if (!L.close_btn.empty()) draw_caption_button(g, hdc, L.close_btn, 0, true, false, false);
        if (!L.max_btn.empty()) draw_caption_button(g, hdc, L.max_btn, 1, (w->style & WS_MAXIMIZEBOX) != 0, false, w->maximized);
        if (!L.min_btn.empty()) draw_caption_button(g, hdc, L.min_btn, 2, (w->style & WS_MINIMIZEBOX) != 0, false, false);
    }
    if (!L.menubar.empty()) menu_bar_draw(g, w, hdc, L.menubar);
    int e = edge_width(w);
    if (e) {
        Rect r = L.client_edge;
        if (w->exstyle & WS_EX_CLIENTEDGE) {
            edge_rect(g, hdc, r, g.sys_color(16), g.sys_color(20));
            edge_rect(g, hdc, {r.l + 1, r.t + 1, r.r - 1, r.b - 1}, g.sys_color(21), g.sys_color(22));
        } else {
            edge_rect(g, hdc, r, g.sys_color(16), g.sys_color(20));
        }
    }
    draw_scrollbars(g, w, hdc);
    release_dc(g, hdc);
}

// ---------------------------------------------------------------------------
// Scroll bars (both the window kind here and the SCROLLBAR control reuse it)
// ---------------------------------------------------------------------------

// Thumb position and size within a track of `len` pixels.
static void thumb_metrics(const ScrollInfo& si, int len, int& pos, int& size) {
    int range = si.max - si.min + 1;
    if (range <= 0 || len <= 0) {
        pos = size = 0;
        return;
    }
    size = si.page ? std::max(8, len * si.page / range) : std::min(kScrollW, len / 2);
    if (size > len) size = len;
    int maxpos = si.max - std::max(si.page - 1, 0);
    int span = maxpos - si.min;
    pos = span > 0 ? (len - size) * (si.pos - si.min) / span : 0;
    pos = std::max(0, std::min(pos, len - size));
}

void draw_scrollbar_rect(Gui& g, uint32_t hdc, const Rect& r, const ScrollInfo& si, bool vertical) {
    uint32_t face = g.sys_color(15), track = 0xF0F0F0 - 0x101010 + 0x101010;
    track = g.sys_color(0);  // COLOR_SCROLLBAR
    draw::fill_rect(g, hdc, r, track);
    int a = vertical ? std::min(kScrollW, r.h() / 2) : std::min(kScrollW, r.w() / 2);
    Rect b1 = vertical ? Rect{r.l, r.t, r.r, r.t + a} : Rect{r.l, r.t, r.l + a, r.b};
    Rect b2 = vertical ? Rect{r.l, r.b - a, r.r, r.b} : Rect{r.r - a, r.t, r.r, r.b};
    for (const Rect& b : {b1, b2}) {
        draw::fill_rect(g, hdc, b, face);
        edge_rect(g, hdc, b, g.sys_color(22), g.sys_color(21));
        edge_rect(g, hdc, {b.l + 1, b.t + 1, b.r - 1, b.b - 1}, g.sys_color(20), g.sys_color(16));
    }
    bool enabled = si.enabled && si.max > si.min;
    uint32_t ac = enabled ? 0 : g.sys_color(17);
    auto arrow = [&](const Rect& b, int dir) {
        int cx = (b.l + b.r) / 2, cy = (b.t + b.b) / 2;
        for (int i = 0; i < 4; ++i) {
            switch (dir) {
                case 0: draw::fill_rect(g, hdc, {cx - i, cy - 2 + i, cx + i + 1, cy - 1 + i}, ac); break;
                case 1: draw::fill_rect(g, hdc, {cx - i, cy + 1 - i, cx + i + 1, cy + 2 - i}, ac); break;
                case 2: draw::fill_rect(g, hdc, {cx - 2 + i, cy - i, cx - 1 + i, cy + i + 1}, ac); break;
                case 3: draw::fill_rect(g, hdc, {cx + 1 - i, cy - i, cx + 2 - i, cy + i + 1}, ac); break;
            }
        }
    };
    arrow(b1, vertical ? 0 : 2);
    arrow(b2, vertical ? 1 : 3);
    if (!enabled) return;
    int len = vertical ? r.h() - 2 * a : r.w() - 2 * a;
    int pos, size;
    thumb_metrics(si, len, pos, size);
    if (size <= 0) return;
    Rect t = vertical ? Rect{r.l, r.t + a + pos, r.r, r.t + a + pos + size}
                      : Rect{r.l + a + pos, r.t, r.l + a + pos + size, r.b};
    draw::fill_rect(g, hdc, t, face);
    edge_rect(g, hdc, t, g.sys_color(22), g.sys_color(21));
    edge_rect(g, hdc, {t.l + 1, t.t + 1, t.r - 1, t.b - 1}, g.sys_color(20), g.sys_color(16));
}

void draw_scrollbars(Gui& g, Window* w, uint32_t hdc) {
    NcLayout L = layout(g, w);
    if (!L.vscroll.empty()) draw_scrollbar_rect(g, hdc, L.vscroll, w->vscroll, true);
    if (!L.hscroll.empty()) draw_scrollbar_rect(g, hdc, L.hscroll, w->hscroll, false);
    if (!L.sizebox.empty()) draw::fill_rect(g, hdc, L.sizebox, g.sys_color(15));
}

// Which part of a scroll bar a point is on: SB_LINEUP 0, SB_LINEDOWN 1,
// SB_PAGEUP 2, SB_PAGEDOWN 3, SB_THUMBTRACK 5, or -1.
int scrollbar_part(const Rect& r, const ScrollInfo& si, bool vertical, Point p, int* thumb_off) {
    if (!r.contains(p.x, p.y)) return -1;
    int a = vertical ? std::min(kScrollW, r.h() / 2) : std::min(kScrollW, r.w() / 2);
    int v = vertical ? p.y - r.t : p.x - r.l;
    int total = vertical ? r.h() : r.w();
    if (v < a) return 0;
    if (v >= total - a) return 1;
    int len = total - 2 * a, pos, size;
    thumb_metrics(si, len, pos, size);
    int t = v - a;
    if (t < pos) return 2;
    if (t >= pos + size) return 3;
    if (thumb_off) *thumb_off = t - pos;
    return 5;
}

// Converts a thumb pixel offset back to a scroll position.
int scrollbar_pos_from_pixel(const Rect& r, const ScrollInfo& si, bool vertical, int pixel) {
    int a = vertical ? std::min(kScrollW, r.h() / 2) : std::min(kScrollW, r.w() / 2);
    int len = (vertical ? r.h() : r.w()) - 2 * a;
    int pos, size;
    thumb_metrics(si, len, pos, size);
    int maxpos = si.max - std::max(si.page - 1, 0);
    int span = len - size;
    if (span <= 0) return si.min;
    int v = si.min + (pixel * (maxpos - si.min) + span / 2) / span;
    return std::max(si.min, std::min(v, maxpos));
}

// Tracks the mouse on a scroll bar until the button comes up, sending the
// scroll messages to `target`.  `bar_screen` is the bar in screen coordinates.
void track_scrollbar(Gui& g, uint32_t target, uint32_t msg, uint32_t ctl_hwnd, const Rect& bar_screen,
                     ScrollInfo* si_ptr, bool vertical, Point start) {
    int thumb_off = 0;
    int part = scrollbar_part(bar_screen, *si_ptr, vertical, start, &thumb_off);
    if (part < 0) return;
    auto sendv = [&](int code, int pos) {
        g.send(target, msg, (static_cast<uint32_t>(pos & 0xFFFF) << 16) | static_cast<uint32_t>(code), ctl_hwnd);
    };
    if (part != 5) sendv(part, 0);
    uint64_t next_repeat = g.host->now_ms() + 400;
    bool done = false;
    Point cur = start;
    while (!done) {
        // Repaints while tracking.
        for (int k = 0; k < 50; ++k) {
            Window* p = g.find_paint_window(g.desktop, 0);
            if (!p) break;
            Msg m;
            m.hwnd = p->hwnd;
            m.message = WM_PAINT;
            if (p->need_ncpaint && p->update.empty() && !p->internal_paint) {
                p->need_ncpaint = false;
                g.send(p->hwnd, WM_NCPAINT, 1, 0);
                continue;
            }
            g.dispatch(m);
        }
        g.present_now();
        g.pump_host(part == 5 ? -1 : 50);
        while (!g.input.empty()) {
            HostEvent ev = g.input.front();
            g.input.erase(g.input.begin());
            if (ev.type == HostEvent::MouseMove) {
                cur = {ev.x, ev.y};
                g.cursor_pos = cur;
                if (part == 5) {
                    int a = vertical ? std::min(kScrollW, bar_screen.h() / 2) : std::min(kScrollW, bar_screen.w() / 2);
                    int pix = (vertical ? cur.y - bar_screen.t : cur.x - bar_screen.l) - a - thumb_off;
                    int pos = scrollbar_pos_from_pixel(bar_screen, *si_ptr, vertical, pix);
                    si_ptr->track = pos;
                    sendv(5, pos);  // SB_THUMBTRACK
                }
            } else if (ev.type == HostEvent::MouseUp && ev.button == 0) {
                if (part == 5) sendv(4, si_ptr->track);  // SB_THUMBPOSITION
                done = true;
                g.key_state[1] &= 0x7F;
                break;
            }
        }
        if (!done && part != 5 && g.host->now_ms() >= next_repeat) {
            if (scrollbar_part(bar_screen, *si_ptr, vertical, cur, nullptr) == part) sendv(part, 0);
            next_repeat = g.host->now_ms() + 50;
        }
    }
    sendv(8, 0);  // SB_ENDSCROLL
}

// ---------------------------------------------------------------------------
// Moving and sizing
// ---------------------------------------------------------------------------

static void paint_pending(Gui& g) {
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

static void move_size_loop(Gui& g, uint32_t hwnd, int ht, Point start) {
    Window* w = g.win(hwnd);
    if (!w) return;
    g.send(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    Rect orig = w->wrect;
    bool done = false;
    while (!done) {
        paint_pending(g);
        g.present_now();
        g.pump_host(-1);
        while (!g.input.empty()) {
            HostEvent ev = g.input.front();
            g.input.erase(g.input.begin());
            if (ev.type == HostEvent::MouseMove) {
                g.cursor_pos = {ev.x, ev.y};
                int dx = ev.x - start.x, dy = ev.y - start.y;
                Rect r = orig;
                if (ht == HTCAPTION) {
                    r = r.offset(dx, dy);
                } else {
                    if (ht == HTLEFT || ht == HTTOPLEFT || ht == HTBOTTOMLEFT) r.l += dx;
                    if (ht == HTRIGHT || ht == HTTOPRIGHT || ht == HTBOTTOMRIGHT) r.r += dx;
                    if (ht == HTTOP || ht == HTTOPLEFT || ht == HTTOPRIGHT) r.t += dy;
                    if (ht == HTBOTTOM || ht == HTBOTTOMLEFT || ht == HTBOTTOMRIGHT) r.b += dy;
                    if (r.w() < 112) {
                        if (r.l != orig.l) r.l = r.r - 112;
                        else r.r = r.l + 112;
                    }
                    if (r.h() < 27) {
                        if (r.t != orig.t) r.t = r.b - 27;
                        else r.b = r.t + 27;
                    }
                }
                if (!g.win(hwnd)) return;
                g.set_window_pos(hwnd, 0, r.l, r.t, r.w(), r.h(), SWP_NOZORDER | SWP_NOACTIVATE);
            } else if (ev.type == HostEvent::MouseUp) {
                g.key_state[1] &= 0x7F;
                done = true;
                break;
            } else if (ev.type == HostEvent::KeyDown && ev.key == 0x1B) {
                g.set_window_pos(hwnd, 0, orig.l, orig.t, orig.w(), orig.h(), SWP_NOZORDER | SWP_NOACTIVATE);
                done = true;
                break;
            }
        }
    }
    if (g.win(hwnd)) g.send(hwnd, WM_EXITSIZEMOVE, 0, 0);
}

// Tracks a caption button press; true if released over it.
static bool track_caption_button(Gui& g, Window* w, int ht) {
    uint32_t hwnd = w->hwnd;
    bool done = false, inside = true;
    nc_set_hot(g, hwnd, ht, true);
    while (!done) {
        g.present_now();
        g.pump_host(-1);
        while (!g.input.empty()) {
            HostEvent ev = g.input.front();
            g.input.erase(g.input.begin());
            Window* ww = g.win(hwnd);
            if (!ww) return false;
            if (ev.type == HostEvent::MouseMove) {
                inside = nc_hittest(g, ww, {ev.x, ev.y}) == ht;
                nc_set_hot(g, inside ? hwnd : 0, inside ? ht : 0, inside);
            } else if (ev.type == HostEvent::MouseUp) {
                inside = nc_hittest(g, ww, {ev.x, ev.y}) == ht;
                g.key_state[1] &= 0x7F;
                done = true;
                break;
            }
        }
    }
    nc_set_hot(g, 0, 0, false);
    return inside;
}

// ---------------------------------------------------------------------------
// DefWindowProc
// ---------------------------------------------------------------------------

uint64_t def_window_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Emulator& e = g.emu;
    Window* w = g.win(hwnd);
    if (!w) return 0;
    switch (msg) {
        case WM_NCCREATE: {
            uint64_t name = lp ? e.mem.read32(lp + 36) : 0;
            if (name >= 0x10000) {
                if (w->ansi) w->text = cp932_to_utf16(e.mem.read_cstring(name));
                else if (e.mem.read16(name) == 0xFFFF) w->text.clear();  // resource ordinal (static icons)
                else w->text = read_wstr(e, name);
            }
            w->vscroll.shown = (w->style & WS_VSCROLL) != 0;
            w->hscroll.shown = (w->style & WS_HSCROLL) != 0;
            return 1;
        }
        case WM_NCCALCSIZE: {
            Rect r = read_rect(e, lp);
            nc_calc(g, w, r);
            write_rect(e, lp, r);
            return 0;
        }
        case WM_NCHITTEST:
            return static_cast<uint32_t>(nc_hittest(g, w, {lo16(lp), hi16(lp)}));
        case WM_NCPAINT:
            nc_paint(g, w, nullptr);
            return 0;
        case WM_NCACTIVATE:
            // The caption follows wParam, not the active window: the message
            // arrives before activation has moved.
            w->nc_active = wp ? 1 : 0;
            if (has_caption(w)) nc_paint(g, w, nullptr);
            return 1;
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND: {
            uint64_t br = w->cls ? w->cls->background : 0;
            if (!br) return 0;
            if (br <= 31) br = get_sys_color_brush(g, static_cast<int>(br - 1));
            // As Windows does: the clip box, so a window DC passed in (MFC's
            // EraseNonClient) gets its frame erased too - except for a
            // CS_PARENTDC class, whose clip is the parent's.
            Rect r = (w->cls && (w->cls->style & 0x80)) ? Rect{0, 0, w->crect.w(), w->crect.h()}
                                                         : dc_clip_box(g, static_cast<uint32_t>(wp));
            draw::fill_rect_brush(g, static_cast<uint32_t>(wp), r, static_cast<uint32_t>(br));
            return 1;
        }
        case WM_SETTEXT: {
            w->text = lp ? (w->ansi ? cp932_to_utf16(e.mem.read_cstring(lp)) : read_wstr(e, lp)) : u"";
            if (has_caption(w)) {
                w->need_ncpaint = true;
                nc_paint(g, w, nullptr);
            }
            return 1;
        }
        case WM_GETTEXT: {
            if (!wp || !lp) return 0;
            return static_cast<uint64_t>(write_wstr(e, lp, w->text, static_cast<int>(wp)));
        }
        case WM_GETTEXTLENGTH:
            return w->text.size();
        case WM_CLOSE:
            g.destroy_window(hwnd);
            return 0;
        case WM_SYSCOMMAND: {
            uint32_t cmd = static_cast<uint32_t>(wp) & 0xFFF0;
            switch (cmd) {
                case 0xF060:  // SC_CLOSE
                    g.send(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case 0xF030:  // SC_MAXIMIZE
                    g.show_window(hwnd, 3);
                    return 0;
                case 0xF020:  // SC_MINIMIZE
                    return 0;
                case 0xF120:  // SC_RESTORE
                    g.show_window(hwnd, 9);
                    return 0;
                case 0xF010:  // SC_MOVE
                    if (!w->maximized) move_size_loop(g, hwnd, HTCAPTION, {lo16(lp), hi16(lp)});
                    return 0;
                case 0xF000:  // SC_SIZE
                    if (!w->maximized) {
                        static const int kMap[9] = {0, HTLEFT, HTRIGHT, HTTOP, HTTOPLEFT, HTTOPRIGHT, HTBOTTOM, HTBOTTOMLEFT, HTBOTTOMRIGHT};
                        int k = static_cast<int>(wp & 0xF);
                        if (k > 0 && k < 9) move_size_loop(g, hwnd, kMap[k], {lo16(lp), hi16(lp)});
                    }
                    return 0;
                case 0xF090:  // SC_MOUSEMENU
                    menu_bar_track(g, w, {lo16(lp), hi16(lp)}, false, 0);
                    return 0;
                case 0xF100:  // SC_KEYMENU
                    if (menu_bar_shown(g, w)) menu_bar_track(g, w, {0, 0}, true, static_cast<int>(lp));
                    else if (w->style & WS_CHILD) {
                        Window* t = g.toplevel_of(w);
                        if (t) g.send(t->hwnd, WM_SYSCOMMAND, wp, lp);
                    }
                    return 0;
            }
            return 0;
        }
        case WM_NCLBUTTONDOWN: {
            int ht = static_cast<int>(wp);
            Point sp{lo16(lp), hi16(lp)};
            switch (ht) {
                case HTCAPTION: {
                    Window* top = g.toplevel_of(w);
                    if (top && top->hwnd != g.active) g.set_active(top->hwnd, true);
                    g.send(hwnd, WM_SYSCOMMAND, 0xF012, lp);  // SC_MOVE | HTCAPTION
                    return 0;
                }
                case HTLEFT: case HTRIGHT: case HTTOP: case HTTOPLEFT: case HTTOPRIGHT:
                case HTBOTTOM: case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
                    g.send(hwnd, WM_SYSCOMMAND, 0xF000 + static_cast<uint32_t>(ht - HTLEFT + 1), lp);
                    return 0;
                case HTMENU:
                    g.send(hwnd, WM_SYSCOMMAND, 0xF090 | HTMENU, lp);
                    return 0;
                case HTCLOSE:
                case HTMAXBUTTON:
                case HTMINBUTTON: {
                    if (track_caption_button(g, w, ht) && g.win(hwnd)) {
                        Window* ww = g.win(hwnd);
                        uint32_t cmd = ht == HTCLOSE ? 0xF060 : (ht == HTMINBUTTON ? 0xF020 : (ww->maximized ? 0xF120 : 0xF030));
                        g.send(hwnd, WM_SYSCOMMAND, cmd, lp);
                    }
                    return 0;
                }
                case HTVSCROLL:
                case HTHSCROLL: {
                    bool v = ht == HTVSCROLL;
                    NcLayout L = layout(g, w);
                    Rect wr = g.window_rect_screen(w);
                    Rect bar = (v ? L.vscroll : L.hscroll).offset(wr.l, wr.t);
                    track_scrollbar(g, hwnd, v ? WM_VSCROLL : WM_HSCROLL, 0, bar, v ? &w->vscroll : &w->hscroll, v, sp);
                    return 0;
                }
                case HTSYSMENU:
                    return 0;
            }
            return 0;
        }
        case WM_NCLBUTTONDBLCLK:
            if (wp == HTCAPTION && (w->style & WS_MAXIMIZEBOX) && (w->style & WS_THICKFRAME))
                g.send(hwnd, WM_SYSCOMMAND, w->maximized ? 0xF120 : 0xF030, lp);
            else if (wp == HTVSCROLL || wp == HTHSCROLL)
                return def_window_proc(g, hwnd, WM_NCLBUTTONDOWN, wp, lp);
            return 0;
        case WM_NCRBUTTONUP:
            return 0;
        case WM_RBUTTONUP: {
            Point p = g.client_to_screen(w, {lo16(lp), hi16(lp)});
            g.send(hwnd, WM_CONTEXTMENU, hwnd, make_lparam(p.x, p.y));
            return 0;
        }
        case WM_SETCURSOR: {
            int ht = lo16(lp);
            if (w->style & WS_CHILD) {
                Window* p = g.win(w->parent);
                if (p && p->hwnd != g.desktop && g.send(p->hwnd, WM_SETCURSOR, wp, lp)) return 1;
            }
            int idc = 32512;  // IDC_ARROW
            uint64_t cur = 0;
            switch (ht) {
                case HTCLIENT:
                    cur = w->cls ? w->cls->cursor : 0;
                    if (!cur) return 0;
                    break;
                case HTLEFT: case HTRIGHT: idc = 32644; break;          // IDC_SIZEWE
                case HTTOP: case HTBOTTOM: idc = 32645; break;          // IDC_SIZENS
                case HTTOPLEFT: case HTBOTTOMRIGHT: idc = 32642; break; // IDC_SIZENWSE
                case HTTOPRIGHT: case HTBOTTOMLEFT: idc = 32643; break; // IDC_SIZENESW
                case HTERROR:
                    return 0;
            }
            if (!cur) cur = system_cursor(g, idc);
            g.current_cursor = static_cast<uint32_t>(cur);
            auto it = g.cursors.find(g.current_cursor);
            g.host->set_cursor(it != g.cursors.end() ? it->second->system_id : 0);
            return 1;
        }
        case WM_MOUSEACTIVATE:
            if (w->style & WS_CHILD) {
                Window* p = g.win(w->parent);
                if (p && p->hwnd != g.desktop) {
                    uint64_t r = g.send(p->hwnd, WM_MOUSEACTIVATE, wp, lp);
                    if (r) return r;
                }
            }
            return 1;  // MA_ACTIVATE
        case WM_ACTIVATE:
            if ((wp & 0xFFFF) && !w->minimized) {
                // Focus goes to the window unless it already holds it or a child does.
                if (!(g.focus && (g.focus == hwnd || is_child_of(g, g.focus, hwnd)))) g.set_focus(hwnd);
            }
            return 0;
        case WM_WINDOWPOSCHANGED: {
            uint32_t flags = e.mem.read32(lp + 24);
            if (!(flags & SWP_NOMOVE)) g.send(hwnd, WM_MOVE, 0, make_lparam(w->crect.l, w->crect.t));
            if (!(flags & SWP_NOSIZE) && g.win(hwnd)) {
                w = g.win(hwnd);
                uint32_t type = w->maximized ? 2 : (w->minimized ? 1 : 0);
                g.send(hwnd, WM_SIZE, type, make_lparam(w->crect.w(), w->crect.h()));
            }
            return 0;
        }
        case WM_CTLCOLORMSGBOX:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSCROLLBAR:
        case WM_CTLCOLORSTATIC: {
            DC* d = g.dc(wp);
            bool white = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;
            if (d) {
                d->s.text_color = g.sys_color(8);  // COLOR_WINDOWTEXT
                d->s.bk_color = white ? g.sys_color(5) : g.sys_color(15);
            }
            if (msg == WM_CTLCOLORSCROLLBAR) return get_sys_color_brush(g, 0);
            return get_sys_color_brush(g, white ? 5 : 15);
        }
        case WM_CONTEXTMENU:
        case WM_MOUSEWHEEL:
        case WM_HELP:
        case 0x0319:  // WM_APPCOMMAND
            if (w->style & WS_CHILD) {
                Window* p = g.win(w->parent);
                if (p && p->hwnd != g.desktop) return g.send(p->hwnd, msg, wp, lp);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == 0x79) g.send(hwnd, WM_SYSCOMMAND, 0xF100, 0);  // F10
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == 0x73 && (lp & (1u << 29))) {  // Alt+F4
                Window* t = g.toplevel_of(w);
                if (t) g.send(t->hwnd, WM_SYSCOMMAND, 0xF060, 0);
            }
            return 0;
        case WM_SYSKEYUP:
            if (wp == 0x12 || wp == 0x79) {
                Window* t = g.toplevel_of(w);
                if (t) g.send(t->hwnd, WM_SYSCOMMAND, 0xF100, 0);
            }
            return 0;
        case WM_SYSCHAR: {
            if (wp == ' ') return 0;
            Window* t = g.toplevel_of(w);
            if (t) g.send(t->hwnd, WM_SYSCOMMAND, 0xF100, wp);
            return 0;
        }
        case WM_SETREDRAW:
            w->redraw_off = wp == 0;
            if (wp) ++g.geom_ver;
            return 0;
        case WM_SHOWWINDOW:
            return 0;
        case WM_GETICON:
            return wp == 0 ? w->icon_small : w->icon_big;
        case WM_SETICON: {
            uint32_t old;
            if (wp == 0) {
                old = w->icon_small;
                w->icon_small = static_cast<uint32_t>(lp);
            } else {
                old = w->icon_big;
                w->icon_big = static_cast<uint32_t>(lp);
            }
            return old;
        }
        case WM_QUERYDRAGICON:
            return w->cls ? w->cls->icon : 0;
        case 0x0011:  // WM_QUERYENDSESSION
        case 0x0013:  // WM_QUERYOPEN
            return 1;
        case WM_GETMINMAXINFO:
        case WM_NCDESTROY:
            return 0;
        case WM_QUERYUISTATE:
            return 0;
        case WM_PRINT: {
            if (lp & 0x2) nc_paint(g, w, nullptr);  // PRF_NONCLIENT
            if (lp & 0x4) g.send(hwnd, WM_PRINTCLIENT, wp, lp);
            return 0;
        }
        case 0x007E:  // WM_DISPLAYCHANGE
        case WM_INPUTLANGCHANGE:
            return 1;
        case WM_NOTIFYFORMAT:
            return 2;  // NFR_UNICODE
        case WM_CANCELMODE:
            if (g.capture == hwnd) g.capture = 0;
            return 0;
        case WM_VKEYTOITEM:
        case WM_CHARTOITEM:
            return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
        case WM_DROPFILES:
            return 0;
        case 0x0290:  // WM_IME_KEYDOWN
        case 0x0286:  // WM_IME_CHAR
            if (msg == 0x0286) g.send(hwnd, WM_CHAR, wp, lp);
            return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Scroll bar APIs
// ---------------------------------------------------------------------------

void install_scroll_api(Emulator& e) {
    auto bar_of = [](Window* w, int bar) -> ScrollInfo* {
        if (!w) return nullptr;
        if (bar == 0) return &w->hscroll;
        if (bar == 1) return &w->vscroll;
        return nullptr;
    };
    auto redraw_bars = [](Gui& g, Window* w) {
        if (!w || !g.is_visible(w)) return;
        uint32_t hdc = get_dc(g, w->hwnd, true, nullptr);
        draw_scrollbars(g, w, hdc);
        release_dc(g, hdc);
    };
    // Showing or hiding a window's own scroll bar changes its client area.
    auto set_shown = [](Gui& g, Window* w, int bar, bool show) {
        bool changed = false;
        auto apply = [&](ScrollInfo& si, uint32_t style_bit) {
            bool cur = (w->style & style_bit) && si.shown;
            if (cur == show) return;
            si.shown = show;
            if (show) w->style |= style_bit;
            else w->style &= ~style_bit;
            changed = true;
        };
        if (bar == 0 || bar == 3) apply(w->hscroll, WS_HSCROLL);
        if (bar == 1 || bar == 3) apply(w->vscroll, WS_VSCROLL);
        if (changed)
            g.set_window_pos(w->hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    };
    api(e, "SetScrollInfo", 4, [bar_of, redraw_bars](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        int bar = I(e, 1);
        uint64_t p = A(e, 2);
        if (bar == 2) return ret(e, g.send(U(e, 0), 0x00E9, U(e, 3), p));  // SBM_SETSCROLLINFO
        ScrollInfo* si = bar_of(w, bar);
        if (!si || !p) return ret(e, 0);
        uint32_t mask = e.mem.read32(p + 4);
        if (mask & 0x1) {  // SIF_RANGE
            si->min = static_cast<int32_t>(e.mem.read32(p + 8));
            si->max = static_cast<int32_t>(e.mem.read32(p + 12));
        }
        if (mask & 0x2) si->page = static_cast<int32_t>(e.mem.read32(p + 16));  // SIF_PAGE
        if (mask & 0x4) si->pos = static_cast<int32_t>(e.mem.read32(p + 20));   // SIF_POS
        int maxpos = si->max - std::max(si->page - 1, 0);
        si->pos = std::max(si->min, std::min(si->pos, std::max(si->min, maxpos)));
        if (U(e, 3)) redraw_bars(g, w);
        ret(e, static_cast<uint32_t>(si->pos));
    });
    api(e, "GetScrollInfo", 3, [bar_of](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        int bar = I(e, 1);
        uint64_t p = A(e, 2);
        if (bar == 2) return ret(e, g.send(U(e, 0), 0x00EA, 0, p));  // SBM_GETSCROLLINFO
        ScrollInfo* si = bar_of(w, bar);
        if (!si || !p) return ret(e, 0);
        uint32_t mask = e.mem.read32(p + 4);
        if (mask & 0x1) {
            e.mem.write32(p + 8, static_cast<uint32_t>(si->min));
            e.mem.write32(p + 12, static_cast<uint32_t>(si->max));
        }
        if (mask & 0x2) e.mem.write32(p + 16, static_cast<uint32_t>(si->page));
        if (mask & 0x4) e.mem.write32(p + 20, static_cast<uint32_t>(si->pos));
        if (mask & 0x10) e.mem.write32(p + 24, static_cast<uint32_t>(si->track));
        ret(e, 1);
    });
    api(e, "SetScrollPos", 4, [bar_of, redraw_bars](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (I(e, 1) == 2) return ret(e, g.send(U(e, 0), 0x00E0, A(e, 2), A(e, 3)));  // SBM_SETPOS
        ScrollInfo* si = bar_of(w, I(e, 1));
        if (!si) return ret(e, 0);
        int old = si->pos;
        si->pos = std::max(si->min, std::min(I(e, 2), si->max));
        if (U(e, 3)) redraw_bars(g, w);
        ret(e, static_cast<uint32_t>(old));
    });
    api(e, "GetScrollPos", 2, [bar_of](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (I(e, 1) == 2) return ret(e, g.send(U(e, 0), 0x00E1, 0, 0));
        ScrollInfo* si = bar_of(w, I(e, 1));
        ret(e, si ? static_cast<uint32_t>(si->pos) : 0);
    });
    api(e, "SetScrollRange", 5, [bar_of, redraw_bars](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (I(e, 1) == 2) return ret(e, g.send(U(e, 0), 0x00E2, A(e, 2), A(e, 3)));
        ScrollInfo* si = bar_of(w, I(e, 1));
        if (!si) return ret(e, 0);
        si->min = I(e, 2);
        si->max = I(e, 3);
        si->pos = std::max(si->min, std::min(si->pos, si->max));
        if (U(e, 4)) redraw_bars(g, w);
        ret(e, 1);
    });
    api(e, "GetScrollRange", 3, [bar_of](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (I(e, 1) == 2) {
            g.send(U(e, 0), 0x00E3, A(e, 2), A(e, 3));
            return ret(e, 1);
        }
        ScrollInfo* si = bar_of(w, I(e, 1));
        if (!si) return ret(e, 0);
        if (A(e, 2)) e.mem.write32(A(e, 2), static_cast<uint32_t>(si->min));
        if (A(e, 3)) e.mem.write32(A(e, 3), static_cast<uint32_t>(si->max));
        ret(e, 1);
    });
    api(e, "ShowScrollBar", 3, [set_shown](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        if (I(e, 1) == 2) {
            g.show_window(U(e, 0), U(e, 2) ? 5 : 0);
            return ret(e, 1);
        }
        set_shown(g, w, I(e, 1), U(e, 2) != 0);
        ret(e, 1);
    });
    api(e, "EnableScrollBar", 3, [bar_of, redraw_bars](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        bool en = U(e, 2) == 0;  // ESB_ENABLE_BOTH
        int bar = I(e, 1);
        if (bar == 2) {
            g.send(U(e, 0), 0x00E4, U(e, 2), 0);  // SBM_ENABLE_ARROWS
            return ret(e, 1);
        }
        if (bar == 0 || bar == 3) w->hscroll.enabled = en;
        if (bar == 1 || bar == 3) w->vscroll.enabled = en;
        redraw_bars(g, w);
        ret(e, 1);
    });
    api(e, "GetScrollBarInfo", 3, [](Emulator& e) { ret(e, 0); });
}

}  // namespace x86emu::win32
