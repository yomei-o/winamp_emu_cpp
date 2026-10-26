// Pixel-level drawing, below the DC: everything here works in the
// coordinates of one pixel buffer, through one clip region, with colours
// already resolved to 0x00RRGGBB.
#pragma once

#include <memory>
#include <vector>

#include "gui.h"

namespace x86emu::win32 {

// How a fill colours a pixel: solid, hatched, patterned, or not at all.
struct Paint {
    enum Kind { Null, Solid, Hatch, Pattern };
    Kind kind = Null;
    uint32_t color = 0;        // pixel
    int hatch = 0;
    uint32_t bk = 0xFFFFFF;    // pixel, for the gaps of a hatch with an opaque bk mode
    bool opaque = true;
    std::shared_ptr<Bitmap> pat;
    uint32_t text = 0, bkc = 0xFFFFFF;  // colours a monochrome pattern takes
    int ox = 0, oy = 0;        // brush origin
    // Returns false when the pixel is to be left alone.
    bool at(int x, int y, uint32_t& out) const;
};

struct Target {
    PixelBuf* pb = nullptr;
    Region clip;      // surface coordinates, already within the buffer
    Rect clip_box;
    bool clip_is_rect = true;
    int rop2 = 13;    // R2_COPYPEN
    bool mono = false;

    void set_clip(const Region& r);
    bool visible(int x, int y) const {
        if (!clip_box.contains(x, y)) return false;
        return clip_is_rect || clip.contains(x, y);
    }
    void plot(int x, int y, uint32_t rgb) {
        if (visible(x, y)) plot_unclipped(x, y, rgb);
    }
    void plot_unclipped(int x, int y, uint32_t rgb);
    // A horizontal run [x0, x1) on row y, through the clip.
    void span(int x0, int x1, int y, const Paint& p);
    void span_color(int x0, int x1, int y, uint32_t rgb);
    void fill_region(const Region& r, const Paint& p);
    void fill_rect(const Rect& r, const Paint& p);
    // A pen line from (x0,y0) up to but not including (x1,y1), cosmetic.
    // `style` follows PS_* for width-1 pens; `phase` carries the dash
    // position from one segment of a polyline to the next.
    void line(int x0, int y0, int x1, int y1, uint32_t rgb, int style, uint32_t bk,
              bool bk_opaque, int& phase, bool include_last = false);
    void wide_line(int x0, int y0, int x1, int y1, uint32_t rgb, int width, int endcap);
    // An alpha mask blended in colour rgb.
    void blend_mask(int x, int y, int w, int h, const uint8_t* alpha, int pitch, uint32_t rgb);
};

uint32_t rop2_apply(int rop2, uint32_t pen, uint32_t dst);
uint32_t rop3_apply(uint32_t rop, uint32_t p, uint32_t s, uint32_t d);
bool rop_uses_source(uint32_t rop);
bool rop_uses_pattern(uint32_t rop);
bool rop_uses_dest(uint32_t rop);

// The pattern of a stock hatch style, 8 rows of 8 bits (MSB = leftmost).
const uint8_t* hatch_pattern(int style);

// Copies with a ternary raster operation.  `src` may be null when the
// operation does not read it.  Coordinates are in the two buffers' own
// pixels; the destination clip comes from `dst`.  sw/sh may differ from
// dw/dh (stretching) or be negative (mirroring).
struct BlitSource {
    const PixelBuf* pb = nullptr;
    bool mono_ddb = false;
    uint32_t mono_fg = 0, mono_bg = 0xFFFFFF;  // what 0 and 1 become (DC colours)
    uint32_t src_bk = 0xFFFFFF;  // colour->mono: this colour becomes 1
};
void blit(Target& dst, int dx, int dy, int dw, int dh, const BlitSource* src, int sx, int sy,
          int sw, int sh, uint32_t rop, const Paint* pattern, bool dst_mono_from_color);
// TransparentBlt / AlphaBlend
void transparent_blit(Target& dst, int dx, int dy, int dw, int dh, const BlitSource& src,
                      int sx, int sy, int sw, int sh, uint32_t key);
void alpha_blit(Target& dst, int dx, int dy, int dw, int dh, const BlitSource& src, int sx,
                int sy, int sw, int sh, uint8_t const_alpha, bool per_pixel);

// Ellipse points, the outline GDI walks for an Ellipse (bounding box
// inclusive-exclusive), as a polygon for filling and as pixels for the pen.
void ellipse_outline(const Rect& r, std::vector<Point>& pixels);

}  // namespace x86emu::win32
