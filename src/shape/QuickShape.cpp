#include "shape/QuickShape.h"

#include <QObject>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kPi = 3.14159265358979323846;
// How close to round a fitted ellipse/rectangle must be to become a circle/square.
constexpr double kPerfectTolerance = 0.07;
// Fitted shapes this close to upright are straightened.
constexpr double kUprightTolerance = 4.0;
// A vertex whose turn takes up no more than this much of its shorter side is a corner.
// Measured on hand-drawn strokes: corners, rounded ones included, come out at 0.01 to 0.12, bends at 0.2 and up.
constexpr double kCornerShare = 0.17;

double dist(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }
double dot(const QPointF& a, const QPointF& b) { return a.x() * b.x() + a.y() * b.y(); }
double cross(const QPointF& a, const QPointF& b) { return a.x() * b.y() - a.y() * b.x(); }
double length(const QPointF& v) { return std::hypot(v.x(), v.y()); }
double toDegrees(double radians) { return radians * 180.0 / kPi; }

QPointF rotated(const QPointF& p, double degrees)
{
    const double a = degrees * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {c * p.x() - s * p.y(), s * p.x() + c * p.y()};
}

double pathLength(const std::vector<QPointF>& p)
{
    double total = 0.0;
    for (size_t i = 1; i < p.size(); ++i)
        total += dist(p[i - 1], p[i]);
    return total;
}

// Evenly spaced points along the path, so every part of the stroke weighs the
// same no matter how fast it was drawn.
std::vector<QPointF> resample(const std::vector<QPointF>& p, int n)
{
    std::vector<QPointF> out;
    out.reserve(size_t(n));
    out.push_back(p.front());
    const double step = pathLength(p) / double(n - 1);
    QPointF prev = p.front();
    double need = step;
    size_t i = 1;
    while (int(out.size()) < n - 1 && i < p.size()) {
        const double d = dist(prev, p[i]);
        if (d >= need && d > 1e-12) {
            prev += (p[i] - prev) * (need / d);
            out.push_back(prev);
            need = step;
        } else {
            need -= d;
            prev = p[i];
            ++i;
        }
    }
    while (int(out.size()) < n)
        out.push_back(p.back());
    return out;
}

double segmentDistance(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double len2 = dot(ab, ab);
    if (len2 < 1e-12)
        return dist(p, a);
    const double t = std::clamp(dot(p - a, ab) / len2, 0.0, 1.0);
    return dist(p, a + ab * t);
}

// Angle in degrees between two directions (0 = straight on).
double turnBetween(const QPointF& a, const QPointF& b)
{
    const double la = length(a), lb = length(b);
    if (la < 1e-9 || lb < 1e-9)
        return 0.0;
    return toDegrees(std::acos(std::clamp(dot(a, b) / (la * lb), -1.0, 1.0)));
}

// Ramer-Douglas-Peucker: indices strictly between a and b that deviate from the
// chord by more than eps.
void rdp(const std::vector<QPointF>& p, int a, int b, double eps, std::vector<int>& keep)
{
    double worst = 0.0;
    int index = -1;
    for (int i = a + 1; i < b; ++i) {
        const double d = segmentDistance(p[size_t(i)], p[size_t(a)], p[size_t(b)]);
        if (d > worst) {
            worst = d;
            index = i;
        }
    }
    if (index >= 0 && worst > eps) {
        rdp(p, a, index, eps, keep);
        keep.push_back(index);
        rdp(p, index, b, eps, keep);
    }
}

// Corner candidates of a stroke as indices into p. Shallow bends and stubby
// edges are folded away so a wobbly side stays one side.
std::vector<int> findVertices(const std::vector<QPointF>& p, bool closed, double eps, double minTurn, double minEdge)
{
    const int n = int(p.size());
    std::vector<int> idx;
    if (closed) {
        int far = 0;
        double farDist = 0.0;
        for (int i = 1; i < n; ++i) {
            const double d = dist(p[0], p[size_t(i)]);
            if (d > farDist) {
                farDist = d;
                far = i;
            }
        }
        idx.push_back(0);
        rdp(p, 0, far, eps, idx);
        idx.push_back(far);
        rdp(p, far, n - 1, eps, idx);   // the last sample sits beside the first, so it is not a vertex
    } else {
        idx.push_back(0);
        rdp(p, 0, n - 1, eps, idx);
        idx.push_back(n - 1);
    }

    const size_t minCount = closed ? 3 : 2;
    auto at = [&](int k) -> const QPointF& {
        const int m = int(idx.size());
        return p[size_t(idx[size_t(((k % m) + m) % m)])];
    };
    auto removable = [&](int k) { return closed || (k > 0 && k < int(idx.size()) - 1); };
    auto turnAt = [&](int k) { return turnBetween(at(k) - at(k - 1), at(k + 1) - at(k)); };

    while (idx.size() > minCount) {
        // Weakest bend first.
        int weakest = -1;
        double weakestTurn = minTurn;
        for (int k = 0; k < int(idx.size()); ++k) {
            if (!removable(k))
                continue;
            const double t = turnAt(k);
            if (t < weakestTurn) {
                weakestTurn = t;
                weakest = k;
            }
        }
        if (weakest < 0) {
            // Then edges too short to be a side of their own.
            const int edges = closed ? int(idx.size()) : int(idx.size()) - 1;
            for (int k = 0; k < edges && weakest < 0; ++k) {
                if (dist(at(k), at(k + 1)) >= minEdge)
                    continue;
                const int next = (k + 1) % int(idx.size());
                const bool canK = removable(k), canNext = removable(next);
                if (canK && canNext)
                    weakest = turnAt(k) < turnAt(next) ? k : next;
                else if (canK || canNext)
                    weakest = canK ? k : next;
            }
        }
        if (weakest < 0)
            break;
        idx.erase(idx.begin() + weakest);
    }
    return idx;
}

struct FittedLine {
    QPointF point, dir;
    bool ok = false;
};

// Total-least-squares line through a run of samples.
FittedLine fitLine(const std::vector<QPointF>& pts)
{
    FittedLine line;
    if (pts.size() < 2)
        return line;
    QPointF mean;
    for (const QPointF& q : pts)
        mean += q;
    mean /= double(pts.size());
    double sxx = 0, sxy = 0, syy = 0;
    for (const QPointF& q : pts) {
        const QPointF d = q - mean;
        sxx += d.x() * d.x();
        sxy += d.x() * d.y();
        syy += d.y() * d.y();
    }
    if (sxx + syy < 1e-9)
        return line;
    const double theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    line.point = mean;
    line.dir = QPointF(std::cos(theta), std::sin(theta));
    line.ok = true;
    return line;
}

// Hand-drawn corners are rounded, so the vertices found on the stroke sit
// inside the true corner. Fit a line to the middle of each side and intersect
// neighbours to recover crisp corners.
std::vector<QPointF> refineVertices(const std::vector<QPointF>& p, const std::vector<int>& idx, bool closed, double diag)
{
    const int m = int(idx.size());
    const int n = int(p.size());
    const int edges = closed ? m : m - 1;
    std::vector<FittedLine> lines(static_cast<size_t>(edges));
    for (int e = 0; e < edges; ++e) {
        const int a = idx[size_t(e)];
        const int b = idx[size_t((e + 1) % m)];
        std::vector<QPointF> run;
        for (int i = a; i != b; i = (i + 1) % n)
            run.push_back(p[size_t(i)]);
        run.push_back(p[size_t(b)]);
        const size_t skip = run.size() * 15 / 100;
        if (run.size() - 2 * skip >= 3)
            run = std::vector<QPointF>(run.begin() + long(skip), run.end() - long(skip));
        lines[size_t(e)] = fitLine(run);
    }

    std::vector<QPointF> out(static_cast<size_t>(m));
    for (int k = 0; k < m; ++k) {
        const QPointF orig = p[size_t(idx[size_t(k)])];
        out[size_t(k)] = orig;
        if (!closed && (k == 0 || k == m - 1)) {
            const FittedLine& l = lines[size_t(k == 0 ? 0 : edges - 1)];
            if (l.ok)
                out[size_t(k)] = l.point + l.dir * dot(orig - l.point, l.dir);
            continue;
        }
        const FittedLine& before = lines[size_t((k - 1 + edges) % edges)];
        const FittedLine& after = lines[size_t(k % edges)];
        if (!before.ok || !after.ok)
            continue;
        const double denom = cross(before.dir, after.dir);
        if (std::abs(denom) < 0.2)
            continue;   // nearly parallel sides: the intersection would be unstable
        const double t = cross(after.point - before.point, after.dir) / denom;
        const QPointF x = before.point + before.dir * t;
        if (dist(x, orig) <= 0.25 * diag)
            out[size_t(k)] = x;
    }
    return out;
}

double polygonError(const std::vector<QPointF>& p, const std::vector<QPointF>& v, bool closed)
{
    const size_t edges = closed ? v.size() : v.size() - 1;
    double sum = 0.0;
    for (const QPointF& q : p) {
        double best = std::numeric_limits<double>::max();
        for (size_t e = 0; e < edges; ++e)
            best = std::min(best, segmentDistance(q, v[e], v[(e + 1) % v.size()]));
        sum += best * best;
    }
    return std::sqrt(sum / double(p.size()));
}

// Counts sharp corners: places where the direction changes by more than
// `threshold` degrees across a short window. A circle spreads its turning
// evenly and never gets there; a polygon concentrates it at the vertices.
// A running average along an open stroke: takes the tremor out of a hand-drawn line.
std::vector<QPointF> smoothed(const std::vector<QPointF>& p, int radius)
{
    const int n = int(p.size());
    if (radius < 1 || n < 3)
        return p;
    std::vector<QPointF> out(p.size());
    for (int i = 0; i < n; ++i) {
        // Shrink the window towards the ends so they stay where they are.
        const int r = std::min({radius, i, n - 1 - i});
        QPointF sum;
        for (int k = i - r; k <= i + r; ++k)
            sum += p[size_t(k)];
        out[size_t(i)] = sum / double(2 * r + 1);
    }
    return out;
}

// What each vertex of an open stroke is: a corner, where the line changes
// direction in a short stretch between two straighter sides, or a bend, where
// the turning is spread along the sides themselves.
//
// The measure is how much of the shorter neighbouring side the turn takes up:
// the stretch around the vertex in which four fifths of the turn happens, set
// against the length of that side. It is a ratio of lengths within the stroke,
// so it means the same for a small drawing as for a large one, and it reads
// the direction of the line over a baseline long enough to see past an
// unsteady hand (`pixel` is how large a screen pixel is in stroke units).
struct VertexTurn {
    double turn = 0.0;    // degrees, how far the direction changes from one side to the next
    double share = 1.0;   // of the shorter side, taken up by the turn
};

std::vector<VertexTurn> vertexTurns(const std::vector<QPointF>& p, const std::vector<int>& idx, double diag, double pixel)
{
    const int n = int(p.size());
    std::vector<VertexTurn> out(idx.size());
    if (n < 8 || idx.size() < 3)
        return out;
    const double spacing = pathLength(p) / std::max(1, n - 1);
    const int reach = std::max(2, int(std::lround(std::max(5.0 * pixel, 0.015 * diag) / std::max(spacing, 1e-6))));
    // The direction of the line at every sample, in degrees, carried on past 180 rather than wrapped.
    std::vector<double> heading(static_cast<size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const QPointF d = p[size_t(std::min(n - 1, i + reach))] - p[size_t(std::max(0, i - reach))];
        double a = toDegrees(std::atan2(d.y(), d.x()));
        if (i > 0) {
            while (a - heading[size_t(i - 1)] > 180.0)
                a -= 360.0;
            while (a - heading[size_t(i - 1)] < -180.0)
                a += 360.0;
        }
        heading[size_t(i)] = a;
    }
    for (size_t k = 1; k + 1 < idx.size(); ++k) {
        const int at = idx[k], before = (idx[k - 1] + at) / 2, after = (at + idx[k + 1]) / 2;
        const double turn = heading[size_t(after)] - heading[size_t(before)];
        out[k].turn = turn;
        if (std::abs(turn) < 1.0)
            continue;
        // Widen a window about the vertex until it holds four fifths of the turn.
        int half = 1;
        const int limit = std::max(1, std::min(at - before, after - at));
        while (half < limit && std::abs(heading[size_t(at + half)] - heading[size_t(at - half)]) < 0.8 * std::abs(turn))
            ++half;
        // Reading the direction over a baseline smears even the sharpest corner
        // across that baseline; that much of the window is the measuring, not the line.
        const double width = std::max(0.0, 2.0 * half - 1.6 * reach) * spacing;
        const double side = std::min(at - idx[k - 1], idx[k + 1] - at) * spacing;
        out[k].share = width / std::max(side, 1e-6);
    }
    return out;
}

// Centripetal Catmull-Rom: the point at t (0..1) between b and c, with a and d
// as the neighbours that shape the bend. This flavour never loops or overshoots.
QPointF splinePoint(const QPointF& a, const QPointF& b, const QPointF& c, const QPointF& d, double t)
{
    auto knot = [](double from, const QPointF& u, const QPointF& v) { return from + std::sqrt(std::max(dist(u, v), 1e-6)); };
    const double t0 = 0.0, t1 = knot(t0, a, b), t2 = knot(t1, b, c), t3 = knot(t2, c, d);
    const double u = t1 + (t2 - t1) * t;
    auto mix = [](const QPointF& p0, const QPointF& p1, double ta, double tb, double at) {
        return p0 * ((tb - at) / (tb - ta)) + p1 * ((at - ta) / (tb - ta));
    };
    const QPointF a1 = mix(a, b, t0, t1, u), a2 = mix(b, c, t1, t2, u), a3 = mix(c, d, t2, t3, u);
    const QPointF b1 = mix(a1, a2, t0, t2, u), b2 = mix(a2, a3, t1, t3, u);
    return mix(b1, b2, t1, t2, u);
}

// The neighbours of segment k of an open run of anchors; past either end the
// line is imagined to carry straight on.
void splineNeighbours(const std::vector<QPointF>& v, size_t k, QPointF& before, QPointF& after)
{
    before = k > 0 ? v[k - 1] : v[0] * 2.0 - v[1];
    after = k + 2 < v.size() ? v[k + 2] : v[k + 1] * 2.0 - v[k];
}

// A smooth line through as few points as will follow the stroke closely: start
// with the places it strays furthest from a straight run, then keep adding a
// point wherever the smooth line and the stroke are furthest apart.
std::vector<QPointF> fitCurveAnchors(const std::vector<QPointF>& stroke, double diag)
{
    const int n = int(stroke.size());
    const std::vector<QPointF> p = smoothed(stroke, std::max(1, n / 80));
    std::vector<int> idx{0};
    rdp(p, 0, n - 1, 0.06 * diag, idx);
    idx.push_back(n - 1);
    const double tolerance = std::max(1.2, 0.007 * diag);
    const int gap = std::max(2, n / 60);   // anchors closer than this only make the line lumpy
    for (int round = 0; round < 40 && idx.size() < 28; ++round) {
        std::vector<QPointF> anchors;
        for (int i : idx)
            anchors.push_back(p[size_t(i)]);
        double worst = 0.0;
        int worstAt = -1;
        for (size_t k = 0; k + 1 < idx.size(); ++k) {
            QPointF before, after;
            splineNeighbours(anchors, k, before, after);
            constexpr int kSamples = 14;
            QPointF samples[kSamples + 1];
            for (int j = 0; j <= kSamples; ++j)
                samples[j] = splinePoint(before, anchors[k], anchors[k + 1], after, double(j) / kSamples);
            for (int i = idx[k] + gap; i <= idx[k + 1] - gap; ++i) {
                double nearest = std::numeric_limits<double>::max();
                for (int j = 0; j < kSamples; ++j)
                    nearest = std::min(nearest, segmentDistance(p[size_t(i)], samples[j], samples[j + 1]));
                if (nearest > worst) {
                    worst = nearest;
                    worstAt = i;
                }
            }
        }
        if (worstAt < 0 || worst <= tolerance)
            break;
        idx.insert(std::upper_bound(idx.begin(), idx.end(), worstAt), worstAt);
    }
    std::vector<QPointF> anchors;
    for (int i : idx)
        anchors.push_back(p[size_t(i)]);
    return anchors;
}

int countCorners(const std::vector<QPointF>& p, bool closed, double threshold)
{
    const int n = int(p.size());
    const int w = std::max(2, n / 24);
    std::vector<double> turn(size_t(n), 0.0);
    for (int i = 0; i < n; ++i) {
        int a = i - w, b = i + w;
        if (closed) {
            a = ((a % n) + n) % n;
            b = b % n;
        } else if (a < 0 || b >= n) {
            continue;
        }
        turn[size_t(i)] = turnBetween(p[size_t(i)] - p[size_t(a)], p[size_t(b)] - p[size_t(i)]);
    }
    int corners = 0;
    for (;;) {
        const auto it = std::max_element(turn.begin(), turn.end());
        if (*it < threshold)
            break;
        ++corners;
        const int peak = int(it - turn.begin());
        for (int d = -2 * w; d <= 2 * w; ++d) {
            int j = peak + d;
            if (closed)
                j = ((j % n) + n) % n;
            else if (j < 0 || j >= n)
                continue;
            turn[size_t(j)] = 0.0;
        }
    }
    return corners;
}

// Least-squares circle (Kasa fit, centred for numerical stability).
bool fitCircle(const std::vector<QPointF>& p, QPointF& centre, double& radius)
{
    QPointF mean;
    for (const QPointF& q : p)
        mean += q;
    mean /= double(p.size());
    double sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
    for (const QPointF& q : p) {
        const double x = q.x() - mean.x(), y = q.y() - mean.y();
        const double z = x * x + y * y;
        sxx += x * x;
        sxy += x * y;
        syy += y * y;
        sxz += x * z;
        syz += y * z;
    }
    const double det = sxx * syy - sxy * sxy;
    if (std::abs(det) < 1e-9 * (sxx + syy) * (sxx + syy) + 1e-12)
        return false;
    const double uc = 0.5 * (sxz * syy - syz * sxy) / det;
    const double vc = 0.5 * (syz * sxx - sxz * sxy) / det;
    centre = mean + QPointF(uc, vc);
    radius = std::sqrt(uc * uc + vc * vc + (sxx + syy) / double(p.size()));
    return std::isfinite(radius);
}

struct FittedEllipse {
    QPointF centre;
    double a = 0, b = 0, theta = 0;   // radii along / across the principal axis (radians)
    double error = 0;                 // RMS deviation in pixels
    bool ok = false;
};

FittedEllipse fitEllipse(const std::vector<QPointF>& p)
{
    FittedEllipse e;
    QPointF mean;
    for (const QPointF& q : p)
        mean += q;
    mean /= double(p.size());
    double sxx = 0, sxy = 0, syy = 0;
    for (const QPointF& q : p) {
        const QPointF d = q - mean;
        sxx += d.x() * d.x();
        sxy += d.x() * d.y();
        syy += d.y() * d.y();
    }
    // Orientation from the principal axes, then the radii by least squares on
    // u^2/a^2 + v^2/b^2 = 1 in that frame.
    e.theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    const double c = std::cos(e.theta), s = std::sin(e.theta);
    double suuuu = 0, suuvv = 0, svvvv = 0, suu = 0, svv = 0;
    std::vector<QPointF> local;
    local.reserve(p.size());
    for (const QPointF& q : p) {
        const QPointF d = q - mean;
        const double u = c * d.x() + s * d.y(), v = -s * d.x() + c * d.y();
        local.emplace_back(u, v);
        suu += u * u;
        svv += v * v;
        suuuu += u * u * u * u;
        suuvv += u * u * v * v;
        svvvv += v * v * v * v;
    }
    const double det = suuuu * svvvv - suuvv * suuvv;
    double A = 0, B = 0;
    if (std::abs(det) > 1e-12) {
        A = (suu * svvvv - svv * suuvv) / det;
        B = (svv * suuuu - suu * suuvv) / det;
    }
    if (A <= 0 || B <= 0) {
        A = double(p.size()) / (2.0 * std::max(suu, 1e-9));
        B = double(p.size()) / (2.0 * std::max(svv, 1e-9));
    }
    e.centre = mean;
    e.a = 1.0 / std::sqrt(A);
    e.b = 1.0 / std::sqrt(B);
    if (!std::isfinite(e.a) || !std::isfinite(e.b) || e.a < 1.0 || e.b < 1.0)
        return e;
    const double scale = std::sqrt(e.a * e.b);
    double sum = 0;
    for (const QPointF& q : local) {
        const double r = std::sqrt(q.x() * q.x() / (e.a * e.a) + q.y() * q.y() / (e.b * e.b));
        sum += (r - 1.0) * (r - 1.0) * scale * scale;
    }
    e.error = std::sqrt(sum / double(p.size()));
    e.ok = true;
    return e;
}

double shoelaceArea(const std::vector<QPointF>& p)
{
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i)
        a += cross(p[i], p[(i + 1) % p.size()]);
    return std::abs(a) * 0.5;
}

bool circleThrough(const QPointF& a, const QPointF& b, const QPointF& c, QPointF& centre, double& radius)
{
    const double d = 2.0 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y()) + c.x() * (a.y() - b.y()));
    const double scale = dist(a, c);
    if (std::abs(d) < 1e-6 * scale * scale + 1e-12)
        return false;
    const double a2 = dot(a, a), b2 = dot(b, b), c2 = dot(c, c);
    centre = QPointF((a2 * (b.y() - c.y()) + b2 * (c.y() - a.y()) + c2 * (a.y() - b.y())) / d,
                     (a2 * (c.x() - b.x()) + b2 * (a.x() - c.x()) + c2 * (b.x() - a.x())) / d);
    radius = dist(centre, a);
    return radius < 60.0 * scale;   // flatter than that is a straight line for all practical purposes
}

// Where the arc through three points starts and how far round it goes, the
// way that passes through the middle one.
bool arcThrough(const std::vector<QPointF>& pts, QPointF& c, double& r, double& a0, double& sweep, double& toMiddle)
{
    if (pts.size() < 3 || !circleThrough(pts[0], pts[1], pts[2], c, r))
        return false;
    a0 = std::atan2(pts[0].y() - c.y(), pts[0].x() - c.x());
    const double a1 = std::atan2(pts[1].y() - c.y(), pts[1].x() - c.x());
    const double a2 = std::atan2(pts[2].y() - c.y(), pts[2].x() - c.x());
    auto positive = [](double a) { return std::fmod(std::fmod(a, 2.0 * kPi) + 2.0 * kPi, 2.0 * kPi); };
    sweep = positive(a2 - a0);
    toMiddle = positive(a1 - a0);
    if (toMiddle > sweep) {
        sweep -= 2.0 * kPi;
        toMiddle -= 2.0 * kPi;
    }
    return true;
}

// One run of a Path, from a point to the next, with the two handles between.
struct Cubic {
    QPointF a, b, c, d;
    QPointF at(double t) const
    {
        // Even steps along a straight run; the general form would bunch them at the ends.
        if (straight())
            return a + (d - a) * t;
        const double u = 1.0 - t;
        return a * (u * u * u) + b * (3.0 * u * u * t) + c * (3.0 * u * t * t) + d * (t * t * t);
    }
    bool straight() const { return dist(a, b) < 1e-9 && dist(c, d) < 1e-9; }
    // Between the chord and the frame of the handles, which bracket it.
    double roughLength() const { return (dist(a, d) + dist(a, b) + dist(b, c) + dist(c, d)) * 0.5; }
};

Cubic cubicOf(const QuickShape& s, size_t segment)
{
    const size_t next = (segment + 1) % s.nodes.size();
    return {s.nodes[segment].pos, s.handleOut(segment), s.handleIn(next), s.nodes[next].pos};
}

// Complex product and quotient, for turning and stretching a handle.
QPointF bent(const QPointF& v, const QPointF& by) { return {v.x() * by.x() - v.y() * by.y(), v.x() * by.y() + v.y() * by.x()}; }
QPointF bendBetween(const QPointF& to, const QPointF& from)
{
    const double d = dot(from, from);
    return {(to.x() * from.x() + to.y() * from.y()) / d, (to.y() * from.x() - to.x() * from.y()) / d};
}

// A point's handles as offsets from it.
void nodeHandles(const QuickShape& s, size_t k, QPointF& in, QPointF& out)
{
    const QuickShape::Node& n = s.nodes[k];
    const size_t count = s.nodes.size();
    in = out = QPointF();
    if (!n.smooth || count < 2)
        return;
    const bool hasPrev = s.closedPath || k > 0, hasNext = s.closedPath || k + 1 < count;
    // What the neighbours give it: the handles of the centripetal spline a
    // Curve is drawn with. Past either end the line is imagined to carry
    // straight on.
    const QPointF p = n.pos;
    QPointF a = hasPrev ? s.nodes[(k + count - 1) % count].pos : QPointF();
    QPointF c = hasNext ? s.nodes[(k + 1) % count].pos : QPointF();
    if (!hasPrev)
        a = p * 2.0 - c;
    if (!hasNext)
        c = p * 2.0 - a;
    const double da = std::sqrt(std::max(dist(p, a), 1e-6)), dc = std::sqrt(std::max(dist(c, p), 1e-6));
    const QPointF tangent = (c - p) * (da * da) + (p - a) * (dc * dc);
    if (hasNext)
        out = bent(tangent / (3.0 * da * (da + dc)), n.bendOut);
    if (hasPrev)
        in = bent(-tangent / (3.0 * dc * (da + dc)), n.bendIn);
}

// Gives a smooth point exactly these handles, whatever its neighbours would.
void pinHandles(QuickShape& s, size_t k, const QPointF& in, const QPointF& out)
{
    QuickShape::Node& n = s.nodes[k];
    if (!n.smooth)
        return;
    n.bendIn = n.bendOut = QPointF(1.0, 0.0);
    QPointF plainIn, plainOut;
    nodeHandles(s, k, plainIn, plainOut);
    if (length(plainIn) > 1e-9)
        n.bendIn = bendBetween(in, plainIn);
    if (length(plainOut) > 1e-9)
        n.bendOut = bendBetween(out, plainOut);
}

QuickShape makeLine(const QPointF& a, const QPointF& b)
{
    QuickShape s;
    s.kind = QuickShape::Kind::Line;
    s.pts = {a, b};
    QuickShape::snapLineEnd(s.pts[0], s.pts[1], false);
    return s;
}

QuickShape makePolygon(std::vector<QPointF> vertices)
{
    QuickShape s;
    s.kind = QuickShape::Kind::Polygon;
    s.pts = std::move(vertices);
    if (s.pts.size() != 4)
        return s;
    // Four sides with roughly right angles: a rectangle.
    for (size_t k = 0; k < 4; ++k) {
        const double turn = turnBetween(s.pts[k] - s.pts[(k + 3) % 4], s.pts[(k + 1) % 4] - s.pts[k]);
        if (std::abs(turn - 90.0) > 22.0)
            return s;
    }
    QuickShape r = s.asRectangle();
    if (std::abs(r.rx - r.ry) <= kPerfectTolerance * std::max(r.rx, r.ry))
        r.makePerfect();
    return r;
}

} // namespace

// ------------------------------------------------------------- recognition

std::vector<QPointF> QuickShape::withoutRest(const std::vector<QPointF>& stroke, double radius)
{
    if (stroke.size() < 3 || radius <= 0.0)
        return stroke;
    // The rest begins at the last point the pen then stayed within reach of:
    // found the way the wait itself is timed, by moving the mark along
    // whenever the pen leaves its circle.
    size_t anchor = 0;
    for (size_t i = 1; i < stroke.size(); ++i) {
        if (dist(stroke[i], stroke[anchor]) > radius)
            anchor = i;
    }
    return std::vector<QPointF>(stroke.begin(), stroke.begin() + std::ptrdiff_t(std::max<size_t>(anchor, 1)) + 1);
}

QuickShape QuickShape::recognize(const std::vector<QPointF>& stroke, double pixel)
{
    std::vector<QPointF> clean;
    for (const QPointF& q : stroke) {
        if (clean.empty() || dist(q, clean.back()) > 0.75)
            clean.push_back(q);
    }
    if (clean.size() < 2)
        return {};
    const double L = pathLength(clean);
    if (L < 12.0)
        return {};

    std::vector<QPointF> p = resample(clean, std::clamp(int(L / 3.0), 48, 320));
    const QPointF first = p.front();
    double chord = dist(first, p.back());

    // Closed if the ends nearly meet, or if the tail runs back past the start.
    bool closed = chord < 0.14 * L;
    if (!closed) {
        int best = -1;
        double bestDist = std::numeric_limits<double>::max();
        for (int i = int(p.size()) * 7 / 10; i < int(p.size()); ++i) {
            const double d = dist(p[size_t(i)], first);
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        if (best > 0 && bestDist < 0.07 * L) {
            closed = true;
            p.resize(size_t(best) + 1);
        }
    }
    // A stroke that doubles back on itself encloses nothing: treat it as open.
    if (closed && shoelaceArea(p) < 0.012 * L * L)
        closed = false;

    double minX = p[0].x(), maxX = minX, minY = p[0].y(), maxY = minY;
    for (const QPointF& q : p) {
        minX = std::min(minX, q.x());
        maxX = std::max(maxX, q.x());
        minY = std::min(minY, q.y());
        maxY = std::max(maxY, q.y());
    }
    const double diag = std::hypot(maxX - minX, maxY - minY);
    const double eps = std::max(1.5, 0.04 * diag);
    const QPointF last = p.back();
    chord = dist(first, last);

    if (!closed) {
        double maxDev = 0.0;
        for (const QPointF& q : p)
            maxDev = std::max(maxDev, segmentDistance(q, first, last));
        if (chord > 1e-6 && maxDev <= 0.07 * chord && L <= 1.15 * chord)
            return makeLine(first, last);

        // A curve without corners that sits on a circle is an arc.
        if (countCorners(p, false, 50.0) == 0) {
            QPointF c;
            double r = 0;
            if (fitCircle(p, c, r) && r < 20.0 * L) {
                double sum = 0.0;
                for (const QPointF& q : p)
                    sum += (dist(q, c) - r) * (dist(q, c) - r);
                if (std::sqrt(sum / double(p.size())) < 0.03 * L) {
                    auto onCircle = [&](const QPointF& q) {
                        const QPointF d = q - c;
                        return c + d * (r / std::max(length(d), 1e-9));
                    };
                    QuickShape arc;
                    arc.kind = Kind::Arc;
                    arc.pts = {onCircle(first), onCircle(p[p.size() / 2]), onCircle(last)};
                    return arc;
                }
            }
        }

        const std::vector<int> idx = findVertices(p, false, eps, 22.0, 0.08 * diag);
        std::vector<QPointF> v = refineVertices(p, idx, false, diag);
        if (v.size() == 2)
            return makeLine(v[0], v[1]);
        // Where the line turns, does it turn corners or does it bend? Whichever
        // accounts for more of the turning decides; a line that bends is a
        // curve, and straightening it into segments would lose what was drawn.
        double inCorners = 0.0, inBends = 0.0;
        const std::vector<VertexTurn> turns = vertexTurns(p, idx, diag, pixel);
        for (size_t k = 1; k + 1 < turns.size(); ++k) {
            if (std::abs(turns[k].turn) < 20.0)
                continue;
            (turns[k].share <= kCornerShare ? inCorners : inBends) += std::abs(turns[k].turn);
        }
        if (inCorners <= inBends) {
            QuickShape curve;
            curve.kind = Kind::Curve;
            curve.pts = fitCurveAnchors(p, diag);
            if (curve.pts.size() >= 3)
                return curve;
        }
        QuickShape poly;
        poly.kind = Kind::Polyline;
        poly.pts = std::move(v);
        return poly;
    }

    const FittedEllipse ell = fitEllipse(p);
    const std::vector<int> idx = findVertices(p, true, eps, 25.0, 0.1 * diag);
    std::vector<QPointF> v;
    double polyError = std::numeric_limits<double>::max();
    if (idx.size() >= 3) {
        v = refineVertices(p, idx, true, diag);
        polyError = polygonError(p, v, true);
    }
    const bool polygonLike = v.size() >= 3 && v.size() <= 8 && countCorners(p, true, 50.0) >= 3;
    const bool ellipseOk = ell.ok && ell.error < 0.1 * std::sqrt(ell.a * ell.b);

    const bool choosePolygon = (polygonLike && (!ellipseOk || polyError <= ell.error * 1.15)) || (!ellipseOk && v.size() >= 3);
    if (choosePolygon)
        return makePolygon(std::move(v));
    if (!ell.ok)
        return {};

    QuickShape s;
    s.kind = Kind::Ellipse;
    s.center = ell.centre;
    s.rx = ell.a;
    s.ry = ell.b;
    s.rot = toDegrees(ell.theta);
    if (std::abs(s.rx - s.ry) <= kPerfectTolerance * std::max(s.rx, s.ry)) {
        s.makePerfect();
        s.rot = 0.0;   // nothing to orient on a circle; keep its handles upright
    } else if (std::abs(s.rot) < kUprightTolerance) {
        s.rot = 0.0;
    } else if (std::abs(std::abs(s.rot) - 90.0) < kUprightTolerance) {
        std::swap(s.rx, s.ry);
        s.rot = 0.0;
    }
    return s;
}

// ---------------------------------------------------------------- geometry

bool QuickShape::perfect() const
{
    return (kind == Kind::Ellipse || kind == Kind::Rectangle) && std::abs(rx - ry) <= 1e-6 * std::max(rx, ry);
}

QString QuickShape::name() const
{
    switch (kind) {
    case Kind::Line: return QObject::tr("Line");
    case Kind::Arc: return QObject::tr("Arc");
    case Kind::Ellipse: return perfect() ? QObject::tr("Circle") : QObject::tr("Ellipse");
    case Kind::Rectangle: return perfect() ? QObject::tr("Square") : QObject::tr("Rectangle");
    case Kind::Polygon:
        return pts.size() == 3 ? QObject::tr("Triangle") : pts.size() == 4 ? QObject::tr("Quadrilateral") : QObject::tr("Polygon");
    case Kind::Polyline: return QObject::tr("Polyline");
    case Kind::Curve: return QObject::tr("Curve");
    case Kind::Path: {
        const bool sharp = std::none_of(nodes.begin(), nodes.end(), [](const Node& n) { return n.smooth; });
        if (!closedPath)
            return !sharp ? QObject::tr("Curve") : nodes.size() == 2 ? QObject::tr("Line") : QObject::tr("Polyline");
        if (!sharp)
            return QObject::tr("Shape");
        return nodes.size() == 3 ? QObject::tr("Triangle") : nodes.size() == 4 ? QObject::tr("Quadrilateral") : QObject::tr("Polygon");
    }
    case Kind::None: break;
    }
    return {};
}

std::vector<QPointF> QuickShape::smoothThrough(const std::vector<QPointF>& anchors, double step)
{
    std::vector<QPointF> out;
    if (anchors.size() < 2)
        return anchors;
    step = std::max(step, 0.5);
    out.push_back(anchors[0]);
    for (size_t k = 0; k + 1 < anchors.size(); ++k) {
        QPointF before, after;
        splineNeighbours(anchors, k, before, after);
        // A bend is longer than its chord; err on the side of more samples.
        const int n = std::clamp(int(std::ceil(dist(anchors[k], anchors[k + 1]) * 1.3 / step)), 2, 4000);
        for (int j = 1; j <= n; ++j)
            out.push_back(splinePoint(before, anchors[k], anchors[k + 1], after, double(j) / n));
    }
    return out;
}

std::vector<QPointF> QuickShape::corners() const
{
    return {center + rotated(QPointF(-rx, -ry), rot), center + rotated(QPointF(rx, -ry), rot),
            center + rotated(QPointF(rx, ry), rot), center + rotated(QPointF(-rx, ry), rot)};
}

std::vector<QPointF> QuickShape::handles() const
{
    switch (kind) {
    case Kind::Ellipse:
        return {center + rotated(QPointF(rx, 0), rot), center + rotated(QPointF(0, ry), rot),
                center - rotated(QPointF(rx, 0), rot), center - rotated(QPointF(0, ry), rot)};
    case Kind::Rectangle:
        return corners();
    case Kind::Path: {
        std::vector<QPointF> out;
        for (const Node& n : nodes)
            out.push_back(n.pos);
        return out;
    }
    default:
        return pts;
    }
}

QPointF QuickShape::centroid() const
{
    const std::vector<QPointF> v = kind == Kind::Path ? handles() : pts;
    if (kind == Kind::Ellipse || kind == Kind::Rectangle || v.empty())
        return center;
    QPointF sum;
    for (const QPointF& q : v)
        sum += q;
    return sum / double(v.size());
}

void QuickShape::translate(const QPointF& delta)
{
    for (QPointF& q : pts)
        q += delta;
    for (Node& n : nodes)
        n.pos += delta;
    center += delta;
}

void QuickShape::transformSimilar(const QPointF& pivot, double scale, double degrees)
{
    for (QPointF& q : pts)
        q = pivot + rotated((q - pivot) * scale, degrees);
    // A point's handles are kept relative to its neighbours, so they turn and grow with them.
    for (Node& n : nodes)
        n.pos = pivot + rotated((n.pos - pivot) * scale, degrees);
    center = pivot + rotated((center - pivot) * scale, degrees);
    rx *= scale;
    ry *= scale;
    rot += degrees;
}

void QuickShape::makePerfect()
{
    if (kind != Kind::Ellipse && kind != Kind::Rectangle)
        return;
    rx = ry = (rx + ry) * 0.5;
}

QuickShape QuickShape::asPolygon() const
{
    if (kind != Kind::Rectangle)
        return *this;
    QuickShape s;
    s.kind = Kind::Polygon;
    s.pts = corners();
    return s;
}

QuickShape QuickShape::asRectangle() const
{
    if (kind != Kind::Polygon || pts.size() != 4)
        return *this;
    // Side directions agree modulo 90 degrees; average them on the circle of
    // four-times angles so opposite and adjacent sides all vote together.
    double sx = 0.0, sy = 0.0;
    for (size_t k = 0; k < 4; ++k) {
        const QPointF e = pts[(k + 1) % 4] - pts[k];
        const double a = std::atan2(e.y(), e.x()) * 4.0;
        sx += std::cos(a) * length(e);
        sy += std::sin(a) * length(e);
    }
    const double theta = std::atan2(sy, sx) / 4.0;
    const QPointF u(std::cos(theta), std::sin(theta)), w(-std::sin(theta), std::cos(theta));
    std::vector<double> pu, pw;
    for (const QPointF& q : pts) {
        pu.push_back(dot(q, u));
        pw.push_back(dot(q, w));
    }
    std::sort(pu.begin(), pu.end());
    std::sort(pw.begin(), pw.end());
    const double u0 = (pu[0] + pu[1]) * 0.5, u1 = (pu[2] + pu[3]) * 0.5;
    const double w0 = (pw[0] + pw[1]) * 0.5, w1 = (pw[2] + pw[3]) * 0.5;

    QuickShape s;
    s.kind = Kind::Rectangle;
    s.center = u * ((u0 + u1) * 0.5) + w * ((w0 + w1) * 0.5);
    s.rx = std::max(1.0, (u1 - u0) * 0.5);
    s.ry = std::max(1.0, (w1 - w0) * 0.5);
    s.rot = toDegrees(theta);
    if (std::abs(s.rot) < kUprightTolerance)
        s.rot = 0.0;
    return s;
}

double QuickShape::snapLineEnd(const QPointF& start, QPointF& end, bool force)
{
    const QPointF d = end - start;
    const double len = length(d);
    if (len < 1e-6)
        return -1.0;
    const double angle = toDegrees(std::atan2(d.y(), d.x()));
    double snapped = 0.0;
    if (force) {
        snapped = std::round(angle / 15.0) * 15.0;
    } else {
        snapped = std::round(angle / 45.0) * 45.0;
        if (std::abs(angle - snapped) > 2.5)
            return -1.0;
    }
    end = start + rotated(QPointF(len, 0.0), snapped);
    // Report it the way people read angles: anticlockwise, 0..180.
    double shown = std::fmod(-snapped + 360.0, 180.0);
    if (shown < 0.0)
        shown += 180.0;
    return std::abs(shown - 180.0) < 1e-6 ? 0.0 : shown;
}

std::vector<PathPoint> QuickShape::path(double step) const
{
    std::vector<PathPoint> out;
    step = std::max(step, 0.5);
    auto lineTo = [&](const QPointF& a, const QPointF& b, bool cornerAtEnd) {
        const int n = std::max(1, int(std::ceil(dist(a, b) / step)));
        for (int i = 1; i <= n; ++i)
            out.push_back({a + (b - a) * (double(i) / n), i == n && cornerAtEnd});
    };
    auto closedLoop = [&](const std::vector<QPointF>& v) {
        // Start mid-side so the seam of the stroke lands on a straight run
        // rather than on a corner.
        const size_t n = v.size();
        const QPointF start = (v[0] + v[1]) * 0.5;
        out.push_back({start, false});
        lineTo(start, v[1], true);
        for (size_t k = 1; k < n; ++k)
            lineTo(v[k], v[(k + 1) % n], true);
        lineTo(v[0], start, false);
    };

    switch (kind) {
    case Kind::Line:
        out.push_back({pts[0], false});
        lineTo(pts[0], pts[1], false);
        break;
    case Kind::Polyline:
        out.push_back({pts[0], false});
        for (size_t k = 0; k + 1 < pts.size(); ++k)
            lineTo(pts[k], pts[k + 1], k + 2 < pts.size());
        break;
    case Kind::Curve:
        for (const QPointF& q : smoothThrough(pts, step))
            out.push_back({q, false});
        break;
    case Kind::Polygon:
        if (pts.size() >= 3)
            closedLoop(pts);
        break;
    case Kind::Rectangle:
        closedLoop(corners());
        break;
    case Kind::Ellipse: {
        const int n = std::clamp(int(std::ceil(2.0 * kPi * std::max(rx, ry) / step)), 64, 6000);
        for (int i = 0; i <= n; ++i) {
            const double t = 2.0 * kPi * double(i % n) / n;
            out.push_back({center + rotated(QPointF(rx * std::cos(t), ry * std::sin(t)), rot), false});
        }
        break;
    }
    case Kind::Arc: {
        QPointF c;
        double r = 0, a0 = 0, sweep = 0, toMiddle = 0;
        if (!arcThrough(pts, c, r, a0, sweep, toMiddle)) {
            out.push_back({pts[0], false});
            lineTo(pts[0], pts[2], false);
            break;
        }
        const int n = std::clamp(int(std::ceil(std::abs(sweep) * r / step)), 8, 6000);
        for (int i = 0; i <= n; ++i) {
            const double a = a0 + sweep * double(i) / n;
            out.push_back({c + QPointF(std::cos(a), std::sin(a)) * r, false});
        }
        break;
    }
    case Kind::Path: {
        const size_t n = nodes.size(), runs = segmentCount();
        if (n < 2)
            break;
        auto run = [&](size_t k, double from, double to, bool cornerAtEnd) {
            const Cubic c = cubicOf(*this, k);
            // A bend is sampled a little more finely than a straight run of its length.
            const double length = (c.straight() ? dist(c.a, c.d) : c.roughLength() * 1.15) * (to - from);
            const int count = std::clamp(int(std::ceil(length / step)), c.straight() ? 1 : 2, 4000);
            for (int i = 1; i <= count; ++i)
                out.push_back({c.at(from + (to - from) * double(i) / count), i == count && cornerAtEnd});
        };
        if (closedPath) {
            // Like a polygon's, the seam goes in the middle of the first side.
            out.push_back({cubicOf(*this, 0).at(0.5), false});
            run(0, 0.5, 1.0, !nodes[1].smooth);
            for (size_t k = 1; k < runs; ++k)
                run(k, 0.0, 1.0, !nodes[(k + 1) % n].smooth);
            run(0, 0.0, 0.5, false);
        } else {
            out.push_back({nodes[0].pos, false});
            for (size_t k = 0; k < runs; ++k)
                run(k, 0.0, 1.0, k + 2 < n && !nodes[k + 1].smooth);
        }
        break;
    }
    case Kind::None:
        break;
    }
    return out;
}

// -------------------------------------------------------------------- paths

QuickShape QuickShape::toPath() const
{
    if (kind == Kind::Path || kind == Kind::None)
        return *this;
    QuickShape s;
    s.kind = Kind::Path;
    auto points = [&](const std::vector<QPointF>& v, bool smooth) {
        for (const QPointF& q : v) {
            Node n;
            n.pos = q;
            n.smooth = smooth;
            s.nodes.push_back(n);
        }
    };
    switch (kind) {
    case Kind::Line:
    case Kind::Polyline:
        points(pts, false);
        break;
    case Kind::Polygon:
        points(pts, false);
        s.closedPath = true;
        break;
    case Kind::Rectangle:
        points(corners(), false);
        s.closedPath = true;
        break;
    case Kind::Curve:
        points(pts, true);
        break;
    case Kind::Ellipse: {
        // A quarter turn per run: the handle length that keeps it round to
        // within three parts in ten thousand.
        constexpr double kQuarter = 0.5522847498;
        s.closedPath = true;
        std::vector<QPointF> reach;
        for (int i = 0; i < 4; ++i) {
            const double t = kPi * 0.5 * i;
            Node n;
            n.pos = center + rotated(QPointF(rx * std::cos(t), ry * std::sin(t)), rot);
            n.smooth = true;
            s.nodes.push_back(n);
            reach.push_back(rotated(QPointF(-rx * std::sin(t), ry * std::cos(t)), rot) * kQuarter);
        }
        for (size_t i = 0; i < 4; ++i)
            pinHandles(s, i, -reach[i], reach[i]);
        break;
    }
    case Kind::Arc: {
        QPointF c;
        double r = 0, a0 = 0, sweep = 0, toMiddle = 0;
        if (!arcThrough(pts, c, r, a0, sweep, toMiddle)) {
            points({pts[0], pts[2]}, false);
            break;
        }
        // The arc's three handles stay points; in between, as many more as
        // keep every run within a quarter turn.
        std::vector<QPointF> in, out;
        auto piece = [&](double from, double to, bool withStart) {
            const int runs = std::max(1, int(std::ceil(std::abs(to - from) / (kPi * 0.5) - 1e-9)));
            const double each = (to - from) / runs;
            const double reach = 4.0 / 3.0 * std::tan(std::abs(each) / 4.0) * r * (each < 0.0 ? -1.0 : 1.0);
            for (int i = withStart ? 0 : 1; i <= runs; ++i) {
                const double a = from + each * i;
                Node n;
                n.pos = c + QPointF(std::cos(a), std::sin(a)) * r;
                n.smooth = true;
                s.nodes.push_back(n);
                in.push_back(QPointF(std::sin(a), -std::cos(a)) * reach);
                out.push_back(-in.back());
            }
            // The handle that leaves this piece's first point is this piece's length, not the last one's.
            const size_t first = s.nodes.size() - size_t(runs) - 1;
            out[first] = QPointF(-std::sin(from), std::cos(from)) * reach;
        };
        piece(a0, a0 + toMiddle, true);
        piece(a0 + toMiddle, a0 + sweep, false);
        for (size_t i = 0; i < s.nodes.size(); ++i)
            pinHandles(s, i, in[i], out[i]);
        break;
    }
    default:
        break;
    }
    return s;
}

size_t QuickShape::segmentCount() const
{
    if (kind != Kind::Path || nodes.size() < 2)
        return 0;
    return closedPath ? nodes.size() : nodes.size() - 1;
}

QPointF QuickShape::handleIn(size_t node) const
{
    QPointF in, out;
    nodeHandles(*this, node, in, out);
    return nodes[node].pos + in;
}

QPointF QuickShape::handleOut(size_t node) const
{
    QPointF in, out;
    nodeHandles(*this, node, in, out);
    return nodes[node].pos + out;
}

QuickShape::Hit QuickShape::nearest(const QPointF& p) const
{
    Hit best;
    best.distance = std::numeric_limits<double>::max();
    for (size_t k = 0; k < segmentCount(); ++k) {
        const Cubic c = cubicOf(*this, k);
        const int steps = c.straight() ? 1 : 64;
        QPointF from = c.a;
        for (int j = 1; j <= steps; ++j) {
            const QPointF to = c.at(double(j) / steps);
            const QPointF ab = to - from;
            const double len2 = dot(ab, ab);
            const double f = len2 > 1e-12 ? std::clamp(dot(p - from, ab) / len2, 0.0, 1.0) : 0.0;
            const double d = dist(p, from + ab * f);
            if (d < best.distance) {
                best.distance = d;
                best.segment = int(k);
                best.t = (double(j - 1) + f) / steps;
            }
            from = to;
        }
    }
    if (best.segment >= 0)
        best.pos = cubicOf(*this, size_t(best.segment)).at(best.t);
    return best;
}

int QuickShape::insertNode(int segment, double t)
{
    if (kind != Kind::Path || segment < 0 || segment >= int(segmentCount()))
        return -1;
    t = std::clamp(t, 0.02, 0.98);
    const size_t i = size_t(segment), j = (i + 1) % nodes.size();
    const Cubic c = cubicOf(*this, i);
    Node node;
    node.pos = c.at(t);
    if (c.straight()) {
        // On a straight side, another corner.
        nodes.insert(nodes.begin() + std::ptrdiff_t(i) + 1, node);
        return int(i) + 1;
    }
    // On a bend, split the run where it is: the two halves trace the very
    // same line. The handles at its ends reach only as far as the new point
    // now, and the far side of each end stays as it was.
    auto mix = [t](const QPointF& u, const QPointF& v) { return u + (v - u) * t; };
    const QPointF ab = mix(c.a, c.b), bc = mix(c.b, c.c), cd = mix(c.c, c.d);
    const QPointF abc = mix(ab, bc), bcd = mix(bc, cd);
    QPointF startIn, startOut, endIn, endOut;
    nodeHandles(*this, i, startIn, startOut);
    nodeHandles(*this, j, endIn, endOut);
    node.smooth = true;
    nodes.insert(nodes.begin() + std::ptrdiff_t(i) + 1, node);
    const size_t at = i + 1, end = (at + 1) % nodes.size();
    pinHandles(*this, i, startIn, ab - c.a);
    pinHandles(*this, at, abc - node.pos, bcd - node.pos);
    pinHandles(*this, end, cd - c.d, endOut);
    return int(at);
}

bool QuickShape::removeNode(size_t node)
{
    if (kind != Kind::Path || node >= nodes.size() || nodes.size() <= (closedPath ? 3u : 2u))
        return false;
    const size_t n = nodes.size();
    const bool hasPrev = closedPath || node > 0, hasNext = closedPath || node + 1 < n;
    const size_t before = (node + n - 1) % n, after = (node + 1) % n;
    QPointF beforeIn, beforeOut, afterIn, afterOut, in, out;
    if (hasPrev)
        nodeHandles(*this, before, beforeIn, beforeOut);
    if (hasNext)
        nodeHandles(*this, after, afterIn, afterOut);
    nodeHandles(*this, node, in, out);
    // The reverse of putting a point on a run: the handles either side of it
    // reach out again over the whole of what is one run once more. How its
    // own two handles compare says how far along that run it sat.
    const double reachIn = length(in), reachOut = length(out);
    if (hasPrev && hasNext && reachIn > 1e-9 && reachOut > 1e-9) {
        const double t = std::clamp(reachIn / (reachIn + reachOut), 0.02, 0.98);
        const double span = 1.5 * dist(nodes[before].pos, nodes[after].pos);
        beforeOut /= t;
        afterIn /= 1.0 - t;
        // A handle longer than the run it shapes would throw a loop.
        if (length(beforeOut) > span && span > 0.0)
            beforeOut *= span / length(beforeOut);
        if (length(afterIn) > span && span > 0.0)
            afterIn *= span / length(afterIn);
    }
    nodes.erase(nodes.begin() + std::ptrdiff_t(node));
    if (hasPrev)
        pinHandles(*this, before > node ? before - 1 : before, beforeIn, beforeOut);
    if (hasNext)
        pinHandles(*this, after > node ? after - 1 : after, afterIn, afterOut);
    return true;
}

void QuickShape::setSmooth(size_t node, bool smooth)
{
    if (kind != Kind::Path || node >= nodes.size())
        return;
    Node& n = nodes[node];
    n.smooth = smooth;
    n.bendIn = n.bendOut = QPointF(1.0, 0.0);
}

void QuickShape::setHandle(size_t node, bool outSide, const QPointF& to)
{
    if (kind != Kind::Path || node >= nodes.size() || !nodes[node].smooth)
        return;
    QPointF in, out;
    nodeHandles(*this, node, in, out);
    QPointF& moved = outSide ? out : in;
    QPointF& other = outSide ? in : out;
    const QPointF offset = to - nodes[node].pos;
    const double reach = length(offset);
    if (reach < 1e-6 || length(moved) < 1e-9)
        return;   // an end has no handle on its open side, and none may shrink to nothing
    moved = offset;
    other = offset * (-length(other) / reach);
    pinHandles(*this, node, in, out);
}

bool QuickShape::closePath()
{
    if (!valid() || closed())
        return false;
    QuickShape p = toPath();
    if (p.kind != Kind::Path || p.nodes.size() < 2)
        return false;
    // The line already there stays exactly as it is: each end keeps the
    // handle it has on its drawn side.
    QPointF firstIn, firstOut, lastIn, lastOut;
    nodeHandles(p, 0, firstIn, firstOut);
    nodeHandles(p, p.nodes.size() - 1, lastIn, lastOut);
    // Ends drawn to meet but not quite touching become a single point.
    double extent = 0.0;
    for (const Node& n : p.nodes)
        extent = std::max(extent, dist(n.pos, p.nodes.front().pos));
    const bool merge = p.nodes.size() > 3 && dist(p.nodes.front().pos, p.nodes.back().pos) < std::max(4.0, extent * 0.03);
    if (merge)
        p.nodes.pop_back();
    if (p.nodes.size() < 3)
        return false;   // a line closed on itself is no shape
    p.closedPath = true;
    // A smooth end turns into the run that joins it as smoothly as it can:
    // its new handle in line with the one it kept, as long as the spline would make it.
    auto carryOn = [&](size_t k, const QPointF& kept, bool keptIsOut) {
        if (!p.nodes[k].smooth || length(kept) < 1e-9)
            return;
        QPointF in, out;
        nodeHandles(p, k, in, out);
        const QPointF other = kept * (-length(keptIsOut ? in : out) / length(kept));
        pinHandles(p, k, keptIsOut ? other : kept, keptIsOut ? kept : other);
    };
    if (merge && p.nodes[0].smooth && length(lastIn) > 1e-9 && length(firstOut) > 1e-9)
        pinHandles(p, 0, lastIn, firstOut);   // the run into the end that went now comes into this point
    else if (merge)
        carryOn(0, firstOut, true);
    else {
        carryOn(0, firstOut, true);
        carryOn(p.nodes.size() - 1, lastIn, false);
    }
    *this = p;
    return true;
}

bool QuickShape::openPath(size_t node)
{
    if (!valid() || !closed())
        return false;
    QuickShape p = toPath();
    const size_t n = p.nodes.size();
    if (p.kind != Kind::Path || !p.closedPath || n < 3)
        return false;
    node %= n;
    // Every point keeps the handles it has now, the cut one on both sides.
    std::vector<QPointF> ins(n), outs(n);
    for (size_t k = 0; k < n; ++k)
        nodeHandles(p, k, ins[k], outs[k]);
    std::rotate(p.nodes.begin(), p.nodes.begin() + std::ptrdiff_t(node), p.nodes.end());
    std::rotate(ins.begin(), ins.begin() + std::ptrdiff_t(node), ins.end());
    std::rotate(outs.begin(), outs.begin() + std::ptrdiff_t(node), outs.end());
    p.nodes.push_back(p.nodes.front());
    ins.push_back(ins.front());
    outs.push_back(outs.front());
    p.closedPath = false;
    for (size_t k = 0; k < p.nodes.size(); ++k)
        pinHandles(p, k, ins[k], outs[k]);
    *this = p;
    return true;
}
