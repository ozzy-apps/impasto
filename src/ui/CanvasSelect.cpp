// Selection mode and colour fill for the viewport.

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"

#include <QPainter>
#include <cmath>

// --------------------------------------------------------------- selecting

void CanvasWidget::setSelectMode(bool on)
{
    if (m_select.mode == on)
        return;
    if (m_drag == Drag::Flood)
        endFloodDrag(true);
    if (on) {
        if (m_tf.active || m_shape.editing || m_engine->editActive() || m_drag != Drag::None)
            return;
        armEyedropper(false);
        armFill(false);
        setGradientMode(false);
        setTextMode(false);
        m_select.subtract = false;   // a fresh visit starts out adding
    }
    m_select.mode = on;
    m_select.points.clear();
    updateCursorShape();
    emit selectModeChanged(on);
    update();
}

void CanvasWidget::selectPress(const PointerInput& in)
{
    m_select.start = m_select.current = widgetToCanvas(in.pos);
    m_select.points = {m_select.start};
    m_select.lastPos = in.pos;
    m_drag = Drag::Select;
    update();
}

void CanvasWidget::selectMove(const PointerInput& in)
{
    m_select.current = widgetToCanvas(in.pos);
    if (std::hypot(in.pos.x() - m_select.lastPos.x(), in.pos.y() - m_select.lastPos.y()) >= 1.5) {
        m_select.points.push_back(m_select.current);
        m_select.lastPos = in.pos;
    }
}

QPainterPath CanvasWidget::selectDragPath() const
{
    QPainterPath path;
    const QRectF box = QRectF(m_select.start, m_select.current).normalized();
    switch (m_select.shape) {
    case SelectShape::Automatic:
        break;
    case SelectShape::Rectangle:
        path.addRect(box);
        break;
    case SelectShape::Ellipse:
        path.addEllipse(box);
        break;
    case SelectShape::Freehand:
        if (m_select.points.size() >= 3) {
            // A lasso may cross itself; winding fill keeps every loop selected.
            path.setFillRule(Qt::WindingFill);
            path.moveTo(m_select.points.front());
            for (size_t i = 1; i < m_select.points.size(); ++i)
                path.lineTo(m_select.points[i]);
            path.closeSubpath();
        }
        break;
    }
    return path;
}

void CanvasWidget::selectRelease(const PointerInput& in)
{
    const QPainterPath drawn = selectDragPath();
    m_select.points.clear();
    const QRectF box = drawn.boundingRect();
    if (drawn.isEmpty() || box.width() < 2.0 || box.height() < 2.0)
        return;   // a stray tap changes nothing
    // Alt flips the mode for one drag: remove while adding, add while removing.
    const bool subtract = m_select.subtract != bool(m_alt || (in.mods & Qt::AltModifier));
    m_engine->selectPath(drawn, subtract ? SelectOp::Subtract : SelectOp::Add);
}

void CanvasWidget::drawSelectionOverlay(QPainter& p)
{
    // The selection itself is outlined by the presentation shader; this is
    // only the shape being dragged out.
    if (m_drag != Drag::Select)
        return;
    const double dpr = devicePixelRatioF();
    const QTransform toWidget = viewTransform() * QTransform::fromScale(1.0 / dpr, 1.0 / dpr);
    const bool subtract = m_select.subtract != m_alt;
    const QColor c = subtract ? QColor(0xff, 0x45, 0x3a) : theme::accent;
    QPainterPath drawn = selectDragPath();
    if (drawn.isEmpty() && m_select.points.size() == 2) {
        drawn.moveTo(m_select.points[0]);
        drawn.lineTo(m_select.points[1]);
    }
    p.setPen(QPen(QColor(0, 0, 0, 90), 2.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(toWidget.map(drawn));
    p.setPen(QPen(c, 1.3));
    p.setBrush(QColor(c.red(), c.green(), c.blue(), 30));
    p.drawPath(toWidget.map(drawn));
}

// ------------------------------------------------- flood: fill and automatic

namespace {
constexpr double kThresholdSpan = 480.0;   // widget px of sideways travel from 0 to 100%
constexpr double kThresholdSlack = 10.0;   // travel that is still just a tap
} // namespace

bool CanvasWidget::beginFloodDrag(const QPointF& widgetPos, bool select, bool subtract, bool fromDrop)
{
    if (m_tf.active || m_shape.editing || m_engine->editActive() || m_engine->strokeActive() || m_engine->floodActive())
        return false;
    const QPointF c = widgetToCanvas(widgetPos);
    const QPoint px(int(std::floor(c.x())), int(std::floor(c.y())));
    if (!m_engine->canvasRect().contains(px)) {
        if (!select && !fromDrop)
            emit toast(tr("Drop the colour on the canvas"));
        return false;
    }
    withdrawShapeOffer();
    const float tolerance = select ? m_selectTolerance : m_fillTolerance;
    if (!m_engine->beginFlood(select ? FloodKind::Select : FloodKind::Fill, px, tolerance, m_tools->color(),
                              subtract ? SelectOp::Subtract : SelectOp::Add))
        return false;
    m_flood = {};
    m_flood.select = select;
    m_flood.fromDrop = fromDrop;
    m_flood.start = widgetPos;
    m_flood.startTolerance = tolerance;
    if (!select)
        m_tools->noteColorUsed();
    update();
    return true;
}

void CanvasWidget::moveFloodDrag(const QPointF& widgetPos)
{
    if (!m_engine->floodActive())
        return;
    const double dx = widgetPos.x() - m_flood.start.x();
    if (!m_flood.adjusting) {
        if (std::abs(dx) < kThresholdSlack)
            return;
        // Count from here, so the threshold does not jump as it starts to move.
        m_flood.adjusting = true;
        emit thresholdShown();
        m_flood.start.rx() += dx > 0 ? kThresholdSlack : -kThresholdSlack;
    }
    const float t = std::clamp(m_flood.startTolerance + float((widgetPos.x() - m_flood.start.x()) / kThresholdSpan), 0.0f, 1.0f);
    if (std::abs(t - m_engine->floodTolerance()) >= 0.004f)
        m_engine->setFloodTolerance(t);
    update();
}

bool CanvasWidget::floodAdjusting() const
{
    return m_engine->floodActive();
}

void CanvasWidget::endFloodDrag(bool commit)
{
    if (!m_engine->floodActive())
        return;
    const float tolerance = m_engine->floodTolerance();
    const FloodDrag drag = m_flood;
    m_flood = {};
    if (commit)
        m_engine->endFlood();
    else
        m_engine->cancelFlood();
    if (m_drag == Drag::Flood)
        m_drag = Drag::None;
    if (!drag.select)
        armFill(false);
    if (commit && drag.adjusting) {
        // The threshold that was settled on is the one the next fill starts from.
        if (drag.select) {
            m_selectTolerance = tolerance;
            emit selectToleranceChanged(tolerance);
        } else {
            m_fillTolerance = tolerance;
            emit fillToleranceChanged(tolerance);
        }
    }
    update();
}

void CanvasWidget::drawThreshold(QPainter& p)
{
    if (!m_engine->floodActive() || !m_flood.adjusting)
        return;
    const float t = m_engine->floodTolerance();
    const QRectF pill((width() - 230) / 2.0, 78, 230, 34);
    QPainterPath shape;
    shape.addRoundedRect(pill, 17, 17);
    p.save();
    p.setClipPath(shape);
    p.fillRect(pill, QColor(28, 28, 30, 235));
    p.fillRect(QRectF(pill.left(), pill.top(), pill.width() * t, pill.height()), theme::accent);
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 60), 1));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    p.setFont(theme::font(12, QFont::DemiBold));
    p.setPen(Qt::white);
    p.drawText(pill, Qt::AlignCenter,
               (m_flood.select ? tr("Selection threshold %1%") : tr("Fill threshold %1%")).arg(qRound(t * 100)));
}

// -------------------------------------------------------------------- fill

void CanvasWidget::armFill(bool armed)
{
    if (m_fillArmed == armed)
        return;
    if (armed) {
        armEyedropper(false);
        setSelectMode(false);
    }
    m_fillArmed = armed;
    emit fillArmedChanged(armed);
    update();
}

void CanvasWidget::dropMove(const QPointF& widgetPos, bool allowed)
{
    if (m_engine->floodActive() && m_flood.fromDrop) {
        moveFloodDrag(widgetPos);
        return;
    }
    m_dropPos = widgetPos;
    m_dropVisible = true;
    if (!allowed) {
        m_dropHold.stop();
        m_dropTracking = false;
    } else if (!m_dropTracking || std::hypot(widgetPos.x() - m_dropAnchor.x(), widgetPos.y() - m_dropAnchor.y()) > 5.0) {
        // Still on the move: the wait for it to come to rest starts over.
        m_dropAnchor = widgetPos;
        m_dropTracking = true;
        m_dropHold.start();
    }
    update();
}

bool CanvasWidget::dropRelease(const QPointF& widgetPos, bool allowed)
{
    m_dropHold.stop();
    m_dropTracking = false;
    m_dropVisible = false;
    update();
    if (m_engine->floodActive() && m_flood.fromDrop) {
        endFloodDrag(true);
        return true;
    }
    return allowed && fillAt(widgetPos);
}

bool CanvasWidget::fillAt(const QPointF& widgetPos)
{
    if (m_tf.active || m_shape.editing || m_engine->editActive() || m_drag == Drag::Stroke)
        return false;
    const QPointF c = widgetToCanvas(widgetPos);
    const QPoint px(int(std::floor(c.x())), int(std::floor(c.y())));
    if (!m_engine->canvasRect().contains(px)) {
        emit toast(tr("Drop the colour on the canvas"));
        return false;
    }
    withdrawShapeOffer();
    if (!m_engine->floodFill(px, m_tools->color(), m_fillTolerance))
        return false;
    m_tools->noteColorUsed();
    return true;
}
