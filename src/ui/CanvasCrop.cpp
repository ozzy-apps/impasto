// Cropping in the viewport: a frame with handles over the canvas. Pulled
// inwards it crops; pulled out past the edge it adds room.

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"

#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace {
constexpr double kHandleReach = 16.0;   // widget px
constexpr double kEdgeSnap = 9.0;       // widget px within which the frame catches on the canvas edge
constexpr int kMinSide = 8;             // canvas px
constexpr int kMoveHandle = 8;
}

bool CanvasWidget::startCrop()
{
    if (m_crop.active || m_tf.active || m_shape.editing || m_engine->editActive() || m_drag != Drag::None || !m_engine->hasDocument())
        return false;
    setSelectMode(false);
    armFill(false);
    armEyedropper(false);
    withdrawShapeOffer();
    m_crop = {};
    m_crop.active = true;
    m_crop.rect = m_engine->canvasRect();
    updateCursorShape();
    update();
    emit cropChanged();
    return true;
}

void CanvasWidget::finishCrop(bool commit)
{
    if (!m_crop.active)
        return;
    const QRect frame = m_crop.rect;
    m_crop = {};
    if (m_drag == Drag::Crop)
        m_drag = Drag::None;
    if (commit && frame != m_engine->canvasRect())
        m_engine->resizeCanvas(frame.size(), QTransform::fromTranslate(-frame.left(), -frame.top()), tr("Crop Canvas"));
    updateCursorShape();
    update();
    emit cropFinished();
}

void CanvasWidget::setCropRect(const QRect& rect)
{
    if (!m_crop.active)
        return;
    const int limit = CanvasEngine::maxCanvasDimension();
    QRect r = rect.normalized();
    r.setWidth(std::clamp(r.width(), kMinSide, limit));
    r.setHeight(std::clamp(r.height(), kMinSide, limit));
    if (r == m_crop.rect)
        return;
    m_crop.rect = r;
    update();
    emit cropChanged();
}

// Corners and edge midpoints, clockwise from the top left; the same numbering as the transform box.
QVector<QPointF> CanvasWidget::cropHandlePositions() const
{
    if (!m_crop.active)
        return {};
    const QRectF r(m_crop.rect);
    const QPointF c = r.center();
    QVector<QPointF> out;
    for (const QPointF& q : {r.topLeft(), QPointF(c.x(), r.top()), r.topRight(), QPointF(r.right(), c.y()), r.bottomRight(),
                             QPointF(c.x(), r.bottom()), r.bottomLeft(), QPointF(r.left(), c.y())})
        out.push_back(canvasToWidget(q));
    return out;
}

void CanvasWidget::cropPress(const PointerInput& in)
{
    const QVector<QPointF> handles = cropHandlePositions();
    int hit = -1;
    double best = kHandleReach;
    for (int i = 0; i < handles.size(); ++i) {
        const double d = std::hypot(handles[i].x() - in.pos.x(), handles[i].y() - in.pos.y());
        if (d < best) {
            best = d;
            hit = i;
        }
    }
    // Anywhere else inside the frame moves it whole; outside does nothing.
    if (hit < 0 && QRectF(m_crop.rect).contains(widgetToCanvas(in.pos)))
        hit = kMoveHandle;
    if (hit < 0)
        return;
    m_crop.handle = hit;
    m_crop.startMouse = widgetToCanvas(in.pos);
    m_crop.startRect = m_crop.rect;
    m_drag = Drag::Crop;
    update();
}

void CanvasWidget::cropMove(const PointerInput& in)
{
    const QPointF d = widgetToCanvas(in.pos) - m_crop.startMouse;
    const QRect start = m_crop.startRect;
    const QRect canvas = m_engine->canvasRect();
    // Edges are whole pixels, and catch on the edges of the canvas as they pass.
    const double catchPx = kEdgeSnap * devicePixelRatioF() / m_zoom;
    auto settle = [&](double v, int a, int b) {
        if (std::abs(v - a) < catchPx)
            return a;
        if (std::abs(v - b) < catchPx)
            return b;
        return int(std::lround(v));
    };
    int left = start.left(), top = start.top(), right = start.left() + start.width(), bottom = start.top() + start.height();
    const int h = m_crop.handle;
    if (h == kMoveHandle) {
        const int nx = settle(left + d.x(), canvas.left(), canvas.left() + canvas.width() - start.width());
        const int ny = settle(top + d.y(), canvas.top(), canvas.top() + canvas.height() - start.height());
        setCropRect(QRect(nx, ny, start.width(), start.height()));
        return;
    }
    const bool movesLeft = h == 0 || h == 6 || h == 7, movesRight = h == 2 || h == 3 || h == 4;
    const bool movesTop = h == 0 || h == 1 || h == 2, movesBottom = h == 4 || h == 5 || h == 6;
    if (movesLeft)
        left = std::min(settle(left + d.x(), canvas.left(), canvas.left() + canvas.width()), right - kMinSide);
    if (movesRight)
        right = std::max(settle(right + d.x(), canvas.left(), canvas.left() + canvas.width()), left + kMinSide);
    if (movesTop)
        top = std::min(settle(top + d.y(), canvas.top(), canvas.top() + canvas.height()), bottom - kMinSide);
    if (movesBottom)
        bottom = std::max(settle(bottom + d.y(), canvas.top(), canvas.top() + canvas.height()), top + kMinSide);
    setCropRect(QRect(left, top, right - left, bottom - top));
}

void CanvasWidget::drawCropOverlay(QPainter& p)
{
    if (!m_crop.active)
        return;
    const QRectF frame(m_crop.rect);
    QPolygonF corners;
    for (const QPointF& q : {frame.topLeft(), frame.topRight(), frame.bottomRight(), frame.bottomLeft()})
        corners << canvasToWidget(q);

    // Everything that will not be in the picture goes dark.
    QPainterPath outside;
    outside.addRect(QRectF(rect()));
    QPainterPath kept;
    kept.addPolygon(corners);
    kept.closeSubpath();
    p.fillPath(outside.subtracted(kept), QColor(0, 0, 0, 150));

    // Thirds, to compose by.
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    for (int i = 1; i <= 2; ++i) {
        const double t = i / 3.0;
        p.drawLine(canvasToWidget(QPointF(frame.left() + frame.width() * t, frame.top())),
                   canvasToWidget(QPointF(frame.left() + frame.width() * t, frame.bottom())));
        p.drawLine(canvasToWidget(QPointF(frame.left(), frame.top() + frame.height() * t)),
                   canvasToWidget(QPointF(frame.right(), frame.top() + frame.height() * t)));
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 3));
    p.drawPolygon(corners);
    p.setPen(QPen(Qt::white, 1.5));
    p.drawPolygon(corners);
    const QVector<QPointF> handles = cropHandlePositions();
    for (int i = 0; i < handles.size(); ++i) {
        p.setPen(QPen(QColor(0, 0, 0, 130), 1));
        p.setBrush(i == m_crop.handle && m_drag == Drag::Crop ? theme::accent : QColor(Qt::white));
        p.drawEllipse(handles[i], 6, 6);
    }

    // The size it will be, at the middle of the frame.
    const QString label = QStringLiteral("%1 × %2").arg(m_crop.rect.width()).arg(m_crop.rect.height());
    p.setFont(theme::font(13, QFont::DemiBold));
    const QPointF mid = canvasToWidget(frame.center());
    const QRectF tag(mid.x() - 62, mid.y() - 14, 124, 28);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(28, 28, 30, 215));
    p.drawRoundedRect(tag, 14, 14);
    p.setPen(Qt::white);
    p.drawText(tag, Qt::AlignCenter, label);
}

// ---------------------------------------------------------------- gradient

void CanvasWidget::setGradientMode(bool on)
{
    if (m_gradient.mode == on)
        return;
    if (on) {
        if (m_tf.active || m_shape.editing || m_crop.active || m_engine->editActive() || m_drag != Drag::None)
            return;
        setSelectMode(false);
        setTextMode(false);
        armFill(false);
        armEyedropper(false);
    } else if (m_drag == Drag::Gradient) {
        m_engine->endGradient(true);
        m_drag = Drag::None;
    }
    m_gradient.mode = on;
    updateCursorShape();
    update();
    emit gradientModeChanged(on);
}

void CanvasWidget::applyGradient()
{
    if (m_gradient.custom && m_gradient.stops.size() >= 2) {
        m_engine->setGradient(m_gradient.from, m_gradient.to, m_gradient.stops, m_gradient.kind);
        return;
    }
    const QColor first = m_tools->color();
    QColor second = m_gradient.second;
    if (m_gradient.toTransparent)
        second = QColor(first.red(), first.green(), first.blue(), 0);
    m_engine->setGradient(m_gradient.from, m_gradient.to, first, second, m_gradient.kind);
}
