// Dialogs: templates, dialog units, DefDlgProc, keyboard navigation, modal
// loops, and MessageBox.
#include <algorithm>
#include <map>
#include <cstring>

#include "font.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

RealFont* dc_realfont(Gui& g, uint32_t hdc);

namespace {

constexpr int DWLP_MSGRESULT = 0, DWLP_DLGPROC = 4, DWLP_USER = 8;
constexpr uint32_t DS_SETFONT = 0x40, DS_MODALFRAME = 0x80, DS_CENTER = 0x800,
                   DS_CENTERMOUSE = 0x1000, DS_ABSALIGN = 0x01, DS_CONTROL = 0x400;
constexpr uint32_t DLGC_WANTARROWS = 0x1, DLGC_WANTTAB = 0x2, DLGC_WANTALLKEYS = 0x4,
                   DLGC_WANTMESSAGE = 0x4, DLGC_HASSETSEL = 0x8, DLGC_DEFPUSHBUTTON = 0x10,
                   DLGC_UNDEFPUSHBUTTON = 0x20, DLGC_RADIOBUTTON = 0x40, DLGC_WANTCHARS = 0x80,
                   DLGC_STATIC = 0x100, DLGC_BUTTON = 0x2000;

uint32_t get_extra(Window* w, int off) {
    if (!w || off + 4 > static_cast<int>(w->extra.size())) return 0;
    uint32_t v;
    std::memcpy(&v, w->extra.data() + off, 4);
    return v;
}
void set_extra(Window* w, int off, uint32_t v) {
    if (!w || off + 4 > static_cast<int>(w->extra.size())) return;
    std::memcpy(w->extra.data() + off, &v, 4);
}

struct SzOrOrd {
    bool is_ord = false;
    uint16_t ord = 0;
    std::u16string str;
};

uint64_t read_sz_or_ord(Emulator& e, uint64_t p, SzOrOrd& out) {
    uint16_t w = e.mem.read16(p);
    if (w == 0) return p + 2;
    if (w == 0xFFFF) {
        out.is_ord = true;
        out.ord = e.mem.read16(p + 2);
        return p + 4;
    }
    out.str = read_wstr(e, p);
    return p + (out.str.size() + 1) * 2;
}

int base_units_x(Gui& g, uint32_t font) {
    GdiObject* f = g.obj(font, ObjKind::Font);
    RealFont* rf = realize_font(f ? f->font : g.obj(default_gui_font(g))->font);
    static const char16_t* kAlpha = u"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    int w = 0;
    for (const char16_t* c = kAlpha; *c; ++c) w += rf->advance(*c);
    return (w / 26 + 1) / 2;
}
int base_units_y(Gui& g, uint32_t font) {
    GdiObject* f = g.obj(font, ObjKind::Font);
    return realize_font(f ? f->font : g.obj(default_gui_font(g))->font)->height;
}

int mul_div(int a, int b, int c) {
    int64_t p = static_cast<int64_t>(a) * b;
    return static_cast<int>(p >= 0 ? (p + c / 2) / c : -((-p + c / 2) / c));
}

uint32_t first_tabstop(Gui& g, Window* dlg) {
    for (uint32_t c : dlg->children) {
        Window* w = g.win(c);
        if (w && (w->style & WS_TABSTOP) && (w->style & WS_VISIBLE) && !(w->style & WS_DISABLED)) return c;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

uint32_t create_dialog(Gui& g, uint64_t hinst, uint64_t tmpl, uint32_t owner, uint64_t proc,
                       uint64_t param, bool ansi) {
    Emulator& e = g.emu;
    bool ex = e.mem.read16(tmpl) == 1 && e.mem.read16(tmpl + 2) == 0xFFFF;
    uint32_t style, exstyle, help = 0;
    int count, x, y, cx, cy;
    uint64_t p;
    if (ex) {
        help = e.mem.read32(tmpl + 4);
        exstyle = e.mem.read32(tmpl + 8);
        style = e.mem.read32(tmpl + 12);
        count = e.mem.read16(tmpl + 16);
        x = static_cast<int16_t>(e.mem.read16(tmpl + 18));
        y = static_cast<int16_t>(e.mem.read16(tmpl + 20));
        cx = static_cast<int16_t>(e.mem.read16(tmpl + 22));
        cy = static_cast<int16_t>(e.mem.read16(tmpl + 24));
        p = tmpl + 26;
    } else {
        style = e.mem.read32(tmpl);
        exstyle = e.mem.read32(tmpl + 4);
        count = e.mem.read16(tmpl + 8);
        x = static_cast<int16_t>(e.mem.read16(tmpl + 10));
        y = static_cast<int16_t>(e.mem.read16(tmpl + 12));
        cx = static_cast<int16_t>(e.mem.read16(tmpl + 14));
        cy = static_cast<int16_t>(e.mem.read16(tmpl + 16));
        p = tmpl + 18;
    }
    (void)help;
    SzOrOrd menu, cls, title;
    p = read_sz_or_ord(e, p, menu);
    p = read_sz_or_ord(e, p, cls);
    p = read_sz_or_ord(e, p, title);
    uint32_t font = 0;
    bool owns_font = false;
    if (style & DS_SETFONT) {
        int pt = e.mem.read16(p);
        p += 2;
        LogFont lf;
        if (ex) {
            lf.weight = e.mem.read16(p);
            lf.italic = e.mem.read8(p + 2);
            lf.charset = e.mem.read8(p + 3);
            p += 4;
        }
        lf.face = read_wstr(e, p);
        p += (lf.face.size() + 1) * 2;
        if (pt == 0x7FFF || lf.face == u"MS Shell Dlg" || lf.face == u"MS Shell Dlg 2") {
            if (lf.face.rfind(u"MS Shell Dlg", 0) == 0) lf.face = u"MS UI Gothic";
        }
        if (pt == 0x7FFF) pt = 9;
        lf.height = -mul_div(pt, 96, 72);
        if (!lf.weight) lf.weight = 400;
        auto o = std::make_unique<GdiObject>();
        o->font = lf;
        font = g.new_obj(ObjKind::Font, std::move(o));
        owns_font = true;
    } else {
        font = 0;
    }
    int bux = base_units_x(g, font ? font : default_gui_font(g));
    int buy = base_units_y(g, font ? font : default_gui_font(g));
    if (!font) {
        bux = 8;
        buy = 16;
    }

    // The window rectangle.
    Rect client{0, 0, mul_div(cx, bux, 4), mul_div(cy, buy, 8)};
    uint32_t wstyle = style & ~(WS_VISIBLE);
    uint32_t wex = exstyle;
    if (style & DS_MODALFRAME) wex |= WS_EX_DLGMODALFRAME;
    if (style & DS_CONTROL) {
        wstyle &= ~(WS_CAPTION | WS_SYSMENU);
        wex |= WS_EX_CONTROLPARENT;
    }
    bool child = (style & WS_CHILD) != 0;
    Rect ins = g.nc_insets(wstyle, wex, menu.is_ord || !menu.str.empty(), client.w());
    int ww = client.w() + ins.l + ins.r, wh = client.h() + ins.t + ins.b;
    int px = mul_div(x, bux, 4), py = mul_div(y, buy, 8);
    if (!child) {
        Window* ow = g.win(owner);
        if (style & (DS_CENTER | DS_CENTERMOUSE)) {
            Rect area{0, 0, g.screen_w, g.screen_h};
            px = area.l + (area.w() - ww) / 2;
            py = area.t + (area.h() - wh) / 2;
        } else if (!(style & DS_ABSALIGN) && ow) {
            Point o = g.client_to_screen(ow, {0, 0});
            px += o.x;
            py += o.y;
        }
        // Keep it on screen.
        px = std::max(0, std::min(px, g.screen_w - ww));
        py = std::max(0, std::min(py, g.screen_h - wh));
    }
    uint32_t menu_h = 0;
    if (menu.is_ord || !menu.str.empty()) {
        extern uint32_t load_menu_resource(Gui&, uint64_t, uint64_t);
        uint64_t mname = menu.is_ord ? menu.ord : 0;
        uint64_t tmp = 0;
        if (!menu.is_ord) {
            tmp = g.scratch_alloc((menu.str.size() + 1) * 2);
            write_wstr(e, tmp, menu.str, static_cast<int>(menu.str.size() + 1));
            mname = tmp;
        }
        menu_h = load_menu_resource(g, hinst, mname);
        if (tmp) g.scratch_free(tmp);
    }
    std::u16string cname = cls.is_ord ? u"#" + from_utf8(std::to_string(cls.ord)) : (cls.str.empty() ? u"#32770" : cls.str);
    uint32_t atom = cls.is_ord ? cls.ord : (cls.str.empty() ? 32770 : 0);
    uint32_t hwnd = g.create_window(wex, cname, atom, title.str, wstyle, px, py, ww, wh, owner,
                                    child ? 0 : menu_h, hinst, param, ansi);
    if (!hwnd) {
        if (owns_font) g.objs.erase(font);
        return 0;
    }
    Window* dw = g.win(hwnd);
    dw->is_dialog = true;
    dw->dlg_bux = bux;
    dw->dlg_buy = buy;
    dw->font = font;
    dw->owns_font = owns_font;
    if (dw->extra.size() < 30) dw->extra.resize(30, 0);
    set_extra(dw, DWLP_DLGPROC, static_cast<uint32_t>(proc));
    if (font) g.send(hwnd, WM_SETFONT, font, 0);

    // The controls.
    for (int i = 0; i < count; ++i) {
        p = (p + 3) & ~3ull;
        uint32_t cstyle, cex;
        int ix, iy, icx, icy;
        uint32_t id;
        if (ex) {
            cex = e.mem.read32(p + 4);
            cstyle = e.mem.read32(p + 8);
            ix = static_cast<int16_t>(e.mem.read16(p + 12));
            iy = static_cast<int16_t>(e.mem.read16(p + 14));
            icx = static_cast<int16_t>(e.mem.read16(p + 16));
            icy = static_cast<int16_t>(e.mem.read16(p + 18));
            id = e.mem.read32(p + 20);
            p += 24;
        } else {
            cstyle = e.mem.read32(p);
            cex = e.mem.read32(p + 4);
            ix = static_cast<int16_t>(e.mem.read16(p + 8));
            iy = static_cast<int16_t>(e.mem.read16(p + 10));
            icx = static_cast<int16_t>(e.mem.read16(p + 12));
            icy = static_cast<int16_t>(e.mem.read16(p + 14));
            id = e.mem.read16(p + 16);
            p += 18;
        }
        SzOrOrd ccls, ctitle;
        p = read_sz_or_ord(e, p, ccls);
        uint64_t title_ptr = p;
        p = read_sz_or_ord(e, p, ctitle);
        uint16_t extra_len = e.mem.read16(p);
        uint64_t extra = extra_len ? p : 0;
        p += 2 + extra_len;
        std::u16string ccname;
        if (ccls.is_ord) {
            switch (ccls.ord) {
                case 0x80: ccname = u"Button"; break;
                case 0x81: ccname = u"Edit"; break;
                case 0x82: ccname = u"Static"; break;
                case 0x83: ccname = u"ListBox"; break;
                case 0x84: ccname = u"ScrollBar"; break;
                case 0x85: ccname = u"ComboBox"; break;
                default: ccname = u"Static"; break;
            }
        } else {
            ccname = ccls.str;
        }
        Rect r{mul_div(ix, bux, 4), mul_div(iy, buy, 8), mul_div(ix + icx, bux, 4), mul_div(iy + icy, buy, 8)};
        std::u16string ctext = ctitle.is_ord ? u"" : ctitle.str;
        uint32_t ch = 0;
        {
            // The title can be a resource ordinal (an icon for a static).
            uint64_t tbuf = 0;
            uint64_t name_arg;
            if (ctitle.is_ord) {
                tbuf = g.scratch_alloc(8);
                e.mem.write16(tbuf, 0xFFFF);
                e.mem.write16(tbuf + 2, ctitle.ord);
                e.mem.write16(tbuf + 4, 0);
                name_arg = tbuf;
            } else {
                name_arg = title_ptr;
            }
            (void)name_arg;
            ch = g.create_window(cex | WS_EX_NOPARENTNOTIFY, ccname, 0, ctext, cstyle | WS_CHILD, r.l, r.t,
                                 r.w(), r.h(), hwnd, id, hinst, extra, ansi);
            if (ch && ctitle.is_ord) {
                // Static icon/bitmap controls take the ordinal through STM_SETIMAGE.
                Window* cw = g.win(ch);
                if (cw && upper(ccname) == u"STATIC") {
                    uint32_t ss = cstyle & 0x1F;
                    if (ss == 3) {  // SS_ICON
                        uint32_t icon = load_icon_or_cursor(g, hinst, ctitle.ord, true, 32, 32);
                        g.send(ch, 0x0172, 1, icon);  // STM_SETIMAGE IMAGE_ICON
                    } else if (ss == 0xE) {  // SS_BITMAP
                        uint32_t bmp = load_bitmap_resource(g, hinst, ctitle.ord);
                        g.send(ch, 0x0172, 0, bmp);
                    }
                }
            }
            if (tbuf) g.scratch_free(tbuf);
        }
        if (!ch) {
            if (g.trace_msgs) std::fprintf(stderr, "[gui] dialog control %s id %u failed\n", to_utf8(ccname).c_str(), id);
            continue;
        }
        if (font) g.send(ch, WM_SETFONT, font, 0);
    }
    dw = g.win(hwnd);
    if (!dw) return 0;
    // The default button, as DM_GETDEFID will report it.
    for (uint32_t c : dw->children) {
        Window* cw = g.win(c);
        if (cw && cw->cls && cw->cls->name == u"BUTTON" && (cw->style & 0xF) == 1) {  // BS_DEFPUSHBUTTON
            dw->dlg_def_id = static_cast<uint32_t>(cw->id);
            break;
        }
    }
    uint32_t focus = first_tabstop(g, dw);
    uint64_t r = g.send(hwnd, WM_INITDIALOG, focus, param);
    dw = g.win(hwnd);
    if (!dw) return 0;
    if (r && focus && g.win(focus)) {
        g.set_focus(focus);
        Window* fw = g.win(focus);
        if (fw && g.send(focus, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) g.send(focus, 0x00B1, 0, static_cast<uint64_t>(-1) & 0xFFFFFFFF);
    }
    if ((dw = g.win(hwnd)) && (style & WS_VISIBLE) && !(dw->style & WS_VISIBLE)) {
        g.show_window(hwnd, child ? 5 : 1);
    }
    return hwnd;
}

int64_t run_dialog_modal(Gui& g, uint32_t hdlg, uint32_t owner) {
    Window* dw = g.win(hdlg);
    if (!dw) return -1;
    bool reenable = false;
    if (owner && g.win(owner) && !(g.win(owner)->style & WS_DISABLED)) {
        Window* ow = g.win(owner);
        ow->style |= WS_DISABLED;
        reenable = true;
    }
    if (!(dw->style & WS_VISIBLE) && !dw->dlg_ended) g.show_window(hdlg, 1);
    g.modal_loop([&]() {
        Window* w = g.win(hdlg);
        return !w || w->dlg_ended;
    });
    int64_t result = -1;
    if (Window* w = g.win(hdlg)) result = w->dlg_result;
    if (reenable && g.win(owner)) g.win(owner)->style &= ~WS_DISABLED;
    if (g.win(hdlg)) g.destroy_window(hdlg);
    if (owner && g.win(owner)) g.set_active(g.toplevel_of(g.win(owner)) ? g.toplevel_of(g.win(owner))->hwnd : owner);
    return result;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

uint32_t next_tab_item(Gui& g, uint32_t dlg, uint32_t from, bool prev) {
    Window* d = g.win(dlg);
    if (!d) return 0;
    // Flatten the tab order, descending into WS_EX_CONTROLPARENT children.
    std::vector<uint32_t> order;
    std::function<void(Window*)> walk = [&](Window* p) {
        for (uint32_t c : p->children) {
            Window* w = g.win(c);
            if (!w || !(w->style & WS_VISIBLE) || (w->style & WS_DISABLED)) continue;
            if (w->exstyle & WS_EX_CONTROLPARENT) {
                walk(w);
                continue;
            }
            if (w->style & WS_TABSTOP) order.push_back(c);
        }
    };
    walk(d);
    if (order.empty()) return 0;
    auto it = std::find(order.begin(), order.end(), from);
    if (it == order.end()) return prev ? order.back() : order.front();
    size_t i = static_cast<size_t>(it - order.begin());
    if (prev) return order[(i + order.size() - 1) % order.size()];
    return order[(i + 1) % order.size()];
}

static uint32_t next_group_item(Gui& g, uint32_t dlg, uint32_t from, bool prev) {
    Window* d = g.win(dlg);
    Window* f = g.win(from);
    if (!d || !f) return 0;
    Window* p = g.win(f->parent);
    if (!p) return 0;
    auto& v = p->children;
    auto it = std::find(v.begin(), v.end(), from);
    if (it == v.end()) return 0;
    // The group: from the nearest WS_GROUP at or before `from` to the next WS_GROUP.
    size_t i = static_cast<size_t>(it - v.begin());
    size_t start = i;
    while (start > 0 && !(g.win(v[start]) && (g.win(v[start])->style & WS_GROUP))) --start;
    size_t end = i + 1;
    while (end < v.size() && !(g.win(v[end]) && (g.win(v[end])->style & WS_GROUP))) ++end;
    size_t n = end - start;
    for (size_t k = 1; k <= n; ++k) {
        size_t j = prev ? start + (i - start + n - k % n) % n : start + (i - start + k) % n;
        Window* w = g.win(v[j]);
        if (w && (w->style & WS_VISIBLE) && !(w->style & WS_DISABLED)) return v[j];
    }
    return from;
}

static void dlg_set_focus(Gui& g, uint32_t dlg, uint32_t ctl) {
    if (!ctl) return;
    uint64_t code = g.send(ctl, WM_GETDLGCODE, 0, 0);
    g.set_focus(ctl);
    if (code & DLGC_HASSETSEL) g.send(ctl, 0x00B1, 0, 0xFFFFFFFF);  // EM_SETSEL 0, -1
    // A push button that takes the focus becomes the default one.
    Window* d = g.win(dlg);
    if (!d) return;
    if (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) {
        for (uint32_t c : d->children) {
            if (c == ctl) continue;
            uint64_t cc = g.send(c, WM_GETDLGCODE, 0, 0);
            if (cc & DLGC_DEFPUSHBUTTON) g.send(c, 0x00F4, 0, 1);  // BM_SETSTYLE BS_PUSHBUTTON
        }
        g.send(ctl, 0x00F4, 1, 1);  // BS_DEFPUSHBUTTON
    } else {
        // Focus left the buttons: the dialog's default button is the default again.
        for (uint32_t c : d->children) {
            Window* w = g.win(c);
            if (!w) continue;
            uint64_t cc = g.send(c, WM_GETDLGCODE, 0, 0);
            if (!(cc & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))) continue;
            bool def = w->id == d->dlg_def_id;
            if (def != ((cc & DLGC_DEFPUSHBUTTON) != 0)) g.send(c, 0x00F4, def ? 1 : 0, 1);
        }
    }
}

static uint32_t find_item_by_id(Gui& g, uint32_t dlg, uint32_t id) {
    Window* d = g.win(dlg);
    if (!d) return 0;
    for (uint32_t c : d->children) {
        Window* w = g.win(c);
        if (w && static_cast<uint32_t>(w->id) == id) return c;
    }
    // Controls inside WS_EX_CONTROLPARENT children count too.
    for (uint32_t c : d->children) {
        Window* w = g.win(c);
        if (w && (w->exstyle & WS_EX_CONTROLPARENT))
            if (uint32_t r = find_item_by_id(g, c, id)) return r;
    }
    return 0;
}

bool is_dialog_message(Gui& g, uint32_t hdlg, const Msg& m) {
    Window* d = g.win(hdlg);
    if (!d) return false;
    if (m.hwnd != hdlg && !is_child_of(g, m.hwnd, hdlg)) return false;
    auto dispatch = [&]() {
        if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
            int vk = static_cast<int>(m.wparam);
            uint32_t ch = 0;
            bool ctrl = (g.key_state[0x11] & 0x80) != 0;
            if (vk == 0x08 || vk == 0x09 || vk == 0x0D || vk == 0x1B) ch = static_cast<uint32_t>(vk);
            else if (ctrl && vk >= 'A' && vk <= 'Z') ch = static_cast<uint32_t>(vk - 'A' + 1);
            if (ch) g.post(m.hwnd, m.message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, m.lparam);
        }
        g.dispatch(m);
    };
    if (m.message == WM_KEYDOWN) {
        uint64_t code = g.win(m.hwnd) ? g.send(m.hwnd, WM_GETDLGCODE, m.wparam, 0) : 0;
        switch (m.wparam) {
            case 0x09: {  // VK_TAB
                if (code & (DLGC_WANTTAB | DLGC_WANTALLKEYS)) break;
                bool shift = (g.key_state[0x10] & 0x80) != 0;
                uint32_t next = next_tab_item(g, hdlg, g.focus, shift);
                if (next) dlg_set_focus(g, hdlg, next);
                return true;
            }
            case 0x25: case 0x26: case 0x27: case 0x28: {  // arrows
                if (code & (DLGC_WANTARROWS | DLGC_WANTALLKEYS)) break;
                bool prev = m.wparam == 0x25 || m.wparam == 0x26;
                uint32_t next = next_group_item(g, hdlg, g.focus, prev);
                if (next && next != g.focus) {
                    dlg_set_focus(g, hdlg, next);
                    uint64_t nc = g.send(next, WM_GETDLGCODE, 0, 0);
                    if (nc & DLGC_RADIOBUTTON) g.send(next, 0x00F5, 0, 0);  // BM_CLICK
                }
                return true;
            }
            case 0x0D: {  // VK_RETURN
                if (code & DLGC_WANTALLKEYS) break;
                uint32_t id = 1;
                uint32_t btn = 0;
                if (code & DLGC_DEFPUSHBUTTON) {
                    btn = m.hwnd;
                    id = control_id(g.win(m.hwnd));
                } else {
                    uint64_t def = g.send(hdlg, 0x0400, 0, 0);  // DM_GETDEFID
                    if ((def >> 16) == 0x534B) id = static_cast<uint32_t>(def & 0xFFFF);
                    btn = find_item_by_id(g, hdlg, id);
                }
                Window* bw = g.win(btn);
                if (bw && (bw->style & WS_DISABLED)) return true;
                g.send(hdlg, WM_COMMAND, id & 0xFFFF, btn);
                return true;
            }
            case 0x1B: {  // VK_ESCAPE
                if (code & DLGC_WANTALLKEYS) break;
                uint32_t btn = find_item_by_id(g, hdlg, 2);
                Window* bw = g.win(btn);
                if (bw && (bw->style & WS_DISABLED)) return true;
                g.send(hdlg, WM_COMMAND, 2, btn);  // IDCANCEL
                return true;
            }
        }
    } else if (m.message == WM_SYSCHAR || (m.message == WM_CHAR && m.wparam > 0x20)) {
        uint64_t code = g.win(m.hwnd) ? g.send(m.hwnd, WM_GETDLGCODE, m.wparam, 0) : 0;
        if (m.message == WM_SYSCHAR || !(code & (DLGC_WANTCHARS | DLGC_WANTALLKEYS))) {
            // Mnemonics: a control (or the static before it) whose text has &c.
            char16_t c = static_cast<char16_t>(m.wparam);
            auto up = [](char16_t x) { return x >= 'a' && x <= 'z' ? static_cast<char16_t>(x - 32) : x; };
            for (uint32_t ch : d->children) {
                Window* w = g.win(ch);
                if (!w || !(w->style & WS_VISIBLE) || (w->style & WS_DISABLED)) continue;
                size_t a = w->text.find(u'&');
                if (a == std::u16string::npos || a + 1 >= w->text.size() || up(w->text[a + 1]) != up(c)) continue;
                uint64_t cc = g.send(ch, WM_GETDLGCODE, 0, 0);
                if (cc & DLGC_STATIC) {
                    uint32_t next = next_tab_item(g, hdlg, ch, false);
                    // The next control after the label in z-order.
                    auto& v = d->children;
                    auto it = std::find(v.begin(), v.end(), ch);
                    for (++it; it != v.end(); ++it) {
                        Window* nw = g.win(*it);
                        if (nw && (nw->style & WS_TABSTOP) && !(nw->style & WS_DISABLED)) {
                            next = *it;
                            break;
                        }
                    }
                    if (next) dlg_set_focus(g, hdlg, next);
                } else if (cc & DLGC_BUTTON || cc & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON | DLGC_RADIOBUTTON)) {
                    dlg_set_focus(g, hdlg, ch);
                    g.send(ch, 0x00F5, 0, 0);  // BM_CLICK
                } else {
                    dlg_set_focus(g, hdlg, ch);
                }
                return true;
            }
            if (m.message == WM_SYSCHAR) return false;
        }
    }
    dispatch();
    return true;
}

// ---------------------------------------------------------------------------
// DefDlgProc
// ---------------------------------------------------------------------------

uint64_t def_dlg_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    if (w->extra.size() < 30) w->extra.resize(30, 0);
    uint64_t proc = get_extra(w, DWLP_DLGPROC);
    if (proc) {
        set_extra(w, DWLP_MSGRESULT, 0);
        uint64_t r = g.call_wndproc(proc, hwnd, msg, wp, lp);
        w = g.win(hwnd);
        if (!w) return 0;
        if (r) {
            switch (msg) {
                case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
                case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR:
                case WM_CTLCOLORSTATIC: case WM_COMPAREITEM: case WM_VKEYTOITEM:
                case WM_CHARTOITEM: case WM_QUERYDRAGICON: case WM_INITDIALOG:
                    return r;
            }
            return get_extra(w, DWLP_MSGRESULT);
        }
    }
    switch (msg) {
        case WM_ERASEBKGND: {
            uint32_t br = ctl_color(g, hwnd, static_cast<uint32_t>(wp), WM_CTLCOLORDLG);
            if (!br) br = get_sys_color_brush(g, 15);
            // The dialog colours its own background (the parent is not asked).
            br = static_cast<uint32_t>(def_window_proc(g, hwnd, WM_CTLCOLORDLG, wp, hwnd));
            uint64_t app = proc ? g.call_wndproc(proc, hwnd, WM_CTLCOLORDLG, wp, hwnd) : 0;
            if (app) br = static_cast<uint32_t>(app);
            draw::fill_rect_brush(g, static_cast<uint32_t>(wp), {0, 0, w->crect.w(), w->crect.h()}, br);
            return 1;
        }
        case WM_SHOWWINDOW:
            if (!wp && g.focus && is_child_of(g, g.focus, hwnd)) w->dlg_focus = g.focus;
            return def_window_proc(g, hwnd, msg, wp, lp);
        case WM_ACTIVATE:
            if (wp & 0xFFFF) {
                uint32_t f = w->dlg_focus && g.win(w->dlg_focus) ? w->dlg_focus : first_tabstop(g, w);
                if (f && !(g.focus && is_child_of(g, g.focus, hwnd))) g.set_focus(f);
                else if (!f) g.set_focus(hwnd);
            } else if (g.focus && is_child_of(g, g.focus, hwnd)) {
                w->dlg_focus = g.focus;
            }
            return 0;
        case WM_SETFOCUS: {
            uint32_t f = w->dlg_focus && g.win(w->dlg_focus) ? w->dlg_focus : first_tabstop(g, w);
            if (f) g.set_focus(f);
            return 0;
        }
        case WM_CLOSE: {
            uint32_t btn = find_item_by_id(g, hwnd, 2);
            Window* bw = g.win(btn);
            if (!bw || !(bw->style & WS_DISABLED)) g.post(hwnd, WM_COMMAND, 2, btn);
            return 0;
        }
        case 0x0028: {  // WM_NEXTDLGCTL
            uint32_t next;
            if (lp & 0xFFFF) next = static_cast<uint32_t>(wp);
            else next = next_tab_item(g, hwnd, g.focus, wp != 0);
            if (next) dlg_set_focus(g, hwnd, next);
            return 0;
        }
        case 0x0400:  // DM_GETDEFID
            return (0x534Bu << 16) | (w->dlg_def_id & 0xFFFF);
        case 0x0401: {  // DM_SETDEFID
            uint32_t old = w->dlg_def_id;
            w->dlg_def_id = static_cast<uint32_t>(wp);
            if (uint32_t ob = find_item_by_id(g, hwnd, old)) g.send(ob, 0x00F4, 0, 1);
            if (uint32_t nb = find_item_by_id(g, hwnd, w->dlg_def_id)) g.send(nb, 0x00F4, 1, 1);
            return 1;
        }
        case 0x0402:  // DM_REPOSITION
            return 0;
        case WM_GETFONT:
            return w->font;
        case WM_SETFONT:
            w->font = wp;
            return 0;
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORMSGBOX:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSCROLLBAR:
            return def_window_proc(g, hwnd, msg, wp, lp);
        case WM_NCDESTROY:
            if (w->owns_font && w->font) {
                g.objs.erase(static_cast<uint32_t>(w->font));
                w->font = 0;
                w->owns_font = false;
            }
            return 0;
        case WM_GETDLGCODE:
            return 0;
        case WM_INITDIALOG:
            return 0;
    }
    return def_window_proc(g, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// MessageBox
// ---------------------------------------------------------------------------

namespace {

// A message box's look on Windows 11: a white body with the icon and text,
// and a grey band along the bottom holding the buttons.
struct MsgBoxLook {
    int footer_top = 0;
    uint32_t icon = 0;
};
std::map<uint32_t, MsgBoxLook> g_msgbox_look;

uint64_t msgbox_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t) {
    Window* w = g.win(hwnd);
    if (!w) return 0;
    switch (msg) {
        case WM_COMMAND:
            if ((wp >> 16) != 0) return 0;
            w->dlg_ended = true;
            w->dlg_result = static_cast<int64_t>(wp & 0xFFFF);
            return 1;
        case WM_ERASEBKGND: {
            const MsgBoxLook& look = g_msgbox_look[hwnd];
            uint32_t hdc = static_cast<uint32_t>(wp);
            draw::fill_rect(g, hdc, {0, 0, w->crect.w(), look.footer_top}, 0xFFFFFF);
            draw::fill_rect(g, hdc, {0, look.footer_top, w->crect.w(), w->crect.h()}, 0xF0F0F0);
            if (look.icon) draw::draw_icon(g, hdc, 21, 23, look.icon, 32, 32);
            set_extra(w, DWLP_MSGRESULT, 1);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
            if (DC* d = g.dc(static_cast<uint32_t>(wp))) d->s.bk_color = 0xFFFFFF;
            return g.stock[0];  // WHITE_BRUSH
        case WM_DESTROY:
            g_msgbox_look.erase(hwnd);
            return 0;
    }
    return 0;
}

int message_box(Gui& g, uint32_t owner, const std::u16string& text, const std::u16string& caption, uint32_t type) {
    std::fprintf(stderr, "[MessageBox] %s: %s\n", to_utf8(caption).c_str(), to_utf8(text).c_str());
    static uint64_t proc_hook = 0;
    if (!proc_hook) {
        proc_hook = g.emu.add_hook("__msgbox_dlgproc", g.emu.is64() ? 0 : 16, [](Emulator& e) {
            ret(e, msgbox_proc(gui(e), U(e, 0), U(e, 1), A(e, 2), A(e, 3)));
        });
    }
    struct Btn {
        int id;
        const char16_t* label;
    };
    std::vector<Btn> btns;
    switch (type & 0xF) {
        case 0: btns = {{1, u"OK"}}; break;
        case 1: btns = {{1, u"OK"}, {2, u"キャンセル"}}; break;
        case 2: btns = {{3, u"中止(&A)"}, {4, u"再試行(&R)"}, {5, u"無視(&I)"}}; break;
        case 3: btns = {{6, u"はい(&Y)"}, {7, u"いいえ(&N)"}, {2, u"キャンセル"}}; break;
        case 4: btns = {{6, u"はい(&Y)"}, {7, u"いいえ(&N)"}}; break;
        case 5: btns = {{4, u"再試行(&R)"}, {2, u"キャンセル"}}; break;
        case 6: btns = {{2, u"キャンセル"}, {10, u"再実行(&T)"}, {11, u"続行(&C)"}}; break;
        default: btns = {{1, u"OK"}}; break;
    }
    // MB_ICONHAND 0x10, MB_ICONQUESTION 0x20, MB_ICONEXCLAMATION 0x30,
    // MB_ICONASTERISK 0x40: the system icons IDI_HAND .. IDI_ASTERISK.
    uint32_t icon = 0;
    if (uint32_t k = (type >> 4) & 0xF; k >= 1 && k <= 4) icon = system_icon(g, 32512 + static_cast<int>(k));
    // Measured on Windows 11: icon at (21, 23), text 62 in (21 without an
    // icon) and 30 short of the right edge, the grey band starting 21 below
    // the taller of the two, 42 tall, with 75x23 buttons 9 apart, 9 from its
    // top and 15 from the right.
    uint32_t font = g.ui_font ? g.ui_font : default_gui_font(g);
    uint32_t hdc = g.create_dc(DC::Kind::Memory, 0, false);
    draw::select_font(g, hdc, font);
    Rect tr{0, 0, g.screen_w * 2 / 3, 0};
    draw_text_impl(g, hdc, text, tr, 0x400 | 0x10 | 0x800);  // CALCRECT|WORDBREAK|NOPREFIX
    std::vector<int> bws;
    for (const auto& b : btns) {
        Rect br{0, 0, 0, 0};
        draw_text_impl(g, hdc, b.label, br, 0x400 | 0x20);  // CALCRECT|SINGLELINE
        bws.push_back(std::max(75, br.w() + 16));
    }
    g.delete_dc(hdc);
    int tx = icon ? 62 : 21, ty = 23;
    int th = tr.h();
    if (icon && th < 32) ty += (32 - th) / 2;
    int body = std::max(ty + th, icon ? 23 + 32 : 0) + 21;
    int btn_total = 0;
    for (int bw : bws) btn_total += bw;
    btn_total += 9 * (static_cast<int>(btns.size()) - 1);
    int cw = std::max({tx + tr.w() + 30, btn_total + 30, 160});
    int ch = body + 42;
    uint32_t style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_CLIPSIBLINGS;
    Rect ins = g.nc_insets(style, WS_EX_DLGMODALFRAME, false, cw);
    int ww = cw + ins.l + ins.r, wh = ch + ins.t + ins.b;
    int x = (g.screen_w - ww) / 2, y = (g.screen_h - wh) / 2;
    uint32_t top_owner = owner;
    if (Window* ow = g.win(owner)) {
        Window* t = g.toplevel_of(ow);
        top_owner = t ? t->hwnd : 0;
    }
    uint32_t hwnd = g.create_window(WS_EX_DLGMODALFRAME | 0x8, u"#32770", 32770, caption.empty() ? u"エラー" : caption, style, x, y,
                                    ww, wh, top_owner, 0, 0, 0, false);
    if (!hwnd) return 1;
    Window* w = g.win(hwnd);
    w->is_dialog = true;
    if (w->extra.size() < 30) w->extra.resize(30, 0);
    g_msgbox_look[hwnd] = {body, icon};
    set_extra(w, DWLP_DLGPROC, static_cast<uint32_t>(proc_hook));
    w->font = font;
    uint32_t st = g.create_window(0, u"Static", 0, text, WS_CHILD | WS_VISIBLE | 0x80 /*SS_NOPREFIX*/, tx, ty,
                                  tr.w() + 4, th, hwnd, 0xFFFF, 0, 0, false);
    if (st) g.send(st, WM_SETFONT, font, 0);
    int bx = cw - 15 - btn_total, by = body + 9;
    uint32_t def_btn = 0;
    int def_index = static_cast<int>((type >> 8) & 0xF);
    for (size_t i = 0; i < btns.size(); ++i) {
        bool def = static_cast<int>(i) == def_index;
        uint32_t b = g.create_window(0, u"Button", 0, btns[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (def ? 1u : 0u), bx, by,
                                     bws[i], 23, hwnd, static_cast<uint64_t>(btns[i].id), 0, 0, false);
        if (b) {
            g.send(b, WM_SETFONT, font, 0);
            if (def) {
                def_btn = b;
                w->dlg_def_id = static_cast<uint32_t>(btns[i].id);
            }
        }
        bx += bws[i] + 9;
    }
    g.show_window(hwnd, 1);
    if (def_btn) g.set_focus(def_btn);
    int64_t r = run_dialog_modal(g, hwnd, top_owner);
    return r <= 0 ? (btns.size() == 1 ? 1 : 2) : static_cast<int>(r);
}

// The print dialog's controls, and where OK leaves what they say.
constexpr uint32_t IDOK = 1, IDCANCEL = 2, BM_GETCHECK = 0xF0, BM_SETCHECK = 0xF1;
constexpr uint32_t CB_ADDSTRING = 0x143, CB_GETCURSEL = 0x147, CB_SETCURSEL = 0x14E;
constexpr uint32_t kPrnPaper = 100, kPrnPortrait = 101, kPrnLandscape = 102, kPrnColor = 103;
struct PrintChoice {
    int paper = 0;
    bool landscape = false, color = true;
};
PrintChoice* g_print_choice = nullptr;

uint64_t print_proc(Gui& g, uint32_t hwnd, uint32_t msg, uint64_t wp, uint64_t) {
    if (msg != WM_COMMAND || (wp >> 16) != 0) return 0;
    uint32_t id = static_cast<uint32_t>(wp & 0xFFFF);
    Window* w = g.win(hwnd);
    if (!w) return 0;
    if (id == kPrnPortrait || id == kPrnLandscape) {
        g.send(find_item_by_id(g, hwnd, kPrnPortrait), BM_SETCHECK, id == kPrnPortrait, 0);
        g.send(find_item_by_id(g, hwnd, kPrnLandscape), BM_SETCHECK, id == kPrnLandscape, 0);
        return 1;
    }
    if (id != IDOK && id != IDCANCEL) return 0;
    if (id == IDOK && g_print_choice) {
        int sel = static_cast<int>(g.send(find_item_by_id(g, hwnd, kPrnPaper), CB_GETCURSEL, 0, 0));
        if (sel >= 0) g_print_choice->paper = sel;
        g_print_choice->landscape = g.send(find_item_by_id(g, hwnd, kPrnLandscape), BM_GETCHECK, 0, 0) != 0;
        g_print_choice->color = g.send(find_item_by_id(g, hwnd, kPrnColor), BM_GETCHECK, 0, 0) != 0;
    }
    w->dlg_ended = true;
    w->dlg_result = id;
    return 1;
}

}  // namespace

bool print_dialog(Gui& g, uint32_t owner, bool setup, const std::vector<std::u16string>& papers,
                  int& paper, bool& landscape, bool& color) {
    static uint64_t proc_hook = 0;
    if (!proc_hook) {
        proc_hook = g.emu.add_hook("__print_dlgproc", g.emu.is64() ? 0 : 16, [](Emulator& e) {
            ret(e, print_proc(gui(e), U(e, 0), U(e, 1), A(e, 2), A(e, 3)));
        });
    }
    uint32_t font = default_gui_font(g);
    const int cw = 340, ch = 190;
    uint32_t style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_CLIPSIBLINGS;
    Rect ins = g.nc_insets(style, WS_EX_DLGMODALFRAME, false, cw);
    int ww = cw + ins.l + ins.r, wh = ch + ins.t + ins.b;
    uint32_t top_owner = owner;
    if (Window* ow = g.win(owner)) {
        Window* t = g.toplevel_of(ow);
        top_owner = t ? t->hwnd : 0;
    }
    uint32_t hwnd = g.create_window(WS_EX_DLGMODALFRAME | 0x8, u"#32770", 32770, setup ? u"プリンタの設定" : u"印刷", style,
                                    (g.screen_w - ww) / 2, (g.screen_h - wh) / 2, ww, wh, top_owner, 0, 0, 0, false);
    if (!hwnd) return false;
    Window* w = g.win(hwnd);
    w->is_dialog = true;
    if (w->extra.size() < 30) w->extra.resize(30, 0);
    set_extra(w, DWLP_DLGPROC, static_cast<uint32_t>(proc_hook));
    w->font = font;
    auto child = [&](const char16_t* cls, const std::u16string& text, uint32_t st, int x, int y, int cx, int cy, uint32_t id) {
        uint32_t c = g.create_window(0, cls, 0, text, WS_CHILD | WS_VISIBLE | st, x, y, cx, cy, hwnd, id, 0, 0, false);
        if (c) g.send(c, WM_SETFONT, font, 0);
        return c;
    };
    child(u"Static", u"プリンタ:  PDF / PNG 出力", 0, 16, 14, 300, 18, 0xFFFF);
    child(u"Static", u"C:\\jww\\印刷 に PDF と PNG を書き出します", 0, 16, 34, 310, 18, 0xFFFF);
    child(u"Static", u"用紙サイズ", 0, 16, 70, 80, 18, 0xFFFF);
    uint32_t combo = child(u"ComboBox", u"", 3 /*CBS_DROPDOWNLIST*/ | WS_TABSTOP, 110, 66, 150, 220, kPrnPaper);
    for (const auto& p : papers) {
        std::vector<uint16_t> buf(p.begin(), p.end());
        buf.push_back(0);
        uint64_t s = g.emu.heap_alloc(buf.size() * 2);
        g.emu.mem.write(s, buf.data(), buf.size() * 2);
        g.send(combo, CB_ADDSTRING, 0, s);
        g.emu.heap_free(s);
    }
    g.send(combo, CB_SETCURSEL, static_cast<uint64_t>(paper), 0);
    child(u"Static", u"印刷の向き", 0, 16, 102, 80, 18, 0xFFFF);
    uint32_t por = child(u"Button", u"縦", 4 /*BS_RADIOBUTTON*/ | WS_GROUP | WS_TABSTOP, 110, 100, 60, 20, kPrnPortrait);
    uint32_t lan = child(u"Button", u"横", 4, 180, 100, 60, 20, kPrnLandscape);
    g.send(landscape ? lan : por, BM_SETCHECK, 1, 0);
    uint32_t col = child(u"Button", u"カラー", 3 /*BS_AUTOCHECKBOX*/ | WS_GROUP | WS_TABSTOP, 110, 128, 100, 20, kPrnColor);
    g.send(col, BM_SETCHECK, color ? 1 : 0, 0);
    uint32_t ok = child(u"Button", u"OK", 1 /*BS_DEFPUSHBUTTON*/ | WS_TABSTOP, cw - 190, ch - 36, 80, 24, IDOK);
    child(u"Button", u"キャンセル", WS_TABSTOP, cw - 100, ch - 36, 80, 24, IDCANCEL);
    w->dlg_def_id = IDOK;
    PrintChoice choice{paper, landscape, color};
    g_print_choice = &choice;
    g.show_window(hwnd, 1);
    if (ok) g.set_focus(ok);
    int64_t r = run_dialog_modal(g, hwnd, top_owner);
    g_print_choice = nullptr;
    if (r != IDOK) return false;
    paper = choice.paper;
    landscape = choice.landscape;
    color = choice.color;
    return true;
}

namespace {
}  // namespace

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

void install_dialog(Emulator& e) {
    Gui& g = gui(e);
    register_builtin_class(e, u"#32770", 0x0008 | 0x0800, 30, 32512, 0, def_dlg_proc);  // CS_DBLCLKS|CS_SAVEBITS
    g.defdlgproc_hook = g.builtin_procs[u"#32770"];
    e.add_hook("DefDlgProcW", e.is64() ? 0 : 16, [](Emulator& e) {
        ret(e, def_dlg_proc(gui(e), U(e, 0), U(e, 1), A(e, 2), A(e, 3)));
    });
    e.alias_hook("DefDlgProcW", "DefDlgProcA");

    auto create_indirect = [](Emulator& e, bool ansi) {
        Gui& g = gui(e);
        ret(e, create_dialog(g, A(e, 0), A(e, 1), U(e, 2), A(e, 3), A(e, 4), ansi));
    };
    api(e, "CreateDialogIndirectParamW", 5, [create_indirect](Emulator& e) { create_indirect(e, false); });
    api(e, "CreateDialogIndirectParamA", 5, [create_indirect](Emulator& e) { create_indirect(e, true); });
    api(e, "CreateDialogParamW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t addr;
        uint32_t size;
        if (!find_resource(e, A(e, 0), 5, A(e, 1), addr, size)) return ret(e, 0);
        ret(e, create_dialog(g, A(e, 0), addr, U(e, 2), A(e, 3), A(e, 4), false));
    });
    e.alias_hook("CreateDialogParamW", "CreateDialogParamA");
    auto modal_indirect = [](Emulator& e, uint64_t tmpl) {
        Gui& g = gui(e);
        uint32_t owner = U(e, 2);
        if (Window* ow = g.win(owner)) {
            Window* t = g.toplevel_of(ow);
            if (t) owner = t->hwnd;
        }
        uint32_t h = create_dialog(g, A(e, 0), tmpl, owner, A(e, 3), A(e, 4), false);
        if (!h) return ret(e, static_cast<uint64_t>(-1) & 0xFFFFFFFF);
        ret(e, static_cast<uint64_t>(run_dialog_modal(g, h, owner)) & 0xFFFFFFFF);
    };
    api(e, "DialogBoxIndirectParamW", 5, [modal_indirect](Emulator& e) { modal_indirect(e, A(e, 1)); });
    e.alias_hook("DialogBoxIndirectParamW", "DialogBoxIndirectParamA");
    api(e, "DialogBoxParamW", 5, [modal_indirect](Emulator& e) {
        uint64_t addr;
        uint32_t size;
        if (!find_resource(e, A(e, 0), 5, A(e, 1), addr, size)) return ret(e, static_cast<uint64_t>(-1) & 0xFFFFFFFF);
        modal_indirect(e, addr);
    });
    e.alias_hook("DialogBoxParamW", "DialogBoxParamA");
    api(e, "EndDialog", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        if (!w) return ret(e, 0);
        w->dlg_ended = true;
        w->dlg_result = static_cast<int32_t>(U(e, 1));
        // The owner comes back before the dialog goes, so activation can return to it.
        if (w->owner && g.win(w->owner)) g.win(w->owner)->style &= ~WS_DISABLED;
        g.set_window_pos(w->hwnd, 0, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        ret(e, 1);
    });
    api(e, "IsDialogMessageW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Msg m = g.read_msg(A(e, 1));
        ret(e, is_dialog_message(g, U(e, 0), m) ? 1 : 0);
    });
    e.alias_hook("IsDialogMessageW", "IsDialogMessageA");
    e.alias_hook("IsDialogMessageW", "IsDialogMessage");
    api(e, "GetDlgItem", 2, [](Emulator& e) { ret(e, find_item_by_id(gui(e), U(e, 0), U(e, 1))); });
    api(e, "SetDlgItemTextW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        if (!c) return ret(e, 0);
        ret(e, g.send(c, WM_SETTEXT, 0, A(e, 2)) ? 1 : 0);
    });
    api(e, "SetDlgItemTextA", 3, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        if (!c) return ret(e, 0);
        std::u16string s = A(e, 2) ? cp932_to_utf16(e.mem.read_cstring(A(e, 2))) : u"";
        uint64_t buf = g.scratch_alloc((s.size() + 1) * 2);
        write_wstr(e, buf, s, static_cast<int>(s.size() + 1));
        uint64_t r = g.send(c, WM_SETTEXT, 0, buf);
        g.scratch_free(buf);
        ret(e, r ? 1 : 0);
    });
    api(e, "GetDlgItemTextW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        if (!c || I(e, 3) <= 0) {
            if (A(e, 2) && I(e, 3) > 0) e.mem.write16(A(e, 2), 0);
            return ret(e, 0);
        }
        ret(e, g.send(c, WM_GETTEXT, U(e, 3), A(e, 2)));
    });
    api(e, "SetDlgItemInt", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        if (!c) return ret(e, 0);
        std::string s = U(e, 3) ? std::to_string(I(e, 2)) : std::to_string(U(e, 2));
        std::u16string w = from_utf8(s);
        uint64_t buf = g.scratch_alloc((w.size() + 1) * 2);
        write_wstr(e, buf, w, static_cast<int>(w.size() + 1));
        g.send(c, WM_SETTEXT, 0, buf);
        g.scratch_free(buf);
        ret(e, 1);
    });
    api(e, "GetDlgItemInt", 4, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        if (A(e, 2)) e.mem.write32(A(e, 2), 0);
        if (!c) return ret(e, 0);
        Window* w = g.win(c);
        std::u16string s = w->text;
        if (w->cls && (w->cls->name == u"EDIT" || w->cls->name == u"COMBOBOX")) {
            uint64_t buf = g.scratch_alloc(64 * 2);
            g.send(c, WM_GETTEXT, 64, buf);
            s = read_wstr(e, buf);
            g.scratch_free(buf);
        }
        size_t i = 0;
        while (i < s.size() && s[i] == u' ') ++i;
        bool neg = false;
        if (i < s.size() && s[i] == u'-' && U(e, 3)) {
            neg = true;
            ++i;
        }
        if (i >= s.size() || s[i] < u'0' || s[i] > u'9') return ret(e, 0);
        int64_t v = 0;
        for (; i < s.size() && s[i] >= u'0' && s[i] <= u'9'; ++i) v = v * 10 + (s[i] - u'0');
        while (i < s.size() && s[i] == u' ') ++i;
        if (i != s.size()) return ret(e, 0);
        if (A(e, 2)) e.mem.write32(A(e, 2), 1);
        ret(e, static_cast<uint32_t>(neg ? -v : v));
    });
    api(e, "CheckDlgButton", 3, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        if (!c) return ret(e, 0);
        g.send(c, 0x00F1, U(e, 2), 0);  // BM_SETCHECK
        ret(e, 1);
    });
    api(e, "IsDlgButtonChecked", 2, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        ret(e, c ? g.send(c, 0x00F0, 0, 0) : 0);  // BM_GETCHECK
    });
    api(e, "CheckRadioButton", 4, [](Emulator& e) {
        Gui& g = gui(e);
        for (uint32_t id = U(e, 1); id <= U(e, 2); ++id) {
            uint32_t c = find_item_by_id(g, U(e, 0), id);
            if (c) g.send(c, 0x00F1, id == U(e, 3) ? 1 : 0, 0);
        }
        ret(e, 1);
    });
    api(e, "SendDlgItemMessageW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t c = find_item_by_id(g, U(e, 0), U(e, 1));
        ret(e, c ? g.send(c, U(e, 2), A(e, 3), A(e, 4)) : 0);
    });
    api(e, "MapDialogRect", 2, [](Emulator& e) {
        Gui& g = gui(e);
        Window* w = g.win(A(e, 0));
        int bux = w ? w->dlg_bux : 8, buy = w ? w->dlg_buy : 16;
        Rect r = read_rect(e, A(e, 1));
        write_rect(e, A(e, 1), {mul_div(r.l, bux, 4), mul_div(r.t, buy, 8), mul_div(r.r, bux, 4), mul_div(r.b, buy, 8)});
        ret(e, 1);
    });
    api(e, "GetDialogBaseUnits", 0, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t f = default_gui_font(g);
        ret(e, make_lparam(base_units_x(g, f), base_units_y(g, f)));
    });
    api(e, "GetNextDlgTabItem", 3, [](Emulator& e) { ret(e, next_tab_item(gui(e), U(e, 0), U(e, 1), U(e, 2) != 0)); });
    api(e, "GetNextDlgGroupItem", 3, [](Emulator& e) { ret(e, next_group_item(gui(e), U(e, 0), U(e, 1), U(e, 2) != 0)); });
    api(e, "MessageBoxW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string text = A(e, 1) ? read_wstr(e, A(e, 1)) : u"";
        std::u16string cap = A(e, 2) ? read_wstr(e, A(e, 2)) : u"";
        ret(e, static_cast<uint64_t>(message_box(g, U(e, 0), text, cap, U(e, 3))));
    });
    api(e, "MessageBoxA", 4, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string text = A(e, 1) ? cp932_to_utf16(e.mem.read_cstring(A(e, 1))) : u"";
        std::u16string cap = A(e, 2) ? cp932_to_utf16(e.mem.read_cstring(A(e, 2))) : u"";
        ret(e, static_cast<uint64_t>(message_box(g, U(e, 0), text, cap, U(e, 3))));
    });
    api(e, "MessageBoxExW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string text = A(e, 1) ? read_wstr(e, A(e, 1)) : u"";
        std::u16string cap = A(e, 2) ? read_wstr(e, A(e, 2)) : u"";
        ret(e, static_cast<uint64_t>(message_box(g, U(e, 0), text, cap, U(e, 3))));
    });
    api(e, "MessageBoxIndirectW", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t p = A(e, 0);
        std::u16string text = e.mem.read32(p + 12) ? read_wstr(e, e.mem.read32(p + 12)) : u"";
        std::u16string cap = e.mem.read32(p + 16) ? read_wstr(e, e.mem.read32(p + 16)) : u"";
        ret(e, static_cast<uint64_t>(message_box(g, e.mem.read32(p + 4), text, cap, e.mem.read32(p + 20))));
    });
}

}  // namespace x86emu::win32
