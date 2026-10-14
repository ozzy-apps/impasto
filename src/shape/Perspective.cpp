#include "shape/Perspective.h"

#include <cmath>

namespace perspective {

namespace {

constexpr double kPi = 3.14159265358979323846;

QPointF unit(const QPointF& v)
{
    const double len = std::hypot(v.x(), v.y());
    return len > 1e-9 ? v / len : QPointF();
}

Constraint line(const QPointF& origin, const QPointF& direction)
{
    Constraint c;
    c.origin = origin;
    c.dir = unit(direction);
    c.valid = !c.dir.isNull();
    return c;
}

} // namespace

// The circle through a and b that also passes through p; a straight line when
// the three are in a row.
Constraint arc(const QPointF& a, const QPointF& b, const QPointF& p)
{
    const double d = 2.0 * (a.x() * (b.y() - p.y()) + b.x() * (p.y() - a.y()) + p.x() * (a.y() - b.y()));
    const double scale = std::hypot(b.x() - a.x(), b.y() - a.y());
    if (std::abs(d) < 1e-6 * scale * scale + 1e-9)
        return line(p, b - a);
    const double a2 = a.x() * a.x() + a.y() * a.y(), b2 = b.x() * b.x() + b.y() * b.y(), p2 = p.x() * p.x() + p.y() * p.y();
    Constraint c;
    c.circle = true;
    c.origin = p;
    c.centre = QPointF((a2 * (b.y() - p.y()) + b2 * (p.y() - a.y()) + p2 * (a.y() - b.y())) / d,
                       (a2 * (p.x() - b.x()) + b2 * (a.x() - p.x()) + p2 * (b.x() - a.x())) / d);
    c.radius = std::hypot(p.x() - c.centre.x(), p.y() - c.centre.y());
    // So flat an arc is a line for all practical purposes, and kinder to arithmetic.
    if (c.radius > 200.0 * scale)
        return line(p, b - a);
    c.valid = c.radius > 1e-6;
    return c;
}

QPointF Constraint::apply(const QPointF& p) const
{
    if (!valid)
        return p;
    if (!circle)
        return origin + dir * QPointF::dotProduct(p - origin, dir);
    const QPointF out = unit(p - centre);
    return out.isNull() ? origin : centre + out * radius;
}

QPointF Constraint::tangentAt(const QPointF& p) const
{
    if (!circle)
        return dir;
    const QPointF out = unit(p - centre);
    return QPointF(-out.y(), out.x());
}

int handleCount(Guides::Perspective mode)
{
    switch (mode) {
    case Guides::Perspective::OnePoint: return 1;
    case Guides::Perspective::TwoPoint: return 2;
    case Guides::Perspective::ThreePoint: return 3;
    case Guides::Perspective::FivePoint: return 2;   // the centre, and a point on the rim
    case Guides::Perspective::Off:
    case Guides::Perspective::Isometric: break;
    }
    return 0;
}

void placeDefaults(Guides& g, const QSizeF& canvas)
{
    const double w = canvas.width(), h = canvas.height();
    switch (g.perspective) {
    case Guides::Perspective::OnePoint:
        g.vp[0] = QPointF(w * 0.5, h * 0.45);
        break;
    case Guides::Perspective::TwoPoint:
        g.vp[0] = QPointF(w * 0.08, h * 0.45);
        g.vp[1] = QPointF(w * 0.92, h * 0.45);
        break;
    case Guides::Perspective::ThreePoint:
        g.vp[0] = QPointF(w * 0.08, h * 0.35);
        g.vp[1] = QPointF(w * 0.92, h * 0.35);
        g.vp[2] = QPointF(w * 0.5, h * 0.96);
        break;
    case Guides::Perspective::FivePoint:
        g.vp[0] = QPointF(w * 0.5, h * 0.5);
        g.vp[1] = g.vp[0] + QPointF(std::min(w, h) * 0.44, 0.0);
        break;
    case Guides::Perspective::Off:
    case Guides::Perspective::Isometric:
        break;
    }
}

void fisheyeRim(const Guides& g, QPointF rim[4], double* radius)
{
    const QPointF c = g.vp[0];
    const double r = std::max(1.0, std::hypot(g.vp[1].x() - c.x(), g.vp[1].y() - c.y()));
    rim[0] = c - QPointF(r, 0);
    rim[1] = c + QPointF(r, 0);
    rim[2] = c - QPointF(0, r);
    rim[3] = c + QPointF(0, r);
    if (radius)
        *radius = r;
}

void isometricDirections(const Guides& g, QPointF dirs[3])
{
    const double left = double(g.isoLeft) * kPi / 180.0, right = double(g.isoRight) * kPi / 180.0;
    dirs[0] = QPointF(0.0, -1.0);                          // upright
    dirs[1] = QPointF(std::cos(right), -std::sin(right));  // receding to the right, rising
    dirs[2] = QPointF(-std::cos(left), -std::sin(left));   // receding to the left, rising
}

QVector<Constraint> candidates(const Guides& g, const QPointF& start)
{
    QVector<Constraint> out;
    auto add = [&](const Constraint& c) {
        if (c.valid)
            out.push_back(c);
    };
    switch (g.perspective) {
    case Guides::Perspective::Off:
        break;
    case Guides::Perspective::OnePoint:
        add(line(start, g.vp[0] - start));
        add(line(start, QPointF(1, 0)));
        add(line(start, QPointF(0, 1)));
        break;
    case Guides::Perspective::TwoPoint: {
        add(line(start, g.vp[0] - start));
        add(line(start, g.vp[1] - start));
        // Uprights stand square on the horizon, which may be tilted.
        const QPointF horizon = g.vp[1] - g.vp[0];
        add(line(start, horizon.isNull() ? QPointF(0, 1) : QPointF(-horizon.y(), horizon.x())));
        break;
    }
    case Guides::Perspective::ThreePoint:
        for (int i = 0; i < 3; ++i)
            add(line(start, g.vp[i] - start));
        break;
    case Guides::Perspective::FivePoint: {
        QPointF rim[4];
        fisheyeRim(g, rim, nullptr);
        add(line(start, g.vp[0] - start));          // straight into the middle
        add(arc(rim[0], rim[1], start));         // a bent horizontal
        add(arc(rim[2], rim[3], start));         // a bent vertical
        break;
    }
    case Guides::Perspective::Isometric: {
        QPointF dirs[3];
        isometricDirections(g, dirs);
        for (const QPointF& d : dirs)
            add(line(start, d));
        break;
    }
    }
    return out;
}

Constraint choose(const Guides& g, const QPointF& start, const QPointF& towards, double* margin)
{
    const QPointF way = unit(towards - start);
    Constraint best;
    double bestOff = 1e9, secondOff = 1e9;   // degrees between the pen's way and each line
    QPointF bestTangent;
    for (const Constraint& c : candidates(g, start)) {
        const QPointF tangent = c.tangentAt(start);
        const double off = std::acos(std::clamp(std::abs(QPointF::dotProduct(tangent, way)), 0.0, 1.0)) * 180.0 / kPi;
        // Two lines that leave this point the same way are one choice, not a close call.
        const bool sameAsBest = best.valid && std::abs(QPointF::dotProduct(tangent, bestTangent)) > 0.9995;
        if (off < bestOff) {
            if (!sameAsBest)
                secondOff = bestOff;
            bestOff = off;
            best = c;
            bestTangent = tangent;
        } else if (off < secondOff && !sameAsBest) {
            secondOff = off;
        }
    }
    if (margin)
        *margin = secondOff - bestOff;
    return best;
}

} // namespace perspective
