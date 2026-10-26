// gdi32: objects, device contexts, and the drawing entry points.
//
// A DC draws into one of three things: a top-level window's surface (window
// DCs, offset to the window and clipped to its visible region), a bitmap
// (memory DCs), or nothing at all (information and printer DCs, which answer
// questions but paint nowhere).  Everything is converted to device pixels at
// the entry point and handed to raster.cpp.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "font.h"
#include "hookutil.h"
#include "raster.h"

namespace x86emu::win32 {

namespace {

constexpr int R2_COPYPEN = 13;
constexpr int OPAQUE_BK = 2;

// ---------------------------------------------------------------------------
// Resolving a DC to pixels
// ---------------------------------------------------------------------------

struct Resolved {
    PixelBuf pb;
    PixelBuf* ppb = nullptr;
    Point org;            // device (0,0) inside the buffer
    bool mono = false;
    Bitmap* bmp = nullptr;
};

Bitmap* dc_bitmap(Gui& g, DC* d) {
    GdiObject* o = g.obj(d->s.bitmap, ObjKind::Bitmap);
    return o ? o->bitmap.get() : nullptr;
}

bool resolve(Gui& g, DC* d, Resolved& r) {
    if (d->kind == DC::Kind::Printer) {
        if (!d->page) return false;  // outside StartPage/EndPage
        r.ppb = &d->page->px;
        r.bmp = d->page.get();
        r.org = {0, 0};
        return true;
    }
    if (d->kind == DC::Kind::Memory) {
        Bitmap* b = dc_bitmap(g, d);
        if (!b || !b->px.bits) return false;
        r.ppb = &b->px;
        r.bmp = b;
        r.mono = b->mono_ddb;
        r.org = {0, 0};
        return true;
    }
    if (d->kind == DC::Kind::Window) {
        Window* w = g.win(d->hwnd);
        if (!w) return false;
        if (w->hwnd == g.desktop) {
            r.pb.bits = reinterpret_cast<uint8_t*>(g.screen.data());
            r.pb.w = g.screen_w;
            r.pb.h = g.screen_h;
            r.pb.stride = g.screen_w * 4;
            r.pb.bpp = 32;
            r.ppb = &r.pb;
            r.org = {0, 0};
            return true;
        }
        Window* top = g.toplevel_of(w);
        if (!top) return false;
        g.ensure_surface(top);
        if (!top->surface || top->surf_w <= 0 || top->surf_h <= 0) return false;
        r.pb.bits = reinterpret_cast<uint8_t*>(top->surface->data());
        r.pb.w = top->surf_w;
        r.pb.h = top->surf_h;
        r.pb.stride = top->surf_w * 4;
        r.pb.bpp = 32;
        r.ppb = &r.pb;
        r.org = g.surface_origin(w, !d->whole_window);
        return true;
    }
    return false;
}

// The clip in buffer coordinates.
Region clip_region(Gui& g, DC* d, const Resolved& r) {
    Region clip;
    if (d->kind == DC::Kind::Window) {
        Window* w = g.win(d->hwnd);
        if (w && w->hwnd != g.desktop) {
            if (d->vis_ver != g.geom_ver) {
                d->vis_cache = g.visible_region(w, !d->whole_window);
                d->vis_ver = g.geom_ver;
            }
            clip = d->vis_cache;
        } else {
            clip.set_rect({0, 0, r.ppb->w, r.ppb->h});
        }
        if (d->has_paint_clip) clip.intersect(d->paint_clip);
    } else {
        clip.set_rect({0, 0, r.ppb->w, r.ppb->h});
    }
    if (d->s.has_clip) {
        Region c = d->s.clip;
        c.offset(r.org.x, r.org.y);
        clip.intersect(c);
    }
    return clip;
}

bool make_target(Gui& g, DC* d, Resolved& r, Target& t) {
    if (!resolve(g, d, r)) return false;
    t.pb = r.ppb;
    t.mono = r.mono;
    t.rop2 = d->s.rop2;
    t.set_clip(clip_region(g, d, r));
    return !t.clip.empty();
}

// ---------------------------------------------------------------------------
// Coordinates
// ---------------------------------------------------------------------------

bool world_identity(const DCState& s) {
    return s.graphics_mode != 2 || (s.world.m11 == 1 && s.world.m12 == 0 && s.world.m21 == 0 &&
                                    s.world.m22 == 1 && s.world.dx == 0 && s.world.dy == 0);
}

void lp_to_dp_d(const DCState& s, double x, double y, double& ox, double& oy) {
    if (!world_identity(s)) {
        double wx = x * s.world.m11 + y * s.world.m21 + s.world.dx;
        double wy = x * s.world.m12 + y * s.world.m22 + s.world.dy;
        x = wx;
        y = wy;
    }
    if (s.map_mode == 1) {
        ox = x - s.win_org.x + s.vp_org.x;
        oy = y - s.win_org.y + s.vp_org.y;
        return;
    }
    double wex = s.win_ext.x ? s.win_ext.x : 1, wey = s.win_ext.y ? s.win_ext.y : 1;
    ox = (x - s.win_org.x) * s.vp_ext.x / wex + s.vp_org.x;
    oy = (y - s.win_org.y) * s.vp_ext.y / wey + s.vp_org.y;
}

Point lp_to_dp(const DCState& s, int x, int y) {
    if (s.map_mode == 1 && world_identity(s))
        return {x - s.win_org.x + s.vp_org.x, y - s.win_org.y + s.vp_org.y};
    double ox, oy;
    lp_to_dp_d(s, x, y, ox, oy);
    return {static_cast<int>(std::lround(ox)), static_cast<int>(std::lround(oy))};
}

Point dp_to_lp(const DCState& s, int x, int y) {
    if (s.map_mode == 1 && world_identity(s))
        return {x - s.vp_org.x + s.win_org.x, y - s.vp_org.y + s.win_org.y};
    double vx = s.vp_ext.x ? s.vp_ext.x : 1, vy = s.vp_ext.y ? s.vp_ext.y : 1;
    double lx = (x - s.vp_org.x) * s.win_ext.x / vx + s.win_org.x;
    double ly = (y - s.vp_org.y) * s.win_ext.y / vy + s.win_org.y;
    if (!world_identity(s)) {
        const XForm& m = s.world;
        double det = m.m11 * m.m22 - m.m12 * m.m21;
        if (std::fabs(det) > 1e-12) {
            double tx = lx - m.dx, ty = ly - m.dy;
            double ix = (tx * m.m22 - ty * m.m21) / det;
            double iy = (ty * m.m11 - tx * m.m12) / det;
            lx = ix;
            ly = iy;
        }
    }
    return {static_cast<int>(std::lround(lx)), static_cast<int>(std::lround(ly))};
}

// A logical length along x, in device pixels (for pen widths).
int len_to_dp(const DCState& s, int len) {
    if (s.map_mode == 1 && world_identity(s)) return len;
    double ox0, oy0, ox1, oy1;
    lp_to_dp_d(s, 0, 0, ox0, oy0);
    lp_to_dp_d(s, len, 0, ox1, oy1);
    return static_cast<int>(std::lround(std::hypot(ox1 - ox0, oy1 - oy0)));
}

// A logical rectangle as device pixels, normalised.
Rect rect_to_dp(const DCState& s, int l, int t, int r, int b) {
    Point a = lp_to_dp(s, l, t), c = lp_to_dp(s, r, b);
    Rect out{std::min(a.x, c.x), std::min(a.y, c.y), std::max(a.x, c.x), std::max(a.y, c.y)};
    return out;
}

// Fixed mapping modes set both extents from the device size.
void apply_map_mode(Gui& g, DCState& s, int mode) {
    s.map_mode = mode;
    double mmx = g.screen_w * 25.4 / 96.0, mmy = g.screen_h * 25.4 / 96.0;
    auto set = [&](double per_mm) {
        s.win_ext = {static_cast<int>(std::lround(mmx * per_mm)), static_cast<int>(std::lround(mmy * per_mm))};
        s.vp_ext = {g.screen_w, -g.screen_h};
    };
    switch (mode) {
        case 1: s.win_ext = {1, 1}; s.vp_ext = {1, 1}; break;
        case 2: set(10); break;           // MM_LOMETRIC
        case 3: set(100); break;          // MM_HIMETRIC
        case 4: set(100 / 25.4); break;   // MM_LOENGLISH
        case 5: set(1000 / 25.4); break;  // MM_HIENGLISH
        case 6: set(1440 / 25.4); break;  // MM_TWIPS
        case 7:
        case 8:
            if (s.win_ext.x == 0) s.win_ext = {1, 1};
            break;
    }
}

void fix_isotropic(DCState& s) {
    if (s.map_mode != 7) return;
    if (!s.win_ext.x || !s.win_ext.y) return;
    double xr = std::fabs(static_cast<double>(s.vp_ext.x) / s.win_ext.x);
    double yr = std::fabs(static_cast<double>(s.vp_ext.y) / s.win_ext.y);
    if (xr > yr) {
        int sign = (s.vp_ext.x < 0) != (s.win_ext.x < 0) ? -1 : 1;
        s.vp_ext.x = sign * static_cast<int>(std::lround(std::fabs(s.win_ext.x) * yr));
    } else if (yr > xr) {
        int sign = (s.vp_ext.y < 0) != (s.win_ext.y < 0) ? -1 : 1;
        s.vp_ext.y = sign * static_cast<int>(std::lround(std::fabs(s.win_ext.y) * xr));
    }
}

// ---------------------------------------------------------------------------
// Pens and brushes as raster paints
// ---------------------------------------------------------------------------

Paint brush_paint(Gui& g, DC* d, uint32_t hbrush) {
    Paint p;
    GdiObject* b = g.obj(hbrush, ObjKind::Brush);
    if (!b) return p;
    p.ox = d->s.brush_org.x;
    p.oy = d->s.brush_org.y;
    p.bk = colorref_to_pixel(d->s.bk_color);
    p.opaque = d->s.bk_mode == OPAQUE_BK;
    switch (b->brush_style) {
        case 0:  // BS_SOLID
            p.kind = Paint::Solid;
            p.color = colorref_to_pixel(b->color);
            break;
        case 1:  // BS_NULL
            break;
        case 2:  // BS_HATCHED
            p.kind = Paint::Hatch;
            p.color = colorref_to_pixel(b->color);
            p.hatch = b->hatch;
            break;
        case 3:  // BS_PATTERN
        case 5:  // BS_DIBPATTERN
        case 6:  // BS_DIBPATTERNPT
            p.kind = Paint::Pattern;
            p.pat = b->pattern;
            p.text = colorref_to_pixel(d->s.text_color);
            p.bkc = colorref_to_pixel(d->s.bk_color);
            break;
    }
    return p;
}

struct PenInfo {
    bool null = false;
    int style = 0;
    int width = 1;
    uint32_t color = 0;
    int endcap = 0;
    bool geometric = false;
};

PenInfo pen_info(Gui& g, DC* d) {
    PenInfo pi;
    GdiObject* p = g.obj(d->s.pen);
    if (!p || (p->kind != ObjKind::Pen && p->kind != ObjKind::ExtPen)) {
        pi.null = true;
        return pi;
    }
    pi.style = p->pen_style & 0xF;
    pi.endcap = p->pen_style & 0xF00;
    pi.color = colorref_to_pixel(p->color);
    pi.geometric = (p->pen_style & 0x10000) != 0;  // PS_GEOMETRIC
    pi.width = p->pen_width <= 1 ? 1 : std::max(1, len_to_dp(d->s, p->pen_width));
    if (pi.style == 5) pi.null = true;  // PS_NULL
    if (pi.width > 1 && pi.style >= 1 && pi.style <= 4 && !pi.geometric) pi.style = 0;
    return pi;
}

// A polyline in device coordinates with the current pen.
void stroke_poly(Gui& g, DC* d, Target& t, const std::vector<Point>& pts, bool closed) {
    PenInfo pen = pen_info(g, d);
    if (pen.null || pts.empty()) return;
    int phase = 0;
    size_t n = pts.size();
    size_t segs = closed ? n : n - 1;
    if (n == 1) return;
    for (size_t i = 0; i < segs; ++i) {
        const Point& a = pts[i];
        const Point& b = pts[(i + 1) % n];
        if (pen.width <= 1) {
            t.line(a.x, a.y, b.x, b.y, pen.color, pen.style, colorref_to_pixel(d->s.bk_color),
                   d->s.bk_mode == OPAQUE_BK, phase);
        } else {
            t.wide_line(a.x, a.y, b.x, b.y, pen.color, pen.width, pen.endcap);
        }
    }
}

// Pixels of a region boundary, drawn with the pen (Ellipse/RoundRect outline).
void stroke_region_outline(Gui& g, DC* d, Target& t, const Region& reg) {
    PenInfo pen = pen_info(g, d);
    if (pen.null) return;
    Paint p;
    p.kind = Paint::Solid;
    p.color = pen.color;
    if (pen.width <= 1) {
        for (const auto& band : reg.bands()) {
            for (int y = band.top; y < band.bottom; ++y)
                for (const auto& s : band.spans)
                    for (int x = s.first; x < s.second; ++x)
                        if (!reg.contains(x - 1, y) || !reg.contains(x + 1, y) ||
                            !reg.contains(x, y - 1) || !reg.contains(x, y + 1))
                            t.plot(x, y, pen.color);
        }
        return;
    }
    // A wide pen: the region minus itself shrunk by the width.
    Region inner;
    int w = pen.width;
    for (const auto& band : reg.bands()) {
        for (int y = band.top; y < band.bottom; ++y) {
            if (!reg.band_at(y - w + 1) || !reg.band_at(y + w - 1)) continue;
            std::vector<std::pair<int, int>> spans;
            for (const auto& s : band.spans) {
                for (int x = s.first + w - 1; x < s.second - w + 1; ++x) {
                    if (reg.contains(x, y - w + 1) && reg.contains(x, y + w - 1)) {
                        if (!spans.empty() && spans.back().second == x)
                            spans.back().second = x + 1;
                        else
                            spans.push_back({x, x + 1});
                    }
                }
            }
            inner.push_band(y, y + 1, std::move(spans));
        }
    }
    Region ring = Region::combine(reg, inner, Region::Op::Diff);
    t.fill_region(ring, p);
}

// ---------------------------------------------------------------------------
// DIBs in guest memory
// ---------------------------------------------------------------------------

struct DibInfo {
    int w = 0, h = 0;  // h > 0: bottom-up
    int bpp = 0;
    uint32_t compression = 0;
    uint32_t header_size = 0;
    int colors_used = 0;
    std::vector<uint32_t> palette;
    uint32_t rmask = 0, gmask = 0, bmask = 0;
    int stride() const { return ((w * bpp + 31) / 32) * 4; }
};

bool read_dib_info(Emulator& e, Gui& g, DC* d, uint64_t bmi, uint32_t usage, DibInfo& di) {
    if (!bmi) return false;
    di.header_size = e.mem.read32(bmi);
    if (di.header_size == 12) {  // BITMAPCOREHEADER
        di.w = e.mem.read16(bmi + 4);
        di.h = static_cast<int16_t>(e.mem.read16(bmi + 6));
        di.bpp = e.mem.read16(bmi + 10);
    } else {
        di.w = static_cast<int32_t>(e.mem.read32(bmi + 4));
        di.h = static_cast<int32_t>(e.mem.read32(bmi + 8));
        di.bpp = e.mem.read16(bmi + 14);
        di.compression = e.mem.read32(bmi + 16);
        di.colors_used = static_cast<int>(e.mem.read32(bmi + 32));
    }
    if (di.w <= 0 || di.h == 0 || di.w > 65536 || std::abs(di.h) > 65536) return false;
    uint64_t colors = bmi + di.header_size;
    if (di.compression == 3 || di.compression == 6) {  // BI_BITFIELDS
        if (di.header_size >= 52) {
            di.rmask = e.mem.read32(bmi + 40);
            di.gmask = e.mem.read32(bmi + 44);
            di.bmask = e.mem.read32(bmi + 48);
        } else {
            di.rmask = e.mem.read32(colors);
            di.gmask = e.mem.read32(colors + 4);
            di.bmask = e.mem.read32(colors + 8);
            colors += 12;
        }
    }
    if (di.bpp <= 8) {
        int n = di.colors_used ? di.colors_used : (1 << di.bpp);
        if (n > 256) n = 256;
        di.palette.resize(static_cast<size_t>(n));
        GdiObject* pal = d ? g.obj(d->s.palette, ObjKind::Palette) : nullptr;
        for (int i = 0; i < n; ++i) {
            if (usage == 1) {  // DIB_PAL_COLORS
                uint16_t idx = e.mem.read16(colors + i * 2);
                uint32_t c = (pal && idx < pal->pal.size()) ? pal->pal[idx] : 0;
                di.palette[i] = colorref_to_pixel(c & 0xFFFFFF);
            } else if (di.header_size == 12) {
                uint8_t b = e.mem.read8(colors + i * 3), gg = e.mem.read8(colors + i * 3 + 1),
                        r = e.mem.read8(colors + i * 3 + 2);
                di.palette[i] = static_cast<uint32_t>(r) << 16 | gg << 8 | b;
            } else {
                di.palette[i] = e.mem.read32(colors + i * 4) & 0xFFFFFF;
            }
        }
    }
    return true;
}

// Decodes RLE8/RLE4 into a plain top-down 8bpp image.
void decode_rle(const uint8_t* src, size_t len, int w, int h, int bpp, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h, 0);
    int x = 0, y = 0;  // y counts from the bottom
    size_t i = 0;
    auto put = [&](int v) {
        if (x < w && y < h) out[static_cast<size_t>(h - 1 - y) * w + x] = static_cast<uint8_t>(v);
        ++x;
    };
    while (i + 1 < len) {
        uint8_t n = src[i], v = src[i + 1];
        i += 2;
        if (n) {
            for (int k = 0; k < n; ++k) put(bpp == 8 ? v : ((k & 1) ? (v & 0xF) : (v >> 4)));
            continue;
        }
        if (v == 0) {
            x = 0;
            ++y;
        } else if (v == 1) {
            break;
        } else if (v == 2) {
            if (i + 1 >= len) break;
            x += src[i];
            y += src[i + 1];
            i += 2;
        } else {
            int cnt = v;
            int bytes = bpp == 8 ? cnt : (cnt + 1) / 2;
            for (int k = 0; k < cnt && i + (bpp == 8 ? k : k / 2) < len; ++k) {
                uint8_t b = src[i + (bpp == 8 ? k : k / 2)];
                put(bpp == 8 ? b : ((k & 1) ? (b & 0xF) : (b >> 4)));
            }
            i += static_cast<size_t>((bytes + 1) & ~1);
        }
    }
}

// Copies the bits of a DIB out of guest memory into a host buffer and wraps
// them as a PixelBuf.  `lines` rows starting at scanline `start` (counted
// from the bottom for bottom-up DIBs) - SetDIBitsToDevice hands bands.
bool load_dib(Emulator& e, const DibInfo& di, uint64_t bits, PixelBuf& pb,
              std::vector<uint8_t>& store, int start = 0, int lines = -1) {
    int h = std::abs(di.h);
    if (lines < 0) lines = h;
    if (di.compression == 1 || di.compression == 2) {  // BI_RLE8 / BI_RLE4
        // The compressed size is not passed everywhere; read generously.
        size_t cap = static_cast<size_t>(di.w) * h + 4096;
        std::vector<uint8_t> raw(cap);
        for (size_t k = 0; k < cap; ++k) {
            if (!e.mem.is_mapped(bits + k)) {
                raw.resize(k);
                break;
            }
            raw[k] = e.mem.read8(bits + k);
        }
        decode_rle(raw.data(), raw.size(), di.w, h, di.compression == 1 ? 8 : 4, store);
        pb.bits = store.data();
        pb.w = di.w;
        pb.h = h;
        pb.stride = di.w;
        pb.bpp = 8;
        pb.palette = di.palette;
        return true;
    }
    int stride = di.stride();
    store.resize(static_cast<size_t>(stride) * h);
    std::fill(store.begin(), store.end(), 0);
    // Rows `start` .. `start+lines` in DIB order.
    for (int row = 0; row < lines && row + start < h; ++row) {
        uint64_t src = bits + static_cast<uint64_t>(row) * stride;
        size_t dst_row = static_cast<size_t>(row + start);
        e.mem.read(src, store.data() + dst_row * stride, static_cast<uint64_t>(stride));
    }
    pb.w = di.w;
    pb.h = h;
    pb.bpp = di.bpp;
    pb.palette = di.palette;
    if (di.bpp == 16) {
        if (di.rmask) {
            pb.rmask = di.rmask;
            pb.gmask = di.gmask;
            pb.bmask = di.bmask;
        }
    }
    if (di.h > 0) {  // bottom-up: row 0 (top) is the last one stored
        pb.bits = store.data() + static_cast<size_t>(stride) * (h - 1);
        pb.stride = -stride;
    } else {
        pb.bits = store.data();
        pb.stride = stride;
    }
    if (di.bpp == 32 && di.rmask && di.rmask != 0xFF0000) {
        // Unusual masks: convert to plain BGRX.
        PixelBuf src = pb;
        src.bpp = 16;  // reuse the mask path through a 32-bit read below
        for (int y = 0; y < h; ++y) {
            uint32_t* r = reinterpret_cast<uint32_t*>(pb.bits + static_cast<ptrdiff_t>(y) * pb.stride);
            for (int x = 0; x < di.w; ++x) {
                uint32_t v = r[x];
                auto ex = [&](uint32_t m) {
                    if (!m) return 0u;
                    int s = 0;
                    while (!((m >> s) & 1)) ++s;
                    int n = 0;
                    while ((m >> (s + n)) & 1) ++n;
                    uint32_t c = (v & m) >> s;
                    return n >= 8 ? c >> (n - 8) : c << (8 - n);
                };
                r[x] = ex(di.rmask) << 16 | ex(di.gmask) << 8 | ex(di.bmask);
            }
        }
    }
    return true;
}

uint32_t new_bitmap(Gui& g, int w, int h, bool mono) {
    auto o = std::make_unique<GdiObject>();
    auto b = std::make_shared<Bitmap>();
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    b->mono_ddb = mono;
    b->px.w = w;
    b->px.h = h;
    if (mono) {
        b->px.bpp = 1;
        b->px.stride = ((w + 15) / 16) * 2;
        b->px.palette = {0x000000, 0xFFFFFF};
    } else {
        b->px.bpp = 32;
        b->px.stride = w * 4;
    }
    b->host.assign(static_cast<size_t>(b->px.stride) * h, 0);
    b->px.bits = b->host.data();
    o->bitmap = b;
    return g.new_obj(ObjKind::Bitmap, std::move(o));
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

RealFont* dc_font(Gui& g, DC* d) {
    GdiObject* f = g.obj(d->s.font, ObjKind::Font);
    static LogFont sys;
    if (!f) {
        if (sys.face.empty()) {
            sys.face = u"MS UI Gothic";
            sys.height = -12;
        }
        return realize_font(sys);
    }
    if (d->s.map_mode != 1 && f->font.height) {
        LogFont lf = f->font;
        int hh = std::abs(lf.height);
        double ox0, oy0, ox1, oy1;
        lp_to_dp_d(d->s, 0, 0, ox0, oy0);
        lp_to_dp_d(d->s, 0, hh, ox1, oy1);
        int dh = static_cast<int>(std::lround(std::fabs(oy1 - oy0)));
        lf.height = lf.height < 0 ? -std::max(1, dh) : std::max(1, dh);
        if (lf.width) lf.width = std::max(1, len_to_dp(d->s, lf.width));
        return realize_font(lf);
    }
    return realize_font(f->font);
}

std::vector<uint32_t> to_codepoints(const std::u16string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        out.push_back(c);
    }
    return out;
}

int text_width(RealFont* f, const std::u16string& s, int extra) {
    int w = 0;
    for (uint32_t c : to_codepoints(s)) w += f->advance(c) + extra;
    return w;
}

// Draws one glyph rotated by `angle` radians around the pen point.
void draw_glyph_rotated(Target& t, const Glyph& gl, double px, double py, double ca, double sa,
                        uint32_t rgb) {
    if (gl.w <= 0 || gl.h <= 0) return;
    // Bounding box of the rotated bitmap.
    double xs[4], ys[4];
    double cx[4] = {double(gl.x0), double(gl.x0 + gl.w), double(gl.x0), double(gl.x0 + gl.w)};
    double cy[4] = {double(gl.y0), double(gl.y0), double(gl.y0 + gl.h), double(gl.y0 + gl.h)};
    for (int i = 0; i < 4; ++i) {
        // y grows downwards on screen; a positive escapement turns counter-clockwise.
        xs[i] = px + cx[i] * ca + cy[i] * sa;
        ys[i] = py - cx[i] * sa + cy[i] * ca;
    }
    int x0 = static_cast<int>(std::floor(*std::min_element(xs, xs + 4)));
    int x1 = static_cast<int>(std::ceil(*std::max_element(xs, xs + 4)));
    int y0 = static_cast<int>(std::floor(*std::min_element(ys, ys + 4)));
    int y1 = static_cast<int>(std::ceil(*std::max_element(ys, ys + 4)));
    int cr = (rgb >> 16) & 0xFF, cg = (rgb >> 8) & 0xFF, cb = rgb & 0xFF;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (!t.visible(x, y)) continue;
            double dx = x + 0.5 - px, dy = y + 0.5 - py;
            // inverse rotation
            double gx = dx * ca - dy * sa;
            double gy = dx * sa + dy * ca;
            int ix = static_cast<int>(std::floor(gx)) - gl.x0;
            int iy = static_cast<int>(std::floor(gy)) - gl.y0;
            if (ix < 0 || iy < 0 || ix >= gl.w || iy >= gl.h) continue;
            int a = gl.alpha[static_cast<size_t>(iy) * gl.w + ix];
            if (!a) continue;
            if (t.mono || t.pb->bpp <= 8) {
                if (a >= 128) t.plot_unclipped(x, y, rgb);
                continue;
            }
            uint32_t d = t.pb->get(x, y);
            int dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            dr += (cr - dr) * a / 255;
            dg += (cg - dg) * a / 255;
            db += (cb - db) * a / 255;
            t.pb->put(x, y, static_cast<uint32_t>(dr << 16 | dg << 8 | db));
        }
    }
}

// ExtTextOut in full.  `x, y` logical; `rect` logical (if has_rect).
bool text_out(Gui& g, DC* d, int x, int y, uint32_t options, const Rect* lrect,
              const std::u16string& str, const std::vector<int>* dx, bool dx_pairs) {
    RealFont* f = dc_font(g, d);
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) {
        // Still has to update the current position.
        if (d->s.text_align & 1) d->s.cur.x += text_width(f, str, d->s.char_extra);
        return true;
    }
    t.rop2 = R2_COPYPEN;
    Point p = (d->s.text_align & 1) ? lp_to_dp(d->s, d->s.cur.x, d->s.cur.y) : lp_to_dp(d->s, x, y);
    p.x += r.org.x;
    p.y += r.org.y;
    std::vector<uint32_t> cps = to_codepoints(str);
    std::vector<int> adv(cps.size());
    int total = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (dx && i < dx->size() / (dx_pairs ? 2 : 1))
            adv[i] = (*dx)[dx_pairs ? i * 2 : i];
        else
            adv[i] = f->advance(cps[i]) + d->s.char_extra;
        total += adv[i];
    }
    int dev_total = d->s.map_mode == 1 ? total : len_to_dp(d->s, total);
    if (dx && d->s.map_mode != 1)
        for (auto& a : adv) a = len_to_dp(d->s, a);
    double ang = f->escapement * 3.14159265358979 / 1800.0;
    double ca = std::cos(ang), sa = std::sin(ang);
    bool rotated = f->escapement % 3600 != 0;

    int align_h = d->s.text_align & 6;  // TA_LEFT 0, TA_RIGHT 2, TA_CENTER 6
    int align_v = d->s.text_align & 24; // TA_TOP 0, TA_BOTTOM 8, TA_BASELINE 24
    double shift_x = 0, shift_y = 0;     // in the text's own frame
    if (align_h == 2) shift_x = -dev_total;
    else if (align_h == 6) shift_x = -dev_total / 2.0;
    if (align_v == 0) shift_y = f->ascent;
    else if (align_v == 8) shift_y = -f->descent;
    // Baseline origin in buffer coordinates.
    double bx = p.x + shift_x * ca + shift_y * sa;
    double by = p.y - shift_x * sa + shift_y * ca;

    Region saved_clip;
    bool clipped = false;
    if (lrect && (options & 2)) {  // ETO_OPAQUE
        Rect dr = rect_to_dp(d->s, lrect->l, lrect->t, lrect->r, lrect->b).offset(r.org.x, r.org.y);
        Paint bg;
        bg.kind = Paint::Solid;
        bg.color = colorref_to_pixel(d->s.bk_color);
        t.fill_rect(dr, bg);
    }
    if (lrect && (options & 4)) {  // ETO_CLIPPED
        Rect dr = rect_to_dp(d->s, lrect->l, lrect->t, lrect->r, lrect->b).offset(r.org.x, r.org.y);
        Region c = t.clip;
        c.intersect(dr);
        t.set_clip(c);
        clipped = true;
    }
    (void)clipped;
    if (d->s.bk_mode == OPAQUE_BK && !(options & 2) && !rotated) {
        Rect box{static_cast<int>(std::lround(bx)), static_cast<int>(std::lround(by)) - f->ascent,
                 static_cast<int>(std::lround(bx)) + dev_total,
                 static_cast<int>(std::lround(by)) + f->descent};
        Paint bg;
        bg.kind = Paint::Solid;
        bg.color = colorref_to_pixel(d->s.bk_color);
        t.fill_rect(box, bg);
    }
    uint32_t col = colorref_to_pixel(d->s.text_color);
    double pen = 0;
    for (size_t i = 0; i < cps.size(); ++i) {
        const Glyph& gl = f->glyph(cps[i]);
        double gx = bx + pen * ca, gy = by - pen * sa;
        if (!rotated) {
            int ix = static_cast<int>(std::lround(gx)), iy = static_cast<int>(std::lround(gy));
            if (gl.w > 0) t.blend_mask(ix + gl.x0, iy + gl.y0, gl.w, gl.h, gl.alpha.data(), gl.w, col);
        } else {
            draw_glyph_rotated(t, gl, gx, gy, ca, sa, col);
        }
        pen += adv[i];
    }
    if ((f->underline || f->strikeout) && !rotated) {
        int ix = static_cast<int>(std::lround(bx)), iy = static_cast<int>(std::lround(by));
        int thick = std::max(1, f->height / 14);
        if (f->underline)
            for (int k = 0; k < thick; ++k) t.span_color(ix, ix + dev_total, iy + 1 + k, col);
        if (f->strikeout)
            for (int k = 0; k < thick; ++k)
                t.span_color(ix, ix + dev_total, iy - f->ascent / 3 + k, col);
    }
    if (d->s.text_align & 1) d->s.cur.x += total;
    return true;
}

// ---------------------------------------------------------------------------
// Object helpers
// ---------------------------------------------------------------------------

LogFont read_logfont(Emulator& e, uint64_t p) {
    LogFont lf;
    lf.height = static_cast<int32_t>(e.mem.read32(p));
    lf.width = static_cast<int32_t>(e.mem.read32(p + 4));
    lf.escapement = static_cast<int32_t>(e.mem.read32(p + 8));
    lf.orientation = static_cast<int32_t>(e.mem.read32(p + 12));
    lf.weight = static_cast<int32_t>(e.mem.read32(p + 16));
    lf.italic = e.mem.read8(p + 20);
    lf.underline = e.mem.read8(p + 21);
    lf.strikeout = e.mem.read8(p + 22);
    lf.charset = e.mem.read8(p + 23);
    lf.out_precision = e.mem.read8(p + 24);
    lf.clip_precision = e.mem.read8(p + 25);
    lf.quality = e.mem.read8(p + 26);
    lf.pitch_family = e.mem.read8(p + 27);
    lf.face = read_wstr(e, p + 28, 32);
    return lf;
}

void write_logfont(Emulator& e, uint64_t p, const LogFont& lf) {
    e.mem.write32(p, static_cast<uint32_t>(lf.height));
    e.mem.write32(p + 4, static_cast<uint32_t>(lf.width));
    e.mem.write32(p + 8, static_cast<uint32_t>(lf.escapement));
    e.mem.write32(p + 12, static_cast<uint32_t>(lf.orientation));
    e.mem.write32(p + 16, static_cast<uint32_t>(lf.weight));
    e.mem.write8(p + 20, lf.italic);
    e.mem.write8(p + 21, lf.underline);
    e.mem.write8(p + 22, lf.strikeout);
    e.mem.write8(p + 23, lf.charset);
    e.mem.write8(p + 24, lf.out_precision);
    e.mem.write8(p + 25, lf.clip_precision);
    e.mem.write8(p + 26, lf.quality);
    e.mem.write8(p + 27, lf.pitch_family);
    for (int i = 0; i < 32; ++i)
        e.mem.write16(p + 28 + i * 2, i < static_cast<int>(lf.face.size()) ? lf.face[i] : 0);
}

void write_textmetric(Emulator& e, uint64_t p, RealFont* f) {
    e.mem.write32(p + 0, static_cast<uint32_t>(f->height));
    e.mem.write32(p + 4, static_cast<uint32_t>(f->ascent));
    e.mem.write32(p + 8, static_cast<uint32_t>(f->descent));
    e.mem.write32(p + 12, static_cast<uint32_t>(f->internal_leading));
    e.mem.write32(p + 16, static_cast<uint32_t>(f->external_leading));
    e.mem.write32(p + 20, static_cast<uint32_t>(f->ave_width));
    e.mem.write32(p + 24, static_cast<uint32_t>(f->max_width));
    e.mem.write32(p + 28, static_cast<uint32_t>(f->weight));
    e.mem.write32(p + 32, 0);
    e.mem.write32(p + 36, 96);
    e.mem.write32(p + 40, 96);
    e.mem.write16(p + 44, 0x20);
    e.mem.write16(p + 46, 0xFFFC);
    e.mem.write16(p + 48, 0x1F);
    e.mem.write16(p + 50, 0x20);
    e.mem.write8(p + 52, f->italic ? 255 : 0);
    e.mem.write8(p + 53, f->underline ? 255 : 0);
    e.mem.write8(p + 54, f->strikeout ? 255 : 0);
    // TMPF_FIXED_PITCH *set* means variable pitch; TMPF_TRUETYPE.
    uint8_t pf = static_cast<uint8_t>((f->monospace_ja ? 0 : 1) | 0x04 | 0x02 |
                                      (f->monospace_ja ? 0x30 : 0x20));
    e.mem.write8(p + 55, pf);
    e.mem.write8(p + 56, f->charset);
}

int region_complexity(const Region& r) {
    if (r.empty()) return 1;  // NULLREGION
    return r.is_rect() ? 2 : 3;
}

// Logical points to device coordinates, offset into the buffer.
std::vector<Point> read_points_dp(Emulator& e, DC* d, uint64_t p, int n, Point org) {
    std::vector<Point> out;
    out.reserve(static_cast<size_t>(std::max(0, n)));
    for (int i = 0; i < n; ++i) {
        Point lp = read_point(e, p + static_cast<uint64_t>(i) * 8);
        Point dp = lp_to_dp(d->s, lp.x, lp.y);
        out.push_back({dp.x + org.x, dp.y + org.y});
    }
    return out;
}

// Points along an elliptical arc from angle a0 sweeping by `sweep`
// (radians, positive counter-clockwise on screen), on the ellipse inscribed
// in the device rectangle [l, r] x [t, b] (both edges included, the way GDI
// fits an arc).
void arc_points(double l, double t, double r, double b, double a0, double sweep,
                std::vector<Point>& out) {
    double cx = (l + r) / 2.0, cy = (t + b) / 2.0;
    double rx = (r - l) / 2.0, ry = (b - t) / 2.0;
    double circ = std::fabs(sweep) * std::max(rx, ry);
    int n = std::max(4, static_cast<int>(circ / 2.0));
    for (int i = 0; i <= n; ++i) {
        double a = a0 + sweep * i / n;
        out.push_back({static_cast<int>(std::lround(cx + rx * std::cos(a))),
                       static_cast<int>(std::lround(cy - ry * std::sin(a)))});
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Gui methods that belong to GDI
// ---------------------------------------------------------------------------

uint32_t Gui::new_obj(ObjKind kind, std::unique_ptr<GdiObject> o) {
    o->kind = kind;
    uint32_t h = (static_cast<uint32_t>(kind) << 24) | ((next_obj++ & 0xFFFFF) << 2);
    objs[h] = std::move(o);
    return h;
}

GdiObject* Gui::obj(uint64_t h, ObjKind kind) const {
    auto it = objs.find(static_cast<uint32_t>(h));
    if (it == objs.end()) return nullptr;
    if (kind != ObjKind::None && it->second->kind != kind) {
        if (!(kind == ObjKind::Pen && it->second->kind == ObjKind::ExtPen)) return nullptr;
    }
    return it->second.get();
}

ObjKind Gui::kind_of(uint64_t h) const {
    if (dcs.count(static_cast<uint32_t>(h))) return ObjKind::DC;
    GdiObject* o = obj(h);
    return o ? o->kind : ObjKind::None;
}

uint32_t Gui::create_dc(DC::Kind kind, uint32_t hwnd, bool whole_window) {
    auto d = std::make_unique<DC>();
    d->kind = kind;
    d->hwnd = hwnd;
    d->whole_window = whole_window;
    d->s.pen = stock[7];     // BLACK_PEN
    d->s.brush = stock[0];   // WHITE_BRUSH
    d->s.font = stock[13];   // SYSTEM_FONT
    d->s.palette = stock[15];
    if (kind == DC::Kind::Memory) {
        d->default_bitmap = stock[19 - 19] ? 0 : 0;
        // Every memory DC starts with the same 1x1 monochrome bitmap selected.
        static uint32_t mono1 = 0;
        if (!mono1 || !obj(mono1, ObjKind::Bitmap)) {
            mono1 = new_bitmap(*this, 1, 1, true);
            objs[mono1]->stock = true;
        }
        d->default_bitmap = mono1;
        d->s.bitmap = mono1;
    }
    uint32_t h = (static_cast<uint32_t>(ObjKind::DC) << 24) | ((next_obj++ & 0xFFFFF) << 2);
    dcs[h] = std::move(d);
    return h;
}

void Gui::delete_dc(uint32_t hdc) {
    DC* d = dc(hdc);
    if (!d) return;
    if (GdiObject* b = obj(d->s.bitmap, ObjKind::Bitmap))
        if (b->bitmap && b->bitmap->selected_into == hdc) b->bitmap->selected_into = 0;
    dcs.erase(hdc);
}

uint32_t Gui::sys_color(int idx) const {
    // Windows 10 defaults.
    static const uint32_t kColors[31] = {
        0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464,
        0x000000, 0x000000, 0x000000, 0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800,
        0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000, 0xFFFFFF,
        0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9,
        0xF2E4D7, 0xD77800, 0xF0F0F0,
    };
    if (idx < 0 || idx >= 31) return 0;
    return kColors[idx];
}

// ---------------------------------------------------------------------------
// Drawing helpers for host code (controls, non-client areas)
// ---------------------------------------------------------------------------

namespace draw {

void fill_rect(Gui& g, uint32_t hdc, const Rect& rc, uint32_t colorref) {
    DC* d = g.dc(hdc);
    if (!d) return;
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) return;
    t.rop2 = R2_COPYPEN;
    Paint p;
    p.kind = Paint::Solid;
    p.color = colorref_to_pixel(colorref);
    t.fill_rect(rect_to_dp(d->s, rc.l, rc.t, rc.r, rc.b).offset(r.org.x, r.org.y), p);
}

void fill_rect_brush(Gui& g, uint32_t hdc, const Rect& rc, uint32_t hbrush) {
    DC* d = g.dc(hdc);
    if (!d) return;
    if (hbrush > 0 && hbrush <= 31) hbrush = g.sys_color_brush[hbrush - 1];
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) return;
    t.rop2 = R2_COPYPEN;
    Paint p = brush_paint(g, d, hbrush);
    t.fill_rect(rect_to_dp(d->s, rc.l, rc.t, rc.r, rc.b).offset(r.org.x, r.org.y), p);
}

void frame_rect(Gui& g, uint32_t hdc, const Rect& rc, uint32_t colorref) {
    if (rc.empty()) return;
    fill_rect(g, hdc, {rc.l, rc.t, rc.r, rc.t + 1}, colorref);
    fill_rect(g, hdc, {rc.l, rc.b - 1, rc.r, rc.b}, colorref);
    fill_rect(g, hdc, {rc.l, rc.t, rc.l + 1, rc.b}, colorref);
    fill_rect(g, hdc, {rc.r - 1, rc.t, rc.r, rc.b}, colorref);
}

void line(Gui& g, uint32_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref) {
    DC* d = g.dc(hdc);
    if (!d) return;
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) return;
    t.rop2 = R2_COPYPEN;
    Point a = lp_to_dp(d->s, x0, y0), b = lp_to_dp(d->s, x1, y1);
    int phase = 0;
    t.line(a.x + r.org.x, a.y + r.org.y, b.x + r.org.x, b.y + r.org.y, colorref_to_pixel(colorref),
           0, 0, false, phase);
}

void focus_rect(Gui& g, uint32_t hdc, const Rect& rc) {
    DC* d = g.dc(hdc);
    if (!d) return;
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) return;
    t.rop2 = 6;  // R2_NOT, as DrawFocusRect does (it is an XOR of a dotted pattern)
    Rect dr = rect_to_dp(d->s, rc.l, rc.t, rc.r, rc.b).offset(r.org.x, r.org.y);
    for (int x = dr.l; x < dr.r; ++x) {
        if (((x - dr.l) & 1) == 0) {
            t.plot(x, dr.t, 0);
            t.plot(x, dr.b - 1, 0);
        }
    }
    for (int y = dr.t + 1; y < dr.b - 1; ++y) {
        if (((y - dr.t) & 1) == 0) {
            t.plot(dr.l, y, 0);
            t.plot(dr.r - 1, y, 0);
        }
    }
}

void text(Gui& g, uint32_t hdc, int x, int y, const std::u16string& s, uint32_t colorref, int bk) {
    DC* d = g.dc(hdc);
    if (!d) return;
    DCState save = d->s;
    d->s.text_color = colorref;
    d->s.text_align = 0;
    if (bk >= 0) {
        d->s.bk_mode = OPAQUE_BK;
        d->s.bk_color = static_cast<uint32_t>(bk);
    } else {
        d->s.bk_mode = 1;
    }
    text_out(g, d, x, y, 0, nullptr, s, nullptr, false);
    d->s = save;
}

Point text_extent(Gui& g, uint32_t hdc, const std::u16string& s) {
    DC* d = g.dc(hdc);
    if (!d) return {};
    RealFont* f = dc_font(g, d);
    return {text_width(f, s, d->s.char_extra), f->height};
}

void select_font(Gui& g, uint32_t hdc, uint32_t font) {
    DC* d = g.dc(hdc);
    if (d && g.obj(font, ObjKind::Font)) d->s.font = font;
}

int font_height(Gui& g, uint32_t font) {
    GdiObject* f = g.obj(font, ObjKind::Font);
    if (!f) return 16;
    return realize_font(f->font)->height;
}

void blit_bitmap(Gui& g, uint32_t hdc, int x, int y, uint32_t hbmp, int sx, int sy, int w,
                 int h, uint32_t rop) {
    DC* d = g.dc(hdc);
    GdiObject* bo = g.obj(hbmp, ObjKind::Bitmap);
    if (!d || !bo || !bo->bitmap) return;
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) return;
    BlitSource src;
    src.pb = &bo->bitmap->px;
    src.mono_ddb = bo->bitmap->mono_ddb;
    src.mono_fg = colorref_to_pixel(d->s.text_color);
    src.mono_bg = colorref_to_pixel(d->s.bk_color);
    Point p = lp_to_dp(d->s, x, y);
    Paint pat = brush_paint(g, d, d->s.brush);
    blit(t, p.x + r.org.x, p.y + r.org.y, w, h, &src, sx, sy, w, h, rop, &pat, false);
}

void draw_icon(Gui& g, uint32_t hdc, int x, int y, uint32_t hicon, int w, int h) {
    DC* d = g.dc(hdc);
    auto it = g.cursors.find(hicon);
    if (!d || it == g.cursors.end()) return;
    const Cursor& c = *it->second;
    if (c.color.empty() || c.w <= 0 || c.h <= 0) return;
    if (w <= 0) w = c.w;
    if (h <= 0) h = c.h;
    Resolved r;
    Target t;
    if (!make_target(g, d, r, t)) return;
    Point p = lp_to_dp(d->s, x, y);
    int ox = p.x + r.org.x, oy = p.y + r.org.y;
    for (int yy = 0; yy < h; ++yy) {
        int sy = yy * c.h / h;
        for (int xx = 0; xx < w; ++xx) {
            int sx = xx * c.w / w;
            uint32_t v = c.color[static_cast<size_t>(sy) * c.w + sx];
            uint32_t a = v >> 24;
            if (!a) continue;
            int dx = ox + xx, dy = oy + yy;
            if (!t.visible(dx, dy)) continue;
            uint32_t rgb = v & 0xFFFFFF;
            if (a >= 255) {
                t.pb->put(dx, dy, rgb);
                continue;
            }
            uint32_t dst = t.pb->get(dx, dy);
            auto mix = [&](int sh) {
                int s2 = static_cast<int>((rgb >> sh) & 0xFF), d2 = static_cast<int>((dst >> sh) & 0xFF);
                return static_cast<uint32_t>(d2 + (s2 - d2) * static_cast<int>(a) / 255);
            };
            t.pb->put(dx, dy, mix(16) << 16 | mix(8) << 8 | mix(0));
        }
    }
}

}  // namespace draw

// The pixels of a bitmap as 0xAARRGGBB (alpha 255), for icon construction.
bool bitmap_pixels(Gui& g, uint32_t hbmp, int& w, int& h, std::vector<uint32_t>& out, bool& mono) {
    GdiObject* o = g.obj(hbmp, ObjKind::Bitmap);
    if (!o || !o->bitmap) return false;
    Bitmap& b = *o->bitmap;
    w = b.px.w;
    h = b.px.h;
    mono = b.mono_ddb || b.px.bpp == 1;
    out.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t v = b.px.get(x, y);
            if (b.px.bpp == 32 && b.dib) v = reinterpret_cast<uint32_t*>(b.px.bits + static_cast<ptrdiff_t>(y) * b.px.stride)[x];
            else v |= 0xFF000000u;
            out[static_cast<size_t>(y) * w + x] = v;
        }
    return true;
}

// A 32-bit DDB from 0x00RRGGBB pixels.
uint32_t bitmap_from_pixels(Gui& g, int w, int h, const uint32_t* px) {
    uint32_t hb = new_bitmap(g, w, h, false);
    Bitmap& b = *g.obj(hb)->bitmap;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) b.px.row32(y)[x] = px[static_cast<size_t>(y) * w + x] & 0xFFFFFF;
    return hb;
}

uint32_t mono_bitmap_from_mask(Gui& g, int w, int h, const std::vector<uint8_t>& mask) {
    uint32_t hb = new_bitmap(g, w, h, true);
    Bitmap& b = *g.obj(hb)->bitmap;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) b.px.put(x, y, mask[static_cast<size_t>(y) * w + x] ? 0xFFFFFF : 0);
    return hb;
}

// A DIB in guest memory (a resource) as a 32-bit DDB.
uint32_t bitmap_from_dib(Gui& g, uint64_t bmi, uint64_t bits, uint32_t usage, bool map3d) {
    Emulator& e = g.emu;
    DibInfo di;
    if (!read_dib_info(e, g, nullptr, bmi, usage, di)) return 0;
    if (map3d && di.bpp <= 8) {
        for (auto& c : di.palette) {
            if (c == 0x808080) c = colorref_to_pixel(g.sys_color(16));
            else if (c == 0xC0C0C0) c = colorref_to_pixel(g.sys_color(15));
            else if (c == 0xDFDFDF || c == 0xE0E0E0) c = colorref_to_pixel(g.sys_color(22));
        }
    }
    PixelBuf src;
    std::vector<uint8_t> store;
    if (!bits) {
        int ncol = di.bpp <= 8 ? static_cast<int>(di.palette.size()) : 0;
        bits = bmi + di.header_size + static_cast<uint64_t>(ncol) * (usage == 1 ? 2 : 4);
        if (di.compression == 3 && di.header_size == 40) bits += 12;
    }
    if (!load_dib(e, di, bits, src, store)) return 0;
    if (di.bpp == 1 && !map3d && false) {
        uint32_t hb = new_bitmap(g, src.w, src.h, true);
        Bitmap& b = *g.obj(hb)->bitmap;
        for (int y = 0; y < src.h; ++y)
            for (int x = 0; x < src.w; ++x) b.px.put(x, y, src.get(x, y));
        return hb;
    }
    uint32_t hb = new_bitmap(g, src.w, src.h, false);
    Bitmap& b = *g.obj(hb)->bitmap;
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) b.px.row32(y)[x] = src.get(x, y);
    return hb;
}

uint32_t text_metrics_height(Gui& g, uint32_t font) { return static_cast<uint32_t>(draw::font_height(g, font)); }

uint32_t default_gui_font(Gui& g) { return g.stock[17]; }

Rect dc_clip_box(Gui& g, uint32_t hdc, int* complexity) {
    DC* d = g.dc(hdc);
    if (!d) return {};
    Resolved r;
    Region c;
    if (resolve(g, d, r)) {
        c = clip_region(g, d, r);
        c.offset(-r.org.x, -r.org.y);
    } else {
        c.set_rect({0, 0, g.screen_w, g.screen_h});
    }
    if (complexity) *complexity = region_complexity(c);
    Rect b = c.bounds();
    Point a = dp_to_lp(d->s, b.l, b.t), z = dp_to_lp(d->s, b.r, b.b);
    return {std::min(a.x, z.x), std::min(a.y, z.y), std::max(a.x, z.x), std::max(a.y, z.y)};
}

// Exposed for user32 (DrawText and friends live there but need the font).
RealFont* dc_realfont(Gui& g, uint32_t hdc) {
    DC* d = g.dc(hdc);
    if (!d) return realize_font(LogFont{});
    return dc_font(g, d);
}

bool dc_text_out(Gui& g, uint32_t hdc, int x, int y, uint32_t options, const Rect* rc,
                 const std::u16string& s) {
    DC* d = g.dc(hdc);
    if (!d) return false;
    return text_out(g, d, x, y, options, rc, s, nullptr, false);
}

// ---------------------------------------------------------------------------
// The hooks
// ---------------------------------------------------------------------------

void install_gdi(Emulator& e) {
    Gui& g = gui(e);

    // Stock objects.
    auto mk_brush = [&](uint32_t color, int style) {
        auto o = std::make_unique<GdiObject>();
        o->brush_style = style;
        o->color = color;
        o->stock = true;
        return g.new_obj(ObjKind::Brush, std::move(o));
    };
    auto mk_pen = [&](uint32_t color, int style) {
        auto o = std::make_unique<GdiObject>();
        o->pen_style = style;
        o->color = color;
        o->stock = true;
        return g.new_obj(ObjKind::Pen, std::move(o));
    };
    auto mk_font = [&](const char16_t* face, int h, int weight) {
        auto o = std::make_unique<GdiObject>();
        o->font.face = face;
        o->font.height = h;
        o->font.weight = weight;
        o->font.charset = 128;
        o->stock = true;
        return g.new_obj(ObjKind::Font, std::move(o));
    };
    g.stock[0] = mk_brush(0xFFFFFF, 0);
    g.stock[1] = mk_brush(0xC0C0C0, 0);
    g.stock[2] = mk_brush(0x808080, 0);
    g.stock[3] = mk_brush(0x404040, 0);
    g.stock[4] = mk_brush(0x000000, 0);
    g.stock[5] = mk_brush(0, 1);
    g.stock[6] = mk_pen(0xFFFFFF, 0);
    g.stock[7] = mk_pen(0x000000, 0);
    g.stock[8] = mk_pen(0, 5);
    g.stock[10] = mk_font(u"Terminal", 12, 400);
    g.stock[11] = mk_font(u"Courier New", 13, 400);
    g.stock[12] = mk_font(u"MS UI Gothic", 12, 400);
    g.stock[13] = mk_font(u"System", 16, 700);
    g.stock[14] = g.stock[13];
    {
        auto o = std::make_unique<GdiObject>();
        o->stock = true;
        for (int i = 0; i < 20; ++i) o->pal.push_back(0);
        g.stock[15] = g.new_obj(ObjKind::Palette, std::move(o));
    }
    g.stock[16] = mk_font(u"FixedSys", 16, 400);
    g.stock[17] = mk_font(u"MS UI Gothic", -12, 400);
    g.ui_font = mk_font(u"Yu Gothic UI", -12, 400);
    g.stock[18] = mk_brush(0xFFFFFF, 0);  // DC_BRUSH
    g.stock[19] = mk_pen(0x000000, 0);    // DC_PEN
    for (int i = 0; i < 31; ++i) g.sys_color_brush[i] = mk_brush(g.sys_color(i), 0);

    api(e, "GetStockObject", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t i = U(e, 0);
        ret(e, i < 20 ? g.stock[i] : 0);
    });

    // ---- creating objects ----------------------------------------------------
    api(e, "CreatePen", 3, [](Emulator& e) {
        auto o = std::make_unique<GdiObject>();
        o->pen_style = I(e, 0);
        o->pen_width = I(e, 1);
        o->color = U(e, 2) & 0xFFFFFF;
        ret(e, gui(e).new_obj(ObjKind::Pen, std::move(o)));
    });
    api(e, "CreatePenIndirect", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        auto o = std::make_unique<GdiObject>();
        o->pen_style = static_cast<int32_t>(e.mem.read32(p));
        o->pen_width = static_cast<int32_t>(e.mem.read32(p + 4));
        o->color = e.mem.read32(p + 12) & 0xFFFFFF;
        ret(e, gui(e).new_obj(ObjKind::Pen, std::move(o)));
    });
    api(e, "ExtCreatePen", 5, [](Emulator& e) {
        uint64_t lb = A(e, 2);
        auto o = std::make_unique<GdiObject>();
        o->pen_style = I(e, 0);
        o->pen_width = I(e, 1);
        o->color = lb ? (e.mem.read32(lb + 4) & 0xFFFFFF) : 0;
        if ((o->pen_style & 0xF) == 7 && A(e, 4)) {  // PS_USERSTYLE
            for (uint32_t i = 0; i < U(e, 3); ++i) o->pen_dashes.push_back(e.mem.read32(A(e, 4) + i * 4));
        }
        ret(e, gui(e).new_obj(ObjKind::ExtPen, std::move(o)));
    });
    api(e, "CreateSolidBrush", 1, [](Emulator& e) {
        auto o = std::make_unique<GdiObject>();
        o->color = U(e, 0) & 0xFFFFFF;
        ret(e, gui(e).new_obj(ObjKind::Brush, std::move(o)));
    });
    api(e, "CreateHatchBrush", 2, [](Emulator& e) {
        auto o = std::make_unique<GdiObject>();
        o->brush_style = 2;
        o->hatch = I(e, 0);
        o->color = U(e, 1) & 0xFFFFFF;
        ret(e, gui(e).new_obj(ObjKind::Brush, std::move(o)));
    });
    api(e, "CreatePatternBrush", 1, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* b = g.obj(A(e, 0), ObjKind::Bitmap);
        if (!b) return ret(e, 0);
        auto o = std::make_unique<GdiObject>();
        o->brush_style = 3;
        auto copy = std::make_shared<Bitmap>(*b->bitmap);
        if (copy->dib) {
            // Snapshot the DIB's pixels into host memory.
            copy->host.resize(static_cast<size_t>(std::abs(copy->px.stride)) * copy->px.h);
            for (int y = 0; y < copy->px.h; ++y)
                std::memcpy(copy->host.data() + static_cast<size_t>(y) * std::abs(copy->px.stride),
                            b->bitmap->px.bits + static_cast<ptrdiff_t>(y) * b->bitmap->px.stride,
                            static_cast<size_t>(std::abs(copy->px.stride)));
            copy->px.stride = std::abs(copy->px.stride);
            copy->dib = false;
        }
        copy->px.bits = copy->host.data();
        o->pattern = copy;
        ret(e, g.new_obj(ObjKind::Brush, std::move(o)));
    });
    api(e, "CreateDIBPatternBrushPt", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DibInfo di;
        if (!read_dib_info(e, g, nullptr, A(e, 0), U(e, 1), di)) return ret(e, 0);
        int ncol = di.bpp <= 8 ? static_cast<int>(di.palette.size()) : 0;
        uint64_t bits = A(e, 0) + di.header_size + ncol * (U(e, 1) == 1 ? 2 : 4);
        auto bmp = std::make_shared<Bitmap>();
        load_dib(e, di, bits, bmp->px, bmp->host);
        auto o = std::make_unique<GdiObject>();
        o->brush_style = 6;
        o->pattern = bmp;
        ret(e, g.new_obj(ObjKind::Brush, std::move(o)));
    });
    api(e, "CreateBrushIndirect", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        auto o = std::make_unique<GdiObject>();
        o->brush_style = static_cast<int>(e.mem.read32(p));
        o->color = e.mem.read32(p + 4) & 0xFFFFFF;
        o->hatch = static_cast<int>(e.mem.read32(p + 8));
        ret(e, gui(e).new_obj(ObjKind::Brush, std::move(o)));
    });
    api(e, "CreateFontIndirectW", 1, [](Emulator& e) {
        auto o = std::make_unique<GdiObject>();
        o->font = read_logfont(e, A(e, 0));
        ret(e, gui(e).new_obj(ObjKind::Font, std::move(o)));
    });
    api(e, "CreateFontW", 14, [](Emulator& e) {
        auto o = std::make_unique<GdiObject>();
        LogFont& lf = o->font;
        lf.height = I(e, 0);
        lf.width = I(e, 1);
        lf.escapement = I(e, 2);
        lf.orientation = I(e, 3);
        lf.weight = I(e, 4);
        lf.italic = static_cast<uint8_t>(U(e, 5));
        lf.underline = static_cast<uint8_t>(U(e, 6));
        lf.strikeout = static_cast<uint8_t>(U(e, 7));
        lf.charset = static_cast<uint8_t>(U(e, 8));
        lf.out_precision = static_cast<uint8_t>(U(e, 9));
        lf.clip_precision = static_cast<uint8_t>(U(e, 10));
        lf.quality = static_cast<uint8_t>(U(e, 11));
        lf.pitch_family = static_cast<uint8_t>(U(e, 12));
        if (A(e, 13)) lf.face = read_wstr(e, A(e, 13), 32);
        ret(e, gui(e).new_obj(ObjKind::Font, std::move(o)));
    });
    api(e, "CreateBitmap", 5, [](Emulator& e) {
        Gui& g = gui(e);
        int w = I(e, 0), h = I(e, 1), bpp = I(e, 3) * I(e, 2);
        uint32_t hb = new_bitmap(g, w, h, bpp == 1);
        uint64_t bits = A(e, 4);
        if (bits) {
            Bitmap& b = *g.obj(hb)->bitmap;
            int src_stride = ((w * bpp + 15) / 16) * 2;
            if (bpp == 1) {
                for (int y = 0; y < h; ++y)
                    e.mem.read(bits + static_cast<uint64_t>(y) * src_stride,
                               b.host.data() + static_cast<size_t>(y) * b.px.stride,
                               static_cast<uint64_t>(std::min(src_stride, b.px.stride)));
            } else if (bpp == 32) {
                for (int y = 0; y < h; ++y)
                    e.mem.read(bits + static_cast<uint64_t>(y) * src_stride,
                               b.host.data() + static_cast<size_t>(y) * b.px.stride,
                               static_cast<uint64_t>(w * 4));
            } else if (bpp == 24 || bpp == 16 || bpp == 8 || bpp == 4) {
                PixelBuf src;
                std::vector<uint8_t> store(static_cast<size_t>(src_stride) * h);
                e.mem.read(bits, store.data(), store.size());
                src.bits = store.data();
                src.w = w;
                src.h = h;
                src.stride = src_stride;
                src.bpp = bpp;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) b.px.put(x, y, src.get(x, y));
            }
        }
        ret(e, hb);
    });
    api(e, "CreateBitmapIndirect", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        int w = static_cast<int32_t>(e.mem.read32(p + 4)), h = static_cast<int32_t>(e.mem.read32(p + 8));
        int bpp = e.mem.read16(p + 18) * e.mem.read16(p + 16);
        ret(e, new_bitmap(gui(e), w, h, bpp == 1));
    });
    api(e, "CreateCompatibleBitmap", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        bool mono = false;
        if (d && d->kind == DC::Kind::Memory) {
            Bitmap* b = dc_bitmap(g, d);
            mono = b && b->mono_ddb;
        }
        int w = I(e, 1), h = I(e, 2);
        if (w <= 0 || h <= 0) {
            // A zero-sized request gets the stock 1x1 monochrome bitmap.
            w = h = 1;
            mono = true;
        }
        ret(e, new_bitmap(g, w, h, mono));
    });
    api(e, "CreateDiscardableBitmap", 3, [](Emulator& e) {
        ret(e, new_bitmap(gui(e), I(e, 1), I(e, 2), false));
    });
    api(e, "CreateDIBSection", 6, [](Emulator& e) {
        Gui& g = gui(e);
        DibInfo di;
        if (!read_dib_info(e, g, g.dc(A(e, 0)), A(e, 1), U(e, 2), di)) return ret(e, 0);
        int h = std::abs(di.h);
        size_t bytes = static_cast<size_t>(di.stride()) * h;
        uint64_t addr = e.alloc_pages((bytes + 0xFFF) & ~0xFFFull, 0x1000, "DIB section");
        if (!addr) return ret(e, 0);
        e.mem.map_contiguous(addr, (bytes + 0xFFF) & ~0xFFFull);
        auto o = std::make_unique<GdiObject>();
        auto b = std::make_shared<Bitmap>();
        b->dib = true;
        b->guest_bits = addr;
        uint8_t* host = e.mem.host_span(addr, bytes);
        b->px.w = di.w;
        b->px.h = h;
        b->px.bpp = di.bpp;
        b->px.palette = di.palette;
        if (di.bpp == 16 && di.rmask) {
            b->px.rmask = di.rmask;
            b->px.gmask = di.gmask;
            b->px.bmask = di.bmask;
        }
        if (di.h > 0) {
            b->px.bits = host + static_cast<size_t>(di.stride()) * (h - 1);
            b->px.stride = -di.stride();
        } else {
            b->px.bits = host;
            b->px.stride = di.stride();
        }
        b->dib_info.resize(40);
        e.mem.read(A(e, 1), b->dib_info.data(), 40);
        o->bitmap = b;
        if (A(e, 3)) e.mem.write32(A(e, 3), static_cast<uint32_t>(addr));
        ret(e, g.new_obj(ObjKind::Bitmap, std::move(o)));
    });
    api(e, "CreateDIBitmap", 6, [](Emulator& e) {
        Gui& g = gui(e);
        uint64_t hdr = A(e, 1);
        int w = static_cast<int32_t>(e.mem.read32(hdr + 4));
        int h = std::abs(static_cast<int32_t>(e.mem.read32(hdr + 8)));
        int bpp = e.mem.read16(hdr + 14);
        DC* d = g.dc(A(e, 0));
        bool mono = bpp == 1 && false;
        (void)d;
        uint32_t hb = new_bitmap(g, w, h, mono);
        if ((U(e, 2) & 4) && A(e, 3) && A(e, 4)) {  // CBM_INIT
            DibInfo di;
            if (read_dib_info(e, g, d, A(e, 4), U(e, 5), di)) {
                PixelBuf src;
                std::vector<uint8_t> store;
                if (load_dib(e, di, A(e, 3), src, store)) {
                    Bitmap& b = *g.obj(hb)->bitmap;
                    for (int y = 0; y < std::min(h, src.h); ++y)
                        for (int x = 0; x < std::min(w, src.w); ++x) b.px.put(x, y, src.get(x, y));
                }
            }
        }
        ret(e, hb);
    });
    api(e, "CreatePalette", 1, [](Emulator& e) {
        uint64_t p = A(e, 0);
        auto o = std::make_unique<GdiObject>();
        int n = e.mem.read16(p + 2);
        for (int i = 0; i < n; ++i) o->pal.push_back(e.mem.read32(p + 4 + i * 4));
        ret(e, gui(e).new_obj(ObjKind::Palette, std::move(o)));
    });
    api(e, "CreateHalftonePalette", 1, [](Emulator& e) {
        auto o = std::make_unique<GdiObject>();
        for (int i = 0; i < 256; ++i) o->pal.push_back(static_cast<uint32_t>(i | i << 8 | i << 16));
        ret(e, gui(e).new_obj(ObjKind::Palette, std::move(o)));
    });

    api(e, "DeleteObject", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        auto it = g.objs.find(h);
        if (it == g.objs.end()) return ret(e, 0);
        if (it->second->stock) return ret(e, 1);
        // Windows refuses to delete an object selected into a DC (and the
        // program carries on with it); bitmaps are the case that matters.
        if (it->second->bitmap && it->second->bitmap->selected_into && g.dc(it->second->bitmap->selected_into))
            return ret(e, 0);
        for (auto& [hd, d] : g.dcs) {
            if (d->s.pen == h || d->s.brush == h || d->s.font == h) return ret(e, 1);
        }
        g.objs.erase(it);
        ret(e, 1);
    });
    api(e, "GetObjectType", 1, [](Emulator& e) {
        Gui& g = gui(e);
        uint32_t h = U(e, 0);
        if (DC* d = g.dc(h)) return ret(e, d->kind == DC::Kind::Memory ? 10 : 3);  // OBJ_MEMDC / OBJ_DC
        GdiObject* o = g.obj(h);
        if (!o) return ret(e, 0);
        static const int kMap[] = {0, 1, 2, 6, 7, 8, 5, 3, 11, 9};
        ret(e, kMap[static_cast<int>(o->kind)]);
    });
    api(e, "GetObjectW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* o = g.obj(A(e, 0));
        int cb = I(e, 1);
        uint64_t p = A(e, 2);
        if (!o) return ret(e, 0);
        switch (o->kind) {
            case ObjKind::Pen:
                if (!p) return ret(e, 16);
                e.mem.write32(p, static_cast<uint32_t>(o->pen_style));
                e.mem.write32(p + 4, static_cast<uint32_t>(o->pen_width));
                e.mem.write32(p + 8, 0);
                e.mem.write32(p + 12, o->color);
                return ret(e, 16);
            case ObjKind::ExtPen:
                if (!p) return ret(e, 24);
                e.mem.write32(p, static_cast<uint32_t>(o->pen_style));
                e.mem.write32(p + 4, static_cast<uint32_t>(o->pen_width));
                e.mem.write32(p + 8, 0);
                e.mem.write32(p + 12, o->color);
                e.mem.write32(p + 16, 0);
                e.mem.write32(p + 20, 0);
                return ret(e, 24);
            case ObjKind::Brush:
                if (!p) return ret(e, 12);
                e.mem.write32(p, static_cast<uint32_t>(o->brush_style));
                e.mem.write32(p + 4, o->color);
                e.mem.write32(p + 8, static_cast<uint32_t>(o->hatch));
                return ret(e, 12);
            case ObjKind::Font: {
                if (!p) return ret(e, 92);
                std::vector<uint8_t> tmp(92);
                uint64_t scratch = g.scratch_alloc(92);
                write_logfont(e, scratch, o->font);
                e.mem.read(scratch, tmp.data(), 92);
                g.scratch_free(scratch);
                int n = std::min(cb, 92);
                e.mem.write(p, tmp.data(), static_cast<uint64_t>(n));
                return ret(e, static_cast<uint64_t>(n));
            }
            case ObjKind::Bitmap: {
                Bitmap& b = *o->bitmap;
                int want = b.dib && cb >= 84 ? 84 : 24;
                if (!p) return ret(e, static_cast<uint64_t>(want));
                int stride = b.dib ? std::abs(b.px.stride) : (b.mono_ddb ? ((b.px.w + 15) / 16) * 2 : b.px.w * 4);
                e.mem.write32(p, 0);
                e.mem.write32(p + 4, static_cast<uint32_t>(b.px.w));
                e.mem.write32(p + 8, static_cast<uint32_t>(b.px.h));
                e.mem.write32(p + 12, static_cast<uint32_t>(stride));
                e.mem.write16(p + 16, 1);
                e.mem.write16(p + 18, static_cast<uint16_t>(b.mono_ddb ? 1 : b.px.bpp));
                e.mem.write32(p + 20, static_cast<uint32_t>(b.dib ? b.guest_bits : 0));
                if (want == 84) {
                    std::vector<uint8_t> hdr = b.dib_info;
                    hdr.resize(40, 0);
                    e.mem.write(p + 24, hdr.data(), 40);
                    e.mem.write32(p + 64, b.px.rmask);
                    e.mem.write32(p + 68, b.px.gmask);
                    e.mem.write32(p + 72, b.px.bmask);
                    e.mem.write32(p + 76, 0);
                    e.mem.write32(p + 80, 0);
                }
                return ret(e, static_cast<uint64_t>(want));
            }
            case ObjKind::Palette:
                if (p && cb >= 2) e.mem.write16(p, static_cast<uint16_t>(o->pal.size()));
                return ret(e, 2);
            default:
                return ret(e, 0);
        }
    });
    api(e, "GetCurrentObject", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        switch (U(e, 1)) {
            case 1: return ret(e, d->s.pen);
            case 2: return ret(e, d->s.brush);
            case 5: return ret(e, d->s.palette);
            case 6: return ret(e, d->s.font);
            case 7: return ret(e, d->s.bitmap);
        }
        ret(e, 0);
    });

    // ---- device contexts -----------------------------------------------------
    api(e, "CreateCompatibleDC", 1, [](Emulator& e) {
        ret(e, gui(e).create_dc(DC::Kind::Memory, 0, false));
    });
    api(e, "CreateDCW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string drv = A(e, 0) ? upper(read_wstr(e, A(e, 0))) : u"";
        if (drv == u"DISPLAY") return ret(e, g.create_dc(DC::Kind::Window, g.desktop, true));
        std::u16string dev = A(e, 1) ? read_wstr(e, A(e, 1)) : u"";
        if (drv == u"WINSPOOL" || is_printer_name(dev)) return ret(e, create_printer_dc(g, A(e, 3)));
        ret(e, 0);
    });
    api(e, "CreateICW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        std::u16string drv = A(e, 0) ? upper(read_wstr(e, A(e, 0))) : u"";
        std::u16string dev = A(e, 1) ? read_wstr(e, A(e, 1)) : u"";
        if (drv == u"WINSPOOL" || is_printer_name(dev)) return ret(e, create_printer_dc(g, A(e, 3)));
        ret(e, g.create_dc(DC::Kind::Info, 0, false));
    });
    api(e, "DeleteDC", 1, [](Emulator& e) {
        gui(e).delete_dc(U(e, 0));
        ret(e, 1);
    });
    api(e, "SaveDC", 1, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        d->saved.push_back(d->s);
        ret(e, d->saved.size());
    });
    api(e, "RestoreDC", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        int n = I(e, 1);
        int target = n < 0 ? static_cast<int>(d->saved.size()) + n : n - 1;
        if (target < 0 || target >= static_cast<int>(d->saved.size())) return ret(e, 0);
        uint32_t old_bmp = d->s.bitmap;
        d->s = d->saved[target];
        d->saved.resize(target);
        if (d->s.bitmap != old_bmp) {
            if (GdiObject* b = g.obj(old_bmp, ObjKind::Bitmap)) b->bitmap->selected_into = 0;
            if (GdiObject* b = g.obj(d->s.bitmap, ObjKind::Bitmap)) b->bitmap->selected_into = U(e, 0);
        }
        ret(e, 1);
    });
    api(e, "SelectObject", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        uint32_t h = U(e, 1);
        GdiObject* o = g.obj(h);
        if (!d || !o) return ret(e, 0);
        switch (o->kind) {
            case ObjKind::Pen:
            case ObjKind::ExtPen: {
                uint32_t old = d->s.pen;
                d->s.pen = h;
                return ret(e, old);
            }
            case ObjKind::Brush: {
                uint32_t old = d->s.brush;
                d->s.brush = h;
                return ret(e, old);
            }
            case ObjKind::Font: {
                uint32_t old = d->s.font;
                d->s.font = h;
                return ret(e, old);
            }
            case ObjKind::Bitmap: {
                if (d->kind != DC::Kind::Memory) return ret(e, 0);
                if (o->bitmap->selected_into && o->bitmap->selected_into != U(e, 0) &&
                    g.dc(o->bitmap->selected_into) && !o->stock)
                    return ret(e, 0);  // already selected elsewhere
                uint32_t old = d->s.bitmap;
                if (GdiObject* ob = g.obj(old, ObjKind::Bitmap)) ob->bitmap->selected_into = 0;
                d->s.bitmap = h;
                if (!o->stock) o->bitmap->selected_into = U(e, 0);
                return ret(e, old);
            }
            case ObjKind::Region: {
                d->s.has_clip = true;
                d->s.clip = o->region;
                return ret(e, region_complexity(o->region));
            }
            default:
                return ret(e, 0);
        }
    });
    api(e, "SelectPalette", 3, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        uint32_t old = d->s.palette;
        d->s.palette = U(e, 1);
        ret(e, old);
    });
    api(e, "RealizePalette", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GetDeviceCaps", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        int idx = I(e, 1);
        int w = g.screen_w, h = g.screen_h;
        if (d && d->kind == DC::Kind::Printer) return ret(e, printer_device_caps(*d, idx));
        if (d && d->kind == DC::Kind::Memory) {
            // A memory DC answers for the display it is compatible with.
        }
        switch (idx) {
            case 0: return ret(e, 0x400);           // DRIVERVERSION
            case 2: return ret(e, 1);               // TECHNOLOGY: DT_RASDISPLAY
            case 4: return ret(e, static_cast<uint64_t>(w * 254 / 960));   // HORZSIZE mm
            case 6: return ret(e, static_cast<uint64_t>(h * 254 / 960));   // VERTSIZE
            case 8: return ret(e, static_cast<uint64_t>(w));   // HORZRES
            case 10: return ret(e, static_cast<uint64_t>(h));  // VERTRES
            case 12: return ret(e, 32);             // BITSPIXEL
            case 14: return ret(e, 1);              // PLANES
            case 16: return ret(e, 0xFFFFFFFF);     // NUMBRUSHES
            case 18: return ret(e, 0xFFFFFFFF);     // NUMPENS
            case 22: return ret(e, 0);              // NUMFONTS
            case 24: return ret(e, 0xFFFFFFFF);     // NUMCOLORS
            case 26: return ret(e, 0);              // PDEVICESIZE
            case 28: return ret(e, 0xFF);           // CURVECAPS
            case 30: return ret(e, 0xFE);           // LINECAPS
            case 32: return ret(e, 0xFF);           // POLYGONALCAPS
            case 34: return ret(e, 0x7807);         // TEXTCAPS
            case 36: return ret(e, 1);              // CLIPCAPS
            case 38: return ret(e, 0x7E99);         // RASTERCAPS
            case 40: return ret(e, 36);             // ASPECTX
            case 42: return ret(e, 36);             // ASPECTY
            case 44: return ret(e, 51);             // ASPECTXY
            case 88: return ret(e, 96);             // LOGPIXELSX
            case 90: return ret(e, 96);             // LOGPIXELSY
            case 104: return ret(e, 0);             // SIZEPALETTE
            case 106: return ret(e, 20);            // NUMRESERVED
            case 108: return ret(e, 24);            // COLORRES
            case 110: return ret(e, static_cast<uint64_t>(w));  // PHYSICALWIDTH
            case 111: return ret(e, static_cast<uint64_t>(h));
            case 112: case 113: case 114: case 115: return ret(e, 0);
            case 116: return ret(e, 60);            // VREFRESH
            case 117: return ret(e, static_cast<uint64_t>(h));  // DESKTOPVERTRES
            case 118: return ret(e, static_cast<uint64_t>(w));  // DESKTOPHORZRES
            case 119: return ret(e, 0);             // BLTALIGNMENT
            case 120: return ret(e, 0);             // SHADEBLENDCAPS
            case 121: return ret(e, 0x00FF);        // COLORMGMTCAPS
        }
        ret(e, 0);
    });

    // ---- DC attributes --------------------------------------------------------
    auto setter = [&](const char* name, int DCState::*field, int mask = -1) {
        api(e, name, 2, [field, mask](Emulator& e) {
            DC* d = gui(e).dc(A(e, 0));
            if (!d) return ret(e, 0);
            int old = d->s.*field;
            d->s.*field = mask == -1 ? I(e, 1) : (I(e, 1) & mask);
            ret(e, static_cast<uint64_t>(static_cast<uint32_t>(old)));
        });
    };
    setter("SetBkMode", &DCState::bk_mode);
    setter("SetROP2", &DCState::rop2);
    setter("SetPolyFillMode", &DCState::poly_fill);
    setter("SetStretchBltMode", &DCState::stretch_mode);
    setter("SetTextAlign", &DCState::text_align);
    setter("SetGraphicsMode", &DCState::graphics_mode);
    setter("SetArcDirection", &DCState::arc_direction);
    setter("SetTextCharacterExtra", &DCState::char_extra);
    auto getter = [&](const char* name, int DCState::*field) {
        api(e, name, 1, [field](Emulator& e) {
            DC* d = gui(e).dc(A(e, 0));
            ret(e, d ? static_cast<uint64_t>(static_cast<uint32_t>(d->s.*field)) : 0);
        });
    };
    getter("GetBkMode", &DCState::bk_mode);
    getter("GetROP2", &DCState::rop2);
    getter("GetPolyFillMode", &DCState::poly_fill);
    getter("GetStretchBltMode", &DCState::stretch_mode);
    getter("GetTextAlign", &DCState::text_align);
    getter("GetGraphicsMode", &DCState::graphics_mode);
    getter("GetMapMode", &DCState::map_mode);
    getter("GetArcDirection", &DCState::arc_direction);
    getter("GetTextCharacterExtra", &DCState::char_extra);
    api(e, "SetLayout", 2, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0xFFFFFFFF);
        int old = d->s.layout;
        d->s.layout = I(e, 1);
        ret(e, static_cast<uint32_t>(old));
    });
    api(e, "GetLayout", 1, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        ret(e, d ? static_cast<uint32_t>(d->s.layout) : 0xFFFFFFFF);
    });
    api(e, "SetTextColor", 2, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0xFFFFFFFF);
        uint32_t old = d->s.text_color;
        d->s.text_color = U(e, 1) & 0x00FFFFFF;
        ret(e, old);
    });
    api(e, "SetBkColor", 2, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0xFFFFFFFF);
        uint32_t old = d->s.bk_color;
        d->s.bk_color = U(e, 1) & 0x00FFFFFF;
        ret(e, old);
    });
    api(e, "GetTextColor", 1, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        ret(e, d ? d->s.text_color : 0);
    });
    api(e, "GetBkColor", 1, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        ret(e, d ? d->s.bk_color : 0);
    });
    api(e, "SetDCBrushColor", 2, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* b = g.obj(g.stock[18]);
        uint32_t old = b->color;
        b->color = U(e, 1) & 0xFFFFFF;
        ret(e, old);
    });
    api(e, "SetDCPenColor", 2, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* b = g.obj(g.stock[19]);
        uint32_t old = b->color;
        b->color = U(e, 1) & 0xFFFFFF;
        ret(e, old);
    });
    api(e, "SetBrushOrgEx", 4, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        write_point(e, A(e, 3), d->s.brush_org);
        d->s.brush_org = {I(e, 1), I(e, 2)};
        ret(e, 1);
    });
    api(e, "GetBrushOrgEx", 2, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        write_point(e, A(e, 1), d->s.brush_org);
        ret(e, 1);
    });
    api(e, "SetMapMode", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        int old = d->s.map_mode;
        apply_map_mode(g, d->s, I(e, 1));
        ret(e, static_cast<uint32_t>(old));
    });
    auto point_pair = [&](const char* name, Point DCState::*field, bool offset, bool ext) {
        api(e, name, 4, [field, offset, ext](Emulator& e) {
            DC* d = gui(e).dc(A(e, 0));
            if (!d) return ret(e, 0);
            if (ext && (d->s.map_mode != 7 && d->s.map_mode != 8)) {
                write_point(e, A(e, 3), d->s.*field);
                return ret(e, 1);
            }
            write_point(e, A(e, 3), d->s.*field);
            if (offset) {
                (d->s.*field).x += I(e, 1);
                (d->s.*field).y += I(e, 2);
            } else {
                if (ext && (I(e, 1) == 0 || I(e, 2) == 0)) return ret(e, 0);
                d->s.*field = {I(e, 1), I(e, 2)};
            }
            if (ext) fix_isotropic(d->s);
            ret(e, 1);
        });
    };
    point_pair("SetWindowOrgEx", &DCState::win_org, false, false);
    point_pair("SetViewportOrgEx", &DCState::vp_org, false, false);
    point_pair("OffsetWindowOrgEx", &DCState::win_org, true, false);
    point_pair("OffsetViewportOrgEx", &DCState::vp_org, true, false);
    point_pair("SetWindowExtEx", &DCState::win_ext, false, true);
    point_pair("SetViewportExtEx", &DCState::vp_ext, false, true);
    auto get_point = [&](const char* name, Point DCState::*field) {
        api(e, name, 2, [field](Emulator& e) {
            DC* d = gui(e).dc(A(e, 0));
            if (!d) return ret(e, 0);
            write_point(e, A(e, 1), d->s.*field);
            ret(e, 1);
        });
    };
    get_point("GetWindowOrgEx", &DCState::win_org);
    get_point("GetViewportOrgEx", &DCState::vp_org);
    get_point("GetWindowExtEx", &DCState::win_ext);
    get_point("GetViewportExtEx", &DCState::vp_ext);
    get_point("GetCurrentPositionEx", &DCState::cur);
    auto scale_ext = [&](const char* name, Point DCState::*field) {
        api(e, name, 6, [field](Emulator& e) {
            DC* d = gui(e).dc(A(e, 0));
            if (!d) return ret(e, 0);
            write_point(e, A(e, 5), d->s.*field);
            if (d->s.map_mode == 7 || d->s.map_mode == 8) {
                if (I(e, 2) && I(e, 4)) {
                    (d->s.*field).x = (d->s.*field).x * I(e, 1) / I(e, 2);
                    (d->s.*field).y = (d->s.*field).y * I(e, 3) / I(e, 4);
                }
                fix_isotropic(d->s);
            }
            ret(e, 1);
        });
    };
    scale_ext("ScaleWindowExtEx", &DCState::win_ext);
    scale_ext("ScaleViewportExtEx", &DCState::vp_ext);
    api(e, "SetWorldTransform", 2, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d || d->s.graphics_mode != 2) return ret(e, 0);
        uint64_t p = A(e, 1);
        auto f = [&](int i) {
            uint32_t v = e.mem.read32(p + i * 4);
            float x;
            std::memcpy(&x, &v, 4);
            return static_cast<double>(x);
        };
        d->s.world = {f(0), f(1), f(2), f(3), f(4), f(5)};
        ret(e, 1);
    });
    api(e, "ModifyWorldTransform", 3, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d || d->s.graphics_mode != 2) return ret(e, 0);
        uint32_t mode = U(e, 2);
        if (mode == 1) {  // MWT_IDENTITY
            d->s.world = XForm{};
            return ret(e, 1);
        }
        uint64_t p = A(e, 1);
        auto f = [&](int i) {
            uint32_t v = e.mem.read32(p + i * 4);
            float x;
            std::memcpy(&x, &v, 4);
            return static_cast<double>(x);
        };
        XForm m{f(0), f(1), f(2), f(3), f(4), f(5)};
        XForm& w = d->s.world;
        auto mul = [](const XForm& a, const XForm& b) {
            XForm r;
            r.m11 = a.m11 * b.m11 + a.m12 * b.m21;
            r.m12 = a.m11 * b.m12 + a.m12 * b.m22;
            r.m21 = a.m21 * b.m11 + a.m22 * b.m21;
            r.m22 = a.m21 * b.m12 + a.m22 * b.m22;
            r.dx = a.dx * b.m11 + a.dy * b.m21 + b.dx;
            r.dy = a.dx * b.m12 + a.dy * b.m22 + b.dy;
            return r;
        };
        if (mode == 2) w = mul(m, w);       // MWT_LEFTMULTIPLY
        else if (mode == 3) w = mul(w, m);  // MWT_RIGHTMULTIPLY
        ret(e, 1);
    });
    api(e, "LPtoDP", 3, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        for (int i = 0; i < I(e, 2); ++i) {
            uint64_t p = A(e, 1) + static_cast<uint64_t>(i) * 8;
            Point lp = read_point(e, p);
            write_point(e, p, lp_to_dp(d->s, lp.x, lp.y));
        }
        ret(e, 1);
    });
    api(e, "DPtoLP", 3, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        for (int i = 0; i < I(e, 2); ++i) {
            uint64_t p = A(e, 1) + static_cast<uint64_t>(i) * 8;
            Point dp = read_point(e, p);
            write_point(e, p, dp_to_lp(d->s, dp.x, dp.y));
        }
        ret(e, 1);
    });

    // ---- clipping ---------------------------------------------------------------
    auto set_clip = [](Gui& g, DC* d, const Region* rgn, int mode) -> int {
        // rgn in device coordinates; mode RGN_AND 1, OR 2, XOR 3, DIFF 4, COPY 5
        if (mode == 5) {
            if (!rgn) {
                d->s.has_clip = false;
                d->s.clip.clear();
                return 2;
            }
            d->s.has_clip = true;
            d->s.clip = *rgn;
        } else {
            if (!rgn) return 0;
            Region base;
            if (d->s.has_clip) {
                base = d->s.clip;
            } else {
                // The whole device surface.
                Resolved r;
                if (resolve(g, d, r)) base.set_rect({-r.org.x, -r.org.y, r.ppb->w - r.org.x, r.ppb->h - r.org.y});
                else base.set_rect({-100000, -100000, 100000, 100000});
            }
            d->s.clip = Region::combine(base, *rgn, static_cast<Region::Op>(mode));
            d->s.has_clip = true;
        }
        return region_complexity(d->s.clip);
    };
    api(e, "SelectClipRgn", 2, [set_clip](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        ret(e, static_cast<uint64_t>(set_clip(g, d, o ? &o->region : nullptr, 5)));
    });
    api(e, "ExtSelectClipRgn", 3, [set_clip](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        ret(e, static_cast<uint64_t>(set_clip(g, d, o ? &o->region : nullptr, I(e, 2))));
    });
    api(e, "IntersectClipRect", 5, [set_clip](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Region r(rect_to_dp(d->s, I(e, 1), I(e, 2), I(e, 3), I(e, 4)));
        ret(e, static_cast<uint64_t>(set_clip(g, d, &r, 1)));
    });
    api(e, "ExcludeClipRect", 5, [set_clip](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Region r(rect_to_dp(d->s, I(e, 1), I(e, 2), I(e, 3), I(e, 4)));
        ret(e, static_cast<uint64_t>(set_clip(g, d, &r, 4)));
    });
    api(e, "OffsetClipRgn", 3, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        if (d->s.has_clip) d->s.clip.offset(len_to_dp(d->s, I(e, 1)), len_to_dp(d->s, I(e, 2)));
        ret(e, d->s.has_clip ? static_cast<uint64_t>(region_complexity(d->s.clip)) : 2);
    });
    api(e, "GetClipRgn", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!d || !o) return ret(e, 0xFFFFFFFF);
        if (!d->s.has_clip) return ret(e, 0);
        o->region = d->s.clip;
        ret(e, 1);
    });
    api(e, "GetClipBox", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        int complexity = 0;
        write_rect(e, A(e, 1), dc_clip_box(g, U(e, 0), &complexity));
        ret(e, static_cast<uint64_t>(complexity));
    });
    api(e, "GetRandomRgn", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!d || !o) return ret(e, 0xFFFFFFFF);
        Resolved r;
        if (!resolve(g, d, r)) return ret(e, 0);
        o->region = clip_region(g, d, r);
        ret(e, 1);
    });
    api(e, "RectVisible", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        if (!resolve(g, d, r)) return ret(e, 0);
        Rect lr = read_rect(e, A(e, 1));
        Rect dr = rect_to_dp(d->s, lr.l, lr.t, lr.r, lr.b).offset(r.org.x, r.org.y);
        ret(e, clip_region(g, d, r).intersects(dr) ? 1 : 0);
    });
    api(e, "PtVisible", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        if (!resolve(g, d, r)) return ret(e, 0);
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        ret(e, clip_region(g, d, r).contains(p.x + r.org.x, p.y + r.org.y) ? 1 : 0);
    });
    api(e, "GetBoundsRect", 3, [](Emulator& e) {
        write_rect(e, A(e, 1), {});
        ret(e, 2);  // DCB_RESET
    });
    api(e, "SetBoundsRect", 3, [](Emulator& e) { ret(e, 2); });

    // ---- regions -----------------------------------------------------------------
    auto new_region = [](Gui& g, Region r) {
        auto o = std::make_unique<GdiObject>();
        o->region = std::move(r);
        return g.new_obj(ObjKind::Region, std::move(o));
    };
    auto norm = [](int l, int t, int r, int b) {
        return Rect{std::min(l, r), std::min(t, b), std::max(l, r), std::max(t, b)};
    };
    api(e, "CreateRectRgn", 4, [new_region, norm](Emulator& e) {
        ret(e, new_region(gui(e), Region(norm(I(e, 0), I(e, 1), I(e, 2), I(e, 3)))));
    });
    api(e, "CreateRectRgnIndirect", 1, [new_region, norm](Emulator& e) {
        Rect r = read_rect(e, A(e, 0));
        ret(e, new_region(gui(e), Region(norm(r.l, r.t, r.r, r.b))));
    });
    api(e, "CreateEllipticRgn", 4, [new_region, norm](Emulator& e) {
        ret(e, new_region(gui(e), Region::ellipse(norm(I(e, 0), I(e, 1), I(e, 2), I(e, 3)))));
    });
    api(e, "CreateEllipticRgnIndirect", 1, [new_region, norm](Emulator& e) {
        Rect r = read_rect(e, A(e, 0));
        ret(e, new_region(gui(e), Region::ellipse(norm(r.l, r.t, r.r, r.b))));
    });
    api(e, "CreateRoundRectRgn", 6, [new_region, norm](Emulator& e) {
        ret(e, new_region(gui(e), Region::round_rect(norm(I(e, 0), I(e, 1), I(e, 2), I(e, 3)), I(e, 4), I(e, 5))));
    });
    api(e, "CreatePolygonRgn", 3, [new_region](Emulator& e) {
        std::vector<Point> pts;
        for (int i = 0; i < I(e, 1); ++i) pts.push_back(read_point(e, A(e, 0) + i * 8));
        ret(e, new_region(gui(e), Region::polygon(pts, {static_cast<int>(pts.size())}, I(e, 2) == 2)));
    });
    api(e, "CreatePolyPolygonRgn", 4, [new_region](Emulator& e) {
        std::vector<int> counts;
        int total = 0;
        for (int i = 0; i < I(e, 2); ++i) {
            counts.push_back(static_cast<int32_t>(e.mem.read32(A(e, 1) + i * 4)));
            total += counts.back();
        }
        std::vector<Point> pts;
        for (int i = 0; i < total; ++i) pts.push_back(read_point(e, A(e, 0) + i * 8));
        ret(e, new_region(gui(e), Region::polygon(pts, counts, I(e, 3) == 2)));
    });
    api(e, "SetRectRgn", 5, [norm](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Region);
        if (!o) return ret(e, 0);
        o->region.set_rect(norm(I(e, 1), I(e, 2), I(e, 3), I(e, 4)));
        ret(e, 1);
    });
    api(e, "CombineRgn", 4, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* dst = g.obj(A(e, 0), ObjKind::Region);
        GdiObject* a = g.obj(A(e, 1), ObjKind::Region);
        GdiObject* b = g.obj(A(e, 2), ObjKind::Region);
        int mode = I(e, 3);
        if (!dst || !a || (mode != 5 && !b)) return ret(e, 0);
        dst->region = mode == 5 ? a->region : Region::combine(a->region, b->region, static_cast<Region::Op>(mode));
        ret(e, static_cast<uint64_t>(region_complexity(dst->region)));
    });
    api(e, "EqualRgn", 2, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* a = g.obj(A(e, 0), ObjKind::Region);
        GdiObject* b = g.obj(A(e, 1), ObjKind::Region);
        ret(e, a && b && a->region == b->region ? 1 : 0);
    });
    api(e, "OffsetRgn", 3, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Region);
        if (!o) return ret(e, 0);
        o->region.offset(I(e, 1), I(e, 2));
        ret(e, static_cast<uint64_t>(region_complexity(o->region)));
    });
    api(e, "GetRgnBox", 2, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Region);
        if (!o) return ret(e, 0);
        write_rect(e, A(e, 1), o->region.bounds());
        ret(e, static_cast<uint64_t>(region_complexity(o->region)));
    });
    api(e, "PtInRegion", 3, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Region);
        ret(e, o && o->region.contains(I(e, 1), I(e, 2)) ? 1 : 0);
    });
    api(e, "RectInRegion", 2, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Region);
        ret(e, o && o->region.intersects(read_rect(e, A(e, 1))) ? 1 : 0);
    });
    api(e, "GetRegionData", 3, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Region);
        if (!o) return ret(e, 0);
        auto rects = o->region.rects();
        uint32_t need = 32 + static_cast<uint32_t>(rects.size()) * 16;
        uint64_t p = A(e, 2);
        if (!p) return ret(e, need);
        if (U(e, 1) < need) return ret(e, 0);
        e.mem.write32(p, 32);
        e.mem.write32(p + 4, 1);
        e.mem.write32(p + 8, static_cast<uint32_t>(rects.size()));
        e.mem.write32(p + 12, static_cast<uint32_t>(rects.size() * 16));
        write_rect(e, p + 16, o->region.bounds());
        for (size_t i = 0; i < rects.size(); ++i) write_rect(e, p + 32 + i * 16, rects[i]);
        ret(e, need);
    });
    api(e, "FillRgn", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!d || !o) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        t.rop2 = R2_COPYPEN;
        Region reg = o->region;
        Point org = lp_to_dp(d->s, 0, 0);
        reg.offset(org.x + r.org.x, org.y + r.org.y);
        t.fill_region(reg, brush_paint(g, d, U(e, 2)));
        ret(e, 1);
    });
    api(e, "PaintRgn", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!d || !o) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        Region reg = o->region;
        Point org = lp_to_dp(d->s, 0, 0);
        reg.offset(org.x + r.org.x, org.y + r.org.y);
        t.fill_region(reg, brush_paint(g, d, d->s.brush));
        ret(e, 1);
    });
    api(e, "FrameRgn", 5, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!d || !o) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        t.rop2 = R2_COPYPEN;
        int fw = std::max(1, I(e, 3)), fh = std::max(1, I(e, 4));
        Region inner = o->region;
        // Erode: a pixel stays if the region holds it shifted by +-fw, +-fh.
        Region s1 = o->region, s2 = o->region, s3 = o->region, s4 = o->region;
        s1.offset(fw, 0);
        s2.offset(-fw, 0);
        s3.offset(0, fh);
        s4.offset(0, -fh);
        inner.intersect(s1);
        inner.intersect(s2);
        inner.intersect(s3);
        inner.intersect(s4);
        Region ring = Region::combine(o->region, inner, Region::Op::Diff);
        Point org = lp_to_dp(d->s, 0, 0);
        ring.offset(org.x + r.org.x, org.y + r.org.y);
        t.fill_region(ring, brush_paint(g, d, U(e, 2)));
        ret(e, 1);
    });
    api(e, "InvertRgn", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* o = g.obj(A(e, 1), ObjKind::Region);
        if (!d || !o) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        t.rop2 = 6;
        Region reg = o->region;
        reg.offset(r.org.x, r.org.y);
        Paint p;
        p.kind = Paint::Solid;
        t.fill_region(reg, p);
        ret(e, 1);
    });

    // ---- lines and shapes -------------------------------------------------------
    api(e, "MoveToEx", 4, [](Emulator& e) {
        DC* d = gui(e).dc(A(e, 0));
        if (!d) return ret(e, 0);
        write_point(e, A(e, 3), d->s.cur);
        d->s.cur = {I(e, 1), I(e, 2)};
        ret(e, 1);
    });
    api(e, "LineTo", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        Point to{I(e, 1), I(e, 2)};
        if (make_target(g, d, r, t)) {
            Point a = lp_to_dp(d->s, d->s.cur.x, d->s.cur.y), b = lp_to_dp(d->s, to.x, to.y);
            stroke_poly(g, d, t, {{a.x + r.org.x, a.y + r.org.y}, {b.x + r.org.x, b.y + r.org.y}}, false);
        }
        d->s.cur = to;
        ret(e, 1);
    });
    api(e, "Polyline", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (make_target(g, d, r, t)) stroke_poly(g, d, t, read_points_dp(e, d, A(e, 1), I(e, 2), r.org), false);
        ret(e, 1);
    });
    api(e, "PolylineTo", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        int n = I(e, 2);
        if (make_target(g, d, r, t)) {
            std::vector<Point> pts;
            Point a = lp_to_dp(d->s, d->s.cur.x, d->s.cur.y);
            pts.push_back({a.x + r.org.x, a.y + r.org.y});
            auto rest = read_points_dp(e, d, A(e, 1), n, r.org);
            pts.insert(pts.end(), rest.begin(), rest.end());
            stroke_poly(g, d, t, pts, false);
        }
        if (n > 0) d->s.cur = read_point(e, A(e, 1) + static_cast<uint64_t>(n - 1) * 8);
        ret(e, 1);
    });
    api(e, "PolyPolyline", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        uint64_t pts = A(e, 1);
        for (uint32_t i = 0; i < U(e, 3); ++i) {
            int n = static_cast<int32_t>(e.mem.read32(A(e, 2) + i * 4));
            stroke_poly(g, d, t, read_points_dp(e, d, pts, n, r.org), false);
            pts += static_cast<uint64_t>(n) * 8;
        }
        ret(e, 1);
    });
    auto fill_and_stroke_poly = [](Gui& g, DC* d, Target& t, const std::vector<Point>& pts,
                                   const std::vector<int>& counts) {
        Region reg = Region::polygon(pts, counts, d->s.poly_fill == 2);
        int saved = t.rop2;
        t.fill_region(reg, brush_paint(g, d, d->s.brush));
        t.rop2 = saved;
        size_t base = 0;
        for (int n : counts) {
            std::vector<Point> one(pts.begin() + base, pts.begin() + base + n);
            stroke_poly(g, d, t, one, true);
            base += n;
        }
    };
    api(e, "Polygon", 3, [fill_and_stroke_poly](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        auto pts = read_points_dp(e, d, A(e, 1), I(e, 2), r.org);
        fill_and_stroke_poly(g, d, t, pts, {static_cast<int>(pts.size())});
        ret(e, 1);
    });
    api(e, "PolyPolygon", 4, [fill_and_stroke_poly](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        std::vector<int> counts;
        int total = 0;
        for (int i = 0; i < I(e, 3); ++i) {
            counts.push_back(static_cast<int32_t>(e.mem.read32(A(e, 2) + i * 4)));
            total += counts.back();
        }
        auto pts = read_points_dp(e, d, A(e, 1), total, r.org);
        fill_and_stroke_poly(g, d, t, pts, counts);
        ret(e, 1);
    });
    api(e, "Rectangle", 5, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        Rect dr = rect_to_dp(d->s, I(e, 1), I(e, 2), I(e, 3), I(e, 4)).offset(r.org.x, r.org.y);
        PenInfo pen = pen_info(g, d);
        Paint br = brush_paint(g, d, d->s.brush);
        if (pen.null) {
            t.fill_rect({dr.l, dr.t, dr.r - 1, dr.b - 1}, br);
            return ret(e, 1);
        }
        int w = pen.width;
        if (w <= 1) {
            t.fill_rect({dr.l + 1, dr.t + 1, dr.r - 1, dr.b - 1}, br);
            stroke_poly(g, d, t, {{dr.l, dr.t}, {dr.r - 1, dr.t}, {dr.r - 1, dr.b - 1}, {dr.l, dr.b - 1}}, true);
        } else {
            t.fill_rect({dr.l + w / 2, dr.t + w / 2, dr.r - (w - w / 2), dr.b - (w - w / 2)}, br);
            Paint pp;
            pp.kind = Paint::Solid;
            pp.color = pen.color;
            int h1 = w / 2;
            Rect outer{dr.l - h1, dr.t - h1, dr.r - 1 + (w - h1), dr.b - 1 + (w - h1)};
            Rect inner{outer.l + w, outer.t + w, outer.r - w, outer.b - w};
            Region ring(outer);
            if (!inner.empty()) ring.subtract(inner);
            t.fill_region(ring, pp);
        }
        ret(e, 1);
    });
    api(e, "RoundRect", 7, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        Rect dr = rect_to_dp(d->s, I(e, 1), I(e, 2), I(e, 3), I(e, 4)).offset(r.org.x, r.org.y);
        Region reg = Region::round_rect(dr, len_to_dp(d->s, I(e, 5)), len_to_dp(d->s, I(e, 6)));
        t.fill_region(reg, brush_paint(g, d, d->s.brush));
        stroke_region_outline(g, d, t, reg);
        ret(e, 1);
    });
    api(e, "Ellipse", 5, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        Rect dr = rect_to_dp(d->s, I(e, 1), I(e, 2), I(e, 3), I(e, 4)).offset(r.org.x, r.org.y);
        Region reg = Region::ellipse(dr);
        t.fill_region(reg, brush_paint(g, d, d->s.brush));
        stroke_region_outline(g, d, t, reg);
        ret(e, 1);
    });
    // Arc, Chord and Pie share the geometry: the radials through the two
    // points pick where the curve starts and stops, counter-clockwise unless
    // the DC says otherwise.
    auto arc_like = [](Emulator& e, int kind) {  // 0 arc, 1 chord, 2 pie, 3 arcto
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        bool drawable = make_target(g, d, r, t);
        Point a = lp_to_dp(d->s, I(e, 1), I(e, 2)), b = lp_to_dp(d->s, I(e, 3), I(e, 4));
        Point s = lp_to_dp(d->s, I(e, 5), I(e, 6)), en = lp_to_dp(d->s, I(e, 7), I(e, 8));
        double l = std::min(a.x, b.x), tt = std::min(a.y, b.y);
        double rr = std::max(a.x, b.x) - 1, bb = std::max(a.y, b.y) - 1;
        double cx = (l + rr) / 2.0, cy = (tt + bb) / 2.0;
        double a0 = std::atan2(-(s.y - cy), s.x - cx);
        double a1 = std::atan2(-(en.y - cy), en.x - cx);
        // Device y flips when the mapping does, and with it the direction.
        bool ccw = d->s.arc_direction != 2;
        if (d->s.map_mode != 1 && (d->s.vp_ext.y < 0) != (d->s.win_ext.y < 0)) ccw = !ccw;
        double sweep = ccw ? a1 - a0 : a0 - a1;
        while (sweep <= 0) sweep += 2 * 3.14159265358979;
        if (!ccw) sweep = -sweep;
        std::vector<Point> pts;
        arc_points(l, tt, rr, bb, a0, sweep, pts);
        if (!drawable) return ret(e, 1);
        for (auto& p : pts) {
            p.x += r.org.x;
            p.y += r.org.y;
        }
        if (kind == 0 || kind == 3) {
            stroke_poly(g, d, t, pts, false);
            if (kind == 3 && !pts.empty()) d->s.cur = dp_to_lp(d->s, pts.back().x - r.org.x, pts.back().y - r.org.y);
            return ret(e, 1);
        }
        std::vector<Point> poly = pts;
        if (kind == 2) poly.push_back({static_cast<int>(std::lround(cx)) + r.org.x, static_cast<int>(std::lround(cy)) + r.org.y});
        Region reg = Region::polygon(poly, {static_cast<int>(poly.size())}, false);
        t.fill_region(reg, brush_paint(g, d, d->s.brush));
        stroke_poly(g, d, t, poly, true);
        ret(e, 1);
    };
    api(e, "Arc", 9, [arc_like](Emulator& e) { arc_like(e, 0); });
    api(e, "Chord", 9, [arc_like](Emulator& e) { arc_like(e, 1); });
    api(e, "Pie", 9, [arc_like](Emulator& e) { arc_like(e, 2); });
    api(e, "ArcTo", 9, [arc_like](Emulator& e) { arc_like(e, 3); });
    api(e, "SetPixel", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0xFFFFFFFF);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 0xFFFFFFFF);
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        t.rop2 = R2_COPYPEN;
        t.plot(p.x + r.org.x, p.y + r.org.y, colorref_to_pixel(U(e, 3)));
        ret(e, U(e, 3) & 0xFFFFFF);
    });
    alias_or_copy:
    api(e, "SetPixelV", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 1);
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        t.rop2 = R2_COPYPEN;
        t.plot(p.x + r.org.x, p.y + r.org.y, colorref_to_pixel(U(e, 3)));
        ret(e, 1);
    });
    api(e, "GetPixel", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0xFFFFFFFF);
        Resolved r;
        if (!resolve(g, d, r)) return ret(e, 0xFFFFFFFF);
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        int x = p.x + r.org.x, y = p.y + r.org.y;
        if (x < 0 || y < 0 || x >= r.ppb->w || y >= r.ppb->h) return ret(e, 0xFFFFFFFF);
        uint32_t v = r.ppb->get(x, y);
        ret(e, pixel_to_colorref(v));
    });
    api(e, "ExtFloodFill", 5, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return ret(e, 0);
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        int sx = p.x + r.org.x, sy = p.y + r.org.y;
        if (!t.visible(sx, sy)) return ret(e, 0);
        uint32_t col = colorref_to_pixel(U(e, 3));
        bool surface = U(e, 4) == 1;  // FLOODFILLSURFACE
        auto inside = [&](int x, int y) {
            if (!t.visible(x, y)) return false;
            uint32_t v = r.ppb->get(x, y);
            return surface ? v == col : v != col;
        };
        if (!inside(sx, sy)) return ret(e, 0);
        Region reg;
        std::vector<uint8_t> seen(static_cast<size_t>(r.ppb->w) * r.ppb->h, 0);
        std::vector<Point> stack{{sx, sy}};
        std::vector<std::pair<int, std::pair<int, int>>> runs;
        while (!stack.empty()) {
            Point q = stack.back();
            stack.pop_back();
            if (seen[static_cast<size_t>(q.y) * r.ppb->w + q.x] || !inside(q.x, q.y)) continue;
            int x0 = q.x, x1 = q.x;
            while (x0 > 0 && inside(x0 - 1, q.y) && !seen[static_cast<size_t>(q.y) * r.ppb->w + x0 - 1]) --x0;
            while (x1 + 1 < r.ppb->w && inside(x1 + 1, q.y) && !seen[static_cast<size_t>(q.y) * r.ppb->w + x1 + 1]) ++x1;
            for (int x = x0; x <= x1; ++x) {
                seen[static_cast<size_t>(q.y) * r.ppb->w + x] = 1;
                if (q.y > 0) stack.push_back({x, q.y - 1});
                if (q.y + 1 < r.ppb->h) stack.push_back({x, q.y + 1});
            }
            runs.push_back({q.y, {x0, x1 + 1}});
        }
        Paint br = brush_paint(g, d, d->s.brush);
        for (auto& run : runs) t.span(run.second.first, run.second.second, run.first, br);
        ret(e, 1);
    });
    api(e, "FloodFill", 4, [](Emulator& e) { ret(e, 0); });

    // ---- blits -----------------------------------------------------------------------
    auto do_blit = [](Emulator& e, uint32_t hdst, int x, int y, int w, int h, uint32_t hsrc, int sx,
                      int sy, int sw, int sh, uint32_t rop) -> bool {
        Gui& g = gui(e);
        DC* d = g.dc(hdst);
        if (!d) return false;
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return true;
        t.rop2 = R2_COPYPEN;
        Rect dr{0, 0, 0, 0};
        Point p0 = lp_to_dp(d->s, x, y), p1 = lp_to_dp(d->s, x + w, y + h);
        int dw = p1.x - p0.x, dh = p1.y - p0.y;
        BlitSource src;
        Resolved rs;
        bool use_src = rop_uses_source(rop);
        if (use_src) {
            DC* s = g.dc(hsrc);
            if (!s || !resolve(g, s, rs)) return false;
            Point q0 = lp_to_dp(s->s, sx, sy), q1 = lp_to_dp(s->s, sx + sw, sy + sh);
            sx = q0.x + rs.org.x;
            sy = q0.y + rs.org.y;
            sw = q1.x - q0.x;
            sh = q1.y - q0.y;
            src.pb = rs.ppb;
            src.mono_ddb = rs.mono;
            src.mono_fg = colorref_to_pixel(d->s.text_color);
            src.mono_bg = colorref_to_pixel(d->s.bk_color);
            src.src_bk = colorref_to_pixel(s->s.bk_color);
            // A window DC as source is clipped to what is on screen.
        }
        (void)dr;
        Paint pat = brush_paint(g, d, d->s.brush);
        blit(t, p0.x + r.org.x, p0.y + r.org.y, dw, dh, use_src ? &src : nullptr, sx, sy, sw, sh,
             rop, &pat, use_src && !rs.mono && t.mono);
        return true;
    };
    api(e, "BitBlt", 9, [do_blit](Emulator& e) {
        ret(e, do_blit(e, U(e, 0), I(e, 1), I(e, 2), I(e, 3), I(e, 4), U(e, 5), I(e, 6), I(e, 7),
                       I(e, 3), I(e, 4), U(e, 8)) ? 1 : 0);
    });
    api(e, "StretchBlt", 11, [do_blit](Emulator& e) {
        ret(e, do_blit(e, U(e, 0), I(e, 1), I(e, 2), I(e, 3), I(e, 4), U(e, 5), I(e, 6), I(e, 7),
                       I(e, 8), I(e, 9), U(e, 10)) ? 1 : 0);
    });
    api(e, "PatBlt", 6, [do_blit](Emulator& e) {
        ret(e, do_blit(e, U(e, 0), I(e, 1), I(e, 2), I(e, 3), I(e, 4), 0, 0, 0, 0, 0, U(e, 5)) ? 1 : 0);
    });
    api(e, "MaskBlt", 12, [do_blit](Emulator& e) {
        // Without the mask: the foreground operation everywhere.
        ret(e, do_blit(e, U(e, 0), I(e, 1), I(e, 2), I(e, 3), I(e, 4), U(e, 5), I(e, 6), I(e, 7),
                       I(e, 3), I(e, 4), U(e, 11) & 0x00FFFFFF) ? 1 : 0);
    });
    api(e, "PlgBlt", 10, [](Emulator& e) { ret(e, 0); });
    api(e, "TransparentBlt", 11, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        DC* s = g.dc(A(e, 5));
        if (!d || !s) return ret(e, 0);
        Resolved r, rs;
        Target t;
        if (!make_target(g, d, r, t) || !resolve(g, s, rs)) return ret(e, 1);
        BlitSource src;
        src.pb = rs.ppb;
        src.mono_ddb = rs.mono;
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        transparent_blit(t, p.x + r.org.x, p.y + r.org.y, I(e, 3), I(e, 4), src, I(e, 6) + rs.org.x,
                         I(e, 7) + rs.org.y, I(e, 8), I(e, 9), colorref_to_pixel(U(e, 10)));
        ret(e, 1);
    });
    api(e, "GdiTransparentBlt", 11, [](Emulator& e) { ret(e, 0); });
    api(e, "AlphaBlend", 11, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        DC* s = g.dc(A(e, 5));
        if (!d || !s) return ret(e, 0);
        Resolved r, rs;
        Target t;
        if (!make_target(g, d, r, t) || !resolve(g, s, rs)) return ret(e, 1);
        BlitSource src;
        src.pb = rs.ppb;
        src.mono_ddb = rs.mono;
        uint32_t bf = U(e, 10);
        Point p = lp_to_dp(d->s, I(e, 1), I(e, 2));
        alpha_blit(t, p.x + r.org.x, p.y + r.org.y, I(e, 3), I(e, 4), src, I(e, 6) + rs.org.x,
                   I(e, 7) + rs.org.y, I(e, 8), I(e, 9), static_cast<uint8_t>((bf >> 16) & 0xFF),
                   ((bf >> 24) & 1) != 0);
        ret(e, 1);
    });

    // ---- DIBs ------------------------------------------------------------------------
    auto dib_to_device = [](Emulator& e, uint32_t hdc, int x, int y, int w, int h, int sx, int sy,
                            int sw, int sh, uint64_t bits, uint64_t bmi, uint32_t usage, uint32_t rop,
                            int start, int lines) -> int {
        Gui& g = gui(e);
        DC* d = g.dc(hdc);
        if (!d || !bits) return 0;
        DibInfo di;
        if (!read_dib_info(e, g, d, bmi, usage, di)) return 0;
        PixelBuf src;
        std::vector<uint8_t> store;
        int total = std::abs(di.h);
        if (lines >= 0) {
            // SetDIBitsToDevice: `bits` holds scanlines start..start+lines.
            if (!load_dib(e, di, bits, src, store, start, lines)) return 0;
        } else if (!load_dib(e, di, bits, src, store)) {
            return 0;
        }
        Resolved r;
        Target t;
        if (!make_target(g, d, r, t)) return total;
        t.rop2 = R2_COPYPEN;
        BlitSource bs;
        bs.pb = &src;
        // Source rectangles count from the bottom for a bottom-up DIB.
        int src_y = di.h > 0 ? total - sy - sh : sy;
        Point p0 = lp_to_dp(d->s, x, y), p1 = lp_to_dp(d->s, x + w, y + h);
        Paint pat = brush_paint(g, d, d->s.brush);
        blit(t, p0.x + r.org.x, p0.y + r.org.y, p1.x - p0.x, p1.y - p0.y, &bs, sx, src_y, sw, sh, rop,
             &pat, false);
        return total;
    };
    api(e, "StretchDIBits", 13, [dib_to_device](Emulator& e) {
        int n = dib_to_device(e, U(e, 0), I(e, 1), I(e, 2), I(e, 3), I(e, 4), I(e, 5), I(e, 6), I(e, 7),
                              I(e, 8), A(e, 9), A(e, 10), U(e, 11), U(e, 12), 0, -1);
        ret(e, static_cast<uint64_t>(n));
    });
    api(e, "SetDIBitsToDevice", 12, [dib_to_device](Emulator& e) {
        int n = dib_to_device(e, U(e, 0), I(e, 1), I(e, 2), U(e, 3), U(e, 4), I(e, 5), I(e, 6), U(e, 3),
                              U(e, 4), A(e, 9), A(e, 10), U(e, 11), 0x00CC0020, I(e, 7), U(e, 8));
        ret(e, static_cast<uint64_t>(n));
    });
    api(e, "SetDIBits", 7, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* o = g.obj(A(e, 1), ObjKind::Bitmap);
        if (!o) return ret(e, 0);
        DibInfo di;
        if (!read_dib_info(e, g, g.dc(A(e, 0)), A(e, 5), U(e, 6), di)) return ret(e, 0);
        PixelBuf src;
        std::vector<uint8_t> store;
        if (!load_dib(e, di, A(e, 4), src, store, I(e, 2), U(e, 3))) return ret(e, 0);
        Bitmap& b = *o->bitmap;
        for (int y = 0; y < std::min(b.px.h, src.h); ++y)
            for (int x = 0; x < std::min(b.px.w, src.w); ++x) b.px.put(x, y, src.get(x, y));
        ret(e, U(e, 3));
    });
    api(e, "GetDIBits", 7, [](Emulator& e) {
        Gui& g = gui(e);
        GdiObject* o = g.obj(A(e, 1), ObjKind::Bitmap);
        uint64_t bmi = A(e, 5);
        if (!o || !bmi) return ret(e, 0);
        Bitmap& b = *o->bitmap;
        uint32_t hs = e.mem.read32(bmi);
        int bpp = e.mem.read16(bmi + 14);
        uint64_t bits = A(e, 4);
        int w = b.px.w, h = b.px.h;
        if (bpp == 0) {
            // Fill in the header.
            int obpp = b.mono_ddb ? 1 : (b.dib ? b.px.bpp : 32);
            e.mem.write32(bmi + 4, static_cast<uint32_t>(w));
            e.mem.write32(bmi + 8, static_cast<uint32_t>(h));
            e.mem.write16(bmi + 12, 1);
            e.mem.write16(bmi + 14, static_cast<uint16_t>(obpp));
            e.mem.write32(bmi + 16, obpp == 16 || obpp == 32 ? 3 : 0);
            e.mem.write32(bmi + 20, static_cast<uint32_t>(((w * obpp + 31) / 32) * 4 * h));
            if (obpp == 32 || obpp == 16) {
                uint64_t m = bmi + hs;
                e.mem.write32(m, obpp == 32 ? 0xFF0000 : 0x7C00);
                e.mem.write32(m + 4, obpp == 32 ? 0x00FF00 : 0x03E0);
                e.mem.write32(m + 8, obpp == 32 ? 0x0000FF : 0x001F);
            }
            return ret(e, 1);
        }
        int dh = static_cast<int32_t>(e.mem.read32(bmi + 8));
        int stride = ((w * bpp + 31) / 32) * 4;
        int start = I(e, 2), lines = I(e, 3);
        // Colour table for palettised output.
        PixelBuf out;
        out.w = w;
        out.h = std::abs(dh) ? std::abs(dh) : h;
        out.bpp = bpp;
        if (bpp <= 8) {
            int n = 1 << bpp;
            if (b.px.bpp == bpp && !b.px.palette.empty())
                out.palette = b.px.palette;
            else if (bpp == 1)
                out.palette = {0x000000, 0xFFFFFF};
            else
                for (int i = 0; i < n; ++i) {
                    int v = i * 255 / (n - 1);
                    out.palette.push_back(static_cast<uint32_t>(v << 16 | v << 8 | v));
                }
            for (int i = 0; i < n; ++i)
                e.mem.write32(bmi + hs + i * 4, i < static_cast<int>(out.palette.size()) ? out.palette[i] : 0);
        }
        if (!bits) {
            e.mem.write32(bmi + 20, static_cast<uint32_t>(stride * h));
            return ret(e, static_cast<uint64_t>(h));
        }
        std::vector<uint8_t> row(static_cast<size_t>(stride));
        out.stride = stride;
        int copied = 0;
        for (int k = 0; k < lines; ++k) {
            int dib_row = start + k;  // counted in DIB order
            if (dib_row >= h) break;
            int y = dh > 0 ? h - 1 - dib_row : dib_row;  // bitmap row
            std::fill(row.begin(), row.end(), 0);
            out.bits = row.data();
            for (int x = 0; x < w; ++x) {
                uint32_t v = b.px.get(x, y);
                if (b.mono_ddb) v = v ? 0xFFFFFF : 0;
                out.put(x, 0, v);
            }
            e.mem.write(bits + static_cast<uint64_t>(k) * stride, row.data(), row.size());
            ++copied;
        }
        ret(e, static_cast<uint64_t>(copied));
    });
    api(e, "GetBitmapBits", 3, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Bitmap);
        if (!o) return ret(e, 0);
        Bitmap& b = *o->bitmap;
        size_t n = std::min<size_t>(U(e, 1), b.host.size());
        if (A(e, 2) && n) e.mem.write(A(e, 2), b.host.data(), n);
        ret(e, n);
    });
    api(e, "SetBitmapBits", 3, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Bitmap);
        if (!o) return ret(e, 0);
        Bitmap& b = *o->bitmap;
        size_t n = std::min<size_t>(U(e, 1), b.host.size());
        if (A(e, 2) && n) e.mem.read(A(e, 2), b.host.data(), n);
        ret(e, n);
    });
    api(e, "SetDIBColorTable", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Bitmap* b = dc_bitmap(g, d);
        if (!b || !b->dib || b->px.bpp > 8) return ret(e, 0);
        uint32_t start = U(e, 1), n = U(e, 2);
        for (uint32_t i = 0; i < n && start + i < b->px.palette.size(); ++i)
            b->px.palette[start + i] = e.mem.read32(A(e, 3) + i * 4) & 0xFFFFFF;
        ret(e, n);
    });
    api(e, "GetDIBColorTable", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Bitmap* b = dc_bitmap(g, d);
        if (!b || b->px.bpp > 8) return ret(e, 0);
        uint32_t start = U(e, 1), n = U(e, 2), k = 0;
        for (; k < n && start + k < b->px.palette.size(); ++k)
            e.mem.write32(A(e, 3) + k * 4, b->px.palette[start + k]);
        ret(e, k);
    });
    api(e, "GdiFlush", 0, [](Emulator& e) { ret(e, 1); });
    api(e, "UnrealizeObject", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "GetDCOrgEx", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        Point p{0, 0};
        if (d->kind == DC::Kind::Window) {
            Window* w = g.win(d->hwnd);
            if (w) {
                Rect r = d->whole_window ? g.window_rect_screen(w) : g.client_rect_screen(w);
                p = {r.l, r.t};
            }
        }
        write_point(e, A(e, 1), p);
        ret(e, 1);
    });
    api(e, "TranslateCharsetInfo", 3, [](Emulator& e) {
        // TCI_SRCCHARSET with SHIFTJIS, or anything else: answer for code page 932.
        uint64_t p = A(e, 1);
        if (!p) return ret(e, 0);
        e.mem.write32(p, 128);   // ciCharset
        e.mem.write32(p + 4, 932);  // ciACP
        for (int i = 8; i < 32; i += 4) e.mem.write32(p + i, 0);
        e.mem.write32(p + 8, 0x00020000);  // fsCsb[0]: Japanese
        ret(e, 1);
    });

    // ---- palettes --------------------------------------------------------------------
    api(e, "GetPaletteEntries", 4, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Palette);
        if (!o) return ret(e, 0);
        uint32_t start = U(e, 1), n = U(e, 2);
        if (!A(e, 3)) return ret(e, o->pal.size());
        uint32_t k = 0;
        for (; k < n && start + k < o->pal.size(); ++k) e.mem.write32(A(e, 3) + k * 4, o->pal[start + k]);
        ret(e, k);
    });
    api(e, "SetPaletteEntries", 4, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Palette);
        if (!o) return ret(e, 0);
        uint32_t start = U(e, 1), n = U(e, 2), k = 0;
        for (; k < n && start + k < o->pal.size(); ++k) o->pal[start + k] = e.mem.read32(A(e, 3) + k * 4);
        ret(e, k);
    });
    api(e, "GetSystemPaletteEntries", 4, [](Emulator& e) { ret(e, 0); });
    api(e, "GetNearestPaletteIndex", 2, [](Emulator& e) {
        GdiObject* o = gui(e).obj(A(e, 0), ObjKind::Palette);
        if (!o || o->pal.empty()) return ret(e, 0);
        uint32_t c = U(e, 1);
        int best = 0;
        long bd = 1L << 30;
        for (size_t i = 0; i < o->pal.size(); ++i) {
            uint32_t p = o->pal[i];
            long dr = static_cast<long>(c & 0xFF) - static_cast<long>(p & 0xFF);
            long dg = static_cast<long>((c >> 8) & 0xFF) - static_cast<long>((p >> 8) & 0xFF);
            long db = static_cast<long>((c >> 16) & 0xFF) - static_cast<long>((p >> 16) & 0xFF);
            long dd = dr * dr + dg * dg + db * db;
            if (dd < bd) {
                bd = dd;
                best = static_cast<int>(i);
            }
        }
        ret(e, static_cast<uint64_t>(best));
    });
    api(e, "GetNearestColor", 2, [](Emulator& e) { ret(e, U(e, 1) & 0xFFFFFF); });
    api(e, "UpdateColors", 1, [](Emulator& e) { ret(e, 1); });

    // ---- text ------------------------------------------------------------------------
    api(e, "TextOutW", 5, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        std::u16string s = read_wstr(e, A(e, 3), I(e, 4));
        ret(e, text_out(g, d, I(e, 1), I(e, 2), 0, nullptr, s, nullptr, false) ? 1 : 0);
    });
    api(e, "TextOutA", 5, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        std::string a(static_cast<size_t>(std::max(0, I(e, 4))), '\0');
        if (!a.empty()) e.mem.read(A(e, 3), a.data(), a.size());
        ret(e, text_out(g, d, I(e, 1), I(e, 2), 0, nullptr, cp932_to_utf16(a), nullptr, false) ? 1 : 0);
    });
    api(e, "ExtTextOutW", 8, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        uint32_t opt = U(e, 3);
        Rect rc;
        bool has_rect = A(e, 4) != 0;
        if (has_rect) rc = read_rect(e, A(e, 4));
        int n = I(e, 6);
        std::u16string s;
        if (opt & 0x10) {  // ETO_GLYPH_INDEX: treat indices as characters (best effort)
            s = A(e, 5) ? read_wstr(e, A(e, 5), n) : u"";
        } else {
            s = A(e, 5) && n > 0 ? read_wstr(e, A(e, 5), n) : u"";
        }
        std::vector<int> dx;
        bool pairs = (opt & 0x2000) != 0;  // ETO_PDY
        if (A(e, 7) && n > 0)
            for (int i = 0; i < n * (pairs ? 2 : 1); ++i) dx.push_back(static_cast<int32_t>(e.mem.read32(A(e, 7) + i * 4)));
        ret(e, text_out(g, d, I(e, 1), I(e, 2), opt, has_rect ? &rc : nullptr, s, dx.empty() ? nullptr : &dx, pairs) ? 1 : 0);
    });
    api(e, "GetTextExtentPoint32W", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        RealFont* f = dc_font(g, d);
        std::u16string s = read_wstr(e, A(e, 1), I(e, 2));
        int w = text_width(f, s, d->s.char_extra);
        int h = f->height;
        if (d->s.map_mode != 1) {
            Point a = dp_to_lp(d->s, 0, 0), b = dp_to_lp(d->s, w, h);
            w = std::abs(b.x - a.x);
            h = std::abs(b.y - a.y);
        }
        write_point(e, A(e, 3), {w, h});
        ret(e, 1);
    });
    alias_or_copy2:
    api(e, "GetTextExtentPointW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        RealFont* f = dc_font(g, d);
        std::u16string s = read_wstr(e, A(e, 1), I(e, 2));
        write_point(e, A(e, 3), {text_width(f, s, d->s.char_extra), f->height});
        ret(e, 1);
    });
    api(e, "GetTextExtentPoint32A", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        RealFont* f = dc_font(g, d);
        std::string a(static_cast<size_t>(std::max(0, I(e, 2))), '\0');
        if (!a.empty()) e.mem.read(A(e, 1), a.data(), a.size());
        write_point(e, A(e, 3), {text_width(f, cp932_to_utf16(a), d->s.char_extra), f->height});
        ret(e, 1);
    });
    api(e, "GetTextExtentExPointW", 7, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        RealFont* f = dc_font(g, d);
        std::u16string s = read_wstr(e, A(e, 1), I(e, 2));
        int maxw = I(e, 3);
        int fit = 0, x = 0;
        auto cps = to_codepoints(s);
        for (size_t i = 0; i < cps.size(); ++i) {
            x += f->advance(cps[i]) + d->s.char_extra;
            if (A(e, 5)) e.mem.write32(A(e, 5) + i * 4, static_cast<uint32_t>(x));
            if (x <= maxw) fit = static_cast<int>(i + 1);
        }
        if (A(e, 4)) e.mem.write32(A(e, 4), static_cast<uint32_t>(fit));
        write_point(e, A(e, 6), {x, f->height});
        ret(e, 1);
    });
    api(e, "GetTextMetricsW", 2, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        write_textmetric(e, A(e, 1), dc_font(g, d));
        ret(e, 1);
    });
    api(e, "GetTextFaceW", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        GdiObject* f = g.obj(d->s.font, ObjKind::Font);
        std::u16string face = f ? f->font.face : u"System";
        if (!A(e, 2)) return ret(e, face.size() + 1);
        ret(e, static_cast<uint64_t>(write_wstr(e, A(e, 2), face, I(e, 1))));
    });
    api(e, "GetTextCharsetInfo", 3, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (A(e, 1)) {
            for (int i = 0; i < 24; i += 4) e.mem.write32(A(e, 1) + i, 0);
        }
        GdiObject* f = d ? g.obj(d->s.font, ObjKind::Font) : nullptr;
        ret(e, f ? f->font.charset : 128);
    });
    api(e, "GetTextCharset", 1, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        GdiObject* f = d ? g.obj(d->s.font, ObjKind::Font) : nullptr;
        ret(e, f ? f->font.charset : 128);
    });
    auto char_widths = [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        RealFont* f = dc_font(g, d);
        uint32_t first = U(e, 1), last = U(e, 2);
        for (uint32_t c = first; c <= last && c - first < 0x10000; ++c)
            e.mem.write32(A(e, 3) + (c - first) * 4, static_cast<uint32_t>(f->advance(c)));
        ret(e, 1);
    };
    api(e, "GetCharWidthW", 4, char_widths);
    api(e, "GetCharWidth32W", 4, char_widths);
    api(e, "GetCharABCWidthsW", 4, [](Emulator& e) {
        Gui& g = gui(e);
        DC* d = g.dc(A(e, 0));
        if (!d) return ret(e, 0);
        RealFont* f = dc_font(g, d);
        uint32_t first = U(e, 1), last = U(e, 2);
        for (uint32_t c = first; c <= last && c - first < 0x10000; ++c) {
            uint64_t p = A(e, 3) + (c - first) * 12;
            e.mem.write32(p, 0);
            e.mem.write32(p + 4, static_cast<uint32_t>(f->advance(c)));
            e.mem.write32(p + 8, 0);
        }
        ret(e, 1);
    });
    api(e, "GetOutlineTextMetricsW", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "GetGlyphOutlineW", 7, [](Emulator& e) { ret(e, 0xFFFFFFFF); });
    api(e, "GetFontData", 5, [](Emulator& e) { ret(e, 0xFFFFFFFF); });
    api(e, "GetFontLanguageInfo", 1, [](Emulator& e) { ret(e, 0); });
    api(e, "GetCharacterPlacementW", 6, [](Emulator& e) { ret(e, 0); });
    api(e, "AddFontResourceExW", 3, [](Emulator& e) { ret(e, 0); });
    api(e, "RemoveFontResourceExW", 3, [](Emulator& e) { ret(e, 0); });

    // EnumFontFamilies(Ex): the faces font.cpp stands behind.
    auto enum_fonts = [](Emulator& e, bool ex) {
        Gui& g = gui(e);
        uint64_t proc = A(e, 2), lparam = A(e, 3);
        std::u16string want;
        uint8_t want_cs = 1;
        if (ex) {
            uint64_t lf = A(e, 1);
            if (lf) {
                want = read_wstr(e, lf + 28, 32);
                want_cs = e.mem.read8(lf + 23);
            }
        } else if (A(e, 1)) {
            want = read_wstr(e, A(e, 1), 32);
        }
        uint64_t elf = g.scratch_alloc(420);  // ENUMLOGFONTEXW
        uint64_t tm = g.scratch_alloc(80);    // NEWTEXTMETRICEXW
        uint64_t result = 1;
        for (const FaceInfo& fi : font_faces()) {
            if (!want.empty() && want != fi.name) continue;
            if (ex && want_cs != 1 && want_cs != fi.charset) continue;
            LogFont lf;
            lf.face = fi.name;
            lf.height = -16;
            lf.charset = fi.charset;
            lf.pitch_family = static_cast<uint8_t>((fi.fixed ? 1 : 2) | 0x30);
            for (int i = 0; i < 420; i += 4) e.mem.write32(elf + i, 0);
            write_logfont(e, elf, lf);
            write_wstr(e, elf + 92, fi.name, 64);  // elfFullName
            write_textmetric(e, tm, realize_font(lf));
            for (int i = 60; i < 80; i += 4) e.mem.write32(tm + i, 0);
            result = g.call_wndproc(proc, static_cast<uint32_t>(elf), static_cast<uint32_t>(tm), 4, lparam);
            if (!result) break;
        }
        g.scratch_free(elf);
        g.scratch_free(tm);
        ret(e, result);
    };
    api(e, "EnumFontFamiliesExW", 5, [enum_fonts](Emulator& e) { enum_fonts(e, true); });
    api(e, "EnumFontFamiliesW", 4, [enum_fonts](Emulator& e) { enum_fonts(e, false); });
    api(e, "EnumFontsW", 4, [enum_fonts](Emulator& e) { enum_fonts(e, false); });

    // ---- metafiles: none --------------------------------------------------------------
    api(e, "CopyMetaFileW", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "DeleteMetaFile", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "DeleteEnhMetaFile", 1, [](Emulator& e) { ret(e, 1); });
    api(e, "PlayMetaFile", 2, [](Emulator& e) { ret(e, 1); });
    api(e, "SetMetaFileBitsEx", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "GetColorAdjustment", 2, [](Emulator& e) { ret(e, 0); });
    api(e, "GetICMProfileW", 3, [](Emulator& e) { ret(e, 0); });
}

}  // namespace x86emu::win32
