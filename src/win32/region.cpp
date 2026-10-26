// Regions as y-bands of x-spans.
//
// Set operations work band by band: collect every y where either operand
// changes, and within each slice combine the two span lists as interval sets.
// Neighbouring slices with identical spans are merged afterwards, so the
// representation stays canonical and equality is a plain comparison.
#include <algorithm>
#include <climits>
#include <cmath>

#include "gui.h"

namespace x86emu::win32 {

void Region::set_rect(const Rect& r) {
    bands_.clear();
    if (r.empty()) return;
    bands_.push_back({r.t, r.b, {{r.l, r.r}}});
}

Rect Region::bounds() const {
    if (bands_.empty()) return {};
    Rect b{INT_MAX, bands_.front().top, INT_MIN, bands_.back().bottom};
    for (const auto& band : bands_) {
        b.l = std::min(b.l, band.spans.front().first);
        b.r = std::max(b.r, band.spans.back().second);
    }
    return b;
}

const Region::Band* Region::band_at(int y) const {
    // Bands are sorted by top and disjoint.
    size_t lo = 0, hi = bands_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (bands_[mid].bottom <= y)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < bands_.size() && bands_[lo].top <= y && y < bands_[lo].bottom) return &bands_[lo];
    return nullptr;
}

bool Region::contains(int x, int y) const {
    const Band* b = band_at(y);
    if (!b) return false;
    for (const auto& s : b->spans) {
        if (x < s.first) return false;
        if (x < s.second) return true;
    }
    return false;
}

bool Region::intersects(const Rect& r) const {
    if (r.empty()) return false;
    for (const auto& b : bands_) {
        if (b.bottom <= r.t) continue;
        if (b.top >= r.b) break;
        for (const auto& s : b.spans)
            if (s.first < r.r && s.second > r.l) return true;
    }
    return false;
}

void Region::offset(int dx, int dy) {
    for (auto& b : bands_) {
        b.top += dy;
        b.bottom += dy;
        for (auto& s : b.spans) {
            s.first += dx;
            s.second += dx;
        }
    }
}

std::vector<Rect> Region::rects() const {
    std::vector<Rect> out;
    for (const auto& b : bands_)
        for (const auto& s : b.spans) out.push_back({s.first, b.top, s.second, b.bottom});
    return out;
}

bool Region::operator==(const Region& o) const {
    if (bands_.size() != o.bands_.size()) return false;
    for (size_t i = 0; i < bands_.size(); ++i) {
        if (bands_[i].top != o.bands_[i].top || bands_[i].bottom != o.bands_[i].bottom ||
            bands_[i].spans != o.bands_[i].spans)
            return false;
    }
    return true;
}

void Region::push_band(int top, int bottom, std::vector<std::pair<int, int>> spans) {
    if (top >= bottom || spans.empty()) return;
    if (!bands_.empty() && bands_.back().bottom == top && bands_.back().spans == spans) {
        bands_.back().bottom = bottom;
        return;
    }
    bands_.push_back({top, bottom, std::move(spans)});
}

void Region::normalise() {
    std::vector<Band> old;
    old.swap(bands_);
    for (auto& b : old) push_band(b.top, b.bottom, std::move(b.spans));
}

namespace {

using Spans = std::vector<std::pair<int, int>>;

// Combines two sorted, disjoint span lists.
Spans combine_spans(const Spans& a, const Spans& b, Region::Op op) {
    // Sweep over the edges of both lists, tracking whether we are inside each.
    std::vector<std::pair<int, int>> edges;  // (x, +1/-1 for a as 1, b as 2)
    edges.reserve((a.size() + b.size()) * 2);
    for (const auto& s : a) {
        edges.push_back({s.first, 1});
        edges.push_back({s.second, -1});
    }
    for (const auto& s : b) {
        edges.push_back({s.first, 2});
        edges.push_back({s.second, -2});
    }
    std::sort(edges.begin(), edges.end(),
              [](const auto& p, const auto& q) { return p.first < q.first; });
    Spans out;
    bool in_a = false, in_b = false, inside = false;
    int start = 0;
    size_t i = 0;
    while (i < edges.size()) {
        int x = edges[i].first;
        while (i < edges.size() && edges[i].first == x) {
            switch (edges[i].second) {
                case 1: in_a = true; break;
                case -1: in_a = false; break;
                case 2: in_b = true; break;
                case -2: in_b = false; break;
            }
            ++i;
        }
        bool now = false;
        switch (op) {
            case Region::Op::And: now = in_a && in_b; break;
            case Region::Op::Or: now = in_a || in_b; break;
            case Region::Op::Xor: now = in_a != in_b; break;
            case Region::Op::Diff: now = in_a && !in_b; break;
            case Region::Op::Copy: now = in_a; break;
        }
        if (now && !inside) {
            start = x;
            inside = true;
        } else if (!now && inside) {
            if (x > start) {
                if (!out.empty() && out.back().second == start)
                    out.back().second = x;
                else
                    out.push_back({start, x});
            }
            inside = false;
        }
    }
    return out;
}

}  // namespace

Region Region::combine(const Region& a, const Region& b, Op op) {
    if (op == Op::Copy) return a;
    if (op == Op::And && (a.empty() || b.empty())) return {};
    if (op == Op::Diff && (a.empty() || b.empty())) return a;
    if ((op == Op::Or || op == Op::Xor) && a.empty()) return b;
    if ((op == Op::Or || op == Op::Xor) && b.empty()) return a;

    std::vector<int> ys;
    ys.reserve((a.bands_.size() + b.bands_.size()) * 2);
    for (const auto& band : a.bands_) {
        ys.push_back(band.top);
        ys.push_back(band.bottom);
    }
    for (const auto& band : b.bands_) {
        ys.push_back(band.top);
        ys.push_back(band.bottom);
    }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    static const Spans kNone;
    Region out;
    size_t ia = 0, ib = 0;
    for (size_t k = 0; k + 1 < ys.size(); ++k) {
        int y0 = ys[k], y1 = ys[k + 1];
        while (ia < a.bands_.size() && a.bands_[ia].bottom <= y0) ++ia;
        while (ib < b.bands_.size() && b.bands_[ib].bottom <= y0) ++ib;
        const Spans& sa =
            (ia < a.bands_.size() && a.bands_[ia].top <= y0) ? a.bands_[ia].spans : kNone;
        const Spans& sb =
            (ib < b.bands_.size() && b.bands_[ib].top <= y0) ? b.bands_[ib].spans : kNone;
        if (sa.empty() && sb.empty()) continue;
        Spans s = combine_spans(sa, sb, op);
        out.push_band(y0, y1, std::move(s));
    }
    return out;
}

// Polygon interior by scanline: a pixel (x, y) is inside when its centre
// (x + 0.5, y + 0.5)... GDI actually samples at the integer point with a
// top-left rule, and that is what this does: for each row y the crossings of
// the line y with every edge are found, rounded up, and spans run from one
// crossing up to (excluding) the next.
Region Region::polygon(const std::vector<Point>& pts, const std::vector<int>& counts,
                       bool winding) {
    struct Edge {
        double x0, y0, x1, y1;
        int dir;
    };
    std::vector<Edge> edges;
    size_t base = 0;
    int ymin = INT_MAX, ymax = INT_MIN;
    for (int n : counts) {
        for (int i = 0; i < n; ++i) {
            const Point& p = pts[base + i];
            const Point& q = pts[base + (i + 1) % n];
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, p.y);
            if (p.y == q.y) continue;
            if (p.y < q.y)
                edges.push_back({double(p.x), double(p.y), double(q.x), double(q.y), 1});
            else
                edges.push_back({double(q.x), double(q.y), double(p.x), double(p.y), -1});
        }
        base += n;
    }
    Region out;
    if (edges.empty()) return out;
    std::vector<std::pair<double, int>> xs;
    for (int y = ymin; y < ymax; ++y) {
        xs.clear();
        double sy = y;  // sample row
        for (const auto& e : edges) {
            if (sy < e.y0 || sy >= e.y1) continue;
            double x = e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0);
            xs.push_back({x, e.dir});
        }
        if (xs.empty()) continue;
        std::sort(xs.begin(), xs.end(),
                  [](const auto& p, const auto& q) { return p.first < q.first; });
        Spans spans;
        auto add = [&](double a, double b) {
            int x0 = static_cast<int>(std::ceil(a - 1e-9));
            int x1 = static_cast<int>(std::ceil(b - 1e-9));
            if (x1 <= x0) return;
            if (!spans.empty() && spans.back().second >= x0)
                spans.back().second = std::max(spans.back().second, x1);
            else
                spans.push_back({x0, x1});
        };
        if (!winding) {
            for (size_t i = 0; i + 1 < xs.size(); i += 2) add(xs[i].first, xs[i + 1].first);
        } else {
            int wind = 0;
            double start = 0;
            for (const auto& [x, d] : xs) {
                int before = wind;
                wind += d;
                if (before == 0 && wind != 0) start = x;
                if (before != 0 && wind == 0) add(start, x);
            }
        }
        out.push_band(y, y + 1, std::move(spans));
    }
    return out;
}

Region Region::ellipse(const Rect& r) {
    Region out;
    int w = r.w(), h = r.h();
    if (w <= 0 || h <= 0) return out;
    // Centre and radii measured in half pixels so odd sizes come out symmetric.
    double cx = (r.l + r.r) / 2.0, cy = (r.t + r.b) / 2.0;
    double rx = w / 2.0, ry = h / 2.0;
    for (int y = r.t; y < r.b; ++y) {
        double dy = (y + 0.5 - cy) / ry;
        double t = 1.0 - dy * dy;
        if (t < 0) continue;
        double dx = rx * std::sqrt(t);
        int x0 = static_cast<int>(std::lround(cx - dx));
        int x1 = static_cast<int>(std::lround(cx + dx));
        if (x1 > x0) out.push_band(y, y + 1, {{x0, x1}});
    }
    return out;
}

Region Region::round_rect(const Rect& r, int ew, int eh) {
    Region out;
    if (r.empty()) return out;
    ew = std::min(std::abs(ew), r.w());
    eh = std::min(std::abs(eh), r.h());
    if (ew < 2 || eh < 2) return Region(r);
    double rx = ew / 2.0, ry = eh / 2.0;
    for (int y = r.t; y < r.b; ++y) {
        double yy = y + 0.5;
        double dy = 0;
        if (yy < r.t + ry)
            dy = (r.t + ry - yy) / ry;
        else if (yy > r.b - ry)
            dy = (yy - (r.b - ry)) / ry;
        double t = 1.0 - dy * dy;
        if (t < 0) continue;
        double inset = rx - rx * std::sqrt(t);
        int x0 = static_cast<int>(std::lround(r.l + inset));
        int x1 = static_cast<int>(std::lround(r.r - inset));
        if (x1 > x0) out.push_band(y, y + 1, {{x0, x1}});
    }
    return out;
}

}  // namespace x86emu::win32
