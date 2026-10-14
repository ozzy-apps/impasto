// QuickShape for the viewport: recognising a held stroke, resizing the shape
// while the pen stays down, and the edit mode with snapping handles.

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"

#include <QPainter>
#include <cmath>
#include <limits>

namespace {

constexpr double kHoldRadius = 6.0;      // widget px the pen may wander and still count as resting
constexpr double kMinTravel = 28.0;      // widget px a stroke must cover to be worth recognising
constexpr double kAdjustDeadZone = 9.0;  // widget px before a snapped shape starts following the pen
constexpr double kAspectSnap = 0.05;     // how nearly round before a handle locks to circle / square
constexpr double kRotationSnap = 3.0;    // degrees
constexpr double kPi = 3.14159265358979323846;
// What a press in edit mode took hold of: a point (its index), one of the
// chosen point's handles, or one of these.
constexpr int kShapeIn = 100000;         // + the point's index
constexpr int kShapeOut = 200000;
constexpr int kShapeRotate = 1000000;
constexpr int kShapeMove = 1000001;
constexpr int kShapeIdle = 1000002;      // the press did its work already
constexpr double kTapWobble = 5.0;       // widget px a tap may slide

double dist(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }
double dot(const QPointF& a, const QPointF& b) { return a.x() * b.x() + a.y() * b.y(); }
double angleOf(const QPointF& v) { return std::atan2(v.y(), v.x()) * 180.0 / kPi; }

QPointF rotated(const QPointF& p, double degrees)
{
    const double a = degrees * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    return {c * p.x() - s * p.y(), s * p.x() + c * p.y()};
}

double wrapDegrees(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    return (a < 0.0 ? a + 360.0 : a) - 180.0;
}

// Twice the enclosed area; the sign says which way round the points run.
double signedArea(const std::vector<QPointF>& p)
{
    double a = 0.0;
    for (size_t i = 0; i < p.size(); ++i) {
        const QPointF& q = p[(i + 1) % p.size()];
        a += p[i].x() * q.y() - p[i].y() * q.x();
    }
    return a;
}

QString degreesLabel(double a)
{
    return QStringLiteral("%1°").arg(qRound(a));
}

// Stretches and turns `s` about `pivot` so that the point that was at `from`
// ends up at `to`.
void followPoint(QuickShape& s, const QPointF& pivot, const QPointF& from, const QPointF& to, double rotationDeadZone)
{
    const QPointF v0 = from - pivot, v1 = to - pivot;
    const double l0 = std::hypot(v0.x(), v0.y()), l1 = std::hypot(v1.x(), v1.y());
    if (l0 < 4.0) {
        s.translate(to - from);
        return;
    }
    double turn = wrapDegrees(angleOf(v1) - angleOf(v0));
    // Resizing should not also rotate by accident: ignore the first few degrees.
    turn = turn > 0.0 ? std::max(0.0, turn - rotationDeadZone) : std::min(0.0, turn + rotationDeadZone);
    s.transformSimilar(pivot, std::max(0.02, l1 / l0), turn);
}

} // namespace

// ------------------------------------------------------- recognising a hold

void CanvasWidget::beginShapeCapture(const PointerInput& in, StrokeTool tool)
{
    m_shape = {};
    m_shape.recipe.tool = tool;
    m_shape.recipe.preset = m_tools->preset(tool);
    m_shape.recipe.size = m_tools->size(tool);
    m_shape.recipe.opacity = m_tools->opacity(tool);
    m_shape.recipe.color = m_tools->color();
    m_shape.recipe.seed = m_engine->strokeSeed();
    m_shape.points.push_back(widgetToCanvas(in.pos));
    m_shape.pressures.push_back(curved(in.pressure));
    m_shape.tiltsX.push_back(in.tiltX);
    m_shape.tiltsY.push_back(in.tiltY);
    m_shape.times.push_back(m_clock.nsecsElapsed() * 1e-9);
    m_shape.lastPos = m_shape.holdAnchor = in.pos;
    m_shape.holdSince = m_clock.elapsed();
}

void CanvasWidget::captureShapePoint(const PointerInput& in)
{
    ShapeState& st = m_shape;
    const double step = dist(in.pos, st.lastPos);
    if (step >= 1.5) {
        st.points.push_back(widgetToCanvas(in.pos));
        st.pressures.push_back(curved(in.pressure));
        st.tiltsX.push_back(in.tiltX);
        st.tiltsY.push_back(in.tiltY);
        st.times.push_back(m_clock.nsecsElapsed() * 1e-9);
        st.travelled += step;
        st.lastPos = in.pos;
    }
    if (dist(in.pos, st.holdAnchor) > kHoldRadius) {
        st.holdAnchor = in.pos;
        st.holdSince = m_clock.elapsed();
        st.holdTried = false;
    }
}

void CanvasWidget::trySnapShape()
{
    ShapeState& st = m_shape;
    if (!m_quickShapeEnabled || st.snapped || st.holdTried || st.travelled < kMinTravel
        || m_clock.elapsed() - st.holdSince < m_holdMs)
        return;
    st.holdTried = true;   // one attempt per rest; moving on re-arms it
    // What the pen wandered through while it waited here is not part of the
    // drawing; left in, its jitter reads as a knot of corners at the end.
    const double canvasPerWidget = dist(widgetToCanvas(QPointF(0, 0)), widgetToCanvas(QPointF(100, 0))) / 100.0;
    const QuickShape shape = QuickShape::recognize(QuickShape::withoutRest(st.points, kHoldRadius * canvasPerWidget), canvasPerWidget);
    if (!shape.valid())
        return;

    st.shape = st.atGrab = shape;
    buildShapeDynamics();
    if (shape.closed())
        anchorShapeStart(st.points.front(), signedArea(st.points));
    st.grabPos = m_lastInput.pos;
    st.grab = widgetToCanvas(m_lastInput.pos);
    st.adjusting = false;
    st.snapped = true;
    if (m_shift)
        adjustHeldShape(m_lastInput);
    else
        renderShape();
    emit toast(st.shape.name());
    update();
}

void CanvasWidget::setQuickShape(bool enabled, int holdMs)
{
    m_quickShapeEnabled = enabled;
    m_holdMs = std::clamp(holdMs, 200, 3000);
}

float CanvasWidget::StrokeDynamics::at(const std::vector<float>& values, double t)
{
    if (values.empty())
        return 1.0f;
    const double x = std::clamp(t, 0.0, 1.0) * double(values.size() - 1);
    const size_t i = std::min(size_t(x), values.size() - 1);
    const size_t j = std::min(i + 1, values.size() - 1);
    return values[i] + (values[j] - values[i]) * float(x - double(i));
}

// Turns the samples gathered while drawing into profiles over the stroke's
// length, so they can be laid along an outline of any size.
void CanvasWidget::buildShapeDynamics()
{
    ShapeState& st = m_shape;
    StrokeDynamics& dyn = st.recipe.dynamics;
    constexpr int kSteps = 96;
    const size_t n = st.points.size();

    std::vector<double> along(n, 0.0);
    std::vector<float> speeds(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        const double d = dist(st.points[i], st.points[i - 1]);
        along[i] = along[i - 1] + d;
        speeds[i] = float(d / std::max(st.times[i] - st.times[i - 1], 1e-3));
    }
    if (n > 1)
        speeds[0] = speeds[1];
    const double total = n ? along.back() : 0.0;

    auto resample = [&](const std::vector<float>& values) {
        std::vector<float> out(kSteps, values.empty() ? 1.0f : values.back());
        size_t seg = 1;
        for (int k = 0; k < kSteps && n > 1 && total > 0.0; ++k) {
            const double target = total * k / (kSteps - 1);
            while (seg + 1 < n && along[seg] < target)
                ++seg;
            const double span = along[seg] - along[seg - 1];
            const float f = span > 1e-9 ? float((target - along[seg - 1]) / span) : 0.0f;
            out[size_t(k)] = values[seg - 1] + (values[seg] - values[seg - 1]) * std::clamp(f, 0.0f, 1.0f);
        }
        // Tablet readings are jittery; a light blur keeps the outline's edge
        // from wobbling without flattening the stroke's shape.
        std::vector<float> smooth(out.size());
        for (int k = 0; k < kSteps; ++k) {
            float sum = 0.0f;
            int count = 0;
            for (int d = -2; d <= 2; ++d) {
                if (k + d >= 0 && k + d < kSteps) {
                    sum += out[size_t(k + d)];
                    ++count;
                }
            }
            smooth[size_t(k)] = sum / float(count);
        }
        return smooth;
    };
    dyn.pressure = resample(st.pressures);
    dyn.tiltX = resample(st.tiltsX);
    dyn.tiltY = resample(st.tiltsY);
    dyn.speed = resample(speeds);
}

// Works out where on the shape's own outline the stroke began and which way
// round it went, so the replayed pressure lands where it was applied.
void CanvasWidget::anchorShapeStart(const QPointF& start, double winding)
{
    StrokeRecipe& r = m_shape.recipe;
    const std::vector<PathPoint> outline = m_shape.shape.path(2.0);
    r.startParam = 0.0;
    r.reversed = false;
    if (outline.size() < 3)
        return;
    std::vector<QPointF> pts;
    double total = 0.0, bestDist = std::numeric_limits<double>::max(), bestAlong = 0.0;
    for (size_t i = 0; i < outline.size(); ++i) {
        if (i > 0)
            total += dist(outline[i].pos, outline[i - 1].pos);
        const double d = dist(outline[i].pos, start);
        if (d < bestDist) {
            bestDist = d;
            bestAlong = total;
        }
        pts.push_back(outline[i].pos);
    }
    if (total > 0.0)
        r.startParam = std::fmod(bestAlong / total, 1.0);
    r.reversed = signedArea(pts) * winding < 0.0;
}

// The outline to run the brush along: the shape's geometry, rotated and
// turned to match the original stroke, with the stroke's dynamics laid on it.
std::vector<PathPoint> CanvasWidget::shapeStrokePath() const
{
    const StrokeRecipe& r = m_shape.recipe;
    const bool closed = m_shape.shape.closed();
    const double step = std::clamp(double(r.size) * 0.2, 1.0, 5.0);
    std::vector<PathPoint> path = m_shape.shape.path(step);
    if (path.size() < 2)
        return path;

    if (closed && path.size() > 3) {
        // The outline ends where it starts; treat it as a ring and cut it open
        // at the point where the stroke began.
        path.pop_back();
        const size_t n = path.size();
        double total = 0.0;
        for (size_t i = 0; i < n; ++i)
            total += dist(path[i].pos, path[(i + 1) % n].pos);
        const double target = r.startParam * total;
        size_t k = 0;
        double walked = 0.0, f = 0.0;
        for (; k < n; ++k) {
            const double seg = dist(path[k].pos, path[(k + 1) % n].pos);
            if (walked + seg >= target || k + 1 == n) {
                f = seg > 1e-9 ? std::clamp((target - walked) / seg, 0.0, 1.0) : 0.0;
                break;
            }
            walked += seg;
        }
        PathPoint startPt;
        startPt.pos = path[k].pos + (path[(k + 1) % n].pos - path[k].pos) * f;
        std::vector<PathPoint> ring{startPt};
        for (size_t i = 0; i < n; ++i)
            ring.push_back(r.reversed ? path[(k + n - i) % n] : path[(k + 1 + i) % n]);
        ring.push_back(startPt);
        path = std::move(ring);
    }

    std::vector<double> along(path.size(), 0.0);
    for (size_t i = 1; i < path.size(); ++i)
        along[i] = along[i - 1] + dist(path[i].pos, path[i - 1].pos);
    const double total = along.back();
    const StrokeDynamics& dyn = r.dynamics;
    const float endPressure = StrokeDynamics::at(dyn.pressure, 1.0);
    const float endSpeed = StrokeDynamics::at(dyn.speed, 1.0);
    for (size_t i = 0; i < path.size(); ++i) {
        const double t = total > 0.0 ? along[i] / total : 0.0;
        PathPoint& pt = path[i];
        pt.pressure = StrokeDynamics::at(dyn.pressure, t);
        pt.tiltX = StrokeDynamics::at(dyn.tiltX, t);
        pt.tiltY = StrokeDynamics::at(dyn.tiltY, t);
        pt.speed = StrokeDynamics::at(dyn.speed, t);
        if (closed && t < 0.1) {
            // A loop meets itself: ease out of the pressure it ends on, as the
            // hand-drawn overlap did, instead of stepping from heavy to light.
            const double u = t / 0.1;
            const float blend = float(u * u * (3.0 - 2.0 * u));
            pt.pressure = endPressure + (pt.pressure - endPressure) * blend;
            pt.speed = endSpeed + (pt.speed - endSpeed) * blend;
        }
    }
    return path;
}

void CanvasWidget::queueShapeUpdate(const PointerInput& in, bool handle)
{
    m_shapePending = in;
    (handle ? m_shapePendingHandle : m_shapePendingAdjust) = true;
    if (!m_shapeRedraw.isActive())
        m_shapeRedraw.start();
}

void CanvasWidget::flushShapeUpdate()
{
    m_shapeRedraw.stop();
    const bool adjust = m_shapePendingAdjust, handle = m_shapePendingHandle;
    m_shapePendingAdjust = m_shapePendingHandle = false;
    if (adjust && m_drag == Drag::Stroke && m_shape.snapped)
        adjustHeldShape(m_shapePending);
    if (handle && m_drag == Drag::Shape)
        dragShapeHandle(m_shapePending);
}

void CanvasWidget::renderShape()
{
    m_engine->setStrokePath(shapeStrokePath(), m_shape.shape.closed());
}

void CanvasWidget::adjustHeldShape(const PointerInput& in)
{
    ShapeState& st = m_shape;
    const bool force = m_shift || (in.mods & Qt::ShiftModifier);
    const bool free = m_alt || (in.mods & Qt::AltModifier);
    st.snapLabel.clear();

    if (!st.adjusting && dist(in.pos, st.grabPos) >= kAdjustDeadZone) {
        st.adjusting = true;
        st.grab = widgetToCanvas(in.pos);   // measure from here so the shape doesn't jump
    }
    QuickShape s = st.atGrab;
    const QPointF pen = widgetToCanvas(in.pos);
    if (s.kind == QuickShape::Kind::Line) {
        QPointF end = st.adjusting ? pen : s.pts[1];
        if (force || !free) {
            const double angle = QuickShape::snapLineEnd(s.pts[0], end, force);
            if (angle >= 0.0)
                st.snapLabel = degreesLabel(angle);
        }
        s.pts[1] = end;
    } else if (st.adjusting) {
        if (s.closed()) {
            followPoint(s, s.centroid(), st.grab, pen, free ? 0.0 : 4.0);
        } else {
            const std::vector<QPointF> ends = s.handles();
            followPoint(s, ends.front(), ends.back(), pen, 0.0);
        }
    }
    if (force && (s.kind == QuickShape::Kind::Ellipse || s.kind == QuickShape::Kind::Rectangle)) {
        s.makePerfect();
        st.snapLabel = s.name();
    }
    st.shape = s;
    renderShape();
    update();
}

// ------------------------------------------------------------------ editing

// Whether a point on screen is on the outline of the shape that can still be reopened.
bool CanvasWidget::nearOfferedShape(const QPointF& logical) const
{
    if (!m_shape.offered || !m_shape.shape.valid())
        return false;
    const double toScreen = m_zoom / devicePixelRatioF();
    // Anywhere on the drawn line counts, plus a finger's width beside it.
    const double reach = std::max(12.0, m_shape.recipe.size * toScreen * 0.5 + 8.0);
    const std::vector<PathPoint> outline = m_shape.shape.path(std::max(1.0, 4.0 / toScreen));
    QPointF previous;
    for (size_t i = 0; i < outline.size(); ++i) {
        const QPointF q = canvasToWidget(outline[i].pos);
        if (i > 0) {
            const QPointF ab = q - previous, ap = logical - previous;
            const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
            const double t = len2 > 1e-9 ? std::clamp((ap.x() * ab.x() + ap.y() * ab.y()) / len2, 0.0, 1.0) : 0.0;
            const QPointF nearest = previous + ab * t;
            if (std::hypot(logical.x() - nearest.x(), logical.y() - nearest.y()) <= reach)
                return true;
        }
        previous = q;
    }
    return false;
}

void CanvasWidget::offerShapeEdit()
{
    m_shape.offered = true;
    m_shape.offerSerial = m_engine->history().serial();
    m_shape.offerLayer = m_engine->activeLayerId();
    emit shapeEditOfferChanged(true);
}

void CanvasWidget::withdrawShapeOffer()
{
    if (!m_shape.offered)
        return;
    m_shape.offered = false;
    emit shapeEditOfferChanged(false);
}

void CanvasWidget::checkShapeOffer()
{
    // The offer only stands while the shape is still the latest thing that
    // happened, on the layer it was drawn on.
    if (m_shape.offered && (m_engine->history().serial() != m_shape.offerSerial || m_engine->activeLayerId() != m_shape.offerLayer))
        withdrawShapeOffer();
}

bool CanvasWidget::startShapeEdit()
{
    if (!m_shape.offered || m_shape.editing || m_drag != Drag::None || m_tf.active || m_engine->editActive())
        return false;
    const StrokeRecipe r = m_shape.recipe;
    const QuickShape shape = m_shape.shape;

    // Take the committed shape back off the layer and reopen it as a live stroke.
    m_engine->undo(true);
    StrokeSample first;
    first.pos = shape.centroid();
    first.pressure = StrokeDynamics::at(r.dynamics.pressure, 0.0);
    if (!m_engine->beginStroke(r.tool, r.preset, r.size, r.opacity, r.color, first, r.seed)) {
        m_engine->redo(true);
        return false;
    }
    m_shape.editing = true;
    m_shape.original = m_shape.lastFree = shape;
    m_shape.handle = m_shape.node = -1;
    m_shape.addArmed = false;
    m_shape.snapLabel.clear();
    // Lines that bend or turn are edited point by point from the start. The
    // outline is the same either way; what changes is that a point can show
    // its handles. Shapes with a form of their own keep it, and their own
    // handles, until a point is added, removed or changed.
    using Kind = QuickShape::Kind;
    if (shape.kind == Kind::Curve || shape.kind == Kind::Polyline || (shape.kind == Kind::Polygon && shape.pts.size() != 4))
        m_shape.shape = shape.toPath();
    renderShape();
    updateCursorShape();
    emit shapeEditStarted();
    update();
    return true;
}

void CanvasWidget::finishShapeEdit(bool commit)
{
    if (!m_shape.editing)
        return;
    if (!commit) {
        m_shape.shape = m_shape.original;
        renderShape();
    }
    m_engine->endStroke(tr("Shape"));
    m_shape.editing = false;
    m_shape.handle = m_shape.node = -1;
    m_shape.addArmed = false;
    m_shape.toggleOnRelease = -1;
    m_shape.snapLabel.clear();
    m_drag = Drag::None;
    updateCursorShape();
    offerShapeEdit();   // still the latest thing on the canvas, so it can be reopened
    emit shapeEditFinished();
    update();
}

CanvasWidget::ShapeFamily CanvasWidget::shapeFamily() const
{
    const QuickShape& s = m_shape.shape;
    if (s.kind == QuickShape::Kind::Ellipse)
        return ShapeFamily::Ellipse;
    if (s.kind == QuickShape::Kind::Rectangle || (s.kind == QuickShape::Kind::Polygon && s.pts.size() == 4))
        return ShapeFamily::Quad;
    return ShapeFamily::Other;
}

QString CanvasWidget::shapeVariant() const
{
    const QuickShape& s = m_shape.shape;
    switch (s.kind) {
    case QuickShape::Kind::Ellipse: return s.perfect() ? QStringLiteral("circle") : QStringLiteral("ellipse");
    case QuickShape::Kind::Rectangle: return s.perfect() ? QStringLiteral("square") : QStringLiteral("rectangle");
    case QuickShape::Kind::Polygon: return s.pts.size() == 4 ? QStringLiteral("quad") : QString();
    default: return {};
    }
}

void CanvasWidget::setShapeVariant(const QString& variant)
{
    if (!m_shape.editing)
        return;
    QuickShape s = m_shape.shape;
    using Kind = QuickShape::Kind;
    // Going back from circle/square restores the proportions it had before.
    auto restoreFree = [&](Kind kind) {
        if (s.kind == kind && s.perfect() && m_shape.lastFree.kind == kind && !m_shape.lastFree.perfect()) {
            s.rx = m_shape.lastFree.rx;
            s.ry = m_shape.lastFree.ry;
            if (kind == Kind::Ellipse)
                s.rot = m_shape.lastFree.rot;
        }
    };
    auto perfect = [&] {
        if (!s.perfect()) {
            m_shape.lastFree = s;
            s.makePerfect();
        }
    };

    if (variant == QLatin1String("circle") && s.kind == Kind::Ellipse) {
        perfect();
    } else if (variant == QLatin1String("ellipse")) {
        restoreFree(Kind::Ellipse);
    } else if (variant == QLatin1String("square")) {
        s = s.asRectangle();
        if (s.kind == Kind::Rectangle)
            perfect();
    } else if (variant == QLatin1String("rectangle")) {
        s = s.asRectangle();
        restoreFree(Kind::Rectangle);
    } else if (variant == QLatin1String("quad")) {
        s = s.asPolygon();
    }
    replaceShape(s);
    m_shape.node = -1;
    renderShape();
    emit shapeEditChanged();
    update();
}

// Changes what the shape is made of without moving where its stroke begins
// or which way round it runs: every kind of outline starts at a place of its
// own, and so does a path after a point is added to it or taken out.
void CanvasWidget::replaceShape(const QuickShape& shape)
{
    const bool reanchor = shape.closed() && m_shape.shape.closed();
    QPointF startPoint;
    double winding = 1.0;
    if (reanchor) {
        const std::vector<PathPoint> current = shapeStrokePath();
        std::vector<QPointF> pts;
        for (const PathPoint& pt : current)
            pts.push_back(pt.pos);
        startPoint = pts.empty() ? QPointF() : pts.front();
        winding = signedArea(pts);
    }
    m_shape.shape = shape;
    if (reanchor)
        anchorShapeStart(startPoint, winding);
}

QVector<QPointF> CanvasWidget::shapeHandlePositions() const
{
    QVector<QPointF> out;
    for (const QPointF& h : m_shape.shape.handles())
        out.push_back(canvasToWidget(h));
    return out;
}

QPointF CanvasWidget::shapeKnobPos() const
{
    const QuickShape& s = m_shape.shape;
    const QPointF top = canvasToWidget(s.center + rotated(QPointF(0.0, -s.ry), s.rot));
    const QPointF centre = canvasToWidget(s.center);
    QPointF dir = top - centre;
    const double len = std::hypot(dir.x(), dir.y());
    dir = len > 1e-6 ? dir / len : QPointF(0, -1);
    return top + dir * 30.0;
}

QVector<QPointF> CanvasWidget::shapeArmPositions() const
{
    const QuickShape& s = m_shape.shape;
    const int k = m_shape.node;
    if (!m_shape.editing || s.kind != QuickShape::Kind::Path || k < 0 || size_t(k) >= s.nodes.size() || !s.nodes[size_t(k)].smooth)
        return {};
    return {canvasToWidget(s.handleIn(size_t(k))), canvasToWidget(s.handleOut(size_t(k)))};
}

int CanvasWidget::hitShapeHandle(const QPointF& logical) const
{
    const QuickShape& s = m_shape.shape;
    if ((s.kind == QuickShape::Kind::Ellipse || s.kind == QuickShape::Kind::Rectangle) && dist(shapeKnobPos(), logical) <= 13.0)
        return kShapeRotate;
    int best = -1;
    double bestDist = 13.0;
    const QVector<QPointF> handles = shapeHandlePositions();
    for (int i = 0; i < handles.size(); ++i) {
        const double d = dist(handles[i], logical);
        if (d <= bestDist) {
            bestDist = d;
            best = i;
        }
    }
    // The chosen point's own handles; an end has only the one that points along the line.
    const QVector<QPointF> arms = shapeArmPositions();
    for (int side = 0; side < arms.size(); ++side) {
        if (dist(arms[side], handles[m_shape.node]) < 1.0)
            continue;
        const double d = dist(arms[side], logical);
        if (d <= bestDist) {
            bestDist = d;
            best = (side ? kShapeOut : kShapeIn) + m_shape.node;
        }
    }
    return best;
}

// The point of the path that a handle of the shape stands for, turning the
// shape into a path first if it is not one yet.
int CanvasWidget::shapeNodeFor(int handle)
{
    QuickShape& s = m_shape.shape;
    const std::vector<QPointF> handles = s.handles();
    if (handle < 0 || size_t(handle) >= handles.size())
        return -1;
    if (s.kind == QuickShape::Kind::Path)
        return handle;
    const QPointF at = handles[size_t(handle)];
    const QuickShape path = s.toPath();
    if (path.kind != QuickShape::Kind::Path || path.nodes.size() < 2)
        return -1;
    replaceShape(path);
    int best = -1;
    double bestDist = std::numeric_limits<double>::max();
    for (size_t k = 0; k < path.nodes.size(); ++k) {
        const double d = dist(path.nodes[k].pos, at);
        if (d < bestDist) {
            bestDist = d;
            best = int(k);
        }
    }
    return best;
}

int CanvasWidget::addShapePoint(const QPointF& logical)
{
    ShapeState& st = m_shape;
    QuickShape s = st.shape.toPath();
    if (s.kind != QuickShape::Kind::Path)
        return -1;
    // Anywhere on the drawn line counts, plus a finger's width beside it.
    const double toScreen = m_zoom / devicePixelRatioF();
    const double reach = std::max(14.0, st.recipe.size * toScreen * 0.5 + 8.0);
    const QuickShape::Hit hit = s.nearest(widgetToCanvas(logical));
    if (hit.segment < 0 || hit.distance * toScreen > reach)
        return -1;
    const int added = s.insertNode(hit.segment, hit.t);
    if (added < 0)
        return -1;
    replaceShape(s);
    st.node = added;
    renderShape();
    emit toast(tr("Point added"));
    emit shapeEditChanged();
    update();
    return added;
}

bool CanvasWidget::removeShapePoint(int handle)
{
    ShapeState& st = m_shape;
    if (!st.editing)
        return false;
    const QuickShape before = st.shape;
    const StrokeRecipe recipe = st.recipe;
    const int k = shapeNodeFor(handle);
    QuickShape s = st.shape;
    if (k < 0 || !s.removeNode(size_t(k))) {
        // Nothing to take out: leave the shape as it was, not as a path of the same outline.
        st.shape = before;
        st.recipe = recipe;
        if (k >= 0)
            emit toast(before.closed() ? tr("A closed shape needs three points") : tr("A line needs two points"));
        return false;
    }
    replaceShape(s);
    st.node = -1;
    renderShape();
    emit toast(tr("Point removed"));
    emit shapeEditChanged();
    update();
    return true;
}

bool CanvasWidget::removeShapePoint()
{
    return removeShapePoint(shapePoint());
}

bool CanvasWidget::shapeCanClose() const
{
    QuickShape s = m_shape.shape;
    return m_shape.editing && s.closePath();
}

bool CanvasWidget::setShapeClosed(bool closed)
{
    ShapeState& st = m_shape;
    if (!st.editing || st.shape.closed() == closed)
        return false;
    const QuickShape before = st.shape;
    const StrokeRecipe recipe = st.recipe;
    QuickShape s = st.shape;
    bool done = false;
    if (closed) {
        done = s.closePath();
    } else {
        const int k = st.node >= 0 ? shapeNodeFor(st.node) : 0;   // may turn the shape into a path first
        s = st.shape;
        done = k >= 0 && s.openPath(size_t(k));
    }
    if (!done) {
        st.shape = before;
        st.recipe = recipe;
        emit toast(closed ? tr("A line needs three points or more to close") : tr("This shape cannot be opened"));
        return false;
    }
    replaceShape(s);
    // Opened, the point it was cut at is the start of the line, and chosen, ready to be pulled away.
    st.node = closed ? -1 : 0;
    renderShape();
    emit toast(closed ? tr("Shape closed") : tr("Shape opened: drag the point to pull its ends apart"));
    emit shapeEditChanged();
    update();
    return true;
}

void CanvasWidget::toggleShapePoint(int handle)
{
    ShapeState& st = m_shape;
    if (!st.editing)
        return;
    const int k = shapeNodeFor(handle);
    if (k < 0)
        return;
    const bool smooth = !st.shape.nodes[size_t(k)].smooth;
    st.shape.setSmooth(size_t(k), smooth);
    st.node = k;
    renderShape();
    emit toast(smooth ? tr("Smooth point") : tr("Sharp corner"));
    emit shapeEditChanged();
    update();
}

bool CanvasWidget::shapePointSmooth() const
{
    const QuickShape& s = m_shape.shape;
    const int k = shapePoint();
    if (k < 0)
        return false;
    if (s.kind == QuickShape::Kind::Path)
        return size_t(k) < s.nodes.size() && s.nodes[size_t(k)].smooth;
    // What the point will be once the shape is a path: round shapes are made of smooth ones.
    return s.kind == QuickShape::Kind::Ellipse || s.kind == QuickShape::Kind::Arc || s.kind == QuickShape::Kind::Curve;
}

void CanvasWidget::setShapePointSmooth(bool smooth)
{
    if (shapePoint() >= 0 && shapePointSmooth() != smooth)
        toggleShapePoint(shapePoint());
}

void CanvasWidget::armShapeAdd(bool armed)
{
    if (!m_shape.editing || m_shape.addArmed == armed)
        return;
    m_shape.addArmed = armed;
    if (armed)
        emit toast(tr("Tap the line where the point should go"));
    emit shapeEditChanged();
}

void CanvasWidget::shapePress(const PointerInput& in)
{
    ShapeState& st = m_shape;
    const bool ctrl = in.mods & Qt::ControlModifier;
    const bool alt = m_alt || (in.mods & Qt::AltModifier);
    const int hit = hitShapeHandle(in.pos);
    const bool onPoint = hit >= 0 && hit < kShapeIn;
    st.handle = hit >= 0 ? hit : kShapeMove;
    st.startMouse = widgetToCanvas(in.pos);
    st.pressPos = in.pos;
    st.pressMoved = false;
    st.toggleOnRelease = -1;
    st.snapLabel.clear();
    m_drag = Drag::Shape;

    if (ctrl && onPoint) {
        removeShapePoint(hit);
        st.handle = kShapeIdle;
    } else if ((ctrl || st.addArmed) && hit < 0) {
        // The new point is under the pen: keep hold of it, so that it can be
        // put where it belongs in the same movement.
        const int added = addShapePoint(in.pos);
        if (added >= 0) {
            st.handle = added;
            if (st.addArmed) {
                st.addArmed = false;
                emit shapeEditChanged();
            }
        } else if (ctrl) {
            st.handle = kShapeIdle;
        }
    } else if (onPoint) {
        if (alt)
            st.toggleOnRelease = hit;
        if (st.node != hit) {
            st.node = hit;
            emit shapeEditChanged();
        }
    }
    st.startShape = st.shape;
    update();
}

void CanvasWidget::shapeRelease()
{
    ShapeState& st = m_shape;
    if (st.toggleOnRelease >= 0) {
        toggleShapePoint(st.toggleOnRelease);
    } else if (st.handle == kShapeMove && !st.pressMoved && st.node >= 0) {
        // A tap beside the shape lets go of the point.
        st.node = -1;
        emit shapeEditChanged();
    }
    st.toggleOnRelease = -1;
    st.handle = -1;
    st.snapLabel.clear();
}

void CanvasWidget::dragShapeHandle(const PointerInput& in)
{
    using Kind = QuickShape::Kind;
    ShapeState& st = m_shape;
    const QuickShape& start = st.startShape;
    QuickShape s = start;
    const QPointF m = widgetToCanvas(in.pos);
    // Shift forces the perfect version, Alt switches the magnets off.
    const bool force = m_shift || (in.mods & Qt::ShiftModifier);
    const bool free = m_alt || (in.mods & Qt::AltModifier);
    const int h = st.handle;
    st.snapLabel.clear();
    if (dist(in.pos, st.pressPos) >= kTapWobble) {
        st.pressMoved = true;
        st.toggleOnRelease = -1;
    }
    // Alt went down on a point and the pen has not gone anywhere yet: still a tap.
    if (h < 0 || h == kShapeIdle || st.toggleOnRelease >= 0)
        return;

    if (h == kShapeMove) {
        s.translate(m - st.startMouse);
    } else if (s.kind == Kind::Path) {
        // By how far the pen has gone, so that taking hold of a point a little off its middle does not jolt it.
        const QPointF by = m - st.startMouse;
        const size_t k = size_t(h >= kShapeOut ? h - kShapeOut : h >= kShapeIn ? h - kShapeIn : h);
        if (k >= s.nodes.size())
            return;
        if (h >= kShapeOut)
            s.setHandle(k, true, start.handleOut(k) + by);
        else if (h >= kShapeIn)
            s.setHandle(k, false, start.handleIn(k) + by);
        else
            s.nodes[k].pos = start.nodes[k].pos + by;
    } else if (h == kShapeRotate) {
        double rot = start.rot + angleOf(m - start.center) - angleOf(st.startMouse - start.center);
        if (force) {
            rot = std::round(rot / 15.0) * 15.0;
            st.snapLabel = degreesLabel(wrapDegrees(rot));
        } else if (!free) {
            const double nearest = std::round(rot / 45.0) * 45.0;
            if (std::abs(rot - nearest) < kRotationSnap) {
                rot = nearest;
                st.snapLabel = degreesLabel(wrapDegrees(rot));
            }
        }
        s.rot = rot;
    } else if (s.kind == Kind::Line) {
        QPointF end = m;
        if (force || !free) {
            const double angle = QuickShape::snapLineEnd(start.pts[size_t(1 - h)], end, force);
            if (angle >= 0.0)
                st.snapLabel = degreesLabel(angle);
        }
        s.pts[size_t(h)] = end;
    } else if (s.kind == Kind::Arc) {
        if (h == 1)
            s.pts[1] = m;   // the middle point bends the arc
        else
            followPoint(s, start.pts[size_t(2 - h)], start.pts[size_t(h)], m, 0.0);
    } else if (s.kind == Kind::Polygon || s.kind == Kind::Polyline || s.kind == Kind::Curve) {
        s.pts[size_t(h)] = m;   // a curve bends to pass through wherever its point is put
    } else if (s.kind == Kind::Ellipse) {
        // Handles 0 and 2 sit on the x radius, 1 and 3 on the y radius.
        const bool alongX = (h % 2) == 0;
        const QPointF axis = rotated(alongX ? QPointF(1, 0) : QPointF(0, 1), start.rot);
        double r = std::max(1.0, std::abs(dot(m - start.center, axis)));
        const double other = alongX ? start.ry : start.rx;
        if (force) {
            s.rx = s.ry = r;
        } else {
            if (!free && std::abs(r - other) <= kAspectSnap * std::max(r, other))
                r = other;
            (alongX ? s.rx : s.ry) = r;
        }
        if (s.perfect())
            st.snapLabel = s.name();
    } else if (s.kind == Kind::Rectangle) {
        // Resize from the dragged corner with the opposite corner pinned.
        const double sx = (h == 1 || h == 2) ? 1.0 : -1.0;
        const double sy = h >= 2 ? 1.0 : -1.0;
        const QPointF anchor = start.center + rotated(QPointF(-sx * start.rx, -sy * start.ry), start.rot);
        const QPointF v = rotated(m - anchor, -start.rot);
        double w = std::max(2.0, sx * v.x());
        double ht = std::max(2.0, sy * v.y());
        if (force)
            w = ht = std::max(w, ht);
        else if (!free && std::abs(w - ht) <= kAspectSnap * std::max(w, ht))
            w = ht = (w + ht) * 0.5;
        s.rx = w * 0.5;
        s.ry = ht * 0.5;
        s.center = anchor + rotated(QPointF(sx * s.rx, sy * s.ry), start.rot);
        if (s.perfect())
            st.snapLabel = s.name();
    }

    st.shape = s;
    renderShape();
    emit shapeEditChanged();
    update();
}

// ------------------------------------------------------------------ overlay

void CanvasWidget::drawShapeOverlay(QPainter& p)
{
    const ShapeState& st = m_shape;
    const bool held = m_drag == Drag::Stroke && st.snapped;
    if (!st.editing && !held)
        return;
    const QColor snapColor(0x34, 0xc7, 0x59);
    const bool snapped = !st.snapLabel.isEmpty();

    if (st.editing) {
        const QuickShape& s = st.shape;
        // The ideal outline, so thin or textured strokes are still easy to see.
        QPolygonF outline;
        for (const PathPoint& pt : s.path(std::max(1.0, 4.0 * devicePixelRatioF() / m_zoom)))
            outline << canvasToWidget(pt.pos);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 70), 2.6));
        p.drawPolyline(outline);
        p.setPen(QPen(snapped ? snapColor : theme::accent, 1.1));
        p.drawPolyline(outline);

        if (s.kind == QuickShape::Kind::Ellipse || s.kind == QuickShape::Kind::Rectangle) {
            const QPointF knob = shapeKnobPos();
            p.drawLine(canvasToWidget(s.center + rotated(QPointF(0.0, -s.ry), s.rot)), knob);
            p.setPen(QPen(QColor(0, 0, 0, 120), 1));
            p.setBrush(st.handle == kShapeRotate ? QColor(Qt::white) : snapColor);
            p.drawEllipse(knob, 6.5, 6.5);
        }
        const QVector<QPointF> handles = shapeHandlePositions();
        const bool path = s.kind == QuickShape::Kind::Path;
        // The chosen point's handles: a line out to a small knob either side.
        const QVector<QPointF> arms = shapeArmPositions();
        for (int side = 0; side < arms.size(); ++side) {
            const QPointF at = handles[st.node];
            if (dist(arms[side], at) < 1.0)
                continue;
            p.setPen(QPen(QColor(0, 0, 0, 70), 2.6));
            p.drawLine(at, arms[side]);
            p.setPen(QPen(theme::accent, 1.1));
            p.drawLine(at, arms[side]);
            const bool active = st.handle == (side ? kShapeOut : kShapeIn) + st.node;
            p.setPen(QPen(theme::accent, 1.6));
            p.setBrush(active ? theme::accent : QColor(Qt::white));
            p.drawEllipse(arms[side], active ? 5.5 : 4.5, active ? 5.5 : 4.5);
        }
        for (int i = 0; i < handles.size(); ++i) {
            const bool active = i == st.handle, chosen = i == st.node;
            const double r = active ? 7.0 : chosen ? 6.5 : 5.5;
            p.setPen(chosen && !active ? QPen(theme::accent, 2.0) : QPen(Qt::white, 1.5));
            p.setBrush(active && snapped ? snapColor : active || chosen ? QColor(Qt::white) : theme::accent);
            // Round for a smooth point, square for a sharp corner.
            if (path && !s.nodes[size_t(i)].smooth)
                p.drawRect(QRectF(handles[i].x() - r * 0.88, handles[i].y() - r * 0.88, r * 1.76, r * 1.76));
            else
                p.drawEllipse(handles[i], r, r);
        }
    }

    if (snapped) {
        // Says what the magnet caught: "Circle", "Square", "45°".
        p.setFont(theme::font(12, QFont::DemiBold));
        const QFontMetrics fm = p.fontMetrics();
        QRectF box(0, 0, fm.horizontalAdvance(st.snapLabel) + 18, 24);
        box.moveCenter(m_cursor + QPointF(0, -34));
        p.setPen(Qt::NoPen);
        p.setBrush(snapColor);
        p.drawRoundedRect(box, 12, 12);
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, st.snapLabel);
    }
}
