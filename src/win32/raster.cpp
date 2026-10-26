#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace x86emu::win32 {

// ---------------------------------------------------------------------------
// PixelBuf
// ---------------------------------------------------------------------------

int PixelBuf::nearest_index(uint32_t rgb) const {
    int best = 0;
    long bestd = 1L << 30;
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    for (size_t i = 0; i < palette.size(); ++i) {
        uint32_t p = palette[i];
        long dr = r - static_cast<int>((p >> 16) & 0xFF);
        long dg = g - static_cast<int>((p >> 8) & 0xFF);
        long db = b - static_cast<int>(p & 0xFF);
        long d = dr * dr + dg * dg + db * db;
        if (d < bestd) {
            bestd = d;
            best = static_cast<int>(i);
            if (d == 0) break;
        }
    }
    return best;
}

static int mask_shift(uint32_t m) {
    int s = 0;
    if (!m) return 0;
    while (!(m & 1)) {
        m >>= 1;
        ++s;
    }
    return s;
}
static int mask_bits(uint32_t m) {
    int n = 0;
    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}
static uint32_t expand(uint32_t v, uint32_t mask) {
    int s = mask_shift(mask), n = mask_bits(mask);
    if (!n) return 0;
    uint32_t x = (v & mask) >> s;
    if (n >= 8) return x >> (n - 8);
    return (x << (8 - n)) | (x >> (2 * n - 8 > 0 ? 2 * n - 8 : 0));
}
static uint32_t compress(uint32_t c8, uint32_t mask) {
    int s = mask_shift(mask), n = mask_bits(mask);
    if (!n) return 0;
    uint32_t x = n >= 8 ? c8 << (n - 8) : c8 >> (8 - n);
    return (x << s) & mask;
}

uint32_t PixelBuf::get(int x, int y) const {
    const uint8_t* row = bits + static_cast<ptrdiff_t>(y) * stride;
    switch (bpp) {
        case 32: return reinterpret_cast<const uint32_t*>(row)[x] & 0xFFFFFF;
        case 24: {
            const uint8_t* p = row + x * 3;
            return static_cast<uint32_t>(p[0]) | p[1] << 8 | p[2] << 16;
        }
        case 16: {
            uint16_t v = reinterpret_cast<const uint16_t*>(row)[x];
            return expand(v, rmask) << 16 | expand(v, gmask) << 8 | expand(v, bmask);
        }
        case 8: {
            uint8_t i = row[x];
            return i < palette.size() ? palette[i] : 0;
        }
        case 4: {
            uint8_t v = row[x >> 1];
            uint8_t i = (x & 1) ? (v & 0xF) : (v >> 4);
            return i < palette.size() ? palette[i] : 0;
        }
        case 1: {
            uint8_t v = row[x >> 3];
            int i = (v >> (7 - (x & 7))) & 1;
            return static_cast<size_t>(i) < palette.size() ? palette[i] : (i ? 0xFFFFFF : 0);
        }
    }
    return 0;
}

void PixelBuf::put(int x, int y, uint32_t rgb) {
    uint8_t* row = bits + static_cast<ptrdiff_t>(y) * stride;
    switch (bpp) {
        case 32: reinterpret_cast<uint32_t*>(row)[x] = rgb & 0xFFFFFF; return;
        case 24: {
            uint8_t* p = row + x * 3;
            p[0] = rgb & 0xFF;
            p[1] = (rgb >> 8) & 0xFF;
            p[2] = (rgb >> 16) & 0xFF;
            return;
        }
        case 16:
            reinterpret_cast<uint16_t*>(row)[x] = static_cast<uint16_t>(
                compress((rgb >> 16) & 0xFF, rmask) | compress((rgb >> 8) & 0xFF, gmask) |
                compress(rgb & 0xFF, bmask));
            return;
        case 8: row[x] = static_cast<uint8_t>(nearest_index(rgb)); return;
        case 4: {
            uint8_t i = static_cast<uint8_t>(nearest_index(rgb));
            uint8_t& v = row[x >> 1];
            v = (x & 1) ? static_cast<uint8_t>((v & 0xF0) | i)
                        : static_cast<uint8_t>((v & 0x0F) | (i << 4));
            return;
        }
        case 1: {
            int i;
            if (palette.size() >= 2)
                i = nearest_index(rgb);
            else
                i = rgb ? 1 : 0;
            uint8_t& v = row[x >> 3];
            uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
            v = i ? static_cast<uint8_t>(v | bit) : static_cast<uint8_t>(v & ~bit);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Raster operations
// ---------------------------------------------------------------------------

uint32_t rop2_apply(int rop2, uint32_t p, uint32_t d) {
    switch (rop2) {
        case 13: return p;        // R2_COPYPEN
        case 7: return (p ^ d) & 0xFFFFFF;   // R2_XORPEN
        case 6: return ~d & 0xFFFFFF;        // R2_NOT
        case 11: return d;        // R2_NOP
        case 1: return 0;
        case 16: return 0xFFFFFF;
    }
    uint32_t t = static_cast<uint32_t>(rop2 - 1) & 0xF;
    uint32_t r = 0;
    if (t & 1) r |= ~p & ~d;
    if (t & 2) r |= ~p & d;
    if (t & 4) r |= p & ~d;
    if (t & 8) r |= p & d;
    return r & 0xFFFFFF;
}

uint32_t rop3_apply(uint32_t rop, uint32_t p, uint32_t s, uint32_t d) {
    uint32_t t = (rop >> 16) & 0xFF;
    switch (t) {
        case 0xCC: return s;
        case 0xF0: return p;
        case 0x00: return 0;
        case 0xFF: return 0xFFFFFF;
        case 0xAA: return d;
        case 0x66: return (s ^ d) & 0xFFFFFF;
        case 0x88: return s & d;
        case 0xEE: return (s | d) & 0xFFFFFF;
        case 0x55: return ~d & 0xFFFFFF;
        case 0x33: return ~s & 0xFFFFFF;
        case 0x5A: return (p ^ d) & 0xFFFFFF;
    }
    uint32_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (!(t & (1u << i))) continue;
        uint32_t m = 0xFFFFFFFF;
        m &= (i & 4) ? p : ~p;
        m &= (i & 2) ? s : ~s;
        m &= (i & 1) ? d : ~d;
        r |= m;
    }
    return r & 0xFFFFFF;
}

bool rop_uses_source(uint32_t rop) {
    uint32_t t = (rop >> 16) & 0xFF;
    return ((t >> 2) & 0x33) != (t & 0x33);
}
bool rop_uses_pattern(uint32_t rop) {
    uint32_t t = (rop >> 16) & 0xFF;
    return ((t >> 4) & 0x0F) != (t & 0x0F);
}
bool rop_uses_dest(uint32_t rop) {
    uint32_t t = (rop >> 16) & 0xFF;
    return ((t >> 1) & 0x55) != (t & 0x55);
}

const uint8_t* hatch_pattern(int style) {
    static const uint8_t kHatch[6][8] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},  // HS_HORIZONTAL
        {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08},  // HS_VERTICAL
        {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},  // HS_FDIAGONAL
        {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},  // HS_BDIAGONAL
        {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0xFF},  // HS_CROSS
        {0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81},  // HS_DIAGCROSS
    };
    if (style < 0 || style > 5) style = 0;
    return kHatch[style];
}

bool Paint::at(int x, int y, uint32_t& out) const {
    switch (kind) {
        case Null: return false;
        case Solid: out = color; return true;
        case Hatch: {
            int px = ((x - ox) % 8 + 8) % 8, py = ((y - oy) % 8 + 8) % 8;
            if (hatch_pattern(hatch)[py] & (0x80 >> px)) {
                out = color;
                return true;
            }
            if (!opaque) return false;
            out = bk;
            return true;
        }
        case Pattern: {
            if (!pat || pat->px.w <= 0 || pat->px.h <= 0) return false;
            int w = pat->px.w, h = pat->px.h;
            int px = ((x - ox) % w + w) % w, py = ((y - oy) % h + h) % h;
            uint32_t v = pat->px.get(px, py);
            if (pat->mono_ddb) v = v ? bkc : text;
            out = v;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------

void Target::set_clip(const Region& r) {
    clip = r;
    clip.intersect(Rect{0, 0, pb->w, pb->h});
    clip_box = clip.bounds();
    clip_is_rect = clip.empty() || clip.is_rect();
}

static uint32_t to_mono(uint32_t rgb) {
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (r * 30 + g * 59 + b * 11) / 100 >= 128 ? 0xFFFFFF : 0;
}

void Target::plot_unclipped(int x, int y, uint32_t rgb) {
    if (rop2 == 13 && pb->bpp == 32) {
        pb->row32(y)[x] = rgb;
        return;
    }
    if (mono) rgb = to_mono(rgb);
    uint32_t d = rop2 == 13 ? 0 : pb->get(x, y);
    pb->put(x, y, rop2_apply(rop2, rgb, d));
}

void Target::span_color(int x0, int x1, int y, uint32_t rgb) {
    if (x0 >= x1 || y < clip_box.t || y >= clip_box.b) return;
    auto run = [&](int a, int b) {
        a = std::max(a, clip_box.l);
        b = std::min(b, clip_box.r);
        if (a >= b) return;
        if (rop2 == 13 && pb->bpp == 32) {
            uint32_t* row = pb->row32(y);
            std::fill(row + a, row + b, rgb);
        } else {
            for (int x = a; x < b; ++x) plot_unclipped(x, y, rgb);
        }
    };
    if (clip_is_rect) {
        run(x0, x1);
        return;
    }
    const Region::Band* band = clip.band_at(y);
    if (!band) return;
    for (const auto& s : band->spans) {
        if (s.second <= x0) continue;
        if (s.first >= x1) break;
        run(std::max(x0, s.first), std::min(x1, s.second));
    }
}

void Target::span(int x0, int x1, int y, const Paint& p) {
    if (p.kind == Paint::Null) return;
    if (p.kind == Paint::Solid) {
        span_color(x0, x1, y, p.color);
        return;
    }
    if (x0 >= x1 || y < clip_box.t || y >= clip_box.b) return;
    auto run = [&](int a, int b) {
        a = std::max(a, clip_box.l);
        b = std::min(b, clip_box.r);
        for (int x = a; x < b; ++x) {
            uint32_t c;
            if (p.at(x, y, c)) plot_unclipped(x, y, c);
        }
    };
    if (clip_is_rect) {
        run(x0, x1);
        return;
    }
    const Region::Band* band = clip.band_at(y);
    if (!band) return;
    for (const auto& s : band->spans) {
        if (s.second <= x0) continue;
        if (s.first >= x1) break;
        run(std::max(x0, s.first), std::min(x1, s.second));
    }
}

void Target::fill_rect(const Rect& r, const Paint& p) {
    Rect c = r.intersect(clip_box);
    if (c.empty()) return;
    for (int y = c.t; y < c.b; ++y) span(c.l, c.r, y, p);
}

void Target::fill_region(const Region& r, const Paint& p) {
    for (const auto& band : r.bands()) {
        int y0 = std::max(band.top, clip_box.t), y1 = std::min(band.bottom, clip_box.b);
        for (int y = y0; y < y1; ++y)
            for (const auto& s : band.spans) span(s.first, s.second, y, p);
    }
}

// Dash patterns for cosmetic pens, in pixels on/off, as Windows NT draws them.
static const std::vector<int>& dash_pattern(int style) {
    static const std::vector<int> kDash{18, 6}, kDot{3, 3}, kDashDot{9, 6, 3, 6},
        kDashDotDot{9, 3, 3, 3, 3, 3}, kAlt{1, 1}, kNone{};
    switch (style & 0xF) {
        case 1: return kDash;
        case 2: return kDot;
        case 3: return kDashDot;
        case 4: return kDashDotDot;
        case 8: return kAlt;  // PS_ALTERNATE
    }
    return kNone;
}

void Target::line(int x0, int y0, int x1, int y1, uint32_t rgb, int style, uint32_t bk,
                  bool bk_opaque, int& phase, bool include_last) {
    const std::vector<int>& pat = dash_pattern(style);
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    bool xmaj = dx > dy;
    int mj = xmaj ? dx : dy, mn = xmaj ? dy : dx;
    // GDI's tie rule: the minor axis steps on a zero error only when it runs
    // backwards, which makes a line draw the same pixels from either end
    // (measured against LineTo; see jwcad_win_wasm src/draw.c stroke_at).
    bool tie = xmaj ? (sy < 0) : (sx < 0);
    int e = 2 * mn - mj;
    int n = include_last ? mj + 1 : mj;
    int period = 0;
    for (int v : pat) period += v;
    int x = x0, y = y0;
    // Quick reject when the whole line is outside the clip box.
    int lx = std::min(x0, x1), hx = std::max(x0, x1), ly = std::min(y0, y1), hy = std::max(y0, y1);
    bool off = hx < clip_box.l || lx >= clip_box.r || hy < clip_box.t || ly >= clip_box.b;
    for (int k = 0; k < n; ++k) {
        if (!off) {
            if (period == 0) {
                plot(x, y, rgb);
            } else {
                int pos = phase % period, acc = 0, idx = 0;
                while (idx < static_cast<int>(pat.size()) && pos >= acc + pat[idx]) acc += pat[idx++];
                bool on = (idx % 2) == 0;
                if (on)
                    plot(x, y, rgb);
                else if (bk_opaque)
                    plot(x, y, bk);
            }
        }
        if (period) phase = (phase + 1) % period;
        if (e > 0 || (e == 0 && tie)) {
            if (xmaj)
                y += sy;
            else
                x += sx;
            e -= 2 * mj;
        }
        e += 2 * mn;
        if (xmaj)
            x += sx;
        else
            y += sy;
    }
}

void Target::wide_line(int x0, int y0, int x1, int y1, uint32_t rgb, int width, int endcap) {
    // A geometric pen: the line's outline as a polygon (with round caps by
    // default, as CreatePen gives), filled with the pen colour.
    double dx = x1 - x0, dy = y1 - y0;
    double len = std::sqrt(dx * dx + dy * dy);
    double hw = width / 2.0;
    Region r;
    Paint p;
    p.kind = Paint::Solid;
    p.color = rgb;
    if (len < 1e-9) {
        if (endcap == 0) {
            int rr = width;
            Rect box{x0 - width / 2, y0 - width / 2, x0 - width / 2 + rr, y0 - width / 2 + rr};
            fill_region(Region::ellipse(box), p);
        }
        return;
    }
    double nx = -dy / len * hw, ny = dx / len * hw;
    double ex = 0, ey = 0;
    if (endcap == 0x100) {  // PS_ENDCAP_SQUARE
        ex = dx / len * hw;
        ey = dy / len * hw;
    }
    std::vector<Point> pts = {
        {static_cast<int>(std::lround(x0 + nx - ex + 0.5)), static_cast<int>(std::lround(y0 + ny - ey + 0.5))},
        {static_cast<int>(std::lround(x1 + nx + ex + 0.5)), static_cast<int>(std::lround(y1 + ny + ey + 0.5))},
        {static_cast<int>(std::lround(x1 - nx + ex + 0.5)), static_cast<int>(std::lround(y1 - ny + ey + 0.5))},
        {static_cast<int>(std::lround(x0 - nx - ex + 0.5)), static_cast<int>(std::lround(y0 - ny - ey + 0.5))},
    };
    r = Region::polygon(pts, {4}, true);
    if (endcap == 0) {  // PS_ENDCAP_ROUND
        int o = width / 2;
        r.unite(Region::ellipse(Rect{x0 - o, y0 - o, x0 - o + width, y0 - o + width}));
        r.unite(Region::ellipse(Rect{x1 - o, y1 - o, x1 - o + width, y1 - o + width}));
    }
    fill_region(r, p);
}

void Target::blend_mask(int x, int y, int w, int h, const uint8_t* alpha, int pitch,
                        uint32_t rgb) {
    Rect box = Rect{x, y, x + w, y + h}.intersect(clip_box);
    if (box.empty()) return;
    int cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
    for (int yy = box.t; yy < box.b; ++yy) {
        const uint8_t* a = alpha + static_cast<ptrdiff_t>(yy - y) * pitch;
        for (int xx = box.l; xx < box.r; ++xx) {
            int av = a[xx - x];
            if (!av) continue;
            if (!clip_is_rect && !clip.contains(xx, yy)) continue;
            if (mono || pb->bpp <= 8) {
                if (av >= 128) plot_unclipped(xx, yy, rgb);
                continue;
            }
            if (av >= 250) {
                pb->put(xx, yy, rgb);
                continue;
            }
            uint32_t d = pb->get(xx, yy);
            int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            dr += (cr - dr) * av / 255;
            dg += (cg - dg) * av / 255;
            db += (cb - db) * av / 255;
            pb->put(xx, yy, static_cast<uint32_t>(dr << 16 | dg << 8 | db));
        }
    }
}

// ---------------------------------------------------------------------------
// Blits
// ---------------------------------------------------------------------------

static uint32_t source_pixel(const BlitSource& s, int x, int y, bool dst_mono) {
    uint32_t v = s.pb->get(x, y);
    if (s.mono_ddb) v = v ? s.mono_bg : s.mono_fg;
    else if (dst_mono) v = (v == s.src_bk) ? 0xFFFFFF : 0;
    return v;
}

void blit(Target& dst, int dx, int dy, int dw, int dh, const BlitSource* src, int sx, int sy,
          int sw, int sh, uint32_t rop, const Paint* pattern, bool dst_mono_from_color) {
    bool use_s = rop_uses_source(rop) && src && src->pb;
    bool use_p = rop_uses_pattern(rop);
    bool use_d = rop_uses_dest(rop);
    // Normalise mirrored destinations into mirrored sources.
    bool flip_x = false, flip_y = false;
    if (dw < 0) {
        dx += dw + 1;
        dw = -dw;
        flip_x = !flip_x;
    }
    if (dh < 0) {
        dy += dh + 1;
        dh = -dh;
        flip_y = !flip_y;
    }
    if (use_s) {
        if (sw < 0) {
            sx += sw + 1;
            sw = -sw;
            flip_x = !flip_x;
        }
        if (sh < 0) {
            sy += sh + 1;
            sh = -sh;
            flip_y = !flip_y;
        }
    }
    if (dw == 0 || dh == 0) return;
    Rect box = Rect{dx, dy, dx + dw, dy + dh}.intersect(dst.clip_box);
    if (box.empty()) return;
    bool dst_mono = dst.mono;
    bool stretch = use_s && (sw != dw || sh != dh || flip_x || flip_y);

    // The common case, done in rows: SRCCOPY between 32-bit buffers.
    if (use_s && !stretch && (rop == 0x00CC0020) && dst.pb->bpp == 32 && src->pb->bpp == 32 &&
        !src->mono_ddb && !dst_mono) {
        for (const Rect& cr0 : dst.clip_is_rect ? std::vector<Rect>{dst.clip_box} : dst.clip.rects()) {
            Rect cr = cr0.intersect(box);
            if (cr.empty()) continue;
            for (int y = cr.t; y < cr.b; ++y) {
                int syy = sy + (y - dy);
                if (syy < 0 || syy >= src->pb->h) continue;
                int x0 = cr.l, x1 = cr.r;
                int sx0 = sx + (x0 - dx);
                if (sx0 < 0) {
                    x0 -= sx0;
                    sx0 = 0;
                }
                if (sx0 + (x1 - x0) > src->pb->w) x1 = x0 + (src->pb->w - sx0);
                if (x1 <= x0) continue;
                std::memmove(dst.pb->row32(y) + x0, src->pb->row32(syy) + sx0,
                             static_cast<size_t>(x1 - x0) * 4);
            }
        }
        return;
    }

    for (int y = box.t; y < box.b; ++y) {
        int ry = y - dy;
        int syy = 0;
        if (use_s) {
            if (stretch) {
                int t = static_cast<int>((static_cast<int64_t>(ry) * sh) / dh);
                syy = flip_y ? sy + sh - 1 - t : sy + t;
            } else {
                syy = sy + ry;
            }
            if (syy < 0 || syy >= src->pb->h) continue;
        }
        for (int x = box.l; x < box.r; ++x) {
            if (!dst.clip_is_rect && !dst.clip.contains(x, y)) continue;
            uint32_t s = 0, p = 0, d = 0;
            if (use_s) {
                int rx = x - dx;
                int sxx;
                if (stretch) {
                    int t = static_cast<int>((static_cast<int64_t>(rx) * sw) / dw);
                    sxx = flip_x ? sx + sw - 1 - t : sx + t;
                } else {
                    sxx = sx + rx;
                }
                if (sxx < 0 || sxx >= src->pb->w) continue;
                s = source_pixel(*src, sxx, syy, dst_mono_from_color || dst_mono);
            }
            if (use_p) {
                if (!pattern || !pattern->at(x, y, p)) p = 0;
                if (dst_mono) p = to_mono(p);
            }
            if (use_d) d = dst.pb->get(x, y);
            uint32_t r = rop3_apply(rop, p, s, d);
            if (dst_mono) r = to_mono(r);
            dst.pb->put(x, y, r);
        }
    }
}

void transparent_blit(Target& dst, int dx, int dy, int dw, int dh, const BlitSource& src,
                      int sx, int sy, int sw, int sh, uint32_t key) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    Rect box = Rect{dx, dy, dx + dw, dy + dh}.intersect(dst.clip_box);
    for (int y = box.t; y < box.b; ++y) {
        int syy = sy + static_cast<int>((static_cast<int64_t>(y - dy) * sh) / dh);
        if (syy < 0 || syy >= src.pb->h) continue;
        for (int x = box.l; x < box.r; ++x) {
            if (!dst.clip_is_rect && !dst.clip.contains(x, y)) continue;
            int sxx = sx + static_cast<int>((static_cast<int64_t>(x - dx) * sw) / dw);
            if (sxx < 0 || sxx >= src.pb->w) continue;
            uint32_t s = source_pixel(src, sxx, syy, false);
            if (s == key) continue;
            dst.pb->put(x, y, s);
        }
    }
}

void alpha_blit(Target& dst, int dx, int dy, int dw, int dh, const BlitSource& src, int sx,
                int sy, int sw, int sh, uint8_t ca, bool per_pixel) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    Rect box = Rect{dx, dy, dx + dw, dy + dh}.intersect(dst.clip_box);
    for (int y = box.t; y < box.b; ++y) {
        int syy = sy + static_cast<int>((static_cast<int64_t>(y - dy) * sh) / dh);
        if (syy < 0 || syy >= src.pb->h) continue;
        for (int x = box.l; x < box.r; ++x) {
            if (!dst.clip_is_rect && !dst.clip.contains(x, y)) continue;
            int sxx = sx + static_cast<int>((static_cast<int64_t>(x - dx) * sw) / dw);
            if (sxx < 0 || sxx >= src.pb->w) continue;
            uint32_t s;
            int a = ca;
            if (per_pixel && src.pb->bpp == 32) {
                uint32_t raw = src.pb->row32(syy)[sxx];
                s = raw & 0xFFFFFF;
                a = static_cast<int>((raw >> 24) * ca / 255);
                // premultiplied source: dst = src + dst * (1 - a)
                uint32_t d = dst.pb->get(x, y);
                int ia = 255 - a;
                auto ch = [&](int sh2) {
                    int sv = static_cast<int>((s >> sh2) & 0xFF) * ca / 255;
                    int dv = static_cast<int>((d >> sh2) & 0xFF);
                    return std::min(255, sv + dv * ia / 255);
                };
                dst.pb->put(x, y, static_cast<uint32_t>(ch(16) << 16 | ch(8) << 8 | ch(0)));
                continue;
            }
            s = source_pixel(src, sxx, syy, false);
            uint32_t d = dst.pb->get(x, y);
            auto mix = [&](int sh2) {
                int sv = static_cast<int>((s >> sh2) & 0xFF), dv = static_cast<int>((d >> sh2) & 0xFF);
                return dv + (sv - dv) * a / 255;
            };
            dst.pb->put(x, y, static_cast<uint32_t>(mix(16) << 16 | mix(8) << 8 | mix(0)));
        }
    }
}

void ellipse_outline(const Rect& r, std::vector<Point>& pixels) {
    // The pixels on the boundary of the filled ellipse region: every pixel of
    // the region that has a 4-neighbour outside it.
    Region reg = Region::ellipse(r);
    for (const auto& band : reg.bands()) {
        for (int y = band.top; y < band.bottom; ++y) {
            for (const auto& s : band.spans) {
                for (int x = s.first; x < s.second; ++x) {
                    if (!reg.contains(x - 1, y) || !reg.contains(x + 1, y) ||
                        !reg.contains(x, y - 1) || !reg.contains(x, y + 1))
                        pixels.push_back({x, y});
                }
            }
        }
    }
}

}  // namespace x86emu::win32
