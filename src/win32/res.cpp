// Resources the GUI loads: bitmaps, icons and cursors, strings; and the
// user32 drawing helpers that paint frames and edges.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "files.h"
#include "hookutil.h"
#include "user_internal.h"

namespace x86emu::win32 {

uint32_t bitmap_from_dib(Gui& g, uint64_t bmi, uint64_t bits, uint32_t usage, bool map3d);
bool bitmap_pixels(Gui& g, uint32_t hbmp, int& w, int& h, std::vector<uint32_t>& out, bool& mono);
uint32_t bitmap_from_pixels(Gui& g, int w, int h, const uint32_t* px);
uint32_t mono_bitmap_from_mask(Gui& g, int w, int h, const std::vector<uint8_t>& mask);

namespace {

// Decodes one icon/cursor image (a DIB with an AND mask, or a PNG which we
// cannot decode and replace with a blank) into ARGB.
bool decode_icon_image(Emulator& e, uint64_t p, uint32_t size, Cursor& c) {
    if (size >= 8 && e.mem.read32(p) == 0x474E5089) {  // PNG
        c.w = c.h = 32;
        c.color.assign(32 * 32, 0);
        return true;
    }
    uint32_t hs = e.mem.read32(p);
    if (hs < 40) return false;
    int w = static_cast<int32_t>(e.mem.read32(p + 4));
    int h2 = static_cast<int32_t>(e.mem.read32(p + 8));
    int bpp = e.mem.read16(p + 14);
    int h = h2 / 2;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return false;
    int ncol = 0;
    uint32_t used = e.mem.read32(p + 32);
    if (bpp <= 8) ncol = used ? static_cast<int>(used) : (1 << bpp);
    uint64_t pal = p + hs;
    uint64_t xorp = pal + static_cast<uint64_t>(ncol) * 4;
    int xstride = ((w * bpp + 31) / 32) * 4;
    uint64_t andp = xorp + static_cast<uint64_t>(xstride) * h;
    int astride = ((w + 31) / 32) * 4;
    c.w = w;
    c.h = h;
    c.color.assign(static_cast<size_t>(w) * h, 0);
    bool any_alpha = false;
    for (int y = 0; y < h; ++y) {
        int row = h - 1 - y;  // bottom-up
        for (int x = 0; x < w; ++x) {
            uint32_t rgb = 0, a = 255;
            uint64_t rp = xorp + static_cast<uint64_t>(row) * xstride;
            switch (bpp) {
                case 32: {
                    uint32_t v = e.mem.read32(rp + x * 4);
                    rgb = v & 0xFFFFFF;
                    a = v >> 24;
                    if (a) any_alpha = true;
                    break;
                }
                case 24:
                    rgb = e.mem.read8(rp + x * 3) | e.mem.read8(rp + x * 3 + 1) << 8 | e.mem.read8(rp + x * 3 + 2) << 16;
                    break;
                case 8: rgb = e.mem.read32(pal + e.mem.read8(rp + x) * 4) & 0xFFFFFF; break;
                case 4: {
                    uint8_t b = e.mem.read8(rp + x / 2);
                    rgb = e.mem.read32(pal + ((x & 1) ? (b & 0xF) : (b >> 4)) * 4) & 0xFFFFFF;
                    break;
                }
                case 1: {
                    uint8_t b = e.mem.read8(rp + x / 8);
                    rgb = e.mem.read32(pal + ((b >> (7 - (x & 7))) & 1) * 4) & 0xFFFFFF;
                    break;
                }
            }
            c.color[static_cast<size_t>(y) * w + x] = (a << 24) | rgb;
        }
    }
    if (bpp == 32 && any_alpha) return true;
    // Opacity from the AND mask: 1 = transparent (or, for a monochrome
    // cursor, "invert", which reads best as black on a light background).
    for (int y = 0; y < h; ++y) {
        int row = h - 1 - y;
        for (int x = 0; x < w; ++x) {
            uint8_t b = e.mem.read8(andp + static_cast<uint64_t>(row) * astride + x / 8);
            bool transparent = (b >> (7 - (x & 7))) & 1;
            uint32_t& v = c.color[static_cast<size_t>(y) * w + x];
            if (transparent) v = (v & 0xFFFFFF) ? 0xFF000000u : 0;
            else v |= 0xFF000000u;
        }
    }
    return true;
}

uint32_t new_cursor(Gui& g, std::unique_ptr<Cursor> c) {
    uint32_t h = g.new_user_handle(c->icon ? 0x0D : 0x0E);
    c->handle = h;
    g.cursors[h] = std::move(c);
    return h;
}

// The message box icons in the Windows 11 manner: a red disc with a white
// cross (IDI_HAND), blue discs with a white "?" or "i" (IDI_QUESTION,
// IDI_ASTERISK) and a yellow triangle with a black "!" (IDI_EXCLAMATION).
// Shapes are tested at 4x4 points per pixel for smooth edges.
void draw_message_icon(Cursor& c, int id) {
    auto inside_shape = [&](double x, double y) -> int {
        // 0 outside, 1 the base shape, 2 the mark on top of it.
        double cx = 16, cy = 16;
        if (id == 32515) {  // exclamation: a rounded triangle
            double top = 3, bottom = 29, half = 14;
            if (y < top || y > bottom) return 0;
            double w = half * (y - top) / (bottom - top);
            if (std::abs(x - cx) > w + 0.5) return 0;
            bool bar = std::abs(x - cx) <= 1.6 && y >= 11 && y <= 21;
            bool dot = (x - cx) * (x - cx) + (y - 25) * (y - 25) <= 2.4 * 2.4;
            return bar || dot ? 2 : 1;
        }
        double dx = x - cx, dy = y - cy;
        if (dx * dx + dy * dy > 14.5 * 14.5) return 0;
        if (id == 32513) {  // hand: a cross
            double u = (dx + dy) / 1.41421356, v = (dx - dy) / 1.41421356;
            bool cross = (std::abs(u) <= 1.6 && std::abs(v) <= 7.5) || (std::abs(v) <= 1.6 && std::abs(u) <= 7.5);
            return cross ? 2 : 1;
        }
        if (id == 32516) {  // asterisk: an "i"
            bool stem = std::abs(dx) <= 1.7 && dy >= -3 && dy <= 8;
            bool dot = dx * dx + (dy + 7.5) * (dy + 7.5) <= 2.2 * 2.2;
            return stem || dot ? 2 : 1;
        }
        // question: a "?" - an arc, a short stem and a dot
        double r = std::sqrt(dx * dx + (dy + 3.5) * (dy + 3.5));
        double ang = std::atan2(dy + 3.5, dx);
        bool arc = std::abs(r - 5) <= 1.7 && (ang < 1.1 || ang > 2.6 || dy + 3.5 < 0) && !(dx < 0 && dy + 3.5 > 0);
        bool stem = std::abs(dx) <= 1.7 && dy >= 1.5 && dy <= 4.5;
        bool dot = dx * dx + (dy - 8) * (dy - 8) <= 2.1 * 2.1;
        return arc || stem || dot ? 2 : 1;
    };
    uint32_t base = id == 32513 ? 0xD13438u : id == 32515 ? 0xFFC83Du : 0x0067C0u;
    uint32_t mark = id == 32515 ? 0x000000u : 0xFFFFFFu;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            int n_base = 0, n_mark = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    int k = inside_shape(x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0);
                    if (k == 1) ++n_base;
                    if (k == 2) ++n_mark;
                }
            int n = n_base + n_mark;
            if (!n) continue;
            auto ch = [&](int sh) {
                uint32_t v = (((base >> sh) & 0xFF) * static_cast<uint32_t>(n_base) + ((mark >> sh) & 0xFF) * static_cast<uint32_t>(n_mark)) /
                             static_cast<uint32_t>(n);
                return v << sh;
            };
            uint32_t a = static_cast<uint32_t>(n * 255 / 16);
            c.color[static_cast<size_t>(y) * 32 + x] = (a << 24) | ch(0) | ch(8) | ch(16);
        }
}

// Draws the stock cursors and icons the system would supply.
std::unique_ptr<Cursor> make_system_cursor(int id, bool icon) {
    auto c = std::make_unique<Cursor>();
    c->icon = icon;
    c->system_id = id;
    c->w = c->h = 32;
    c->color.assign(32 * 32, 0);
    auto px = [&](int x, int y, uint32_t v) {
        if (x >= 0 && y >= 0 && x < 32 && y < 32) c->color[static_cast<size_t>(y) * 32 + x] = v;
    };
    if (icon && id >= 32513 && id <= 32516) {
        draw_message_icon(*c, id);
        return c;
    }
    if (icon) {
        // A plain application icon: a framed window.
        for (int y = 4; y < 28; ++y)
            for (int x = 4; x < 28; ++x) px(x, y, (x == 4 || x == 27 || y == 4 || y == 27) ? 0xFF000080u : (y < 9 ? 0xFF0000C0u : 0xFFFFFFFFu));
        return c;
    }
    // Arrow: the outline of the classic pointer.
    for (int y = 0; y < 17; ++y)
        for (int x = 0; x <= y && x < 12; ++x) px(x, y, (x == 0 || x == y || y == 16) ? 0xFF000000u : 0xFFFFFFFFu);
    return c;
}

}  // namespace

uint32_t system_cursor(Gui& g, int idc) {
    for (auto& [h, c] : g.cursors)
        if (c->system_id == idc && !c->icon) return h;
    return new_cursor(g, make_system_cursor(idc, false));
}

uint32_t system_icon(Gui& g, int idi) {
    for (auto& [h, c] : g.cursors)
        if (c->system_id == idi && c->icon) return h;
    return new_cursor(g, make_system_cursor(idi, true));
}

uint32_t load_icon_or_cursor(Gui& g, uint64_t hinst, uint64_t name, bool icon, int cx, int cy) {
    Emulator& e = g.emu;
    uint64_t probe_addr = 0;
    uint32_t probe_size = 0;
    if (!hinst || (is_intresource(name) && name >= 32512 && name <= 32651 &&
                   !find_resource(e, hinst, icon ? 14 : 12, name, probe_addr, probe_size))) {
        return icon ? system_icon(g, static_cast<int>(name)) : system_cursor(g, static_cast<int>(name));
    }
    uint64_t gaddr;
    uint32_t gsize;
    if (!find_resource(e, hinst, icon ? 14 : 12, name, gaddr, gsize)) return 0;  // RT_GROUP_ICON / RT_GROUP_CURSOR
    int count = e.mem.read16(gaddr + 4);
    int want_w = cx ? cx : 32, want_h = cy ? cy : 32;
    int best = -1, best_score = 1 << 30;
    for (int i = 0; i < count; ++i) {
        uint64_t ent = gaddr + 6 + static_cast<uint64_t>(i) * 14;
        int w, h, bpp;
        if (icon) {
            w = e.mem.read8(ent);
            h = e.mem.read8(ent + 1);
            if (!w) w = 256;
            if (!h) h = 256;
            bpp = e.mem.read16(ent + 6);
        } else {
            w = e.mem.read16(ent);
            h = e.mem.read16(ent + 2) / 2;
            bpp = e.mem.read16(ent + 6);
        }
        int score = std::abs(w - want_w) * 100 + std::abs(h - want_h) * 100 + (32 - std::min(bpp, 32));
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best < 0) return 0;
    uint16_t id = e.mem.read16(gaddr + 6 + static_cast<uint64_t>(best) * 14 + 12);
    uint64_t addr;
    uint32_t size;
    if (!find_resource(e, hinst, icon ? 3 : 1, id, addr, size)) return 0;  // RT_ICON / RT_CURSOR
    auto c = std::make_unique<Cursor>();
    c->icon = icon;
    if (!icon) {
        c->hot_x = e.mem.read16(addr);
        c->hot_y = e.mem.read16(addr + 2);
        addr += 4;
        size -= 4;
    }
    if (!decode_icon_image(e, addr, size, *c)) return 0;
    // An application cursor has no stock id; the host draws it from pixels.
    c->system_id = 0;
    return new_cursor(g, std::move(c));
}

uint32_t load_bitmap_resource(Gui& g, uint64_t hinst, uint64_t name) {
    uint64_t addr;
    uint32_t size;
    if (!hinst || !find_resource(g.emu, hinst, 2, name, addr, size)) {
        // OBM_* system bitmaps: a small blank bitmap rather than nothing.
        if (is_intresource(name) && name >= 32734) {
            std::vector<uint32_t> px(16 * 16, g.sys_color(15));
            return bitmap_from_pixels(g, 16, 16, px.data());
        }
        return 0;
    }
    return bitmap_from_dib(g, addr, 0, 0, false);
}

std::u16string load_string(Gui& g, uint64_t hinst, uint32_t id) {
    Emulator& e = g.emu;
    uint64_t addr;
    uint32_t size;
    if (!find_resource(e, hinst, 6, (id >> 4) + 1, addr, size)) return u"";
    uint64_t p = addr;
    for (uint32_t i = 0; i < (id & 15); ++i) p += 2 + static_cast<uint64_t>(e.mem.read16(p)) * 2;
    uint32_t len = e.mem.read16(p);
    std::u16string s;
    for (uint32_t i = 0; i < len; ++i) s.push_back(static_cast<char16_t>(e.mem.read16(p + 2 + i * 2)));
    return s;
}

// ---------------------------------------------------------------------------
// Edges and frame controls
// ---------------------------------------------------------------------------

namespace draw {

void edge(Gui& g, uint32_t hdc, Rect r, uint32_t edge_flags, uint32_t flags) {
    // Which system colour each line takes, indexed by the edge's four BDR_
    // bits, as user32 has them (a lone inner edge sits in the outer column);
    // BF_SOFT swaps the top-left pair - a toolbar's buttons are drawn soft,
    // white outside and light grey inside.
    enum { HI = 20, LT = 22, SH = 16, DK = 21, NO = -1 };
    static const int lt_inner[16] = {NO, NO, NO, NO, NO, HI, HI, NO, NO, DK, DK, NO, NO, NO, NO, NO};
    static const int lt_outer[16] = {NO, LT, SH, NO, HI, LT, SH, NO, DK, LT, SH, NO, NO, LT, SH, NO};
    static const int rb_inner[16] = {NO, NO, NO, NO, NO, SH, SH, NO, NO, LT, LT, NO, NO, NO, NO, NO};
    static const int rb_outer[16] = {NO, DK, HI, NO, SH, DK, HI, NO, LT, DK, HI, NO, NO, DK, HI, NO};
    static const int lt_inner_soft[16] = {NO, NO, NO, NO, NO, LT, LT, NO, NO, SH, SH, NO, NO, NO, NO, NO};
    static const int lt_outer_soft[16] = {NO, HI, DK, NO, LT, HI, DK, NO, SH, HI, DK, NO, NO, HI, DK, NO};
    uint32_t i = edge_flags & 0xF;
    bool soft = (flags & 0x1000) != 0;  // BF_SOFT
    int lto = soft ? lt_outer_soft[i] : lt_outer[i], lti = soft ? lt_inner_soft[i] : lt_inner[i];
    int rbo = rb_outer[i], rbi = rb_inner[i];
    auto color = [&](int idx) { return idx < 0 ? -1 : static_cast<int64_t>(g.sys_color(idx)); };
    int64_t c_lto = color(lto), c_lti = color(lti), c_rbo = color(rbo), c_rbi = color(rbi);
    if (flags & 0x4000) {  // BF_MONO
        int64_t m = g.sys_color(6);
        c_lto = c_rbo = m;
        c_lti = c_rbi = -1;
    } else if (flags & 0x8000) {  // BF_FLAT
        int64_t f = g.sys_color(16);
        c_lto = c_rbo = f;
        c_lti = c_rbi = (c_lti < 0 ? -1 : g.sys_color(15));
    }
    auto pass = [&](int64_t tl, int64_t brc) {
        if (flags & 0x10) return;  // BF_DIAGONAL: not drawn
        if (tl >= 0) {
            if (flags & 0x2) fill_rect(g, hdc, {r.l, r.t, r.r, r.t + 1}, static_cast<uint32_t>(tl));  // BF_TOP
            if (flags & 0x1) fill_rect(g, hdc, {r.l, r.t, r.l + 1, r.b}, static_cast<uint32_t>(tl));  // BF_LEFT
        }
        if (brc >= 0) {
            if (flags & 0x8) fill_rect(g, hdc, {r.l, r.b - 1, r.r, r.b}, static_cast<uint32_t>(brc));  // BF_BOTTOM
            if (flags & 0x4) fill_rect(g, hdc, {r.r - 1, r.t, r.r, r.b}, static_cast<uint32_t>(brc));  // BF_RIGHT
        }
        if (tl >= 0 || brc >= 0) {
            if (flags & 0x1) r.l++;
            if (flags & 0x2) r.t++;
            if (flags & 0x4) r.r--;
            if (flags & 0x8) r.b--;
        }
    };
    pass(c_lto, c_rbo);
    pass(c_lti, c_rbi);
    if ((flags & 0x800) && !r.empty()) fill_rect(g, hdc, r, g.sys_color(15));  // BF_MIDDLE
}

void frame_control(Gui& g, uint32_t hdc, Rect r, uint32_t type, uint32_t state) {
    switch (type) {
        case 4: {  // DFC_BUTTON
            uint32_t kind = state & 0xF;
            bool pushed = (state & 0x200) != 0, checked = (state & 0x400) != 0,
                 inactive = (state & 0x100) != 0;
            if (state & 0x10) {  // DFCS_BUTTONPUSH (DFCS_BUTTONCHECK is 0)
                fill_rect(g, hdc, r, g.sys_color(15));
                edge(g, hdc, r, pushed || checked ? 0xA : 0x5, 0xF);
                return;
            }
            if (kind == 0 || kind == 8) {  // DFCS_BUTTONCHECK, DFCS_BUTTON3STATE
                // The classic box, as Windows still draws it for an unthemed
                // button: a sunken edge round a white square, cut off by the
                // control rather than shrunk when the control is shorter.
                Rect b{r.l, r.t, r.l + 13, r.t + 13};
                fill_rect(g, hdc, b, inactive || pushed ? g.sys_color(15) : g.sys_color(5));
                edge(g, hdc, b, 0xA, 0xF);  // EDGE_SUNKEN, BF_RECT
                if (checked) {
                    uint32_t c = inactive ? g.sys_color(17) : 0;
                    int cx = b.l + 6, cy = b.t + 6;
                    for (int i = 0; i < 3; ++i) fill_rect(g, hdc, {cx - 3 + i, cy - 1 + i, cx - 2 + i, cy + 2 + i}, c);
                    for (int i = 0; i < 4; ++i) fill_rect(g, hdc, {cx + i, cy + 1 - i, cx + i + 1, cy + 4 - i}, c);
                }
                return;
            }
            if (kind == 1 || kind == 2 || kind == 0x4 || (state & 0x4)) {  // radio
                // The classic 12x12 radio button, as measured on Windows:
                // a shadow / dark shadow ring top-left, highlight / 3D-light
                // bottom-right, a white middle and a 4x4 dot.
                static const char* ring[12] = {
                    "....aaaa....", "..aaddddaa..", ".addwwwwddh.", ".adwwwwwwlh.", "adwwwwwwwwlh", "adwwwwwwwwlh",
                    "adwwwwwwwwlh", "adwwwwwwwwlh", ".adwwwwwwlh.", ".allwwwwllh.", "..hhllllhh..", "....hhhh....",
                };
                static const char* dot[12] = {"", "", "", "", ".....##", "....####", "....####", ".....##", "", "", "", ""};
                uint32_t mid = inactive || pushed ? g.sys_color(15) : g.sys_color(5);
                uint32_t ink = inactive ? g.sys_color(17) : 0;
                for (int y = 0; y < 12; ++y)
                    for (int x = 0; x < 12; ++x) {
                        char k = ring[y][x];
                        if (k == '.') continue;
                        uint32_t c = k == 'a' ? g.sys_color(16) : k == 'd' ? g.sys_color(21) : k == 'h' ? g.sys_color(20)
                                   : k == 'l' ? g.sys_color(22) : mid;
                        if (checked && x < static_cast<int>(std::strlen(dot[y])) && dot[y][x] == '#') c = ink;
                        fill_rect(g, hdc, {r.l + x, r.t + y, r.l + x + 1, r.t + y + 1}, c);
                    }
                return;
            }
            fill_rect(g, hdc, r, g.sys_color(15));
            edge(g, hdc, r, pushed ? 0xA : 0x5, 0xF);
            return;
        }
        case 3: {  // DFC_SCROLL: arrows
            fill_rect(g, hdc, r, g.sys_color(15));
            edge(g, hdc, r, (state & 0x200) ? 0xA : 0x5, 0xF);
            uint32_t c = (state & 0x100) ? g.sys_color(17) : 0;
            int cx = (r.l + r.r) / 2, cy = (r.t + r.b) / 2;
            int kind = static_cast<int>(state & 0xF);
            for (int i = 0; i < 4; ++i) {
                switch (kind) {
                    case 0: fill_rect(g, hdc, {cx - i, cy - 2 + i, cx + i + 1, cy - 1 + i}, c); break;  // up
                    case 1: case 5: fill_rect(g, hdc, {cx - i, cy + 1 - i, cx + i + 1, cy + 2 - i}, c); break;  // down / combo
                    case 2: fill_rect(g, hdc, {cx - 2 + i, cy - i, cx - 1 + i, cy + i + 1}, c); break;  // left
                    case 3: fill_rect(g, hdc, {cx + 1 - i, cy - i, cx + 2 - i, cy + i + 1}, c); break;  // right
                }
            }
            return;
        }
        case 1:  // DFC_CAPTION
        case 2:  // DFC_MENU
        default:
            fill_rect(g, hdc, r, g.sys_color(15));
            edge(g, hdc, r, 0x5, 0xF);
            return;
    }
}

}  // namespace draw

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

void install_resources(Emulator& e) {
    api(e, "LoadStringW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string s = load_string(g, A(e, 0), U(e, 1));
        if (I(e, 3) == 0) {
            // A pointer to the read-only resource string itself.
            uint64_t addr;
            uint32_t size;
            if (find_resource(e, A(e, 0), 6, (U(e, 1) >> 4) + 1, addr, size)) {
                uint64_t p = addr;
                for (uint32_t i = 0; i < (U(e, 1) & 15); ++i) p += 2 + static_cast<uint64_t>(e.mem.read16(p)) * 2;
                e.mem.write32(A(e, 2), static_cast<uint32_t>(p + 2));
            }
            return ret(e, s.size());
        }
        if (s.empty()) {
            if (A(e, 2) && I(e, 3) > 0) e.mem.write16(A(e, 2), 0);
            return ret(e, 0);
        }
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 2), s, I(e, 3))));
    });
    api(e, "LoadStringA", 4, [](Emulator& e) {
        std::string a = utf16_to_cp932(load_string(gui(e), A(e, 0), U(e, 1)));
        int cap = I(e, 3);
        if (cap <= 0 || !A(e, 2)) return ret(e, 0);
        int n = std::min(static_cast<int>(a.size()), cap - 1);
        e.mem.write(A(e, 2), a.data(), static_cast<uint64_t>(n));
        e.mem.write8(A(e, 2) + n, 0);
        ret(e, static_cast<uint64_t>(n));
    });
    api(e, "LoadBitmapW", 2, [](Emulator& e) { ret(e, load_bitmap_resource(gui(e), A(e, 0), A(e, 1))); });
    e.alias_hook("LoadBitmapW", "LoadBitmapA");
    api(e, "LoadIconW", 2, [](Emulator& e) { ret(e, load_icon_or_cursor(gui(e), A(e, 0), A(e, 1), true, 32, 32)); });
    e.alias_hook("LoadIconW", "LoadIconA");
    api(e, "LoadCursorW", 2, [](Emulator& e) { ret(e, load_icon_or_cursor(gui(e), A(e, 0), A(e, 1), false, 32, 32)); });
    e.alias_hook("LoadCursorW", "LoadCursorA");
    api(e, "LoadCursorFromFileW", 1, [](Emulator& e) { ret(e, system_cursor(gui(e), 32512)); });
    api(e, "LoadImageW", 6, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t type = U(e, 2), flags = U(e, 5);
        if (flags & 0x10) {  // LR_LOADFROMFILE
            if (type == 0) {
                // A .bmp from disk: read it into guest memory and decode.
                std::string path = to_utf8(read_wstr(e, A(e, 1)));
                FILE* fp = std::fopen(FileTable::host_path(path).c_str(), "rb");
                if (!fp) return ret(e, 0);
                std::vector<uint8_t> data;
                uint8_t buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) data.insert(data.end(), buf, buf + n);
                std::fclose(fp);
                if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') return ret(e, 0);
                uint64_t mem = e.heap_alloc(data.size());
                e.mem.write(mem, data.data(), data.size());
                uint32_t off;
                std::memcpy(&off, data.data() + 10, 4);
                uint32_t hb = bitmap_from_dib(g, mem + 14, mem + off, 0, (flags & 0x1000) != 0);
                e.heap_free(mem);
                return ret(e, hb);
            }
            return ret(e, type == 2 ? system_cursor(g, 32512) : system_icon(g, 32512));
        }
        switch (type) {
            case 0: {  // IMAGE_BITMAP
                uint64_t addr;
                uint32_t size;
                if (!A(e, 0) || !find_resource(e, A(e, 0), 2, A(e, 1), addr, size))
                    return ret(e, load_bitmap_resource(g, A(e, 0), A(e, 1)));
                return ret(e, bitmap_from_dib(g, addr, 0, 0, (flags & 0x1000) != 0));
            }
            case 1:  // IMAGE_ICON
                return ret(e, load_icon_or_cursor(g, A(e, 0), A(e, 1), true, I(e, 3) ? I(e, 3) : ((flags & 0x40) ? 32 : 32), I(e, 4) ? I(e, 4) : 32));
            case 2:  // IMAGE_CURSOR
                return ret(e, load_icon_or_cursor(g, A(e, 0), A(e, 1), false, I(e, 3), I(e, 4)));
        }
        ret(e, 0);
    });
    e.alias_hook("LoadImageW", "LoadImageA");
    api(e, "CopyImage", 5, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t type = U(e, 1);
        if (type == 0) {
            int w, h;
            bool mono;
            std::vector<uint32_t> px;
            if (!bitmap_pixels(g, U(e, 0), w, h, px, mono)) return ret(e, 0);
            int nw = I(e, 2) ? I(e, 2) : w, nh = I(e, 3) ? I(e, 3) : h;
            std::vector<uint32_t> out(static_cast<size_t>(nw) * nh);
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) out[static_cast<size_t>(y) * nw + x] = px[static_cast<size_t>(y * h / nh) * w + x * w / nw];
            return ret(e, bitmap_from_pixels(g, nw, nh, out.data()));
        }
        auto it = g.cursors.find(U(e, 0));
        if (it == g.cursors.end()) return ret(e, 0);
        auto c = std::make_unique<Cursor>(*it->second);
        ret(e, new_cursor(g, std::move(c)));
    });
    api(e, "CopyIcon", 1, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.cursors.find(U(e, 0));
        if (it == g.cursors.end()) return ret(e, 0);
        auto c = std::make_unique<Cursor>(*it->second);
        ret(e, new_cursor(g, std::move(c)));
    });
    api(e, "DestroyIcon", 1, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.cursors.find(U(e, 0));
        if (it == g.cursors.end()) return ret(e, 0);
        if (!it->second->system_id) g.cursors.erase(it);
        ret(e, 1);
    });
    e.alias_hook("DestroyIcon", "DestroyCursor");
    api(e, "GetIconInfo", 2, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.cursors.find(U(e, 0));
        if (it == g.cursors.end()) return ret(e, 0);
        const Cursor& c = *it->second;
        uint64_t p = A(e, 1);
        e.mem.write32(p, c.icon ? 1 : 0);
        e.mem.write32(p + 4, static_cast<uint32_t>(c.hot_x));
        e.mem.write32(p + 8, static_cast<uint32_t>(c.hot_y));
        std::vector<uint8_t> mask(c.color.size());
        std::vector<uint32_t> rgb(c.color.size());
        for (size_t i = 0; i < c.color.size(); ++i) {
            mask[i] = (c.color[i] >> 24) < 128 ? 1 : 0;
            rgb[i] = mask[i] ? 0 : (c.color[i] & 0xFFFFFF);
        }
        e.mem.write32(p + 12, mono_bitmap_from_mask(g, c.w, c.h, mask));
        e.mem.write32(p + 16, bitmap_from_pixels(g, c.w, c.h, rgb.data()));
        ret(e, 1);
    });
    api(e, "CreateIconIndirect", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t p = A(e, 0);
        auto c = std::make_unique<Cursor>();
        c->icon = e.mem.read32(p) != 0;
        c->hot_x = static_cast<int32_t>(e.mem.read32(p + 4));
        c->hot_y = static_cast<int32_t>(e.mem.read32(p + 8));
        int w = 0, h = 0, mw = 0, mh = 0;
        bool mono = false, mmono = false;
        std::vector<uint32_t> color, mask;
        bool has_color = bitmap_pixels(g, e.mem.read32(p + 16), w, h, color, mono);
        bool has_mask = bitmap_pixels(g, e.mem.read32(p + 12), mw, mh, mask, mmono);
        if (!has_color && has_mask) {
            // Monochrome: the mask holds AND over XOR.
            w = mw;
            h = mh / 2;
            color.assign(static_cast<size_t>(w) * h, 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    bool a = (mask[static_cast<size_t>(y) * w + x] & 0xFFFFFF) != 0;
                    bool xr = (mask[static_cast<size_t>(y + h) * w + x] & 0xFFFFFF) != 0;
                    color[static_cast<size_t>(y) * w + x] = a ? (xr ? 0xFF000000u : 0) : (xr ? 0xFFFFFFFFu : 0xFF000000u);
                }
        } else if (has_color) {
            bool alpha = false;
            for (uint32_t v : color)
                if ((v >> 24) != 0 && (v >> 24) != 255) alpha = true;
            for (size_t i = 0; i < color.size(); ++i) {
                if (alpha) continue;
                bool transparent = has_mask && i < mask.size() && (mask[i] & 0xFFFFFF);
                color[i] = transparent ? 0 : (color[i] | 0xFF000000u);
            }
        }
        c->w = w;
        c->h = h;
        c->color = color;
        ret(e, new_cursor(g, std::move(c)));
    });
    api(e, "CreateIconFromResourceEx", 7, [](Emulator& e) {
        Gui& g = gui(e);
        auto c = std::make_unique<Cursor>();
        c->icon = U(e, 2) != 0;
        uint64_t p = A(e, 0);
        uint32_t size = U(e, 1);
        if (!c->icon) {
            c->hot_x = e.mem.read16(p);
            c->hot_y = e.mem.read16(p + 2);
            p += 4;
            size -= 4;
        }
        if (!decode_icon_image(e, p, size, *c)) return ret(e, 0);
        ret(e, new_cursor(g, std::move(c)));
    });
    api(e, "LookupIconIdFromDirectoryEx", 5, [](Emulator& e) {
        uint64_t p = A(e, 0);
        ret(e, e.mem.read16(p + 6 + 12));
    });
    api(e, "DrawIcon", 4, [](Emulator& e) {
        draw::draw_icon(gui(e), U(e, 0), I(e, 1), I(e, 2), U(e, 3), 0, 0);
        ret(e, 1);
    });
    api(e, "DrawIconEx", 9, [](Emulator& e) {
        Gui& g = gui(e);
        auto it = g.cursors.find(U(e, 3));
        if (it == g.cursors.end()) return ret(e, 0);
        int w = I(e, 4), h = I(e, 5);
        if (!w) w = (U(e, 8) & 0x8) ? it->second->w : it->second->w;  // DI_DEFAULTSIZE
        if (!h) h = it->second->h;
        if (A(e, 7)) draw::fill_rect_brush(g, U(e, 0), {I(e, 1), I(e, 2), I(e, 1) + w, I(e, 2) + h}, U(e, 7));
        draw::draw_icon(g, U(e, 0), I(e, 1), I(e, 2), U(e, 3), w, h);
        ret(e, 1);
    });
    api(e, "DrawEdge", 4, [](Emulator& e) {
        Gui& g = gui(e);
        Rect r = read_rect(e, A(e, 1));
        draw::edge(g, U(e, 0), r, U(e, 2), U(e, 3));
        if (U(e, 3) & 0x2000) {  // BF_ADJUST
            uint32_t ed = U(e, 2);
            int n = ((ed & 3) ? 1 : 0) + (((ed >> 2) & 3) ? 1 : 0);
            uint32_t f = U(e, 3);
            if (f & 1) r.l += n;
            if (f & 2) r.t += n;
            if (f & 4) r.r -= n;
            if (f & 8) r.b -= n;
            write_rect(e, A(e, 1), r);
        }
        ret(e, 1);
    });
    api(e, "DrawFrameControl", 4, [](Emulator& e) {
        draw::frame_control(gui(e), U(e, 0), read_rect(e, A(e, 1)), U(e, 2), U(e, 3));
        ret(e, 1);
    });
    api(e, "DrawStateW", 10, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t hdc = U(e, 0), flags = U(e, 9);
        uint32_t type = flags & 0xF;
        int x = I(e, 5), y = I(e, 6), cx = I(e, 7), cy = I(e, 8);
        bool disabled = (flags & 0x20) != 0;  // DSS_DISABLED
        DC* d = g.dc(hdc);
        if (!d) return ret(e, 0);
        switch (type) {
            case 1:    // DST_TEXT
            case 2: {  // DST_PREFIXTEXT
                std::u16string s = A(e, 3) ? read_wstr(e, A(e, 3), U(e, 4) ? static_cast<int>(U(e, 4)) : -1) : u"";
                Rect r{x, y, x + (cx ? cx : 10000), y + (cy ? cy : 10000)};
                uint32_t old = d->s.text_color;
                int oldbk = d->s.bk_mode;
                d->s.bk_mode = 1;
                if (disabled) {
                    d->s.text_color = g.sys_color(20);
                    Rect r2 = r.offset(1, 1);
                    draw_text_impl(g, hdc, s, r2, type == 1 ? 0x800 : 0);
                    d->s.text_color = g.sys_color(16);
                }
                draw_text_impl(g, hdc, s, r, type == 1 ? 0x800 : 0);
                d->s.text_color = old;
                d->s.bk_mode = oldbk;
                return ret(e, 1);
            }
            case 3:  // DST_ICON
                draw::draw_icon(g, hdc, x, y, U(e, 3), cx, cy);
                return ret(e, 1);
            case 4:  // DST_BITMAP
                draw::blit_bitmap(g, hdc, x, y, U(e, 3), 0, 0, cx, cy, 0x00CC0020);
                return ret(e, 1);
            case 0: {  // DST_COMPLEX: the callback draws
                if (A(e, 2)) e.call_guest(A(e, 2), {hdc, A(e, 3), A(e, 4), static_cast<uint64_t>(cx), static_cast<uint64_t>(cy)});
                return ret(e, 1);
            }
        }
        ret(e, 1);
    });
}

}  // namespace x86emu::win32
