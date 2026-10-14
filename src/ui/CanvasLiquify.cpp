// Liquify in the viewport: the pen pushes, twirls, pinches or swells the
// paint of the active layer. The engine does the work on the graphics card;
// this turns pen movement into touches of the brush.

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"

#include <QPainter>
#include <cmath>

bool CanvasWidget::startLiquify()
{
    if (m_liquify.active || m_tf.active || m_shape.editing || m_crop.active || m_engine->editActive() || m_drag != Drag::None)
        return false;
    setSelectMode(false);
    setGradientMode(false);
    armFill(false);
    armEyedropper(false);
    withdrawShapeOffer();
    if (!m_engine->beginLiquify())
        return false;
    m_liquify.active = true;
    updateCursorShape();
    update();
    return true;
}

void CanvasWidget::finishLiquify(bool commit)
{
    if (!m_liquify.active)
        return;
    m_liquifyTimer.stop();
    m_liquify.active = false;
    if (m_drag == Drag::Liquify)
        m_drag = Drag::None;
    m_engine->endLiquify(commit);
    updateCursorShape();
    update();
    emit liquifyFinished();
}

void CanvasWidget::setLiquifySize(float px)
{
    px = std::clamp(px, kLiquifyMinSize, kLiquifyMaxSize);
    if (px == m_liquify.size)
        return;
    m_liquify.size = px;
    update();
    emit liquifySizeChanged();
}

float CanvasWidget::liquifySizeFraction() const
{
    return std::sqrt(std::clamp((m_liquify.size - kLiquifyMinSize) / (kLiquifyMaxSize - kLiquifyMinSize), 0.0f, 1.0f));
}

void CanvasWidget::setLiquifySizeFraction(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    setLiquifySize(kLiquifyMinSize + t * t * (kLiquifyMaxSize - kLiquifyMinSize));
}

void CanvasWidget::liquifyPress(const PointerInput& in)
{
    if (m_shift || (in.mods & Qt::ShiftModifier)) {
        // Shift + drag sideways sizes this brush, as it does the paint brush.
        m_drag = Drag::Size;
        m_sizingLiquify = true;
        m_sizeAnchor = in.pos;
        m_sizeStart = liquifySizeFraction();
        update();
        return;
    }
    m_liquify.last = widgetToCanvas(in.pos);
    m_liquify.pressure = in.pressure;
    m_drag = Drag::Liquify;
    // Twirl, pinch, expand and restore work on a brush held still as well, so
    // they run on a clock; push only acts when the pen moves.
    if (m_liquify.mode != LiquifyMode::Push) {
        liquifyTouch(m_liquify.last);
        m_liquifyTimer.start();
    }
    update();
}

void CanvasWidget::liquifyMove(const PointerInput& in)
{
    m_liquify.pressure = in.pressure;
    liquifyTouch(widgetToCanvas(in.pos));
}

void CanvasWidget::liquifyTouch(const QPointF& canvasPos)
{
    // A light touch does less: the pen's pressure scales the strength.
    const float strength = m_liquify.strength * (0.35f + 0.65f * std::clamp(curved(m_liquify.pressure), 0.0f, 1.0f));
    m_engine->liquify(m_liquify.mode, m_liquify.last, canvasPos, m_liquify.size * 0.5f, strength);
    m_liquify.last = canvasPos;
    update();
}

void CanvasWidget::drawLiquifyCursor(QPainter& p)
{
    if (!m_liquify.active || !m_cursorInside || m_drag == Drag::Size)
        return;
    const double r = std::max(3.0, m_liquify.size * m_zoom / devicePixelRatioF() / 2.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 130), 2.4));
    p.drawEllipse(m_cursor, r, r);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1.1));
    p.drawEllipse(m_cursor, r, r);
    p.drawEllipse(m_cursor, 1.5, 1.5);
}
