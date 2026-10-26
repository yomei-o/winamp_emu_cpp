// The standard controls user32 itself provides: BUTTON, STATIC, EDIT,
// LISTBOX, COMBOBOX (with its drop-down ComboLBox) and SCROLLBAR.
#include <algorithm>
#include <cstring>

#include "font.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

RealFont* dc_realfont(Gui& g, uint32_t hdc);
void draw_scrollbar_rect(Gui& g, uint32_t hdc, const Rect& r, const ScrollInfo& si, bool vertical);
void track_scrollbar(Gui& g, uint32_t target, uint32_t msg, uint32_t ctl_hwnd, const Rect& bar_screen,
                     ScrollInfo* si_ptr, bool vertical, Point start);

namespace {

constexpr uint32_t DLGC_WANTARROWS = 0x1, DLGC_WANTTAB = 0x2, DLGC_WANTALLKEYS = 0x4,
                   DLGC_HASSETSEL = 0x8, DLGC_DEFPUSHBUTTON = 0x10, DLGC_UNDEFPUSHBUTTON = 0x20,
                   DLGC_RADIOBUTTON = 0x40, DLGC_WANTCHARS = 0x80, DLGC_STATIC = 0x100,
                   DLGC_BUTTON = 0x2000;

RealFont* font_of(Gui& g, Window* w) {
    GdiObject* f = g.obj(window_font(g, w), ObjKind::Font);
    return realize_font(f ? f->font : LogFont{});
}

int text_width(RealFont* f, const std::u16string& s, size_t from = 0, size_t n = std::u16string::npos) {
    int w = 0;
    size_t end = n == std::u16string::npos ? s.size() : std::min(s.size(), from + n);
    for (size_t i = from; i < end; ++i) w += f->advance(s[i]);
    return w;
}

// Paints with the colours the parent chooses through WM_CTLCOLOR*.
uint32_t prepare_dc(Gui& g, Window* w, uint32_t hdc, uint32_t msg) {
    draw::select_font(g, hdc, window_font(g, w));
    DC* d = g.dc(hdc);
    if (d) d->s.bk_mode = 1;
    return ctl_color(g, w->hwnd, hdc, msg);
}

void send_command(Gui& g, Window* w, uint16_t code) { notify_parent(g, w, code); }

// ===========================================================================
// BUTTON
// ===========================================================================

struct ButtonState : ControlState {
    int check = 0;
    bool pushed = false, focus = false, captured = false;
    uint32_t image = 0;
    int image_type = 0;
};

ButtonState* bs(Window* w) {
    if (!w->ctl) w->ctl = std::make_unique<ButtonState>();
    return static_cast<ButtonState*>(w->ctl.get());
}

void button_paint(Gui& g, Window* w, uint32_t hdc) {
    ButtonState* s = bs(w);
    uint32_t type = w->style & 0xF;
    Rect r{0, 0, w->crect.w(), w->crect.h()};
    bool disabled = (w->style & WS_DISABLED) != 0;
    if (type == 0xB) {  // BS_OWNERDRAW
        Emulator& e = g.emu;
        uint64_t dis = g.scratch_alloc(48);
        e.mem.write32(dis, 4);  // ODT_BUTTON
        e.mem.write32(dis + 4, static_cast<uint32_t>(w->id));
        e.mem.write32(dis + 8, 0);
        e.mem.write32(dis + 12, 1);  // ODA_DRAWENTIRE
        uint32_t state = (s->pushed ? 1u : 0u) | (disabled ? 4u : 0u) | (s->focus ? 0x10u : 0u);
        e.mem.write32(dis + 16, state);
        e.mem.write32(dis + 20, w->hwnd);
        e.mem.write32(dis + 24, hdc);
        write_rect(e, dis + 28, r);
        e.mem.write32(dis + 44, 0);
        Window* p = g.win(w->parent);
        if (p) g.send(p->hwnd, WM_DRAWITEM, w->id, dis);
        g.scratch_free(dis);
        return;
    }
    bool push = type == 0 || type == 1 || type == 0xA || (w->style & 0x1000);  // BS_PUSHLIKE
    uint32_t brush = prepare_dc(g, w, hdc, push ? WM_CTLCOLORBTN : WM_CTLCOLORSTATIC);
    DC* d = g.dc(hdc);
    uint32_t text_color = d ? d->s.text_color : 0;
    if (disabled) text_color = g.sys_color(17);
    uint32_t align = w->style & 0x300, valign = w->style & 0xC00;
    uint32_t fmt = 0;
    if (!(w->style & 0x2000)) fmt |= 0x20;  // BS_MULTILINE off: single line
    else fmt |= 0x10;
    if (type == 7) {  // BS_GROUPBOX
        if (brush) draw::fill_rect_brush(g, hdc, r, brush);
        RealFont* f = font_of(g, w);
        int th = f->height;
        Rect fr{0, th / 2, r.r, r.b};
        draw::edge(g, hdc, fr, 0x6, 0xF);  // EDGE_ETCHED
        if (!w->text.empty()) {
            int tw = text_width(f, w->text) + 4;
            int tx = 8;
            if (align == 0x200) tx = r.r - 8 - tw;
            else if (align == 0x300) tx = (r.r - tw) / 2;
            Rect tr{tx, 0, tx + tw, th};
            if (brush) draw::fill_rect_brush(g, hdc, tr, brush);
            else draw::fill_rect(g, hdc, tr, g.sys_color(15));
            d->s.text_color = text_color;
            Rect t2{tr.l + 2, tr.t, tr.r, tr.b};
            draw_text_impl(g, hdc, w->text, t2, 0x20 | 0x4);
        }
        return;
    }
    if (push) {
        bool down = s->pushed || ((w->style & 0x1000) && s->check);
        draw::fill_rect(g, hdc, r, g.sys_color(15));
        Rect br = r;
        bool def = type == 1;
        if (def) {
            draw::frame_rect(g, hdc, br, g.sys_color(21));
            br = {br.l + 1, br.t + 1, br.r - 1, br.b - 1};
        }
        if (down) {
            draw::frame_rect(g, hdc, br, g.sys_color(16));
        } else {
            draw::edge(g, hdc, br, 0x5, 0xF);  // EDGE_RAISED
        }
        Rect tr{br.l + 2, br.t + 2, br.r - 2, br.b - 2};
        if (down) tr = tr.offset(1, 1);
        if (s->image && (w->style & (0x40 | 0x80))) {
            if (s->image_type == 1) draw::draw_icon(g, hdc, (r.r - 16) / 2, (r.b - 16) / 2, s->image, 0, 0);
            else {
                GdiObject* bo = g.obj(s->image, ObjKind::Bitmap);
                if (bo) draw::blit_bitmap(g, hdc, (r.r - bo->bitmap->px.w) / 2 + (down ? 1 : 0), (r.b - bo->bitmap->px.h) / 2 + (down ? 1 : 0), s->image, 0, 0, bo->bitmap->px.w, bo->bitmap->px.h, 0x00CC0020);
            }
        } else {
            uint32_t f2 = fmt | (align == 0x100 ? 0 : (align == 0x200 ? 0x2 : 0x1));
            f2 |= valign == 0x400 ? 0 : (valign == 0x800 ? 0x8 : 0x4);
            if (disabled) {
                d->s.text_color = g.sys_color(20);
                Rect t2 = tr.offset(1, 1);
                draw_text_impl(g, hdc, w->text, t2, f2);
            }
            d->s.text_color = disabled ? g.sys_color(16) : g.sys_color(18);
            draw_text_impl(g, hdc, w->text, tr, f2);
        }
        if (s->focus) draw::focus_rect(g, hdc, {br.l + 3, br.t + 3, br.r - 3, br.b - 3});
        return;
    }
    // Check boxes and radio buttons.
    if (brush) draw::fill_rect_brush(g, hdc, r, brush);
    bool radio = type == 4 || type == 9;
    int box = 13;
    bool left_text = (w->style & 0x20) != 0;
    int by = (r.h() - box) / 2;
    if (valign == 0x400) by = 0;
    else if (valign == 0x800) by = r.h() - box;
    int bx = left_text ? r.r - box : 0;
    uint32_t st = (radio ? 0x4 : 0x0) | (s->check ? 0x400 : 0) | (s->pushed ? 0x200 : 0) | (disabled ? 0x100 : 0);
    if (s->check == 2 && !radio) st |= 0x400;
    draw::frame_control(g, hdc, {bx, by, bx + box, by + box}, 4, st | (radio ? 0x4 : 0x0));
    Rect tr = left_text ? Rect{0, 0, r.r - box - 4, r.b} : Rect{box + 4, 0, r.r, r.b};
    uint32_t f2 = fmt | (align == 0x200 ? 0x2 : (align == 0x300 ? 0x1 : 0));
    f2 |= valign == 0x400 ? 0 : (valign == 0x800 ? 0x8 : 0x4);
    d->s.text_color = text_color;
    Rect calc = tr;
    draw_text_impl(g, hdc, w->text, tr, f2);
    if (s->focus && !w->text.empty()) {
        draw_text_impl(g, hdc, w->text, calc, (f2 & ~0xCu) | 0x400);
        int th = calc.h();
        int ty = tr.t + (tr.h() - th) / 2;
        draw::focus_rect(g, hdc, {calc.l - 1, ty - 1, std::min(calc.r + 1, r.r), ty + th + 1});
    }
}

void button_repaint(Gui& g, Window* w) {
    if (!g.is_visible(w)) return;
    uint32_t hdc = get_dc(g, w->hwnd, false, nullptr);
    button_paint(g, w, hdc);
    release_dc(g, hdc);
}

void button_click(Gui& g, Window* w) {
    ButtonState* s = bs(w);
    uint32_t type = w->style & 0xF;
    uint32_t hwnd = w->hwnd;
    if (type == 3) s->check = s->check ? 0 : 1;           // BS_AUTOCHECKBOX
    else if (type == 6) s->check = (s->check + 1) % 3;    // BS_AUTO3STATE
    else if (type == 9) {                                  // BS_AUTORADIOBUTTON
        // Uncheck the others in the group.
        Window* p = g.win(w->parent);
        if (p) {
            auto& v = p->children;
            auto it = std::find(v.begin(), v.end(), hwnd);
            size_t i = static_cast<size_t>(it - v.begin());
            size_t start = i;
            while (start > 0 && !(g.win(v[start]) && (g.win(v[start])->style & WS_GROUP))) --start;
            for (size_t k = start; k < v.size(); ++k) {
                Window* o = g.win(v[k]);
                if (!o) continue;
                if (k > start && (o->style & WS_GROUP)) break;
                if (o != w && o->cls && o->cls->name == u"BUTTON" && (o->style & 0xF) == 9 && bs(o)->check) {
                    bs(o)->check = 0;
                    button_repaint(g, o);
                }
            }
        }
        s->check = 1;
    }
    button_repaint(g, w);
    if (g.win(hwnd)) send_command(g, w, 0);  // BN_CLICKED
}

uint64_t button_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    ButtonState* s = bs(w);
    uint32_t type = w->style & 0xF;
    switch (msg) {
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            button_paint(g, w, hdc);
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_GETDLGCODE:
            switch (type) {
                case 0: return DLGC_BUTTON | DLGC_UNDEFPUSHBUTTON;
                case 1: return DLGC_BUTTON | DLGC_DEFPUSHBUTTON;
                case 4: case 9: return DLGC_BUTTON | DLGC_RADIOBUTTON;
                case 7: return DLGC_STATIC;
                default: return DLGC_BUTTON;
            }
        case WM_SETFONT:
            w->font = wp;
            if (lp) button_repaint(g, w);
            return 0;
        case WM_GETFONT:
            return w->font;
        case WM_SETTEXT:
            def_window_proc(g, hwnd, msg, wp, lp);
            button_repaint(g, w);
            return 1;
        case WM_ENABLE:
            button_repaint(g, w);
            return 0;
        case WM_SETFOCUS:
            s->focus = true;
            button_repaint(g, w);
            if (w->style & 0x4000) send_command(g, w, 6);  // BN_SETFOCUS
            return 0;
        case WM_KILLFOCUS:
            s->focus = false;
            if (s->pushed) {
                s->pushed = false;
                if (g.capture == hwnd) g.capture = 0;
            }
            button_repaint(g, w);
            if (w->style & 0x4000) send_command(g, w, 7);  // BN_KILLFOCUS
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (type == 7) return 0;
            if (msg == WM_LBUTTONDBLCLK && (type == 4 || type == 9 || type == 0xB || (w->style & 0x4000))) {
                send_command(g, w, 5);  // BN_DOUBLECLICKED
                if (type == 4 || type == 9) return 0;
            }
            g.set_focus(hwnd);
            g.capture = hwnd;
            s->captured = true;
            s->pushed = true;
            button_repaint(g, w);
            return 0;
        case WM_MOUSEMOVE:
            if (s->captured) {
                bool in = Rect{0, 0, w->crect.w(), w->crect.h()}.contains(lo16(lp), hi16(lp));
                if (in != s->pushed) {
                    s->pushed = in;
                    button_repaint(g, w);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (s->captured) {
                s->captured = false;
                if (g.capture == hwnd) g.capture = 0;
                bool in = s->pushed;
                s->pushed = false;
                button_repaint(g, w);
                if (in && g.win(hwnd)) button_click(g, w);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (s->captured && lp != hwnd) {
                s->captured = false;
                s->pushed = false;
                button_repaint(g, w);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == ' ' && !s->pushed) {
                s->pushed = true;
                button_repaint(g, w);
            }
            return 0;
        case WM_KEYUP:
            if (wp == ' ' && s->pushed) {
                s->pushed = false;
                button_repaint(g, w);
                button_click(g, w);
            }
            return 0;
        case WM_CHAR:
            return 0;
        case 0x00F0:  // BM_GETCHECK
            return static_cast<uint64_t>(s->check);
        case 0x00F1:  // BM_SETCHECK
            if (s->check != static_cast<int>(wp)) {
                s->check = static_cast<int>(wp);
                button_repaint(g, w);
            }
            return 0;
        case 0x00F2:  // BM_GETSTATE
            return static_cast<uint64_t>(s->check | (s->pushed ? 4 : 0) | (s->focus ? 8 : 0));
        case 0x00F3:  // BM_SETSTATE
            s->pushed = wp != 0;
            button_repaint(g, w);
            return 0;
        case 0x00F4:  // BM_SETSTYLE
            w->style = (w->style & 0xFFFF0000) | static_cast<uint32_t>(wp & 0xFFFF);
            if (lp) button_repaint(g, w);
            return 0;
        case 0x00F5:  // BM_CLICK
            button_click(g, w);
            return 0;
        case 0x00F6:  // BM_GETIMAGE
            return s->image;
        case 0x00F7: {  // BM_SETIMAGE
            uint32_t old = s->image;
            s->image = static_cast<uint32_t>(lp);
            s->image_type = static_cast<int>(wp);
            button_repaint(g, w);
            return old;
        }
        case 0x00F8:  // BM_SETDONTCLICK
            return 0;
        case WM_NCHITTEST:
            if (type == 7) return static_cast<uint64_t>(HTTRANSPARENT) & 0xFFFFFFFF;
            break;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

// ===========================================================================
// STATIC
// ===========================================================================

struct StaticState : ControlState {
    uint32_t image = 0;
    int image_type = 0;  // IMAGE_BITMAP 0, IMAGE_ICON 1
};

StaticState* ss(Window* w) {
    if (!w->ctl) w->ctl = std::make_unique<StaticState>();
    return static_cast<StaticState*>(w->ctl.get());
}

void static_paint(Gui& g, Window* w, uint32_t hdc) {
    uint32_t type = w->style & 0x1F;
    Rect r{0, 0, w->crect.w(), w->crect.h()};
    switch (type) {
        case 4: draw::fill_rect(g, hdc, r, g.sys_color(21)); return;   // SS_BLACKRECT
        case 5: draw::fill_rect(g, hdc, r, g.sys_color(16)); return;   // SS_GRAYRECT
        case 6: draw::fill_rect(g, hdc, r, g.sys_color(20)); return;   // SS_WHITERECT
        case 7: draw::frame_rect(g, hdc, r, g.sys_color(21)); return;  // SS_BLACKFRAME
        case 8: draw::frame_rect(g, hdc, r, g.sys_color(16)); return;  // SS_GRAYFRAME
        case 9: draw::frame_rect(g, hdc, r, g.sys_color(20)); return;  // SS_WHITEFRAME
        case 0x10: draw::edge(g, hdc, {0, 0, r.r, 2}, 0x6, 0xF); return;   // SS_ETCHEDHORZ
        case 0x11: draw::edge(g, hdc, {0, 0, 2, r.b}, 0x6, 0xF); return;   // SS_ETCHEDVERT
        case 0x12: draw::edge(g, hdc, r, 0x6, 0xF); return;               // SS_ETCHEDFRAME
        case 0xD: {  // SS_OWNERDRAW
            Emulator& e = g.emu;
            uint64_t dis = g.scratch_alloc(48);
            e.mem.write32(dis, 3);  // ODT_STATIC
            e.mem.write32(dis + 4, static_cast<uint32_t>(w->id));
            e.mem.write32(dis + 12, 1);
            e.mem.write32(dis + 16, (w->style & WS_DISABLED) ? 4 : 0);
            e.mem.write32(dis + 20, w->hwnd);
            e.mem.write32(dis + 24, hdc);
            write_rect(e, dis + 28, r);
            Window* p = g.win(w->parent);
            if (p) g.send(p->hwnd, WM_DRAWITEM, w->id, dis);
            g.scratch_free(dis);
            return;
        }
    }
    uint32_t brush = prepare_dc(g, w, hdc, WM_CTLCOLORSTATIC);
    if (brush) draw::fill_rect_brush(g, hdc, r, brush);
    StaticState* s = ss(w);
    if (type == 3) {  // SS_ICON
        if (s->image) draw::draw_icon(g, hdc, 0, 0, s->image, 0, 0);
        return;
    }
    if (type == 0xE) {  // SS_BITMAP
        GdiObject* bo = g.obj(s->image, ObjKind::Bitmap);
        if (bo) {
            int bw = bo->bitmap->px.w, bh = bo->bitmap->px.h;
            int x = (w->style & 0x200) ? (r.w() - bw) / 2 : 0, y = (w->style & 0x200) ? (r.h() - bh) / 2 : 0;
            draw::blit_bitmap(g, hdc, x, y, s->image, 0, 0, bw, bh, 0x00CC0020);
        }
        return;
    }
    DC* d = g.dc(hdc);
    uint32_t fmt = 0;
    if (type == 1) fmt |= 0x1;
    else if (type == 2) fmt |= 0x2;
    if (type == 0xB || type == 0xC) fmt |= 0x20;  // SS_SIMPLE / SS_LEFTNOWORDWRAP: no wrap
    else fmt |= 0x10;                            // DT_WORDBREAK
    if (w->style & 0x80) fmt |= 0x800;           // SS_NOPREFIX
    if (w->style & 0x200) fmt |= 0x4 | 0x20;     // SS_CENTERIMAGE: vertical centre, single line
    if ((w->style & 0xC000) == 0x4000) fmt |= 0x8000;  // SS_ENDELLIPSIS
    if (w->style & WS_DISABLED) {
        if (d) d->s.text_color = g.sys_color(17);
    }
    draw_text_impl(g, hdc, w->text, r, fmt);
}

uint64_t static_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    switch (msg) {
        case WM_NCCREATE:
            if (w->style & 0x1000) w->exstyle |= WS_EX_STATICEDGE;  // SS_SUNKEN
            break;
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            static_paint(g, w, hdc);
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            if (!(w->style & 0x100)) return static_cast<uint64_t>(HTTRANSPARENT) & 0xFFFFFFFF;  // SS_NOTIFY
            return HTCLIENT;
        case WM_GETDLGCODE:
            return DLGC_STATIC;
        case WM_SETFONT:
            w->font = wp;
            if (lp) g.invalidate(w, nullptr, true, false);
            return 0;
        case WM_GETFONT:
            return w->font;
        case WM_SETTEXT:
            def_window_proc(g, hwnd, msg, wp, lp);
            if ((w->style & 0x1F) == 3 && lp) {
                // An icon name.
            }
            g.invalidate(w, nullptr, true, false);
            return 1;
        case WM_ENABLE:
            g.invalidate(w, nullptr, true, false);
            return 0;
        case WM_LBUTTONDOWN:
            if (w->style & 0x100) send_command(g, w, 0);  // STN_CLICKED
            return 0;
        case WM_LBUTTONDBLCLK:
            if (w->style & 0x100) send_command(g, w, 1);  // STN_DBLCLK
            return 0;
        case 0x0170:  // STM_SETICON
        case 0x0172: {  // STM_SETIMAGE
            StaticState* s = ss(w);
            uint32_t old = s->image;
            s->image = static_cast<uint32_t>(msg == 0x0170 ? wp : lp);
            s->image_type = msg == 0x0170 ? 1 : static_cast<int>(wp);
            if (!(w->style & 0x800) && s->image_type == 1 && (w->style & 0x1F) == 3) {
                // Sizes itself to the icon.
                g.set_window_pos(hwnd, 0, 0, 0, 32, 32, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            g.invalidate(w, nullptr, true, false);
            return old;
        }
        case 0x0171:  // STM_GETICON
        case 0x0173:  // STM_GETIMAGE
            return ss(w)->image;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

// ===========================================================================
// EDIT
// ===========================================================================

struct EditState : ControlState {
    std::u16string text;
    size_t anchor = 0, caret = 0;  // selection is [min, max)
    int scroll_x = 0;              // single line: pixels scrolled
    int top_line = 0;              // multiline
    size_t limit = 30000;
    bool modified = false, focus = false, captured = false;
    char16_t password = 0;
    int margin_l = 1, margin_r = 1;
    std::u16string undo_text;
    size_t undo_caret = 0;
    bool can_undo = false;
    struct Line {
        size_t start, len;
    };
    std::vector<Line> lines;
};

EditState* es(Window* w) {
    if (!w->ctl) w->ctl = std::make_unique<EditState>();
    return static_cast<EditState*>(w->ctl.get());
}

bool multiline(Window* w) { return (w->style & 0x4) != 0; }

std::u16string display_text(Window* w, EditState* s) {
    if (s->password && !multiline(w)) return std::u16string(s->text.size(), s->password);
    return s->text;
}

void edit_layout(Gui& g, Window* w) {
    EditState* s = es(w);
    s->lines.clear();
    if (!multiline(w)) {
        s->lines.push_back({0, s->text.size()});
        return;
    }
    RealFont* f = font_of(g, w);
    int width = w->crect.w() - s->margin_l - s->margin_r;
    bool wrap = !(w->style & 0x80) && !(w->style & WS_HSCROLL);  // no ES_AUTOHSCROLL
    size_t pos = 0;
    while (true) {
        size_t nl = s->text.find(u'\r', pos);
        size_t nl2 = s->text.find(u'\n', pos);
        size_t end = std::min(nl, nl2);
        if (end == std::u16string::npos) end = s->text.size();
        size_t start = pos;
        if (wrap && width > 0) {
            int x = 0;
            size_t ls = start;
            for (size_t i = start; i < end; ++i) {
                int a = f->advance(s->text[i]);
                if (x + a > width && i > ls) {
                    // Prefer breaking after a space.
                    size_t brk = i;
                    for (size_t k = i; k > ls; --k)
                        if (s->text[k - 1] == u' ') {
                            brk = k;
                            break;
                        }
                    s->lines.push_back({ls, brk - ls});
                    ls = brk;
                    x = text_width(f, s->text, ls, i - ls + 1) - a;
                }
                x += a;
            }
            s->lines.push_back({ls, end - ls});
        } else {
            s->lines.push_back({start, end - start});
        }
        if (end >= s->text.size()) break;
        pos = end;
        if (s->text[pos] == u'\r') ++pos;
        if (pos < s->text.size() && s->text[pos] == u'\n') ++pos;
        if (pos >= s->text.size()) {
            s->lines.push_back({pos, 0});
            break;
        }
    }
}

int line_of(EditState* s, size_t pos) {
    for (size_t i = 0; i < s->lines.size(); ++i) {
        const auto& l = s->lines[i];
        bool last = i + 1 == s->lines.size();
        if (pos < l.start + l.len || (pos == l.start + l.len && (last || s->lines[i + 1].start > pos)))
            return static_cast<int>(i);
        if (pos < l.start) return static_cast<int>(i);
    }
    return static_cast<int>(s->lines.size()) - 1;
}

int line_height(Gui& g, Window* w) { return font_of(g, w)->height; }

// Pixel position of a character index, client coordinates.
Point pos_from_char(Gui& g, Window* w, size_t idx) {
    EditState* s = es(w);
    RealFont* f = font_of(g, w);
    std::u16string t = display_text(w, s);
    if (!multiline(w)) {
        int x = s->margin_l + text_width(f, t, 0, idx) - s->scroll_x;
        int th = f->height;
        int y = std::max(0, (w->crect.h() - th) / 2);
        uint32_t al = w->style & 3;
        if (al) {
            int tw = text_width(f, t);
            int avail = w->crect.w() - s->margin_l - s->margin_r;
            int off = al == 1 ? (avail - tw) / 2 : avail - tw;
            if (off > 0) x += off;
        }
        if (w->crect.h() > th + 4 || true) y = (w->crect.h() - th) / 2;
        if (y < 0) y = 0;
        return {x, y};
    }
    int li = line_of(s, idx);
    if (li < 0) return {s->margin_l, 0};
    const auto& l = s->lines[static_cast<size_t>(li)];
    int x = s->margin_l + text_width(f, t, l.start, idx - l.start);
    int y = (li - s->top_line) * f->height;
    return {x, y};
}

size_t char_from_pos(Gui& g, Window* w, int x, int y) {
    EditState* s = es(w);
    RealFont* f = font_of(g, w);
    std::u16string t = display_text(w, s);
    size_t start = 0, len = t.size();
    int base = s->margin_l - s->scroll_x;
    if (multiline(w)) {
        int li = s->top_line + (y >= 0 ? y / std::max(1, f->height) : -1);
        li = std::max(0, std::min(li, static_cast<int>(s->lines.size()) - 1));
        start = s->lines[static_cast<size_t>(li)].start;
        len = s->lines[static_cast<size_t>(li)].len;
        base = s->margin_l;
    } else {
        Point p0 = pos_from_char(g, w, 0);
        base = p0.x;
    }
    int cx = base;
    for (size_t i = 0; i < len; ++i) {
        int a = f->advance(t[start + i]);
        if (x < cx + a / 2) return start + i;
        cx += a;
    }
    return start + len;
}

void edit_scroll_to_caret(Gui& g, Window* w) {
    EditState* s = es(w);
    RealFont* f = font_of(g, w);
    if (!multiline(w)) {
        int avail = w->crect.w() - s->margin_l - s->margin_r;
        std::u16string t = display_text(w, s);
        int cx = text_width(f, t, 0, s->caret);
        if (cx - s->scroll_x > avail) s->scroll_x = cx - avail + avail / 4;
        if (cx - s->scroll_x < 0) s->scroll_x = std::max(0, cx - avail / 4);
        if (!(w->style & 0x80)) s->scroll_x = 0;  // no ES_AUTOHSCROLL
        return;
    }
    int li = line_of(s, s->caret);
    int visible = std::max(1, w->crect.h() / std::max(1, f->height));
    if (li < s->top_line) s->top_line = li;
    if (li >= s->top_line + visible) s->top_line = li - visible + 1;
    if (w->style & WS_VSCROLL) {
        w->vscroll.min = 0;
        w->vscroll.max = std::max(0, static_cast<int>(s->lines.size()) - 1);
        w->vscroll.page = visible;
        w->vscroll.pos = s->top_line;
    }
}

void edit_update_caret(Gui& g, Window* w) {
    EditState* s = es(w);
    if (!s->focus) return;
    Point p = pos_from_char(g, w, s->caret);
    g.caret_hwnd = w->hwnd;
    g.caret_pos = p;
    g.caret_h = line_height(g, w);
    g.caret_w = 1;
}

void edit_paint(Gui& g, Window* w, uint32_t hdc) {
    EditState* s = es(w);
    bool ro = (w->style & 0x800) != 0, disabled = (w->style & WS_DISABLED) != 0;
    uint32_t brush = prepare_dc(g, w, hdc, ro || disabled ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
    DC* d = g.dc(hdc);
    Rect r{0, 0, w->crect.w(), w->crect.h()};
    if (brush) draw::fill_rect_brush(g, hdc, r, brush);
    uint32_t tc = disabled ? g.sys_color(17) : (d ? d->s.text_color : 0);
    RealFont* f = font_of(g, w);
    std::u16string t = display_text(w, s);
    size_t s0 = std::min(s->anchor, s->caret), s1 = std::max(s->anchor, s->caret);
    bool show_sel = s->focus || (w->style & 0x100);
    auto draw_run = [&](size_t from, size_t n, int x, int y) {
        // Text with the selection highlighted.
        size_t end = from + n;
        size_t i = from;
        while (i < end) {
            bool sel = show_sel && i >= s0 && i < s1;
            size_t j = i;
            while (j < end && ((show_sel && j >= s0 && j < s1) == sel)) ++j;
            std::u16string part = t.substr(i, j - i);
            int pw = text_width(f, t, i, j - i);
            if (sel) {
                draw::fill_rect(g, hdc, {x, y, x + pw, y + f->height}, g.sys_color(13));
                draw::text(g, hdc, x, y, part, g.sys_color(14));
            } else {
                draw::text(g, hdc, x, y, part, tc);
            }
            x += pw;
            i = j;
        }
    };
    // Clip to the formatting rectangle.
    if (!multiline(w)) {
        Point p = pos_from_char(g, w, 0);
        draw_run(0, t.size(), p.x, p.y);
    } else {
        int visible = w->crect.h() / std::max(1, f->height) + 1;
        for (int li = s->top_line; li < static_cast<int>(s->lines.size()) && li < s->top_line + visible; ++li) {
            const auto& l = s->lines[static_cast<size_t>(li)];
            draw_run(l.start, l.len, s->margin_l, (li - s->top_line) * f->height);
        }
    }
}

void edit_repaint(Gui& g, Window* w) {
    if (!g.is_visible(w)) return;
    uint32_t hdc = get_dc(g, w->hwnd, false, nullptr);
    edit_paint(g, w, hdc);
    release_dc(g, hdc);
    if (w->style & WS_VSCROLL) {
        uint32_t wdc = get_dc(g, w->hwnd, true, nullptr);
        draw_scrollbars(g, w, wdc);
        release_dc(g, wdc);
    }
}

void edit_changed(Gui& g, Window* w, bool notify) {
    EditState* s = es(w);
    s->modified = true;
    edit_layout(g, w);
    edit_scroll_to_caret(g, w);
    edit_repaint(g, w);
    edit_update_caret(g, w);
    if (notify) {
        uint32_t hwnd = w->hwnd;
        send_command(g, w, 0x400);  // EN_UPDATE
        if (g.win(hwnd)) send_command(g, w, 0x300);  // EN_CHANGE
    }
}

void edit_replace_sel(Gui& g, Window* w, const std::u16string& in, bool notify) {
    EditState* s = es(w);
    std::u16string str = in;
    if (w->style & 0x8) for (auto& c : str) if (c >= 'a' && c <= 'z') c = static_cast<char16_t>(c - 32);
    if (w->style & 0x10) for (auto& c : str) if (c >= 'A' && c <= 'Z') c = static_cast<char16_t>(c + 32);
    if (!multiline(w)) {
        // A single-line edit stops at the first line break.
        size_t nl = str.find_first_of(u"\r\n");
        if (nl != std::u16string::npos) str = str.substr(0, nl);
    }
    size_t s0 = std::min(s->anchor, s->caret), s1 = std::max(s->anchor, s->caret);
    size_t room = s->limit > s->text.size() - (s1 - s0) ? s->limit - (s->text.size() - (s1 - s0)) : 0;
    bool truncated = false;
    if (str.size() > room) {
        str = str.substr(0, room);
        truncated = true;
    }
    s->undo_text = s->text;
    s->undo_caret = s->caret;
    s->can_undo = true;
    s->text = s->text.substr(0, s0) + str + s->text.substr(s1);
    s->caret = s->anchor = s0 + str.size();
    edit_changed(g, w, notify);
    if (truncated && notify) send_command(g, w, 0x501);  // EN_MAXTEXT
}

void edit_set_clipboard(Gui& g, const std::u16string& s) {
    g.clipboard.clear();
    std::vector<uint8_t> data;
    for (char16_t c : s) {
        data.push_back(static_cast<uint8_t>(c & 0xFF));
        data.push_back(static_cast<uint8_t>(c >> 8));
    }
    data.push_back(0);
    data.push_back(0);
    g.clipboard[13] = data;
    g.host->clipboard_text(to_utf8(s));
}

std::u16string edit_get_clipboard(Gui& g) {
    auto it = g.clipboard.find(13);
    std::u16string s;
    if (it == g.clipboard.end()) return s;
    for (size_t i = 0; i + 1 < it->second.size(); i += 2) {
        char16_t c = static_cast<char16_t>(it->second[i] | it->second[i + 1] << 8);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

bool is_word_char(char16_t c) { return c != u' ' && c != u'\t' && c != u'\r' && c != u'\n'; }

uint64_t edit_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    EditState* s = es(w);
    Emulator& e = g.emu;
    bool ml = multiline(w);
    bool ro = (w->style & 0x800) != 0;
    switch (msg) {
        case WM_NCCREATE: {
            uint64_t name = lp ? e.mem.read32(lp + 36) : 0;
            if (name >= 0x10000) s->text = w->ansi ? cp932_to_utf16(e.mem.read_cstring(name)) : read_wstr(e, name);
            if (w->style & 0x20) s->password = u'*';
            def_window_proc(g, hwnd, msg, wp, lp);
            w->text.clear();
            edit_layout(g, w);
            return 1;
        }
        case WM_SIZE:
            edit_layout(g, w);
            edit_scroll_to_caret(g, w);
            return 0;
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            edit_paint(g, w, hdc);
            end_paint(g, hwnd, hdc);
            edit_update_caret(g, w);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_GETDLGCODE: {
            uint64_t c = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
            if (ml && (w->style & 0x1000)) c |= DLGC_WANTALLKEYS;  // ES_WANTRETURN
            else if (ml) c |= DLGC_WANTALLKEYS;
            if (lp) {
                Msg m = g.read_msg(lp);
                if (m.message == WM_KEYDOWN && (m.wparam == 0x0D || m.wparam == 0x1B) && !ml) c &= ~DLGC_WANTALLKEYS;
            }
            return c;
        }
        case WM_SETFONT:
            w->font = wp;
            edit_layout(g, w);
            if (lp) edit_repaint(g, w);
            return 0;
        case WM_GETFONT:
            return w->font;
        case WM_SETTEXT:
            s->text = lp ? (w->ansi ? cp932_to_utf16(e.mem.read_cstring(lp)) : read_wstr(e, lp)) : u"";
            if (s->text.size() > s->limit) s->text.resize(s->limit);
            s->anchor = s->caret = 0;
            s->scroll_x = 0;
            s->top_line = 0;
            s->can_undo = false;
            edit_changed(g, w, false);
            s->modified = false;
            send_command(g, w, 0x300);  // EN_CHANGE (Windows sends it for WM_SETTEXT too)
            return 1;
        case WM_GETTEXT:
            if (!wp) return 0;
            return static_cast<uint64_t>(write_wstr(e, lp, s->text, static_cast<int>(wp)));
        case WM_GETTEXTLENGTH:
            return s->text.size();
        case WM_ENABLE:
            edit_repaint(g, w);
            return 0;
        case WM_SETFOCUS:
            s->focus = true;
            g.caret_hwnd = hwnd;
            g.caret_hide = 0;
            edit_update_caret(g, w);
            edit_repaint(g, w);
            send_command(g, w, 0x100);  // EN_SETFOCUS
            return 0;
        case WM_KILLFOCUS:
            s->focus = false;
            if (g.caret_hwnd == hwnd) g.caret_hwnd = 0;
            edit_repaint(g, w);
            send_command(g, w, 0x200);  // EN_KILLFOCUS
            return 0;
        case WM_LBUTTONDOWN: {
            if (g.focus != hwnd) g.set_focus(hwnd);
            size_t c = char_from_pos(g, w, lo16(lp), hi16(lp));
            s->caret = c;
            if (!(wp & 0x4)) s->anchor = c;
            s->captured = true;
            g.capture = hwnd;
            edit_scroll_to_caret(g, w);
            edit_repaint(g, w);
            edit_update_caret(g, w);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            size_t c = char_from_pos(g, w, lo16(lp), hi16(lp));
            size_t a = c, b = c;
            while (a > 0 && is_word_char(s->text[a - 1])) --a;
            while (b < s->text.size() && is_word_char(s->text[b])) ++b;
            s->anchor = a;
            s->caret = b;
            edit_repaint(g, w);
            edit_update_caret(g, w);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (s->captured) {
                size_t c = char_from_pos(g, w, lo16(lp), hi16(lp));
                if (c != s->caret) {
                    s->caret = c;
                    edit_scroll_to_caret(g, w);
                    edit_repaint(g, w);
                    edit_update_caret(g, w);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (s->captured) {
                s->captured = false;
                if (g.capture == hwnd) g.capture = 0;
            }
            return 0;
        case WM_CHAR: {
            char16_t c = static_cast<char16_t>(wp);
            bool ctrl = (g.key_state[0x11] & 0x80) != 0;
            if (c == 1 && ctrl) {  // Ctrl+A
                s->anchor = 0;
                s->caret = s->text.size();
                edit_repaint(g, w);
                edit_update_caret(g, w);
                return 0;
            }
            if (c == 3) return edit_proc(g, hwnd, 0x0301, 0, 0);   // Ctrl+C
            if (c == 22) return edit_proc(g, hwnd, 0x0302, 0, 0);  // Ctrl+V
            if (c == 24) return edit_proc(g, hwnd, 0x0300, 0, 0);  // Ctrl+X
            if (c == 26) return edit_proc(g, hwnd, 0x0304, 0, 0);  // Ctrl+Z
            if (ro) return 0;
            if (c == 8) {  // backspace
                if (s->anchor == s->caret && s->caret > 0) s->anchor = s->caret - 1;
                if (s->anchor != s->caret) edit_replace_sel(g, w, u"", true);
                return 0;
            }
            if (c == 0x0D) {
                if (ml) edit_replace_sel(g, w, u"\r\n", true);
                return 0;
            }
            if (c == 0x09) {
                if (ml) edit_replace_sel(g, w, u"\t", true);
                return 0;
            }
            if (c < 0x20 || c == 0x7F) return 0;
            if ((w->style & 0x2000) && !(c >= '0' && c <= '9')) return 0;  // ES_NUMBER
            edit_replace_sel(g, w, std::u16string(1, c), true);
            return 0;
        }
        case WM_KEYDOWN: {
            bool shift = (g.key_state[0x10] & 0x80) != 0, ctrl = (g.key_state[0x11] & 0x80) != 0;
            size_t c = s->caret;
            bool moved = true;
            switch (wp) {
                case 0x25:  // left
                    if (!shift && s->anchor != s->caret) c = std::min(s->anchor, s->caret);
                    else if (ctrl) {
                        while (c > 0 && !is_word_char(s->text[c - 1])) --c;
                        while (c > 0 && is_word_char(s->text[c - 1])) --c;
                    } else if (c > 0) {
                        --c;
                        if (c > 0 && s->text[c] == u'\n' && s->text[c - 1] == u'\r') --c;
                    }
                    break;
                case 0x27:  // right
                    if (!shift && s->anchor != s->caret) c = std::max(s->anchor, s->caret);
                    else if (ctrl) {
                        while (c < s->text.size() && is_word_char(s->text[c])) ++c;
                        while (c < s->text.size() && !is_word_char(s->text[c])) ++c;
                    } else if (c < s->text.size()) {
                        ++c;
                        if (c < s->text.size() && s->text[c - 1] == u'\r' && s->text[c] == u'\n') ++c;
                    }
                    break;
                case 0x24:  // home
                    if (ml && !ctrl) c = s->lines[static_cast<size_t>(std::max(0, line_of(s, c)))].start;
                    else c = 0;
                    break;
                case 0x23:  // end
                    if (ml && !ctrl) {
                        const auto& l = s->lines[static_cast<size_t>(std::max(0, line_of(s, c)))];
                        c = l.start + l.len;
                    } else {
                        c = s->text.size();
                    }
                    break;
                case 0x26:  // up
                case 0x28:  // down
                case 0x21:  // page up
                case 0x22:  // page down
                    if (ml) {
                        int li = line_of(s, c);
                        Point p = pos_from_char(g, w, c);
                        int page = std::max(1, w->crect.h() / std::max(1, line_height(g, w)));
                        int delta = wp == 0x26 ? -1 : (wp == 0x28 ? 1 : (wp == 0x21 ? -page : page));
                        int nl = std::max(0, std::min(li + delta, static_cast<int>(s->lines.size()) - 1));
                        c = char_from_pos(g, w, p.x, (nl - s->top_line) * line_height(g, w) + 1);
                    } else {
                        moved = false;
                    }
                    break;
                case 0x2E:  // delete
                    if (ro) return 0;
                    if (shift) return edit_proc(g, hwnd, 0x0300, 0, 0);  // Shift+Del = cut
                    if (s->anchor == s->caret && s->caret < s->text.size()) {
                        s->anchor = s->caret + 1;
                        if (s->anchor < s->text.size() && s->text[s->caret] == u'\r' && s->text[s->anchor] == u'\n') ++s->anchor;
                    }
                    if (s->anchor != s->caret) edit_replace_sel(g, w, u"", true);
                    return 0;
                case 0x2D:  // insert
                    if (ctrl) return edit_proc(g, hwnd, 0x0301, 0, 0);
                    if (shift) return edit_proc(g, hwnd, 0x0302, 0, 0);
                    return 0;
                default:
                    moved = false;
            }
            if (moved) {
                s->caret = c;
                if (!shift) s->anchor = c;
                edit_scroll_to_caret(g, w);
                edit_repaint(g, w);
                edit_update_caret(g, w);
            }
            return 0;
        }
        case WM_VSCROLL:
            if (ml) {
                int page = std::max(1, w->crect.h() / std::max(1, line_height(g, w)));
                int n = static_cast<int>(s->lines.size());
                switch (wp & 0xFFFF) {
                    case 0: s->top_line--; break;
                    case 1: s->top_line++; break;
                    case 2: s->top_line -= page; break;
                    case 3: s->top_line += page; break;
                    case 4: case 5: s->top_line = static_cast<int16_t>(wp >> 16); break;
                }
                s->top_line = std::max(0, std::min(s->top_line, std::max(0, n - page)));
                w->vscroll.pos = s->top_line;
                edit_repaint(g, w);
                edit_update_caret(g, w);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (ml) {
                int d = static_cast<int16_t>(wp >> 16);
                return edit_proc(g, hwnd, WM_VSCROLL, d > 0 ? 0 : 1, 0) | edit_proc(g, hwnd, WM_VSCROLL, d > 0 ? 0 : 1, 0) |
                       edit_proc(g, hwnd, WM_VSCROLL, d > 0 ? 0 : 1, 0);
            }
            return def_window_proc(g, hwnd, msg, wp, lp);
        case 0x0300:  // WM_CUT
            if (ro || s->password) return 0;
            if (s->anchor != s->caret) {
                size_t s0 = std::min(s->anchor, s->caret), s1 = std::max(s->anchor, s->caret);
                edit_set_clipboard(g, s->text.substr(s0, s1 - s0));
                edit_replace_sel(g, w, u"", true);
            }
            return 0;
        case 0x0301:  // WM_COPY
            if (s->anchor != s->caret && !s->password) {
                size_t s0 = std::min(s->anchor, s->caret), s1 = std::max(s->anchor, s->caret);
                edit_set_clipboard(g, s->text.substr(s0, s1 - s0));
            }
            return 0;
        case 0x0302:  // WM_PASTE
            if (!ro) edit_replace_sel(g, w, edit_get_clipboard(g), true);
            return 0;
        case 0x0303:  // WM_CLEAR
            if (!ro && s->anchor != s->caret) edit_replace_sel(g, w, u"", true);
            return 0;
        case 0x0304:  // WM_UNDO
        case 0x00C7:  // EM_UNDO
            if (s->can_undo) {
                std::swap(s->text, s->undo_text);
                s->caret = s->anchor = std::min(s->undo_caret, s->text.size());
                edit_changed(g, w, true);
            }
            return 1;
        case 0x00B0: {  // EM_GETSEL
            uint32_t s0 = static_cast<uint32_t>(std::min(s->anchor, s->caret)), s1 = static_cast<uint32_t>(std::max(s->anchor, s->caret));
            if (wp) e.mem.write32(wp, s0);
            if (lp) e.mem.write32(lp, s1);
            return (s1 << 16) | (s0 & 0xFFFF);
        }
        case 0x00B1: {  // EM_SETSEL
            int32_t a = static_cast<int32_t>(wp), b = static_cast<int32_t>(lp);
            size_t n = s->text.size();
            if (a < 0) {
                s->anchor = s->caret;
            } else {
                s->anchor = std::min(static_cast<size_t>(a), n);
                s->caret = b < 0 ? n : std::min(static_cast<size_t>(b), n);
            }
            edit_scroll_to_caret(g, w);
            edit_repaint(g, w);
            edit_update_caret(g, w);
            return 1;
        }
        case 0x00B2:  // EM_GETRECT
            write_rect(e, lp, {s->margin_l, 0, w->crect.w() - s->margin_r, w->crect.h()});
            return 0;
        case 0x00B3:  // EM_SETRECT
        case 0x00B4:  // EM_SETRECTNP
            return 0;
        case 0x00B6:  // EM_LINESCROLL
            if (ml) {
                s->top_line = std::max(0, std::min(s->top_line + static_cast<int32_t>(lp), static_cast<int>(s->lines.size()) - 1));
                edit_repaint(g, w);
            }
            return 1;
        case 0x00B7:  // EM_SCROLLCARET
            edit_scroll_to_caret(g, w);
            edit_repaint(g, w);
            return 1;
        case 0x00B8:  // EM_GETMODIFY
            return s->modified ? 1 : 0;
        case 0x00B9:  // EM_SETMODIFY
            s->modified = wp != 0;
            return 0;
        case 0x00BA:  // EM_GETLINECOUNT
            return std::max<size_t>(1, s->lines.size());
        case 0x00BB: {  // EM_LINEINDEX
            int li = static_cast<int32_t>(wp);
            if (li < 0) li = line_of(s, s->caret);
            if (li >= static_cast<int>(s->lines.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            return s->lines[static_cast<size_t>(li)].start;
        }
        case 0x00C1: {  // EM_LINELENGTH
            int32_t idx = static_cast<int32_t>(wp);
            if (idx < 0) return 0;
            int li = line_of(s, static_cast<size_t>(idx));
            return li >= 0 ? s->lines[static_cast<size_t>(li)].len : 0;
        }
        case 0x00C2: {  // EM_REPLACESEL
            std::u16string str = lp ? (w->ansi ? cp932_to_utf16(e.mem.read_cstring(lp)) : read_wstr(e, lp)) : u"";
            edit_replace_sel(g, w, str, true);
            return 0;
        }
        case 0x00C4: {  // EM_GETLINE
            int li = static_cast<int32_t>(wp);
            if (!ml) li = 0;
            if (li < 0 || li >= static_cast<int>(s->lines.size())) return 0;
            const auto& l = s->lines[static_cast<size_t>(li)];
            uint32_t cap = e.mem.read16(lp);
            uint32_t n = std::min<uint32_t>(cap, static_cast<uint32_t>(l.len));
            for (uint32_t i = 0; i < n; ++i) e.mem.write16(lp + i * 2, s->text[l.start + i]);
            return n;
        }
        case 0x00C5:  // EM_LIMITTEXT
            s->limit = wp ? static_cast<size_t>(wp) : (ml ? 0x7FFFFFFE : 0x7FFFFFFE);
            return 0;
        case 0x00D5:  // EM_GETLIMITTEXT
            return s->limit;
        case 0x00C6:  // EM_CANUNDO
            return s->can_undo ? 1 : 0;
        case 0x00C9: {  // EM_LINEFROMCHAR
            int32_t idx = static_cast<int32_t>(wp);
            return static_cast<uint64_t>(std::max(0, line_of(s, idx < 0 ? s->caret : static_cast<size_t>(idx))));
        }
        case 0x00CC:  // EM_SETPASSWORDCHAR
            s->password = static_cast<char16_t>(wp);
            edit_repaint(g, w);
            return 0;
        case 0x00D2:  // EM_GETPASSWORDCHAR
            return s->password;
        case 0x00CD:  // EM_EMPTYUNDOBUFFER
            s->can_undo = false;
            return 0;
        case 0x00CE:  // EM_GETFIRSTVISIBLELINE
            return static_cast<uint64_t>(s->top_line);
        case 0x00CF:  // EM_SETREADONLY
            if (wp) w->style |= 0x800;
            else w->style &= ~0x800u;
            edit_repaint(g, w);
            return 1;
        case 0x00D3:  // EM_SETMARGINS
            if (wp & 1) s->margin_l = lo16(lp);
            if (wp & 2) s->margin_r = hi16(lp);
            edit_layout(g, w);
            return 0;
        case 0x00D4:  // EM_GETMARGINS
            return make_lparam(s->margin_l, s->margin_r);
        case 0x00D6: {  // EM_POSFROMCHAR
            Point p = pos_from_char(g, w, std::min(static_cast<size_t>(wp), s->text.size()));
            return make_lparam(p.x, p.y);
        }
        case 0x00D7: {  // EM_CHARFROMPOS
            size_t c = char_from_pos(g, w, lo16(lp), hi16(lp));
            return (static_cast<uint32_t>(line_of(s, c)) << 16) | (c & 0xFFFF);
        }
        case 0x00BD:  // EM_GETHANDLE
            return 0;
        case 0x00B5:  // EM_SCROLL
            return 0;
        case 0x0108 + 0x00:  // (unused)
            break;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

// ===========================================================================
// LISTBOX (also the drop-down part of a combo box)
// ===========================================================================

struct LbItem {
    std::u16string text;
    uint64_t data = 0;
    bool selected = false;
    int height = 0;
};

struct ListState : ControlState {
    std::vector<LbItem> items;
    int cur = -1;          // the caret / single selection
    int anchor = -1;
    int top = 0;
    int item_h = 0;
    bool focus = false, captured = false;
    uint32_t combo = 0;    // owning combo box, for a ComboLBox
    int hext = 0;
};

ListState* ls(Window* w) {
    if (!w->ctl) w->ctl = std::make_unique<ListState>();
    return static_cast<ListState*>(w->ctl.get());
}

bool lb_has_strings(Window* w) {
    uint32_t st = w->style;
    if (!(st & 0x30)) return true;  // not owner-draw
    return (st & 0x40) != 0;        // LBS_HASSTRINGS
}

bool lb_multi(Window* w) { return (w->style & (0x8 | 0x800)) != 0; }

int lb_item_h(Gui& g, Window* w) {
    ListState* s = ls(w);
    if (s->item_h > 0) return s->item_h;
    return font_of(g, w)->height + ((w->style & 0x30) ? 0 : 1);
}

uint32_t lb_notify_target(Gui& g, Window* w) {
    ListState* s = ls(w);
    if (s->combo) return s->combo;
    Window* p = g.win(w->parent);
    return p ? p->hwnd : 0;
}

int lb_visible(Gui& g, Window* w) { return std::max(1, w->crect.h() / std::max(1, lb_item_h(g, w))); }

void lb_update_scroll(Gui& g, Window* w) {
    ListState* s = ls(w);
    if (!(w->style & WS_VSCROLL)) return;
    int vis = lb_visible(g, w);
    w->vscroll.min = 0;
    w->vscroll.max = std::max(0, static_cast<int>(s->items.size()) - 1);
    w->vscroll.page = vis;
    w->vscroll.pos = s->top;
    bool need = static_cast<int>(s->items.size()) > vis;
    bool show = need || (w->style & 0x1000);  // LBS_DISABLENOSCROLL
    w->vscroll.enabled = need;
    if (show != w->vscroll.shown) {
        w->vscroll.shown = show;
        g.set_window_pos(w->hwnd, 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

void lb_paint(Gui& g, Window* w, uint32_t hdc) {
    ListState* s = ls(w);
    Window* p = g.win(w->parent);
    uint32_t brush = 0;
    draw::select_font(g, hdc, window_font(g, w));
    DC* d = g.dc(hdc);
    if (d) d->s.bk_mode = 1;
    uint32_t target = lb_notify_target(g, w);
    if (target) brush = static_cast<uint32_t>(g.send(target, WM_CTLCOLORLISTBOX, hdc, w->hwnd));
    if (!brush) brush = static_cast<uint32_t>(def_window_proc(g, w->hwnd, WM_CTLCOLORLISTBOX, hdc, w->hwnd));
    (void)p;
    Rect r{0, 0, w->crect.w(), w->crect.h()};
    draw::fill_rect_brush(g, hdc, r, brush);
    int ih = lb_item_h(g, w);
    bool od = (w->style & 0x30) != 0;
    for (int i = s->top; i < static_cast<int>(s->items.size()); ++i) {
        int y = (i - s->top) * ih;
        if (y >= r.b) break;
        Rect ir{0, y, r.r, y + ih};
        const LbItem& it = s->items[static_cast<size_t>(i)];
        bool sel = lb_multi(w) ? it.selected : i == s->cur;
        if (od) {
            Emulator& e = g.emu;
            uint64_t dis = g.scratch_alloc(48);
            e.mem.write32(dis, s->combo ? 3 : 2);  // ODT_COMBOBOX / ODT_LISTBOX
            Window* cw = g.win(s->combo ? s->combo : w->hwnd);
            e.mem.write32(dis + 4, cw ? static_cast<uint32_t>(cw->id) : 0);
            e.mem.write32(dis + 8, static_cast<uint32_t>(i));
            e.mem.write32(dis + 12, 1);
            e.mem.write32(dis + 16, (sel ? 1u : 0u) | ((s->focus && i == s->cur) ? 0x10u : 0u));
            e.mem.write32(dis + 20, s->combo ? s->combo : w->hwnd);
            e.mem.write32(dis + 24, hdc);
            write_rect(e, dis + 28, ir);
            e.mem.write32(dis + 44, static_cast<uint32_t>(it.data));
            Window* owner = g.win(s->combo ? (g.win(s->combo) ? g.win(s->combo)->parent : 0) : w->parent);
            if (owner) g.send(owner->hwnd, WM_DRAWITEM, cw ? cw->id : 0, dis);
            g.scratch_free(dis);
            continue;
        }
        uint32_t tc = d ? d->s.text_color : 0;
        if (sel) {
            draw::fill_rect(g, hdc, ir, g.sys_color(13));
            tc = g.sys_color(14);
        }
        draw::text(g, hdc, 2, y, it.text, tc);
        if (s->focus && i == s->cur && lb_multi(w)) draw::focus_rect(g, hdc, ir);
    }
}

void lb_repaint(Gui& g, Window* w) {
    if (!g.is_visible(w)) return;
    uint32_t hdc = get_dc(g, w->hwnd, false, nullptr);
    lb_paint(g, w, hdc);
    release_dc(g, hdc);
    if ((w->style & WS_VSCROLL) && w->vscroll.shown) {
        uint32_t wdc = get_dc(g, w->hwnd, true, nullptr);
        draw_scrollbars(g, w, wdc);
        release_dc(g, wdc);
    }
}

void lb_ensure_visible(Gui& g, Window* w, int i) {
    ListState* s = ls(w);
    int vis = lb_visible(g, w);
    if (i < s->top) s->top = i;
    if (i >= s->top + vis) s->top = i - vis + 1;
    s->top = std::max(0, std::min(s->top, std::max(0, static_cast<int>(s->items.size()) - vis)));
}

int lb_insert(Gui& g, Window* w, int at, const std::u16string& text, uint64_t data) {
    ListState* s = ls(w);
    if ((w->style & 0x2) && at < 0) {  // LBS_SORT: insert in order (strings)
        at = 0;
        std::u16string key = upper(text);
        while (at < static_cast<int>(s->items.size()) && upper(s->items[static_cast<size_t>(at)].text) <= key) ++at;
    } else if (at < 0 || at > static_cast<int>(s->items.size())) {
        at = static_cast<int>(s->items.size());
    }
    LbItem it;
    it.text = text;
    it.data = data;
    s->items.insert(s->items.begin() + at, it);
    if (s->cur >= at) ++s->cur;
    if ((w->style & 0x30) && s->item_h <= 0) {
        // Owner-drawn items get measured once (fixed) at the first insert.
        Emulator& e = g.emu;
        uint64_t mis = g.scratch_alloc(24);
        e.mem.write32(mis, s->combo ? 3 : 2);
        Window* cw = g.win(s->combo ? s->combo : w->hwnd);
        e.mem.write32(mis + 4, cw ? static_cast<uint32_t>(cw->id) : 0);
        e.mem.write32(mis + 8, static_cast<uint32_t>(at));
        e.mem.write32(mis + 12, static_cast<uint32_t>(w->crect.w()));
        e.mem.write32(mis + 16, static_cast<uint32_t>(font_of(g, w)->height));
        e.mem.write32(mis + 20, static_cast<uint32_t>(data));
        Window* owner = g.win(s->combo ? (g.win(s->combo) ? g.win(s->combo)->parent : 0) : w->parent);
        if (owner) g.send(owner->hwnd, WM_MEASUREITEM, cw ? cw->id : 0, mis);
        s->item_h = static_cast<int>(e.mem.read32(mis + 16));
        g.scratch_free(mis);
    }
    lb_update_scroll(g, w);
    return at;
}

int lb_find(ListState* s, int start, const std::u16string& key, bool exact) {
    int n = static_cast<int>(s->items.size());
    if (!n) return -1;
    std::u16string k = upper(key);
    for (int k2 = 1; k2 <= n; ++k2) {
        int i = (start + k2) % n;
        if (start < 0) i = k2 - 1;
        std::u16string t = upper(s->items[static_cast<size_t>(i)].text);
        if (exact ? t == k : t.compare(0, k.size(), k) == 0) return i;
    }
    return -1;
}

void lb_select(Gui& g, Window* w, int i, bool notify) {
    ListState* s = ls(w);
    if (i < -1 || i >= static_cast<int>(s->items.size())) return;
    bool changed = i != s->cur;
    s->cur = i;
    if (lb_multi(w)) {
        for (auto& it : s->items) it.selected = false;
        if (i >= 0) s->items[static_cast<size_t>(i)].selected = true;
        s->anchor = i;
    }
    if (i >= 0) lb_ensure_visible(g, w, i);
    w->vscroll.pos = s->top;
    lb_repaint(g, w);
    if (changed && notify && ((w->style & 0x1) || s->combo)) {
        uint32_t target = lb_notify_target(g, w);
        Window* cw = g.win(s->combo ? s->combo : w->hwnd);
        if (s->combo) {
            g.send(s->combo, WM_USER + 0x7F00, i, 0);  // internal: list selection moved
        } else if (target) {
            g.send(target, WM_COMMAND, (1u << 16) | (control_id(cw) & 0xFFFF), w->hwnd);  // LBN_SELCHANGE
        }
    }
}

uint64_t lb_get_text(Gui& g, Window* w, int i, uint64_t buf) {
    ListState* s = ls(w);
    if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
    const LbItem& it = s->items[static_cast<size_t>(i)];
    if (!lb_has_strings(w)) {
        if (buf) g.emu.mem.write32(buf, static_cast<uint32_t>(it.data));
        return 4;
    }
    if (buf) write_wstr(g.emu, buf, it.text, static_cast<int>(it.text.size() + 1));
    return it.text.size();
}

uint64_t listbox_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    ListState* s = ls(w);
    Emulator& e = g.emu;
    auto read_item_text = [&](uint64_t p) -> std::u16string {
        if (!lb_has_strings(w)) return u"";
        if (!p) return u"";
        return w->ansi ? cp932_to_utf16(e.mem.read_cstring(p)) : read_wstr(e, p);
    };
    switch (msg) {
        case WM_NCCREATE:
            w->vscroll.shown = (w->style & WS_VSCROLL) && (w->style & 0x1000);
            def_window_proc(g, hwnd, msg, wp, lp);
            w->vscroll.shown = (w->style & 0x1000) != 0;
            if (w->cls && w->cls->name == u"COMBOLBOX") s->combo = 0;
            return 1;
        case WM_CREATE:
            lb_update_scroll(g, w);
            return 0;
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            lb_paint(g, w, hdc);
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            lb_update_scroll(g, w);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_SETFONT:
            w->font = wp;
            if (!(w->style & 0x30)) s->item_h = 0;
            lb_update_scroll(g, w);
            if (lp) lb_repaint(g, w);
            return 0;
        case WM_GETFONT:
            return w->font;
        case WM_SETFOCUS:
            s->focus = true;
            lb_repaint(g, w);
            if (w->style & 1) notify_parent(g, w, 4);  // LBN_SETFOCUS
            return 0;
        case WM_KILLFOCUS:
            s->focus = false;
            lb_repaint(g, w);
            if (w->style & 1) notify_parent(g, w, 5);
            return 0;
        case WM_ENABLE:
            lb_repaint(g, w);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            if (!s->combo && g.focus != hwnd) g.set_focus(hwnd);
            int i = s->top + hi16(lp) / std::max(1, lb_item_h(g, w));
            if (i >= static_cast<int>(s->items.size())) i = -1;
            if (i >= 0) {
                if (lb_multi(w)) {
                    bool ctrl = (wp & 0x8) != 0, shift = (wp & 0x4) != 0;
                    if (w->style & 0x8) {  // LBS_MULTIPLESEL: toggle
                        s->items[static_cast<size_t>(i)].selected = !s->items[static_cast<size_t>(i)].selected;
                    } else if (shift && s->anchor >= 0) {
                        if (!ctrl) for (auto& it : s->items) it.selected = false;
                        for (int k = std::min(s->anchor, i); k <= std::max(s->anchor, i); ++k) s->items[static_cast<size_t>(k)].selected = true;
                    } else if (ctrl) {
                        s->items[static_cast<size_t>(i)].selected = !s->items[static_cast<size_t>(i)].selected;
                        s->anchor = i;
                    } else {
                        for (auto& it : s->items) it.selected = false;
                        s->items[static_cast<size_t>(i)].selected = true;
                        s->anchor = i;
                    }
                    s->cur = i;
                    lb_repaint(g, w);
                    if (w->style & 1) notify_parent(g, w, 1);
                } else {
                    lb_select(g, w, i, true);
                }
            }
            if (!s->combo) {
                s->captured = true;
                g.capture = hwnd;
            }
            if (msg == WM_LBUTTONDBLCLK && (w->style & 1) && i >= 0 && !s->combo) notify_parent(g, w, 2);  // LBN_DBLCLK
            return 0;
        }
        case WM_MOUSEMOVE:
            if (s->captured || s->combo) {
                int y = hi16(lp);
                int i = s->top + (y >= 0 ? y / std::max(1, lb_item_h(g, w)) : -1);
                if (s->captured && y < 0 && s->top > 0) i = s->top - 1;
                i = std::max(0, std::min(i, static_cast<int>(s->items.size()) - 1));
                if (!lb_multi(w) && !s->items.empty() && (s->captured || Rect{0, 0, w->crect.w(), w->crect.h()}.contains(lo16(lp), y)))
                    if (i != s->cur) lb_select(g, w, i, s->combo == 0);
            }
            return 0;
        case WM_LBUTTONUP:
            if (s->captured) {
                s->captured = false;
                if (g.capture == hwnd) g.capture = 0;
            }
            if (s->combo) g.send(s->combo, WM_USER + 0x7F01, 1, 0);  // internal: choose
            return 0;
        case WM_MOUSEWHEEL: {
            int d = static_cast<int16_t>(wp >> 16);
            int vis = lb_visible(g, w);
            s->top = std::max(0, std::min(s->top + (d > 0 ? -3 : 3), std::max(0, static_cast<int>(s->items.size()) - vis)));
            w->vscroll.pos = s->top;
            lb_repaint(g, w);
            return 0;
        }
        case WM_VSCROLL: {
            int vis = lb_visible(g, w);
            int n = static_cast<int>(s->items.size());
            switch (wp & 0xFFFF) {
                case 0: s->top--; break;
                case 1: s->top++; break;
                case 2: s->top -= vis; break;
                case 3: s->top += vis; break;
                case 4: case 5: s->top = static_cast<int16_t>(wp >> 16); break;
                case 6: s->top = 0; break;
                case 7: s->top = n; break;
            }
            s->top = std::max(0, std::min(s->top, std::max(0, n - vis)));
            w->vscroll.pos = s->top;
            lb_repaint(g, w);
            return 0;
        }
        case WM_KEYDOWN: {
            int n = static_cast<int>(s->items.size());
            if (!n) return 0;
            int i = s->cur < 0 ? 0 : s->cur;
            int vis = lb_visible(g, w);
            switch (wp) {
                case 0x26: i--; break;
                case 0x28: i++; break;
                case 0x21: i -= vis - 1; break;
                case 0x22: i += vis - 1; break;
                case 0x24: i = 0; break;
                case 0x23: i = n - 1; break;
                case 0x20:
                    if (w->style & 0x8) {
                        s->items[static_cast<size_t>(i)].selected = !s->items[static_cast<size_t>(i)].selected;
                        lb_repaint(g, w);
                        notify_parent(g, w, 1);
                    }
                    return 0;
                default: return 0;
            }
            i = std::max(0, std::min(i, n - 1));
            lb_select(g, w, i, true);
            return 0;
        }
        case WM_CHAR: {
            if (wp < 0x20) return 0;
            int i = lb_find(s, s->cur, std::u16string(1, static_cast<char16_t>(wp)), false);
            if (i >= 0) lb_select(g, w, i, true);
            return 0;
        }
        case 0x0180:  // LB_ADDSTRING
            return static_cast<uint64_t>(lb_insert(g, w, -1, read_item_text(lp), lb_has_strings(w) ? 0 : lp)) & 0xFFFFFFFF;
        case 0x0181: {  // LB_INSERTSTRING
            int at = static_cast<int32_t>(wp);
            bool sorted = (w->style & 0x2) != 0;
            w->style &= ~0x2u;
            int r = lb_insert(g, w, at < 0 ? static_cast<int>(s->items.size()) : at, read_item_text(lp), lb_has_strings(w) ? 0 : lp);
            if (sorted) w->style |= 0x2;
            lb_repaint(g, w);
            return static_cast<uint64_t>(r);
        }
        case 0x0182: {  // LB_DELETESTRING
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            if (w->style & 0x30) {
                uint64_t ds = g.scratch_alloc(24);
                e.mem.write32(ds, 2);
                e.mem.write32(ds + 4, static_cast<uint32_t>(w->id));
                e.mem.write32(ds + 8, static_cast<uint32_t>(i));
                e.mem.write32(ds + 12, hwnd);
                e.mem.write32(ds + 16, static_cast<uint32_t>(s->items[static_cast<size_t>(i)].data));
                Window* p = g.win(w->parent);
                if (p) g.send(p->hwnd, WM_DELETEITEM, w->id, ds);
                g.scratch_free(ds);
            }
            s->items.erase(s->items.begin() + i);
            if (s->cur == i) s->cur = -1;
            else if (s->cur > i) --s->cur;
            lb_update_scroll(g, w);
            lb_repaint(g, w);
            return s->items.size();
        }
        case 0x0184:  // LB_RESETCONTENT
            s->items.clear();
            s->cur = -1;
            s->top = 0;
            lb_update_scroll(g, w);
            lb_repaint(g, w);
            return 0;
        case 0x0185: {  // LB_SETSEL
            int i = static_cast<int32_t>(lp);
            if (!lb_multi(w)) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            if (i < 0) for (auto& it : s->items) it.selected = wp != 0;
            else if (i < static_cast<int>(s->items.size())) s->items[static_cast<size_t>(i)].selected = wp != 0;
            lb_repaint(g, w);
            return 0;
        }
        case 0x0186: {  // LB_SETCURSEL
            int i = static_cast<int32_t>(wp);
            if (lb_multi(w)) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            if (i >= static_cast<int>(s->items.size())) i = -1;
            lb_select(g, w, i, false);
            return i < 0 ? static_cast<uint64_t>(-1) & 0xFFFFFFFF : static_cast<uint64_t>(i);
        }
        case 0x0187: {  // LB_GETSEL
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            return lb_multi(w) ? (s->items[static_cast<size_t>(i)].selected ? 1 : 0) : (i == s->cur ? 1 : 0);
        }
        case 0x0188:  // LB_GETCURSEL
            return static_cast<uint64_t>(static_cast<uint32_t>(s->cur));
        case 0x0189:  // LB_GETTEXT
            return lb_get_text(g, w, static_cast<int32_t>(wp), lp);
        case 0x018A: {  // LB_GETTEXTLEN
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            return lb_has_strings(w) ? s->items[static_cast<size_t>(i)].text.size() : 4;
        }
        case 0x018B:  // LB_GETCOUNT
            return s->items.size();
        case 0x018C: {  // LB_SELECTSTRING
            int i = lb_find(s, static_cast<int32_t>(wp), read_item_text(lp), false);
            if (i >= 0) lb_select(g, w, i, false);
            return static_cast<uint64_t>(static_cast<uint32_t>(i));
        }
        case 0x018E:  // LB_GETTOPINDEX
            return static_cast<uint64_t>(s->top);
        case 0x018F:  // LB_FINDSTRING
            return static_cast<uint64_t>(static_cast<uint32_t>(lb_find(s, static_cast<int32_t>(wp), read_item_text(lp), false)));
        case 0x01A2:  // LB_FINDSTRINGEXACT
            return static_cast<uint64_t>(static_cast<uint32_t>(lb_find(s, static_cast<int32_t>(wp), read_item_text(lp), true)));
        case 0x0190: {  // LB_GETSELCOUNT
            if (!lb_multi(w)) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            int n = 0;
            for (auto& it : s->items) n += it.selected;
            return static_cast<uint64_t>(n);
        }
        case 0x0191: {  // LB_GETSELITEMS
            int n = 0;
            for (size_t i = 0; i < s->items.size() && n < static_cast<int>(wp); ++i)
                if (s->items[i].selected) e.mem.write32(lp + static_cast<uint64_t>(n++) * 4, static_cast<uint32_t>(i));
            return static_cast<uint64_t>(n);
        }
        case 0x0192:  // LB_SETTABSTOPS
            return 1;
        case 0x0193:  // LB_GETHORIZONTALEXTENT
            return static_cast<uint64_t>(s->hext);
        case 0x0194:  // LB_SETHORIZONTALEXTENT
            s->hext = static_cast<int>(wp);
            return 0;
        case 0x0195:  // LB_SETCOLUMNWIDTH
            return 0;
        case 0x0197:  // LB_SETTOPINDEX
            s->top = std::max(0, std::min(static_cast<int>(wp), std::max(0, static_cast<int>(s->items.size()) - 1)));
            w->vscroll.pos = s->top;
            lb_repaint(g, w);
            return 0;
        case 0x0198: {  // LB_GETITEMRECT
            int i = static_cast<int32_t>(wp);
            int ih = lb_item_h(g, w);
            write_rect(e, lp, {0, (i - s->top) * ih, w->crect.w(), (i - s->top + 1) * ih});
            return 1;
        }
        case 0x0199: {  // LB_GETITEMDATA
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            return s->items[static_cast<size_t>(i)].data;
        }
        case 0x019A: {  // LB_SETITEMDATA
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            s->items[static_cast<size_t>(i)].data = lp;
            return 1;
        }
        case 0x019B:    // LB_SELITEMRANGE
        case 0x0183: {  // LB_SELITEMRANGEEX
            int a = msg == 0x019B ? lo16(lp) : static_cast<int32_t>(wp), b = msg == 0x019B ? hi16(lp) : static_cast<int32_t>(lp);
            bool on = msg == 0x019B ? wp != 0 : a <= b;
            for (int k = std::min(a, b); k <= std::max(a, b) && k < static_cast<int>(s->items.size()); ++k)
                if (k >= 0) s->items[static_cast<size_t>(k)].selected = on;
            lb_repaint(g, w);
            return 0;
        }
        case 0x019C: s->anchor = static_cast<int>(wp); return 0;  // LB_SETANCHORINDEX
        case 0x019D: return static_cast<uint64_t>(static_cast<uint32_t>(s->anchor));
        case 0x019E:  // LB_SETCARETINDEX
            s->cur = static_cast<int>(wp);
            lb_repaint(g, w);
            return 0;
        case 0x019F: return static_cast<uint64_t>(static_cast<uint32_t>(s->cur));  // LB_GETCARETINDEX
        case 0x01A0:  // LB_SETITEMHEIGHT
            s->item_h = lo16(lp);
            lb_update_scroll(g, w);
            return 0;
        case 0x01A1:  // LB_GETITEMHEIGHT
            return static_cast<uint64_t>(lb_item_h(g, w));
        case 0x01A8:  // LB_INITSTORAGE
            return wp;
        case 0x01A9: {  // LB_ITEMFROMPOINT
            int i = s->top + hi16(lp) / std::max(1, lb_item_h(g, w));
            bool outside = i >= static_cast<int>(s->items.size()) || i < 0;
            if (outside) i = std::max(0, static_cast<int>(s->items.size()) - 1);
            return (outside ? 1u << 16 : 0u) | static_cast<uint32_t>(i);
        }
        case 0x01A5: case 0x01A6: return 0x0411;  // LB_SETLOCALE / GETLOCALE
        case 0x018D:  // LB_DIR
            return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

// ===========================================================================
// COMBOBOX
// ===========================================================================

struct ComboState : ControlState {
    uint32_t edit = 0;   // child edit (CBS_DROPDOWN / CBS_SIMPLE)
    uint32_t list = 0;   // the drop-down list window (exists while dropped, or always for CBS_SIMPLE)
    std::vector<LbItem> items;
    int cur = -1;
    bool dropped = false;
    bool focus = false;
    bool pressed = false;
    int drop_h = 0;      // height of the dropped list, from the creation height
    int item_h = 0;
    int dropped_width = 0;
    bool extended_ui = false;
    size_t limit = 0;
};

ComboState* cs(Window* w) {
    if (!w->ctl) w->ctl = std::make_unique<ComboState>();
    return static_cast<ComboState*>(w->ctl.get());
}

int combo_kind(Window* w) { return static_cast<int>(w->style & 3); }

int combo_item_h(Gui& g, Window* w) {
    ComboState* s = cs(w);
    if (s->item_h) return s->item_h;
    return font_of(g, w)->height + 2;
}

Rect combo_button(Window* w) {
    int bw = kScrollW;
    return {w->crect.w() - bw - 2, 2, w->crect.w() - 2, w->crect.h() - 2};
}

void combo_paint(Gui& g, Window* w, uint32_t hdc) {
    ComboState* s = cs(w);
    Rect r{0, 0, w->crect.w(), w->crect.h()};
    bool disabled = (w->style & WS_DISABLED) != 0;
    uint32_t brush = prepare_dc(g, w, hdc, combo_kind(w) == 3 ? WM_CTLCOLORLISTBOX : WM_CTLCOLOREDIT);
    if (combo_kind(w) == 1) {
        draw::fill_rect(g, hdc, r, g.sys_color(15));
        return;
    }
    DC* d = g.dc(hdc);
    // Sunken frame, white field, drop-down button.
    draw::edge(g, hdc, r, 0xA, 0xF);  // EDGE_SUNKEN
    Rect field{2, 2, r.r - 2, r.b - 2};
    if (combo_kind(w) == 3) {
        draw::fill_rect_brush(g, hdc, field, disabled ? get_sys_color_brush(g, 15) : brush);
        Rect tr{field.l, field.t, combo_button(w).l, field.b};
        Rect inner{tr.l + 1, tr.t + 1, tr.r - 1, tr.b - 1};
        bool od = (w->style & 0x30) != 0;
        if (od && s->cur >= 0) {
            Emulator& e = g.emu;
            uint64_t dis = g.scratch_alloc(48);
            e.mem.write32(dis, 3);
            e.mem.write32(dis + 4, static_cast<uint32_t>(w->id));
            e.mem.write32(dis + 8, static_cast<uint32_t>(s->cur));
            e.mem.write32(dis + 12, 1);
            e.mem.write32(dis + 16, (s->focus && !s->dropped ? 0x11u : 0u) | 0x1000u | (disabled ? 4u : 0u));  // ODS_COMBOBOXEDIT
            e.mem.write32(dis + 20, w->hwnd);
            e.mem.write32(dis + 24, hdc);
            write_rect(e, dis + 28, inner);
            e.mem.write32(dis + 44, static_cast<uint32_t>(s->items[static_cast<size_t>(s->cur)].data));
            Window* p = g.win(w->parent);
            if (p) g.send(p->hwnd, WM_DRAWITEM, w->id, dis);
            g.scratch_free(dis);
        } else {
            uint32_t tc = disabled ? g.sys_color(17) : (d ? d->s.text_color : 0);
            if (s->focus && !s->dropped) {
                draw::fill_rect(g, hdc, inner, g.sys_color(13));
                tc = g.sys_color(14);
            }
            if (s->cur >= 0 && s->cur < static_cast<int>(s->items.size())) {
                Rect t2{inner.l + 1, inner.t, inner.r, inner.b};
                d->s.text_color = tc;
                draw_text_impl(g, hdc, s->items[static_cast<size_t>(s->cur)].text, t2, 0x20 | 0x4 | 0x800);
            }
            if (s->focus && !s->dropped) draw::focus_rect(g, hdc, inner);
        }
    } else {
        draw::fill_rect_brush(g, hdc, field, brush);
    }
    draw::frame_control(g, hdc, combo_button(w), 3, 0x5 | (s->pressed ? 0x200 : 0) | (disabled ? 0x100 : 0));  // DFCS_SCROLLCOMBOBOX
    // The field was painted over the edit child (the DC does not clip
    // children); put the text back, or a combo box redrawn when it drops
    // down shows an empty field.
    if (s->edit)
        if (Window* ew = g.win(s->edit)) edit_repaint(g, ew);
}

void combo_repaint(Gui& g, Window* w) {
    if (!g.is_visible(w)) return;
    uint32_t hdc = get_dc(g, w->hwnd, false, nullptr);
    combo_paint(g, w, hdc);
    release_dc(g, hdc);
}

void combo_notify(Gui& g, Window* w, uint16_t code) { notify_parent(g, w, code); }

void combo_sync_edit(Gui& g, Window* w) {
    ComboState* s = cs(w);
    if (!s->edit) return;
    std::u16string t = s->cur >= 0 && s->cur < static_cast<int>(s->items.size()) ? s->items[static_cast<size_t>(s->cur)].text : u"";
    Window* ew = g.win(s->edit);
    if (!ew) return;
    EditState* est = es(ew);
    est->text = t;
    est->anchor = 0;
    est->caret = t.size();
    edit_layout(g, ew);
    edit_scroll_to_caret(g, ew);
    edit_repaint(g, ew);
}

void combo_close(Gui& g, Window* w, bool ok);

void combo_drop(Gui& g, Window* w) {
    ComboState* s = cs(w);
    if (s->dropped || combo_kind(w) == 1) return;
    uint32_t hwnd = w->hwnd;
    combo_notify(g, w, 7);  // CBN_DROPDOWN
    w = g.win(hwnd);
    if (!w) return;
    s = cs(w);
    Rect sr = g.window_rect_screen(w);
    int ih = combo_item_h(g, w);
    int n = static_cast<int>(s->items.size());
    int h = s->drop_h > 0 ? s->drop_h : ih * 8 + 2;
    if (!(w->style & 0x400)) {  // integral height
        int rows = std::max(1, std::min(n, (h - 2) / ih));
        h = rows * ih + 2;
    }
    if (n == 0) h = ih + 2;
    int width = std::max(sr.w(), s->dropped_width);
    int y = sr.b;
    if (y + h > g.screen_h) y = std::max(0, sr.t - h);
    uint32_t lst = g.create_window(0x8 | 0x80, u"ComboLBox", 0, u"",
                                   WS_POPUP | WS_BORDER | WS_CLIPSIBLINGS | (n * ih + 2 > h ? WS_VSCROLL : 0) | 0x8000 | (w->style & 0x30) | 0x40,
                                   sr.l, y, width, h, hwnd, 0, 0, 0, false);
    Window* lw = g.win(lst);
    if (!lw) return;
    ListState* l = ls(lw);
    l->combo = hwnd;
    l->items = s->items;
    l->item_h = ih;
    l->cur = s->cur;
    lw->font = w->font;
    lb_update_scroll(g, lw);
    if (l->cur >= 0) lb_ensure_visible(g, lw, l->cur);
    s->list = lst;
    s->dropped = true;
    g.set_window_pos(lst, 0xFFFFFFFF, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    combo_repaint(g, w);
    // Track the list until it closes: a small modal loop, like Windows'.
    g.capture = lst;
    bool pressed_on_combo = true;
    while (true) {
        Window* cw = g.win(hwnd);
        if (!cw || !cs(cw)->dropped) break;
        Msg m;
        if (!g.get_message(m, 0, 0, 0, true, true)) continue;
        if (m.message == WM_QUIT) {
            g.quit_posted = true;
            combo_close(g, cw, false);
            break;
        }
        cw = g.win(hwnd);
        if (!cw) break;
        Window* lw2 = g.win(cs(cw)->list);
        if (!lw2) break;
        if (m.message >= WM_MOUSEMOVE && m.message <= WM_MBUTTONDBLCLK) {
            // Captured by the list: coordinates are list-client.
            Point sp = m.pt;
            Rect lr = g.window_rect_screen(lw2);
            Rect cr = g.window_rect_screen(cw);
            if (m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN) {
                pressed_on_combo = false;
                if (lr.contains(sp.x, sp.y)) {
                    Rect lcr = g.client_rect_screen(lw2);
                    if (!lcr.contains(sp.x, sp.y)) {
                        // On the list's scroll bar.
                        g.capture = 0;
                        g.send(lw2->hwnd, WM_NCLBUTTONDOWN, HTVSCROLL, make_lparam(sp.x, sp.y));
                        if (g.win(lw2->hwnd)) g.capture = lw2->hwnd;
                        continue;
                    }
                    g.send(lw2->hwnd, WM_LBUTTONDOWN, m.wparam, m.lparam);
                } else if (cr.contains(sp.x, sp.y)) {
                    combo_close(g, cw, false);
                    break;
                } else {
                    combo_close(g, cw, false);
                    break;
                }
                continue;
            }
            if (m.message == WM_LBUTTONUP) {
                Rect lcr = g.client_rect_screen(lw2);
                if (lcr.contains(sp.x, sp.y)) {
                    g.send(lw2->hwnd, WM_LBUTTONUP, m.wparam, m.lparam);
                } else if (!pressed_on_combo) {
                    // released outside after pressing outside: nothing
                }
                pressed_on_combo = false;
                continue;
            }
            if (m.message == WM_MOUSEMOVE) {
                g.send(lw2->hwnd, WM_MOUSEMOVE, m.wparam, m.lparam);
                continue;
            }
            continue;
        }
        if (m.message == WM_MOUSEWHEEL) {
            g.send(lw2->hwnd, WM_MOUSEWHEEL, m.wparam, m.lparam);
            continue;
        }
        if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            switch (m.wparam) {
                case 0x0D: combo_close(g, cw, true); continue;
                case 0x1B: combo_close(g, cw, false); continue;
                case 0x73:  // F4
                    combo_close(g, cw, true);
                    continue;
                case 0x26: case 0x28: case 0x21: case 0x22: case 0x24: case 0x23:
                    if (m.message == WM_SYSKEYDOWN) {
                        combo_close(g, cw, true);
                        continue;
                    }
                    listbox_proc(g, lw2->hwnd, WM_KEYDOWN, m.wparam, m.lparam);
                    continue;
            }
            if (m.wparam == 0x09) {
                combo_close(g, cw, true);
                continue;
            }
        }
        if (m.message == WM_CHAR && cs(cw)->edit) {
            g.dispatch(m);
            continue;
        }
        if (m.message == WM_CHAR) {
            listbox_proc(g, lw2->hwnd, WM_CHAR, m.wparam, m.lparam);
            continue;
        }
        if (m.message == WM_KEYDOWN || m.message == WM_KEYUP) {
            if (cs(cw)->edit) g.dispatch(m);
            continue;
        }
        g.dispatch(m);
    }
}

void combo_close(Gui& g, Window* w, bool ok) {
    ComboState* s = cs(w);
    if (!s->dropped) return;
    uint32_t hwnd = w->hwnd;
    s->dropped = false;
    Window* lw = g.win(s->list);
    int sel = lw ? ls(lw)->cur : s->cur;
    if (g.capture == s->list) g.capture = 0;
    uint32_t lst = s->list;
    s->list = 0;
    if (lw) g.destroy_window(lst);
    w = g.win(hwnd);
    if (!w) return;
    s = cs(w);
    bool changed = ok && sel != s->cur;
    if (ok) {
        s->cur = sel;
        combo_sync_edit(g, w);
    }
    combo_repaint(g, w);
    combo_notify(g, w, 8);  // CBN_CLOSEUP
    if (!g.win(hwnd)) return;
    combo_notify(g, w, ok ? 9 : 10);  // CBN_SELENDOK / CBN_SELENDCANCEL
    if (changed && g.win(hwnd)) combo_notify(g, w, 1);  // CBN_SELCHANGE
}

int combo_insert(Gui& g, Window* w, int at, const std::u16string& text, uint64_t data) {
    ComboState* s = cs(w);
    if ((w->style & 0x100) && at < 0) {  // CBS_SORT
        at = 0;
        std::u16string key = upper(text);
        while (at < static_cast<int>(s->items.size()) && upper(s->items[static_cast<size_t>(at)].text) <= key) ++at;
    } else if (at < 0 || at > static_cast<int>(s->items.size())) {
        at = static_cast<int>(s->items.size());
    }
    LbItem it;
    it.text = text;
    it.data = data;
    s->items.insert(s->items.begin() + at, it);
    if (s->cur >= at) ++s->cur;
    if ((w->style & 0x30) && !s->item_h) {
        Emulator& e = g.emu;
        uint64_t mis = g.scratch_alloc(24);
        e.mem.write32(mis, 3);
        e.mem.write32(mis + 4, static_cast<uint32_t>(w->id));
        e.mem.write32(mis + 8, static_cast<uint32_t>(at));
        e.mem.write32(mis + 12, static_cast<uint32_t>(w->crect.w()));
        e.mem.write32(mis + 16, static_cast<uint32_t>(font_of(g, w)->height + 2));
        e.mem.write32(mis + 20, static_cast<uint32_t>(data));
        Window* p = g.win(w->parent);
        if (p) g.send(p->hwnd, WM_MEASUREITEM, w->id, mis);
        s->item_h = static_cast<int>(e.mem.read32(mis + 16));
        g.scratch_free(mis);
    }
    return at;
}

bool combo_has_strings(Window* w) { return !(w->style & 0x30) || (w->style & 0x200); }

uint64_t combo_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    ComboState* s = cs(w);
    Emulator& e = g.emu;
    auto read_item_text = [&](uint64_t p) -> std::u16string {
        if (!combo_has_strings(w) || !p) return u"";
        return w->ansi ? cp932_to_utf16(e.mem.read_cstring(p)) : read_wstr(e, p);
    };
    switch (msg) {
        case WM_NCCREATE:
            // As Windows does: border and scroll bar styles belong to the list,
            // not to the combo box itself.
            w->style &= ~(WS_BORDER | WS_HSCROLL | WS_VSCROLL);
            def_window_proc(g, hwnd, msg, wp, lp);
            s->drop_h = w->wrect.h();
            return 1;
        case WM_CREATE: {
            // The visible height of a drop-down combo is fixed by its font.
            int h = combo_item_h(g, w) + 6;
            if (combo_kind(w) != 1) {
                s->drop_h = std::max(0, w->wrect.h() - h);
                g.set_window_pos(hwnd, 0, 0, 0, w->wrect.w(), h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            }
            w = g.win(hwnd);
            if (!w) return 0;
            if (combo_kind(w) == 1 || combo_kind(w) == 2) {
                Rect br = combo_button(w);
                int ex = combo_kind(w) == 1 ? 0 : 3;
                uint32_t est = WS_CHILD | WS_VISIBLE | 0x80 /*ES_AUTOHSCROLL*/ | (w->style & 0x2000 ? 0x8 : 0) | (w->style & 0x4000 ? 0x10 : 0);
                s->edit = g.create_window(0, u"Edit", 0, w->text, est, ex, ex, (combo_kind(w) == 1 ? w->crect.w() : br.l - ex - 1), combo_item_h(g, w), hwnd, 1001, w->hinstance, 0, false);
            }
            return 0;
        }
        case WM_WINDOWPOSCHANGING: {
            // A drop-down combo box keeps the height of its selection field;
            // the height a program asks for is how far the list drops (and
            // Jw_cad moves its combo boxes with that height after creating
            // them).  WINDOWPOS: cx at +16, cy at +20, flags at +24.
            if (combo_kind(w) != 1 && lp && !(e.mem.read32(lp + 24) & SWP_NOSIZE)) {
                int cy = static_cast<int>(e.mem.read32(lp + 20));
                int h = combo_item_h(g, w) + 6;
                if (cy != h) {
                    if (cy > h) s->drop_h = cy - h;
                    e.mem.write32(lp + 20, static_cast<uint32_t>(h));
                }
            }
            return def_window_proc(g, hwnd, msg, wp, lp);
        }
        case WM_SIZE:
            if (s->edit) {
                Window* ew = g.win(s->edit);
                if (ew) g.set_window_pos(s->edit, 0, 3, 3, combo_button(w).l - 4, w->crect.h() - 6, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            combo_paint(g, w, hdc);
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS | (s->dropped ? DLGC_WANTALLKEYS : 0);
        case WM_SETFONT:
            w->font = wp;
            if (s->edit) g.send(s->edit, WM_SETFONT, wp, lp);
            if (!(w->style & 0x30)) {
                s->item_h = 0;
                if (combo_kind(w) != 1)
                    g.set_window_pos(hwnd, 0, 0, 0, w->wrect.w(), combo_item_h(g, w) + 6, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (lp) combo_repaint(g, w);
            return 0;
        case WM_GETFONT:
            return w->font;
        case WM_SETFOCUS:
            s->focus = true;
            if (s->edit) {
                g.set_focus(s->edit);
                g.send(s->edit, 0x00B1, 0, 0xFFFFFFFF);
            } else {
                combo_repaint(g, w);
            }
            combo_notify(g, w, 3);  // CBN_SETFOCUS
            return 0;
        case WM_KILLFOCUS:
            if (lp == s->edit || (s->list && lp == s->list)) return 0;
            s->focus = false;
            if (s->dropped) combo_close(g, w, false);
            if ((w = g.win(hwnd))) {
                combo_repaint(g, w);
                combo_notify(g, w, 4);  // CBN_KILLFOCUS
            }
            return 0;
        case WM_ENABLE:
            if (s->edit) g.send(s->edit, msg, wp, lp), (void)0;
            if (s->edit) {
                Window* ew = g.win(s->edit);
                if (ew) {
                    if (wp) ew->style &= ~WS_DISABLED;
                    else ew->style |= WS_DISABLED;
                }
            }
            combo_repaint(g, w);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            if (g.focus != hwnd && g.focus != s->edit) g.set_focus(hwnd);
            w = g.win(hwnd);
            if (!w) return 0;
            s = cs(w);
            Point p{lo16(lp), hi16(lp)};
            bool on_button = combo_button(w).contains(p.x, p.y);
            if (combo_kind(w) == 3 || on_button) {
                if (s->dropped) combo_close(g, w, true);
                else combo_drop(g, w);
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (!s->dropped && combo_kind(w) != 1) {
                int d = static_cast<int16_t>(wp >> 16);
                int n = static_cast<int>(s->items.size());
                if (!n) return 0;
                int i = s->cur + (d > 0 ? -1 : 1);
                i = std::max(0, std::min(i, n - 1));
                if (i != s->cur) {
                    s->cur = i;
                    combo_sync_edit(g, w);
                    combo_repaint(g, w);
                    combo_notify(g, w, 1);
                }
            }
            return 0;
        case WM_KEYDOWN: {
            int n = static_cast<int>(s->items.size());
            if (wp == 0x73 || ((wp == 0x26 || wp == 0x28) && (g.key_state[0x12] & 0x80))) {
                combo_drop(g, w);
                return 0;
            }
            if (!n) return 0;
            int i = s->cur;
            switch (wp) {
                case 0x26: case 0x25: i = i <= 0 ? 0 : i - 1; break;
                case 0x28: case 0x27: i = i < 0 ? 0 : std::min(n - 1, i + 1); break;
                case 0x24: i = 0; break;
                case 0x23: i = n - 1; break;
                case 0x21: i = std::max(0, i - 8); break;
                case 0x22: i = std::min(n - 1, i + 8); break;
                default: return 0;
            }
            if (i != s->cur) {
                s->cur = i;
                combo_sync_edit(g, w);
                combo_repaint(g, w);
                combo_notify(g, w, 1);
            }
            return 0;
        }
        case WM_CHAR:
            if (s->edit) return g.send(s->edit, msg, wp, lp);
            if (wp >= 0x20) {
                int i = -1;
                std::u16string k(1, static_cast<char16_t>(wp));
                int n = static_cast<int>(s->items.size());
                for (int k2 = 1; k2 <= n && i < 0; ++k2) {
                    int j = (s->cur + k2 + n) % n;
                    if (upper(s->items[static_cast<size_t>(j)].text).compare(0, 1, upper(k)) == 0) i = j;
                }
                if (i >= 0 && i != s->cur) {
                    s->cur = i;
                    combo_repaint(g, w);
                    combo_notify(g, w, 1);
                }
            }
            return 0;
        case WM_COMMAND:
            // From the child edit.
            if (static_cast<uint32_t>(lp) == s->edit) {
                uint16_t code = static_cast<uint16_t>(wp >> 16);
                if (code == 0x300) combo_notify(g, w, 5);       // EN_CHANGE -> CBN_EDITCHANGE
                else if (code == 0x400) combo_notify(g, w, 6);  // EN_UPDATE -> CBN_EDITUPDATE
                else if (code == 0x100 && !s->focus) {
                    s->focus = true;
                    combo_notify(g, w, 3);
                } else if (code == 0x200) {
                    if (g.focus != hwnd && !(s->list && g.focus == s->list)) {
                        s->focus = false;
                        combo_notify(g, w, 4);
                    }
                }
            }
            return 0;
        case WM_SETTEXT:
            if (s->edit) return g.send(s->edit, msg, wp, lp);
            {
                std::u16string t = lp ? read_wstr(e, lp) : u"";
                for (size_t i = 0; i < s->items.size(); ++i)
                    if (s->items[i].text == t) {
                        s->cur = static_cast<int>(i);
                        combo_repaint(g, w);
                        return 1;
                    }
            }
            return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
        case WM_GETTEXT:
            if (s->edit) return g.send(s->edit, msg, wp, lp);
            if (!wp) return 0;
            if (s->cur >= 0 && s->cur < static_cast<int>(s->items.size()))
                return static_cast<uint64_t>(write_wstr(e, lp, s->items[static_cast<size_t>(s->cur)].text, static_cast<int>(wp)));
            write_wstr(e, lp, u"", static_cast<int>(wp));
            return 0;
        case WM_GETTEXTLENGTH:
            if (s->edit) return g.send(s->edit, msg, wp, lp);
            return s->cur >= 0 && s->cur < static_cast<int>(s->items.size()) ? s->items[static_cast<size_t>(s->cur)].text.size() : 0;
        case WM_USER + 0x7F00:  // the dropped list moved its selection
            if (Window* lw = g.win(s->list)) {
                (void)lw;
                int i = static_cast<int>(static_cast<int32_t>(wp));
                if (s->edit && i >= 0 && i < static_cast<int>(s->items.size())) {
                    int old = s->cur;
                    s->cur = i;
                    combo_sync_edit(g, w);
                    s->cur = old;
                }
                if (combo_kind(w) == 3) {
                    int old = s->cur;
                    s->cur = i;
                    combo_repaint(g, w);
                    s->cur = old;
                }
            }
            return 0;
        case WM_USER + 0x7F01:  // the dropped list was clicked
            combo_close(g, w, true);
            return 0;
        case 0x0140: {  // CB_GETEDITSEL
            if (!s->edit) return 0;
            return g.send(s->edit, 0x00B0, wp, lp);
        }
        case 0x0141:  // CB_LIMITTEXT
            s->limit = wp;
            if (s->edit) g.send(s->edit, 0x00C5, wp, 0);
            return 1;
        case 0x0142:  // CB_SETEDITSEL
            if (!s->edit) return 0;
            g.send(s->edit, 0x00B1, static_cast<uint64_t>(static_cast<uint32_t>(lo16(lp))), static_cast<uint64_t>(static_cast<uint32_t>(hi16(lp))));
            return 1;
        case 0x0143:  // CB_ADDSTRING
            return static_cast<uint64_t>(combo_insert(g, w, -1, read_item_text(lp), combo_has_strings(w) ? 0 : lp));
        case 0x014A: {  // CB_INSERTSTRING
            bool sorted = (w->style & 0x100) != 0;
            w->style &= ~0x100u;
            int at = static_cast<int32_t>(wp);
            int r = combo_insert(g, w, at < 0 ? static_cast<int>(s->items.size()) : at, read_item_text(lp), combo_has_strings(w) ? 0 : lp);
            if (sorted) w->style |= 0x100;
            return static_cast<uint64_t>(r);
        }
        case 0x0144: {  // CB_DELETESTRING
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            s->items.erase(s->items.begin() + i);
            if (s->cur == i) s->cur = -1;
            else if (s->cur > i) --s->cur;
            combo_repaint(g, w);
            return s->items.size();
        }
        case 0x0146:  // CB_GETCOUNT
            return s->items.size();
        case 0x0147:  // CB_GETCURSEL
            return static_cast<uint64_t>(static_cast<uint32_t>(s->cur));
        case 0x0148: {  // CB_GETLBTEXT
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            const LbItem& it = s->items[static_cast<size_t>(i)];
            if (!combo_has_strings(w)) {
                if (lp) e.mem.write32(lp, static_cast<uint32_t>(it.data));
                return 4;
            }
            if (lp) {
                if (w->ansi) {
                    std::string a = utf16_to_cp932(it.text);
                    e.mem.write_cstring(lp, a);
                    return a.size();
                }
                write_wstr(e, lp, it.text, static_cast<int>(it.text.size() + 1));
            }
            return it.text.size();
        }
        case 0x0149: {  // CB_GETLBTEXTLEN
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            return combo_has_strings(w) ? s->items[static_cast<size_t>(i)].text.size() : 4;
        }
        case 0x014B:  // CB_RESETCONTENT
            s->items.clear();
            s->cur = -1;
            if (s->edit) {
                Window* ew = g.win(s->edit);
                if (ew) {
                    es(ew)->text.clear();
                    es(ew)->anchor = es(ew)->caret = 0;
                    edit_layout(g, ew);
                    edit_repaint(g, ew);
                }
            }
            combo_repaint(g, w);
            return 0;
        case 0x014C:    // CB_FINDSTRING
        case 0x0158: {  // CB_FINDSTRINGEXACT
            std::u16string key = upper(read_item_text(lp));
            int n = static_cast<int>(s->items.size());
            int start = static_cast<int32_t>(wp);
            for (int k = 1; k <= n; ++k) {
                int i = start < 0 ? k - 1 : (start + k) % n;
                std::u16string t = upper(s->items[static_cast<size_t>(i)].text);
                if (msg == 0x0158 ? t == key : t.compare(0, key.size(), key) == 0) return static_cast<uint64_t>(i);
            }
            return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
        }
        case 0x014D: {  // CB_SELECTSTRING
            std::u16string key = upper(read_item_text(lp));
            int n = static_cast<int>(s->items.size());
            int start = static_cast<int32_t>(wp);
            for (int k = 1; k <= n; ++k) {
                int i = start < 0 ? k - 1 : (start + k) % n;
                if (upper(s->items[static_cast<size_t>(i)].text).compare(0, key.size(), key) == 0) {
                    s->cur = i;
                    combo_sync_edit(g, w);
                    combo_repaint(g, w);
                    return static_cast<uint64_t>(i);
                }
            }
            return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
        }
        case 0x014E: {  // CB_SETCURSEL
            int i = static_cast<int32_t>(wp);
            if (i >= static_cast<int>(s->items.size())) i = -1;
            s->cur = i;
            combo_sync_edit(g, w);
            combo_repaint(g, w);
            return i < 0 ? static_cast<uint64_t>(-1) & 0xFFFFFFFF : static_cast<uint64_t>(i);
        }
        case 0x014F:  // CB_SHOWDROPDOWN
            if (wp) combo_drop(g, w);
            else combo_close(g, w, false);
            return 1;
        case 0x0150: {  // CB_GETITEMDATA
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            return s->items[static_cast<size_t>(i)].data;
        }
        case 0x0151: {  // CB_SETITEMDATA
            int i = static_cast<int32_t>(wp);
            if (i < 0 || i >= static_cast<int>(s->items.size())) return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
            s->items[static_cast<size_t>(i)].data = lp;
            return 1;
        }
        case 0x0152: {  // CB_GETDROPPEDCONTROLRECT
            Rect r = g.window_rect_screen(w);
            r.b += s->drop_h;
            write_rect(e, lp, r);
            return 1;
        }
        case 0x0153:  // CB_SETITEMHEIGHT
            if (static_cast<int32_t>(wp) == -1) {
                g.set_window_pos(hwnd, 0, 0, 0, w->wrect.w(), lo16(lp) + 6, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                s->item_h = lo16(lp);
            }
            return 0;
        case 0x0154:  // CB_GETITEMHEIGHT
            if (static_cast<int32_t>(wp) == -1) return static_cast<uint64_t>(w->crect.h() - 6);
            return static_cast<uint64_t>(combo_item_h(g, w));
        case 0x0155: s->extended_ui = wp != 0; return 0;  // CB_SETEXTENDEDUI
        case 0x0156: return s->extended_ui ? 1 : 0;      // CB_GETEXTENDEDUI
        case 0x0157: return s->dropped ? 1 : 0;          // CB_GETDROPPEDSTATE
        case 0x0159: case 0x015A: return 0x0411;        // CB_SETLOCALE / GETLOCALE
        case 0x015B: return 0;                           // CB_GETTOPINDEX
        case 0x015C: return 0;                           // CB_SETTOPINDEX
        case 0x015F: return static_cast<uint64_t>(std::max(s->dropped_width, w->wrect.w()));  // CB_GETDROPPEDWIDTH
        case 0x0160: s->dropped_width = static_cast<int>(wp); return wp;                     // CB_SETDROPPEDWIDTH
        case 0x0161: return wp;                          // CB_INITSTORAGE
        case 0x0164: {  // CB_GETCOMBOBOXINFO
            uint64_t p = lp;
            write_rect(e, p + 4, {3, 3, combo_button(w).l, w->crect.h() - 3});
            write_rect(e, p + 20, combo_button(w));
            e.mem.write32(p + 36, s->pressed ? 8 : 0);
            e.mem.write32(p + 40, hwnd);
            e.mem.write32(p + 44, s->edit ? s->edit : hwnd);
            e.mem.write32(p + 48, s->list);
            return 1;
        }
        case 0x0145:  // CB_DIR
            return static_cast<uint64_t>(-1) & 0xFFFFFFFF;
        case WM_NCDESTROY:
            break;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

// ===========================================================================
// SCROLLBAR
// ===========================================================================

uint64_t scrollbar_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    bool vert = (w->style & 1) != 0;
    ScrollInfo& si = vert ? w->vscroll : w->hscroll;
    Emulator& e = g.emu;
    auto repaint = [&]() {
        if (!g.is_visible(w)) return;
        uint32_t hdc = get_dc(g, hwnd, false, nullptr);
        draw_scrollbar_rect(g, hdc, {0, 0, w->crect.w(), w->crect.h()}, si, vert);
        release_dc(g, hdc);
    };
    switch (msg) {
        case WM_CREATE:
            si.min = 0;
            si.max = 100;
            si.pos = 0;
            si.page = 0;
            return 0;
        case WM_PAINT: {
            Rect rc;
            bool erase;
            uint32_t hdc = begin_paint(g, hwnd, rc, erase);
            if (w->style & 0x8) {  // SBS_SIZEBOX / SIZEGRIP
                draw::fill_rect(g, hdc, {0, 0, w->crect.w(), w->crect.h()}, g.sys_color(15));
            } else {
                draw_scrollbar_rect(g, hdc, {0, 0, w->crect.w(), w->crect.h()}, si, vert);
            }
            end_paint(g, hwnd, hdc);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            Window* p = g.win(w->parent);
            if (!p) return 0;
            Rect bar = g.client_rect_screen(w);
            Point sp = g.client_to_screen(w, {lo16(lp), hi16(lp)});
            track_scrollbar(g, p->hwnd, vert ? WM_VSCROLL : WM_HSCROLL, hwnd, bar, &si, vert, sp);
            return 0;
        }
        case WM_ENABLE:
            si.enabled = wp != 0;
            repaint();
            return 0;
        case 0x00E0: {  // SBM_SETPOS
            int old = si.pos;
            si.pos = std::max(si.min, std::min(static_cast<int32_t>(wp), si.max));
            if (lp) repaint();
            return static_cast<uint64_t>(static_cast<uint32_t>(old));
        }
        case 0x00E1:  // SBM_GETPOS
            return static_cast<uint64_t>(static_cast<uint32_t>(si.pos));
        case 0x00E2:    // SBM_SETRANGE
        case 0x00E6: {  // SBM_SETRANGEREDRAW
            si.min = static_cast<int32_t>(wp);
            si.max = static_cast<int32_t>(lp);
            si.pos = std::max(si.min, std::min(si.pos, si.max));
            if (msg == 0x00E6) repaint();
            return 0;
        }
        case 0x00E3:  // SBM_GETRANGE
            if (wp) e.mem.write32(wp, static_cast<uint32_t>(si.min));
            if (lp) e.mem.write32(lp, static_cast<uint32_t>(si.max));
            return 0;
        case 0x00E4:  // SBM_ENABLE_ARROWS
            si.enabled = wp != 3;
            repaint();
            return 1;
        case 0x00E9: {  // SBM_SETSCROLLINFO
            uint32_t mask = e.mem.read32(lp + 4);
            if (mask & 1) {
                si.min = static_cast<int32_t>(e.mem.read32(lp + 8));
                si.max = static_cast<int32_t>(e.mem.read32(lp + 12));
            }
            if (mask & 2) si.page = static_cast<int32_t>(e.mem.read32(lp + 16));
            if (mask & 4) si.pos = static_cast<int32_t>(e.mem.read32(lp + 20));
            int maxpos = si.max - std::max(si.page - 1, 0);
            si.pos = std::max(si.min, std::min(si.pos, std::max(si.min, maxpos)));
            if (wp) repaint();
            return static_cast<uint64_t>(static_cast<uint32_t>(si.pos));
        }
        case 0x00EA: {  // SBM_GETSCROLLINFO
            uint32_t mask = e.mem.read32(lp + 4);
            if (mask & 1) {
                e.mem.write32(lp + 8, static_cast<uint32_t>(si.min));
                e.mem.write32(lp + 12, static_cast<uint32_t>(si.max));
            }
            if (mask & 2) e.mem.write32(lp + 16, static_cast<uint32_t>(si.page));
            if (mask & 4) e.mem.write32(lp + 20, static_cast<uint32_t>(si.pos));
            if (mask & 0x10) e.mem.write32(lp + 24, static_cast<uint32_t>(si.track));
            return 1;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

}  // namespace

void install_controls(Emulator& e) {
    register_builtin_class(e, u"Button", 0x0008 | 0x0003 | 0x0020 /*CS_PARENTDC*/, 0, 32512, 0, button_proc);
    register_builtin_class(e, u"Static", 0x0008 | 0x0020, 0, 32512, 0, static_proc);
    register_builtin_class(e, u"Edit", 0x0008 | 0x0020, 6, 32513, 0, edit_proc);
    register_builtin_class(e, u"ListBox", 0x0008 | 0x0020, 0, 32512, 0, listbox_proc);
    register_builtin_class(e, u"ComboLBox", 0x0008 | 0x0800, 0, 32512, 0, listbox_proc);
    register_builtin_class(e, u"ComboBox", 0x0008 | 0x0020, 0, 32512, 0, combo_proc);
    register_builtin_class(e, u"ScrollBar", 0x0008 | 0x0020, 0, 32512, 0, scrollbar_proc);
    register_builtin_class(e, u"MDIClient", 0, 0, 32512, 13, [](Gui& g, uint32_t h, uint32_t m, uint64_t wp, uint64_t lp) -> uint64_t {
        return def_window_proc(g, h, m, wp, lp);
    });
    api(e, "GetComboBoxInfo", 2, [](Emulator& e) {
        Gui& g = gui(e);
        ret(e, g.send(U(e, 0), 0x0164, 0, A(e, 1)));
    });
    api(e, "GetListBoxInfo", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "DlgDirListW", 5, [](Emulator& e) { ret(e, 0); });
    api(e, "DlgDirListComboBoxW", 5, [](Emulator& e) { ret(e, 0); });
}

}  // namespace x86emu::win32
