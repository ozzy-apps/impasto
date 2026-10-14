// Drawing guides in the viewport: the grid, the axes of symmetry, perspective
// lines with their movable points, and keeping a stroke on a guide line.

#include "engine/CanvasEngine.h"
#include "shape/Perspective.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"

#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHandleReach = 16.0;   // widget px around a point that still grabs it
constexpr double kAssistSlack = 9.0;    // travel before a stroke can show which way it is going
constexpr double kAssistPatience = 34.0;   // ...and how far it may go before a close call is settled anyway
constexpr double kAssistClear = 14.0;      // degrees by which one guide line must beat the next

constexpr double kTurnArm = 96.0;       // widget px from the centre of symmetry to the point that turns it
constexpr double kCentreSnap = 9.0;     // widget px within which that centre takes to the middle of the canvas
constexpr int kSymmetryCentre = 100;    // handles of symmetry, numbered clear of the perspective points
constexpr int kSymmetryTurn = 101;

const QColor kPointColours[3] = {QColor(0x0a, 0x84, 0xff), QColor(0x30, 0xd1, 0x58), QColor(0xff, 0x9f, 0x0a)};

QColor faded(const QColor& c, int alpha)
{
    return QColor(c.red(), c.green(), c.blue(), alpha);
}

} // namespace

void CanvasWidget::setGuideEditing(bool on)
{
    if (m_guideEdit == on)
        return;
    m_guideEdit = on;
    update();
}

QVector<QPointF> CanvasWidget::guideHandlePositions() const
{
    QVector<QPointF> out;
    const Guides& g = m_engine->guides();
    for (int i = 0; i < perspective::handleCount(g.perspective); ++i)
        out.push_back(canvasToWidget(g.vp[i]));
    return out;
}

QVector<QPointF> CanvasWidget::symmetryHandlePositions() const
{
    const Guides& g = m_engine->guides();
    if (g.symmetry == Guides::Symmetry::Off)
        return {};
    const QPointF centre = m_engine->symmetryCentre();
    const QPointF at = canvasToWidget(centre);
    // The turning point sits on the axis, always the same reach away on
    // screen however far the view is zoomed, turned or mirrored.
    const double a = symmetryAxis(g);
    QPointF along = canvasToWidget(centre + QPointF(std::cos(a), std::sin(a)) * 100.0) - at;
    const double len = std::max(1e-6, std::hypot(along.x(), along.y()));
    return {at, at + along / len * kTurnArm};
}

int CanvasWidget::hitGuideHandle(const QPointF& logical) const
{
    int best = -1;
    double bestDistance = kHandleReach;
    auto consider = [&](const QVector<QPointF>& handles, int firstIndex) {
        for (int i = 0; i < handles.size(); ++i) {
            const double d = std::hypot(handles[i].x() - logical.x(), handles[i].y() - logical.y());
            if (d < bestDistance) {
                bestDistance = d;
                best = firstIndex + i;
            }
        }
    };
    consider(guideHandlePositions(), 0);
    consider(symmetryHandlePositions(), kSymmetryCentre);
    return best;
}

void CanvasWidget::dragGuideHandle(const QPointF& logical, Qt::KeyboardModifiers mods)
{
    Guides g = m_engine->guides();
    const QPointF to = widgetToCanvas(logical);
    if (m_guideHandle == kSymmetryCentre) {
        // Anywhere on the canvas; close to its middle, it settles there.
        const QSizeF size = m_engine->canvasSize();
        const QPointF middle(size.width() / 2.0, size.height() / 2.0);
        QPointF at(std::clamp(to.x(), 0.0, size.width()), std::clamp(to.y(), 0.0, size.height()));
        const QPointF onScreen = canvasToWidget(at);
        auto apart = [&](const QPointF& other) {
            const QPointF there = canvasToWidget(other);
            return std::hypot(onScreen.x() - there.x(), onScreen.y() - there.y());
        };
        if (apart(QPointF(middle.x(), at.y())) < kCentreSnap)
            at.setX(middle.x());
        if (apart(QPointF(at.x(), middle.y())) < kCentreSnap)
            at.setY(middle.y());
        g.symOffset = at - middle;
        m_engine->setGuides(g);
        return;
    }
    if (m_guideHandle == kSymmetryTurn) {
        const QPointF from = to - m_engine->symmetryCentre();
        if (std::hypot(from.x(), from.y()) < 1.0)
            return;
        // Measured from where the axis points when it is not turned at all.
        Guides upright = g;
        upright.symAngle = 0.0f;
        double degrees = (std::atan2(from.y(), from.x()) - symmetryAxis(upright)) * 180.0 / kPi;
        degrees -= 360.0 * std::floor((degrees + 180.0) / 360.0);
        // Shift keeps to steps of 15 degrees; otherwise it only catches on the eighths of a turn.
        const double step = (mods & Qt::ShiftModifier) ? 15.0 : 45.0;
        const double nearest = std::round(degrees / step) * step;
        if ((mods & Qt::ShiftModifier) || std::abs(degrees - nearest) < 2.0)
            degrees = nearest;
        if (degrees >= 180.0)
            degrees -= 360.0;
        g.symAngle = float(degrees);
        m_engine->setGuides(g);
        return;
    }
    if (m_guideHandle < 0 || m_guideHandle >= perspective::handleCount(g.perspective))
        return;
    if (g.perspective == Guides::Perspective::FivePoint && m_guideHandle == 0)
        g.vp[1] += to - g.vp[0];   // moving the centre takes the rim along
    g.vp[m_guideHandle] = to;
    m_engine->setGuides(g);
}

// Holds a stroke to the guide line it set off along. Returns false while the
// pen has not gone far enough from where it landed for that to be known.
bool CanvasWidget::assist(PointerInput& in, bool decideNow)
{
    if (!m_assist.active || m_shape.snapped)
        return true;
    if (!m_assist.locked) {
        const double travelled = std::hypot(in.pos.x() - m_assist.startWidget.x(), in.pos.y() - m_assist.startWidget.y());
        if (travelled < kAssistSlack)
            return false;
        // A pen tends to land with a little hook, and two guide lines can leave
        // a point at nearly the same angle. Nothing is drawn yet, so it costs
        // nothing to wait until one of them is clearly meant.
        double margin = 0.0;
        m_assist.path = perspective::choose(m_engine->guides(), m_assist.start, widgetToCanvas(in.pos), &margin);
        if (margin < kAssistClear && travelled < kAssistPatience && !decideNow)
            return false;   // (a pen that lifts has said all it is going to)
        m_assist.locked = true;
        if (!m_assist.path.valid)
            m_assist.active = false;
    }
    if (m_assist.active)
        in.pos = canvasToWidget(m_assist.path.apply(widgetToCanvas(in.pos)));
    return true;
}

void CanvasWidget::drawGuides(QPainter& p)
{
    const Guides& g = m_engine->guides();
    using Perspective = Guides::Perspective;
    if (!g.grid && g.symmetry == Guides::Symmetry::Off && g.perspective == Perspective::Off)
        return;
    const QSizeF size = m_engine->canvasSize();
    const double dpr = devicePixelRatioF();
    const QTransform toWidget = viewTransform() * QTransform::fromScale(1.0 / dpr, 1.0 / dpr);
    const QPointF c(size.width() / 2.0, size.height() / 2.0);
    const double diagonal = std::hypot(size.width(), size.height());
    p.save();
    QPainterPath sheet;
    sheet.addPolygon(toWidget.map(QPolygonF(QRectF(QPointF(0, 0), size))));
    sheet.closeSubpath();
    p.setClipPath(sheet);
    p.setBrush(Qt::NoBrush);
    auto line = [&](const QPointF& a, const QPointF& b) { p.drawLine(toWidget.map(a), toWidget.map(b)); };

    // Left out when the lines would crowd into a grey film.
    const bool roomForGrid = g.gridSpacing * m_zoom / dpr >= 5.0;
    if (g.grid && roomForGrid) {
        p.setPen(QPen(QColor(90, 110, 140, 95), 1.0));
        for (double x = std::fmod(c.x(), double(g.gridSpacing)); x < size.width(); x += g.gridSpacing)
            line(QPointF(x, 0), QPointF(x, size.height()));
        for (double y = std::fmod(c.y(), double(g.gridSpacing)); y < size.height(); y += g.gridSpacing)
            line(QPointF(0, y), QPointF(size.width(), y));
    }

    // Perspective: a fan of lines through each vanishing point.
    auto fan = [&](const QPointF& vp, const QColor& colour, int count) {
        const double reach = std::hypot(vp.x() - c.x(), vp.y() - c.y()) + diagonal;
        p.setPen(QPen(faded(colour, 105), 1.0));
        for (int k = 0; k < count; ++k) {
            const double a = kPi * k / count;
            const QPointF d(std::cos(a) * reach, std::sin(a) * reach);
            line(vp - d, vp + d);
        }
    };
    auto strong = [&](const QPointF& a, const QPointF& b, const QColor& colour) {
        p.setPen(QPen(faded(colour, 200), 1.4));
        line(a, b);
    };
    switch (g.perspective) {
    case Perspective::Off:
        break;
    case Perspective::OnePoint:
        fan(g.vp[0], kPointColours[0], 36);
        strong(QPointF(g.vp[0].x() - diagonal * 4, g.vp[0].y()), QPointF(g.vp[0].x() + diagonal * 4, g.vp[0].y()), kPointColours[0]);
        break;
    case Perspective::TwoPoint:
    case Perspective::ThreePoint: {
        const int points = g.perspective == Perspective::TwoPoint ? 2 : 3;
        for (int i = 0; i < points; ++i)
            fan(g.vp[i], kPointColours[i], 30);
        const QPointF horizon = g.vp[1] - g.vp[0];
        const double len = std::max(1.0, std::hypot(horizon.x(), horizon.y()));
        const QPointF along = horizon / len * (diagonal * 4 + len);
        strong(g.vp[0] - along, g.vp[1] + along, QColor(255, 255, 255));
        break;
    }
    case Perspective::FivePoint: {
        QPointF rim[4];
        double radius = 0.0;
        perspective::fisheyeRim(g, rim, &radius);
        QPainterPath disc;
        disc.addEllipse(g.vp[0], radius, radius);
        p.setPen(QPen(faded(Qt::white, 200), 1.4));
        p.drawPath(toWidget.map(disc));
        p.setClipPath(sheet.intersected(toWidget.map(disc)));
        fan(g.vp[0], kPointColours[0], 18);
        // Bent horizontals pass through the left and right of the rim, bent
        // verticals through its top and bottom.
        for (int family = 0; family < 2; ++family) {
            p.setPen(QPen(faded(kPointColours[family + 1], 115), 1.0));
            for (int k = -5; k <= 5; ++k) {
                const QPointF third = g.vp[0] + (family == 0 ? QPointF(0, radius * k / 6.0) : QPointF(radius * k / 6.0, 0));
                const perspective::Constraint arc = perspective::arc(rim[family * 2], rim[family * 2 + 1], third);
                if (!arc.circle) {
                    line(rim[family * 2], rim[family * 2 + 1]);
                    continue;
                }
                QPainterPath round;
                round.addEllipse(arc.centre, arc.radius, arc.radius);
                p.drawPath(toWidget.map(round));
            }
        }
        p.setClipPath(sheet);
        break;
    }
    case Perspective::Isometric: {
        if (!roomForGrid)
            break;
        QPointF dirs[3];
        perspective::isometricDirections(g, dirs);
        const int each = int(std::ceil(diagonal / g.gridSpacing));
        for (int i = 0; i < 3; ++i) {
            const QPointF across(-dirs[i].y(), dirs[i].x());
            p.setPen(QPen(faded(kPointColours[i], 100), 1.0));
            for (int k = -each; k <= each; ++k) {
                const QPointF base = c + across * (k * double(g.gridSpacing));
                line(base - dirs[i] * diagonal, base + dirs[i] * diagonal);
            }
        }
        break;
    }
    }

    if (g.symmetry != Guides::Symmetry::Off) {
        // Lines through the centre of symmetry, wherever it has been put and
        // however it is turned: whole lines for the mirror axes, spokes for
        // radial. Where repeats are turned rather than mirrored the lines only
        // mark out the sectors, and are drawn broken to say so.
        const QPointF sc = m_engine->symmetryCentre();
        const double reach = std::hypot(g.symOffset.x(), g.symOffset.y()) + diagonal;
        const double turn = double(g.symAngle) * kPi / 180.0;
        struct Axis {
            double angle;
            bool whole;    // through the centre and out both sides
            bool minor;    // a mirror line between two spokes
        };
        QVector<Axis> axes;
        if (g.symmetry == Guides::Symmetry::Vertical || g.symmetry == Guides::Symmetry::Quadrant)
            axes.push_back({-kPi / 2.0 + turn, true, false});
        if (g.symmetry == Guides::Symmetry::Horizontal || g.symmetry == Guides::Symmetry::Quadrant)
            axes.push_back({turn, true, false});
        if (g.symmetry == Guides::Symmetry::Radial) {
            for (int k = 0; k < g.segments; ++k) {
                axes.push_back({2.0 * kPi * k / g.segments - kPi / 2.0 + turn, false, false});
                if (g.symMirrored)
                    axes.push_back({2.0 * kPi * (k + 0.5) / g.segments - kPi / 2.0 + turn, false, true});
            }
        }
        const bool turned = g.symmetry == Guides::Symmetry::Radial ? !g.symMirrored : g.symRotational;
        for (const Axis& axis : axes) {
            const QPointF d = QPointF(std::cos(axis.angle), std::sin(axis.angle)) * reach;
            const QPointF from = axis.whole ? sc - d : sc, to = sc + d;
            p.setPen(QPen(QColor(0, 0, 0, axis.minor ? 40 : 70), 2.4));
            line(from, to);
            QPen pen(faded(theme::accent, axis.minor ? 110 : 210), 1.2);
            if (turned)
                pen.setDashPattern({7.0, 5.0});
            p.setPen(pen);
            line(from, to);
        }
    }
    p.restore();

    // The points themselves, while the guides are being arranged. They may
    // sit outside the canvas, so they are not clipped to it.
    if (m_guideEdit) {
        auto point = [&](const QPointF& at, const QColor& colour, bool held) {
            p.setPen(QPen(QColor(0, 0, 0, 120), 3.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(at, 9, 9);
            p.setPen(QPen(Qt::white, 2));
            p.setBrush(held ? QColor(Qt::white) : colour);
            p.drawEllipse(at, held ? 10 : 8, held ? 10 : 8);
        };
        const QVector<QPointF> handles = guideHandlePositions();
        for (int i = 0; i < handles.size(); ++i)
            point(handles[i], kPointColours[i % 3], m_drag == Drag::Guide && m_guideHandle == i);
        // Symmetry: its centre, and on a short arm the point that turns it.
        const QVector<QPointF> symmetry = symmetryHandlePositions();
        if (symmetry.size() == 2) {
            p.setPen(QPen(QColor(0, 0, 0, 90), 3.0));
            p.drawLine(symmetry[0], symmetry[1]);
            p.setPen(QPen(Qt::white, 1.4));
            p.drawLine(symmetry[0], symmetry[1]);
            point(symmetry[1], kPointColours[1], m_drag == Drag::Guide && m_guideHandle == kSymmetryTurn);
            point(symmetry[0], kPointColours[0], m_drag == Drag::Guide && m_guideHandle == kSymmetryCentre);
        }
    }
}
