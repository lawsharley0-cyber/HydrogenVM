// HydrogenVM — software rasterizer: paths, strokes, fills, image drawing.
#include "Graphics.hpp"
#include <algorithm>
#include <cstring>

namespace Hydrogen::Graphics {

namespace {
constexpr float kPi = 3.14159265358979f;
int curveSteps(float len) { return std::clamp(int(len / 3.0f) + 2, 4, 64); }
float dist(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }
}

Rect Affine::applyRect(const Rect& r) const {
    const Point p[4] = {apply({r.x, r.y}), apply({r.maxX(), r.y}), apply({r.x, r.maxY()}), apply({r.maxX(), r.maxY()})};
    float x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
    for (const Point& q : p) { x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y); }
    return {x0, y0, x1 - x0, y1 - y0};
}

void channelOffsets(PixelFormat f, int& r, int& g, int& b, int& a) {
    switch (f) {
        case PixelFormat::RGBA: r = 0; g = 1; b = 2; a = 3; break;
        case PixelFormat::BGRA: b = 0; g = 1; r = 2; a = 3; break;
        case PixelFormat::ARGB: a = 0; r = 1; g = 2; b = 3; break;
        case PixelFormat::ABGR: a = 0; b = 1; g = 2; r = 3; break;
        default: r = g = b = a = 0; break;
    }
}

// ------------------------------------------------------------------ path ---

void Path::quadTo(Point c, Point p) {
    if (!hasCurrent) moveTo(c);
    const Point p0 = current;
    const int n = curveSteps(dist(p0, c) + dist(c, p));
    for (int i = 1; i <= n; ++i) {
        const float t = float(i) / float(n), u = 1 - t;
        lineTo({u * u * p0.x + 2 * u * t * c.x + t * t * p.x, u * u * p0.y + 2 * u * t * c.y + t * t * p.y});
    }
}
void Path::cubicTo(Point c1, Point c2, Point p) {
    if (!hasCurrent) moveTo(c1);
    const Point p0 = current;
    const int n = curveSteps(dist(p0, c1) + dist(c1, c2) + dist(c2, p));
    for (int i = 1; i <= n; ++i) {
        const float t = float(i) / float(n), u = 1 - t;
        const float a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, d = t * t * t;
        lineTo({a * p0.x + b * c1.x + cc * c2.x + d * p.x, a * p0.y + b * c1.y + cc * c2.y + d * p.y});
    }
}
void Path::arc(Point c, float r, float a0, float a1, bool clockwise) {
    float sweep = a1 - a0;
    if (clockwise) { while (sweep > 0) sweep -= 2 * kPi; if (sweep < -2 * kPi) sweep = -2 * kPi; }
    else { while (sweep < 0) sweep += 2 * kPi; if (sweep > 2 * kPi) sweep = 2 * kPi; }
    const Point s{c.x + r * std::cos(a0), c.y + r * std::sin(a0)};
    if (hasCurrent) lineTo(s); else moveTo(s);
    const int n = std::max(2, int(std::fabs(sweep) * std::max(r, 1.0f) / 2.0f) + 2);
    const int steps = std::min(n, 256);
    for (int i = 1; i <= steps; ++i) {
        const float t = a0 + sweep * float(i) / float(steps);
        lineTo({c.x + r * std::cos(t), c.y + r * std::sin(t)});
    }
}
void Path::arcTo(Point p1, Point p2, float radius) {
    if (!hasCurrent) { moveTo(p1); return; }
    const Point p0 = current;
    const float v1x = p0.x - p1.x, v1y = p0.y - p1.y, v2x = p2.x - p1.x, v2y = p2.y - p1.y;
    const float l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
    if (l1 == 0 || l2 == 0 || radius == 0) { lineTo(p1); return; }
    const float cosA = (v1x * v2x + v1y * v2y) / (l1 * l2);
    const float angle = std::acos(std::clamp(cosA, -1.0f, 1.0f));
    if (angle < 1e-4f || std::fabs(angle - kPi) < 1e-4f) { lineTo(p1); return; }
    const float tlen = radius / std::tan(angle / 2);
    const Point t1{p1.x + v1x / l1 * tlen, p1.y + v1y / l1 * tlen};
    const Point t2{p1.x + v2x / l2 * tlen, p1.y + v2y / l2 * tlen};
    // centre: along the bisector
    const float bx = v1x / l1 + v2x / l2, by = v1y / l1 + v2y / l2, bl = std::hypot(bx, by);
    const float cd = radius / std::sin(angle / 2);
    const Point c{p1.x + bx / bl * cd, p1.y + by / bl * cd};
    const float a0 = std::atan2(t1.y - c.y, t1.x - c.x), a1 = std::atan2(t2.y - c.y, t2.x - c.x);
    const float cross = v1x * v2y - v1y * v2x;
    lineTo(t1);
    arc(c, radius, a0, a1, cross > 0);
}
void Path::addEllipse(const Rect& r) {
    const Point c{r.x + r.w / 2, r.y + r.h / 2};
    const float rx = r.w / 2, ry = r.h / 2;
    const int n = std::clamp(int((rx + ry) * 1.2f) + 8, 12, 256);
    moveTo({c.x + rx, c.y});
    for (int i = 1; i < n; ++i) {
        const float t = 2 * kPi * float(i) / float(n);
        lineTo({c.x + rx * std::cos(t), c.y + ry * std::sin(t)});
    }
    close();
}
void Path::addRoundedRect(const Rect& r0, float rx, float ry) {
    const Rect r = r0.standardized();
    rx = std::min(rx, r.w / 2); ry = std::min(ry, r.h / 2);
    if (rx <= 0 || ry <= 0) { addRect(r); return; }
    const int n = std::clamp(int((rx + ry) / 2) + 4, 4, 32);
    auto corner = [&](float cx, float cy, float a0) {
        for (int i = 0; i <= n; ++i) {
            const float t = a0 + (kPi / 2) * float(i) / float(n);
            const Point p{cx + rx * std::cos(t), cy + ry * std::sin(t)};
            if (subpaths.empty() || closed.back()) moveTo(p); else lineTo(p);
        }
    };
    corner(r.maxX() - rx, r.y + ry, -kPi / 2);
    corner(r.maxX() - rx, r.maxY() - ry, 0);
    corner(r.x + rx, r.maxY() - ry, kPi / 2);
    corner(r.x + rx, r.y + ry, kPi);
    close();
}
Rect Path::bounds() const {
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (auto& sp : subpaths) for (const Point& p : sp) {
        if (!any) { x0 = x1 = p.x; y0 = y1 = p.y; any = true; continue; }
        x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
    }
    return any ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{};
}
Path Path::transformed(const Affine& t) const {
    Path out = *this;
    for (auto& sp : out.subpaths) for (Point& p : sp) p = t.apply(p);
    out.current = t.apply(current);
    out.start = t.apply(start);
    return out;
}
bool Path::contains(Point p, bool evenOdd) const {
    int winding = 0, crossings = 0;
    for (auto& sp : subpaths) {
        const size_t n = sp.size();
        for (size_t i = 0; i < n; ++i) {
            const Point a = sp[i], b = sp[(i + 1) % n];
            if ((a.y <= p.y) != (b.y <= p.y)) {
                const float x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
                if (x > p.x) { ++crossings; winding += b.y > a.y ? 1 : -1; }
            }
        }
    }
    return evenOdd ? (crossings & 1) : winding != 0;
}

// -------------------------------------------------------------- raster ---

void rasterize(const Path& path, bool evenOdd, int bx0, int by0, int bx1, int by1, std::vector<uint8_t>& coverage) {
    const int w = bx1 - bx0, h = by1 - by0;
    coverage.assign(size_t(std::max(w, 0)) * size_t(std::max(h, 0)), 0);
    if (w <= 0 || h <= 0) return;
    struct Edge { float x0, y0, x1, y1; int dir; };
    std::vector<Edge> edges;
    for (auto& sp : path.subpaths) {
        const size_t n = sp.size();
        if (n < 2) continue;
        for (size_t i = 0; i < n; ++i) {   // fills always close subpaths
            const Point a = sp[i], b = sp[(i + 1) % n];
            if (a.y == b.y) continue;
            if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
            else edges.push_back({b.x, b.y, a.x, a.y, -1});
        }
    }
    if (edges.empty()) return;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
    std::vector<float> acc(size_t(w) + 2);
    struct Cross { float x; int dir; };
    std::vector<Cross> xs;
    constexpr int kSub = 4;
    size_t firstActive = 0;
    for (int y = 0; y < h; ++y) {
        const float py = float(by0 + y);
        std::fill(acc.begin(), acc.end(), 0.0f);
        bool any = false;
        while (firstActive < edges.size() && edges[firstActive].y1 <= py && edges[firstActive].y0 < py - 1e6f) ++firstActive;
        for (int s = 0; s < kSub; ++s) {
            const float sy = py + (float(s) + 0.5f) / kSub;
            xs.clear();
            for (size_t i = 0; i < edges.size(); ++i) {
                const Edge& e = edges[i];
                if (e.y0 > sy) break;
                if (sy >= e.y1) continue;
                xs.push_back({e.x0 + (sy - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0), e.dir});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) { return a.x < b.x; });
            int wind = 0;
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                wind += xs[i].dir;
                const bool inside = evenOdd ? ((i + 1) & 1) : wind != 0;
                if (!inside) continue;
                float xa = xs[i].x - float(bx0), xb = xs[i + 1].x - float(bx0);
                xa = std::clamp(xa, 0.0f, float(w)); xb = std::clamp(xb, 0.0f, float(w));
                if (xb <= xa) continue;
                any = true;
                const int ia = int(xa), ib = int(xb);
                const float wgt = 1.0f / kSub;
                if (ia == ib) { acc[size_t(ia)] += (xb - xa) * wgt; continue; }
                acc[size_t(ia)] += (float(ia + 1) - xa) * wgt;
                for (int k = ia + 1; k < ib; ++k) acc[size_t(k)] += wgt;
                if (ib < w) acc[size_t(ib)] += (xb - float(ib)) * wgt;
            }
        }
        if (!any) continue;
        uint8_t* row = &coverage[size_t(y) * size_t(w)];
        for (int x = 0; x < w; ++x) {
            const float v = acc[size_t(x)];
            row[x] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

namespace {

void addPolygon(Path& out, std::vector<Point> pts) {
    // Normalise orientation (positive signed area) so nonzero filling unions everything.
    float area = 0;
    for (size_t i = 0; i < pts.size(); ++i) { const Point a = pts[i], b = pts[(i + 1) % pts.size()]; area += a.x * b.y - b.x * a.y; }
    if (area < 0) std::reverse(pts.begin(), pts.end());
    out.subpaths.push_back(std::move(pts));
    out.closed.push_back(true);
}
void addDisc(Path& out, Point c, float r) {
    const int n = std::clamp(int(r * 2) + 8, 8, 64);
    std::vector<Point> pts;
    for (int i = 0; i < n; ++i) { const float t = 2 * kPi * float(i) / float(n); pts.push_back({c.x + r * std::cos(t), c.y + r * std::sin(t)}); }
    addPolygon(out, std::move(pts));
}
std::vector<std::vector<Point>> applyDash(const std::vector<Point>& pts, bool closed, const std::vector<float>& dash, float phase) {
    std::vector<std::vector<Point>> out;
    float total = 0;
    for (float d : dash) total += std::max(d, 0.0f);
    if (dash.empty() || total <= 0) { out.push_back(pts); if (closed && !pts.empty()) out.back().push_back(pts.front()); return out; }
    std::vector<Point> p = pts;
    if (closed && !p.empty()) p.push_back(p.front());
    size_t di = 0;
    float left = dash[0], pos = std::fmod(phase, total);
    if (pos < 0) pos += total;
    while (pos > 0) { if (pos >= left) { pos -= left; di = (di + 1) % dash.size(); left = dash[di]; } else { left -= pos; pos = 0; } }
    bool on = (di % 2) == 0;
    std::vector<Point> cur;
    if (on && !p.empty()) cur.push_back(p[0]);
    for (size_t i = 0; i + 1 < p.size(); ++i) {
        Point a = p[i];
        const Point b = p[i + 1];
        float seg = dist(a, b);
        while (seg > 0) {
            if (left >= seg) { left -= seg; if (on) cur.push_back(b); seg = 0; }
            else {
                const float t = left / seg;
                const Point m{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (on) { cur.push_back(m); out.push_back(cur); cur.clear(); }
                else cur = {m};
                on = !on;
                seg -= left;
                a = m;
                di = (di + 1) % dash.size();
                left = dash[di];
            }
        }
    }
    if (on && cur.size() > 1) out.push_back(cur);
    return out;
}

} // namespace

Path strokeToPath(const Path& p, float width, int cap, int join, float miterLimit, const std::vector<float>& dash, float phase) {
    Path out;
    const float hw = std::max(width, 0.0f) / 2;
    if (hw <= 0) return out;
    for (size_t si = 0; si < p.subpaths.size(); ++si) {
        for (auto& pts : applyDash(p.subpaths[si], p.closed[si] && dash.empty() ? false : p.closed[si], dash, phase)) {
            const bool closed = p.closed[si] && dash.empty();
            std::vector<Point> v;
            for (const Point& q : pts) if (v.empty() || dist(v.back(), q) > 1e-6f) v.push_back(q);
            if (closed && v.size() > 2 && dist(v.front(), v.back()) < 1e-6f) v.pop_back();
            if (v.size() == 1) {   // degenerate: dot for round/square caps
                if (cap == 1) addDisc(out, v[0], hw);
                else if (cap == 2) addPolygon(out, {{v[0].x - hw, v[0].y - hw}, {v[0].x + hw, v[0].y - hw}, {v[0].x + hw, v[0].y + hw}, {v[0].x - hw, v[0].y + hw}});
                continue;
            }
            const size_t n = v.size();
            const size_t segs = closed ? n : n - 1;
            for (size_t i = 0; i < segs; ++i) {
                Point a = v[i], b = v[(i + 1) % n];
                const float l = dist(a, b);
                const float dx = (b.x - a.x) / l, dy = (b.y - a.y) / l;
                if (!closed && cap == 2) {
                    if (i == 0) { a.x -= dx * hw; a.y -= dy * hw; }
                    if (i == segs - 1) { b.x += dx * hw; b.y += dy * hw; }
                }
                const float nx = -dy * hw, ny = dx * hw;
                addPolygon(out, {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}});
            }
            // joins
            for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); ++i) {
                const Point prev = v[(i + n - 1) % n], cur = v[i], next = v[(i + 1) % n];
                if (join == 1) { addDisc(out, cur, hw); continue; }
                const float l0 = dist(prev, cur), l1 = dist(cur, next);
                const Point d0{(cur.x - prev.x) / l0, (cur.y - prev.y) / l0}, d1{(next.x - cur.x) / l1, (next.y - cur.y) / l1};
                const float cross = d0.x * d1.y - d0.y * d1.x;
                if (std::fabs(cross) < 1e-6f) continue;
                const float s = cross > 0 ? -1.0f : 1.0f;   // outer side
                const Point n0{-d0.y * hw * s, d0.x * hw * s}, n1{-d1.y * hw * s, d1.x * hw * s};
                const Point o0{cur.x + n0.x, cur.y + n0.y}, o1{cur.x + n1.x, cur.y + n1.y};
                addPolygon(out, {cur, o0, o1});
                if (join == 0) {   // miter
                    const float cosT = -(d0.x * d1.x + d0.y * d1.y);
                    const float theta = std::acos(std::clamp(cosT, -1.0f, 1.0f));
                    const float ratio = 1.0f / std::max(std::sin(theta / 2), 1e-6f);
                    if (ratio <= miterLimit) {
                        const float mx = n0.x + n1.x, my = n0.y + n1.y, ml = std::hypot(mx, my);
                        if (ml > 1e-6f) {
                            const float len = hw * ratio;
                            const Point tip{cur.x + mx / ml * len, cur.y + my / ml * len};
                            addPolygon(out, {o0, tip, o1});
                        }
                    }
                }
            }
            if (!closed && cap == 1) { addDisc(out, v.front(), hw); addDisc(out, v.back(), hw); }
        }
    }
    return out;
}

// --------------------------------------------------------------- blending ---

namespace {

struct Px { float r, g, b, a; };   // premultiplied, 0..1

struct PixelIO {
    const Surface& s;
    int ro, go, bo, ao;
    explicit PixelIO(const Surface& sf) : s(sf) { channelOffsets(sf.format, ro, go, bo, ao); }
    Px load(const uint8_t* p) const {
        switch (s.format) {
            case PixelFormat::A8: return {0, 0, 0, p[0] / 255.0f};
            case PixelFormat::Gray8: { const float v = p[0] / 255.0f; return {v, v, v, 1}; }
            default: {
                const float a = s.hasAlpha ? p[ao] / 255.0f : 1.0f;
                return {p[ro] / 255.0f, p[go] / 255.0f, p[bo] / 255.0f, a};
            }
        }
    }
    void store(uint8_t* p, Px c) const {
        auto q = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        switch (s.format) {
            case PixelFormat::A8: p[0] = q(c.a); return;
            case PixelFormat::Gray8: {
                const float a = c.a > 0 ? c.a : 1;
                p[0] = q((0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / a * (c.a > 0 ? 1 : 0) + (c.a > 0 ? 0 : 0));
                return;
            }
            default:
                if (!s.hasAlpha && c.a > 0 && c.a < 1) { c.r /= c.a; c.g /= c.a; c.b /= c.a; }   // opaque surfaces store unpremultiplied colour
                p[ro] = q(c.r); p[go] = q(c.g); p[bo] = q(c.b); p[ao] = s.hasAlpha ? q(c.a) : 255;
        }
    }
};

inline Px blend(Px s, Px d, float cov, BlendMode m) {
    // s is premultiplied source already scaled by global alpha; cov is coverage (0..1)
    auto lerp = [&](Px res) { return Px{d.r + (res.r - d.r) * cov, d.g + (res.g - d.g) * cov, d.b + (res.b - d.b) * cov, d.a + (res.a - d.a) * cov}; };
    switch (m) {
        case BlendMode::Normal: {
            const float k = 1 - s.a * cov;
            return {s.r * cov + d.r * k, s.g * cov + d.g * k, s.b * cov + d.b * k, s.a * cov + d.a * k};
        }
        case BlendMode::Copy: return lerp(s);
        case BlendMode::Clear: return lerp({0, 0, 0, 0});
        case BlendMode::SourceIn: return lerp({s.r * d.a, s.g * d.a, s.b * d.a, s.a * d.a});
        case BlendMode::DestinationIn: return lerp({d.r * s.a, d.g * s.a, d.b * s.a, d.a * s.a});
        case BlendMode::DestinationOut: return lerp({d.r * (1 - s.a), d.g * (1 - s.a), d.b * (1 - s.a), d.a * (1 - s.a)});
        case BlendMode::PlusLighter: return lerp({std::min(1.0f, s.r + d.r), std::min(1.0f, s.g + d.g), std::min(1.0f, s.b + d.b), std::min(1.0f, s.a + d.a)});
        case BlendMode::Multiply:
            return lerp({s.r * d.r + s.r * (1 - d.a) + d.r * (1 - s.a), s.g * d.g + s.g * (1 - d.a) + d.g * (1 - s.a),
                         s.b * d.b + s.b * (1 - d.a) + d.b * (1 - s.a), s.a + d.a - s.a * d.a});
        case BlendMode::Screen:
            return lerp({s.r + d.r - s.r * d.r, s.g + d.g - s.g * d.g, s.b + d.b - s.b * d.b, s.a + d.a - s.a * d.a});
        case BlendMode::Darken:
            return lerp({std::min(s.r * d.a, d.r * s.a) + s.r * (1 - d.a) + d.r * (1 - s.a), std::min(s.g * d.a, d.g * s.a) + s.g * (1 - d.a) + d.g * (1 - s.a),
                         std::min(s.b * d.a, d.b * s.a) + s.b * (1 - d.a) + d.b * (1 - s.a), s.a + d.a - s.a * d.a});
        case BlendMode::Lighten:
            return lerp({std::max(s.r * d.a, d.r * s.a) + s.r * (1 - d.a) + d.r * (1 - s.a), std::max(s.g * d.a, d.g * s.a) + s.g * (1 - d.a) + d.g * (1 - s.a),
                         std::max(s.b * d.a, d.b * s.a) + s.b * (1 - d.a) + d.b * (1 - s.a), s.a + d.a - s.a * d.a});
        case BlendMode::Overlay: {
            auto ov = [&](float sc, float dc) {
                return 2 * dc < d.a ? 2 * sc * dc + sc * (1 - d.a) + dc * (1 - s.a)
                                    : s.a * d.a - 2 * (d.a - dc) * (s.a - sc) + sc * (1 - d.a) + dc * (1 - s.a);
            };
            return lerp({ov(s.r, d.r), ov(s.g, d.g), ov(s.b, d.b), s.a + d.a - s.a * d.a});
        }
    }
    return d;
}

bool clipBounds(const Surface& dst, const Clip& clip, int& x0, int& y0, int& x1, int& y1) {
    x0 = std::max({x0, 0, clip.x0}); y0 = std::max({y0, 0, clip.y0});
    x1 = std::min({x1, dst.width, clip.x1}); y1 = std::min({y1, dst.height, clip.y1});
    return x1 > x0 && y1 > y0;
}

} // namespace

// Intersect a clip with a device-space path.
Clip intersectClip(const Clip& c, const Path& path, bool evenOdd) {
    const Rect b = path.bounds();
    // Axis-aligned single rectangle on the pixel grid (the common case): no mask needed.
    bool isRect = path.subpaths.size() == 1 && path.subpaths[0].size() == 4;
    if (isRect) {
        const auto& q = path.subpaths[0];
        isRect = ((q[0].y == q[1].y && q[1].x == q[2].x && q[2].y == q[3].y && q[3].x == q[0].x) ||
                  (q[0].x == q[1].x && q[1].y == q[2].y && q[2].x == q[3].x && q[3].y == q[0].y));
        auto onGrid = [](float v) { return std::fabs(v - std::round(v)) < 0.01f; };
        isRect = isRect && onGrid(b.x) && onGrid(b.y) && onGrid(b.maxX()) && onGrid(b.maxY());
    }
    Clip out;
    out.x0 = std::max(c.x0, int(std::floor(b.x)));
    out.y0 = std::max(c.y0, int(std::floor(b.y)));
    out.x1 = std::min(c.x1, int(std::ceil(b.maxX())));
    out.y1 = std::min(c.y1, int(std::ceil(b.maxY())));
    if (out.x1 <= out.x0 || out.y1 <= out.y0) { out.x1 = out.x0; out.y1 = out.y0; return out; }
    if (isRect && !c.mask) {
        out.x0 = std::max(c.x0, int(std::round(b.x))); out.y0 = std::max(c.y0, int(std::round(b.y)));
        out.x1 = std::min(c.x1, int(std::round(b.maxX()))); out.y1 = std::min(c.y1, int(std::round(b.maxY())));
        if (out.x1 < out.x0) out.x1 = out.x0;
        if (out.y1 < out.y0) out.y1 = out.y0;
        return out;
    }
    std::vector<uint8_t> cov;
    rasterize(path, evenOdd, out.x0, out.y0, out.x1, out.y1, cov);
    const int w = out.x1 - out.x0;
    for (int y = out.y0; y < out.y1; ++y)
        for (int xx = out.x0; xx < out.x1; ++xx) {
            uint8_t& v = cov[size_t(y - out.y0) * size_t(w) + size_t(xx - out.x0)];
            v = uint8_t(v * c.at(xx, y) / 255);
        }
    out.mask = std::make_shared<std::vector<uint8_t>>(std::move(cov));
    return out;
}


void fillPath(Surface& dst, const Path& path, bool evenOdd, Color c, const Clip& clip, BlendMode mode, float alpha) {
    const Rect b = path.bounds();
    int x0 = int(std::floor(b.x)), y0 = int(std::floor(b.y)), x1 = int(std::ceil(b.maxX())) + 1, y1 = int(std::ceil(b.maxY())) + 1;
    if (mode == BlendMode::Copy || mode == BlendMode::Clear || mode == BlendMode::SourceIn || mode == BlendMode::DestinationIn) {
        // These modes affect the whole clip even where the path isn't (CG semantics for copy are path-bounded; keep it bounded).
    }
    if (!clipBounds(dst, clip, x0, y0, x1, y1)) return;
    std::vector<uint8_t> cov;
    rasterize(path, evenOdd, x0, y0, x1, y1, cov);
    const int w = x1 - x0;
    const float ca = std::clamp(c.a * alpha, 0.0f, 1.0f);
    const Px src{c.r * ca, c.g * ca, c.b * ca, ca};
    PixelIO io(dst);
    const int bpp = dst.bytesPerPixel();
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = dst.data + size_t(y) * size_t(dst.stride);
        const uint8_t* crow = &cov[size_t(y - y0) * size_t(w)];
        for (int x = x0; x < x1; ++x) {
            uint8_t k = crow[x - x0];
            if (!k) continue;
            const uint8_t m = clip.at(x, y);
            if (!m) continue;
            const float f = (k / 255.0f) * (m / 255.0f);
            uint8_t* p = row + size_t(x) * size_t(bpp);
            io.store(p, blend(src, io.load(p), f, mode));
        }
    }
}

void fillRect(Surface& dst, const Rect& r0, Color c, const Clip& clip, BlendMode mode, float alpha) {
    const Rect r = r0.standardized();
    if (r.empty()) return;
    // Pixel-aligned fast path.
    if (r.x == std::floor(r.x) && r.y == std::floor(r.y) && r.w == std::floor(r.w) && r.h == std::floor(r.h) && !clip.mask) {
        int x0 = int(r.x), y0 = int(r.y), x1 = int(r.maxX()), y1 = int(r.maxY());
        if (!clipBounds(dst, clip, x0, y0, x1, y1)) return;
        const float ca = std::clamp(c.a * alpha, 0.0f, 1.0f);
        const Px src{c.r * ca, c.g * ca, c.b * ca, ca};
        PixelIO io(dst);
        const int bpp = dst.bytesPerPixel();
        if (dst.bytesPerPixel() == 4 && (mode == BlendMode::Copy || (mode == BlendMode::Normal && ca >= 1.0f))) {
            uint8_t px[4];
            io.store(px, src);
            for (int y = y0; y < y1; ++y) {
                uint8_t* p = dst.data + size_t(y) * size_t(dst.stride) + size_t(x0) * 4;
                for (int x = x0; x < x1; ++x, p += 4) std::memcpy(p, px, 4);
            }
            return;
        }
        for (int y = y0; y < y1; ++y) {
            uint8_t* p = dst.data + size_t(y) * size_t(dst.stride) + size_t(x0) * size_t(bpp);
            for (int x = x0; x < x1; ++x, p += bpp) io.store(p, blend(src, io.load(p), 1, mode));
        }
        return;
    }
    Path p;
    p.addRect(r);
    fillPath(dst, p, false, c, clip, mode, alpha);
}

void drawImage(Surface& dst, const Image& img, const Affine& m, const Clip& clip, float alpha, bool smooth, BlendMode mode, const Color* tint) {
    if (img.width <= 0 || img.height <= 0 || alpha <= 0) return;
    const Rect db = m.applyRect({0, 0, float(img.width), float(img.height)});
    int x0 = int(std::floor(db.x)), y0 = int(std::floor(db.y)), x1 = int(std::ceil(db.maxX())), y1 = int(std::ceil(db.maxY()));
    if (!clipBounds(dst, clip, x0, y0, x1, y1)) return;
    const Affine inv = m.inverted();
    PixelIO io(dst);
    const int bpp = dst.bytesPerPixel();
    const uint8_t* src = img.rgba.data();
    const int iw = img.width, ih = img.height;
    // Smooth sampling only matters when scaling; exact pixel copies stay crisp.
    const bool scaled = std::fabs(std::fabs(m.a) - 1) > 1e-3f || std::fabs(std::fabs(m.d) - 1) > 1e-3f || !m.axisAligned();
    const bool bilinear = smooth && scaled;
    auto fetch = [&](int x, int y) -> Px {
        x = std::clamp(x, 0, iw - 1); y = std::clamp(y, 0, ih - 1);
        const uint8_t* p = src + (size_t(y) * size_t(iw) + size_t(x)) * 4;
        return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    };
    for (int y = y0; y < y1; ++y) {
        uint8_t* row = dst.data + size_t(y) * size_t(dst.stride);
        for (int x = x0; x < x1; ++x) {
            const uint8_t cm = clip.at(x, y);
            if (!cm) continue;
            const Point sp = inv.apply({float(x) + 0.5f, float(y) + 0.5f});
            if (sp.x < 0 || sp.y < 0 || sp.x >= float(iw) || sp.y >= float(ih)) continue;
            Px s;
            if (bilinear) {
                const float fx = sp.x - 0.5f, fy = sp.y - 0.5f;
                const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                const float tx = fx - float(ix), ty = fy - float(iy);
                const Px a = fetch(ix, iy), b = fetch(ix + 1, iy), c = fetch(ix, iy + 1), d = fetch(ix + 1, iy + 1);
                auto mix = [&](float p, float q, float r, float t) { return (p * (1 - tx) + q * tx) * (1 - ty) + (r * (1 - tx) + t * tx) * ty; };
                s = {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
            } else {
                s = fetch(int(sp.x), int(sp.y));
            }
            if (tint) s = {tint->r * tint->a * s.a, tint->g * tint->a * s.a, tint->b * tint->a * s.a, tint->a * s.a};
            s = {s.r * alpha, s.g * alpha, s.b * alpha, s.a * alpha};
            if (s.a <= 0 && mode == BlendMode::Normal) continue;
            uint8_t* p = row + size_t(x) * size_t(bpp);
            io.store(p, blend(s, io.load(p), cm / 255.0f, mode));
        }
    }
}

void clear(Surface& dst, Color c) {
    PixelIO io(dst);
    uint8_t px[4] = {0, 0, 0, 0};
    io.store(px, {c.r * c.a, c.g * c.a, c.b * c.a, c.a});
    const int bpp = dst.bytesPerPixel();
    for (int y = 0; y < dst.height; ++y) {
        uint8_t* p = dst.data + size_t(y) * size_t(dst.stride);
        for (int x = 0; x < dst.width; ++x, p += bpp) std::memcpy(p, px, size_t(bpp));
    }
}

ImagePtr snapshot(const Surface& s) {
    auto img = std::make_shared<Image>();
    img->width = s.width;
    img->height = s.height;
    img->rgba.resize(size_t(s.width) * size_t(s.height) * 4);
    PixelIO io(s);
    const int bpp = s.bytesPerPixel();
    bool opaque = true;
    for (int y = 0; y < s.height; ++y) {
        const uint8_t* p = s.data + size_t(y) * size_t(s.stride);
        uint8_t* o = &img->rgba[size_t(y) * size_t(s.width) * 4];
        for (int x = 0; x < s.width; ++x, p += bpp, o += 4) {
            Px c = io.load(p);
            if (s.format == PixelFormat::A8) c = {0, 0, 0, c.a};
            else if (!s.hasAlpha) c.a = 1;
            o[0] = uint8_t(std::clamp(c.r, 0.0f, 1.0f) * 255 + 0.5f);
            o[1] = uint8_t(std::clamp(c.g, 0.0f, 1.0f) * 255 + 0.5f);
            o[2] = uint8_t(std::clamp(c.b, 0.0f, 1.0f) * 255 + 0.5f);
            o[3] = uint8_t(std::clamp(c.a, 0.0f, 1.0f) * 255 + 0.5f);
            if (o[3] != 255) opaque = false;
        }
    }
    img->opaque = opaque;
    return img;
}

} // namespace Hydrogen::Graphics
