// A 3D model placed on the canvas. The panel's dashed frame lands on the
// canvas as a frame of its own with the model still live inside it: a drag in
// the frame turns, poses or lights the model as the mode says, a corner of
// the frame scales the picture, a side crops it, and its edge moves it. Apply
// draws it as a layer the frame's size; nothing touches the artwork before.

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/ModelView.h"
#include "ui/Theme.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kFrameMove = 8;        // the frame's edge: the whole picture moves
constexpr int kFrameInside = 9;      // the model itself
constexpr double kMinFrame = 16.0;   // canvas px
constexpr int kLiveSide = 2048;      // the most pixels along a side while it is being worked on
constexpr int kHx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
constexpr int kHy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};

QPointF framePoint(const QRectF& r, int h)
{
    return {r.center().x() + kHx[h] * r.width() / 2.0, r.center().y() + kHy[h] * r.height() / 2.0};
}

ModelView::Window windowOf(const QRectF& frame, const QPointF& centre, double height, double hitRadius)
{
    return {frame, centre, std::max(1.0, height), hitRadius};
}
} // namespace

bool CanvasWidget::startModelPlace(ModelView* view)
{
    if (!view || !view->model() || m_model.active || m_tf.active || m_shape.editing || m_crop.active || m_liquify.active || m_text.mode
        || m_engine->editActive() || m_drag != Drag::None || !m_engine->hasDocument())
        return false;
    setSelectMode(false);
    setGradientMode(false);
    armFill(false);
    armEyedropper(false);
    withdrawShapeOffer();
    const QSizeF cs(m_engine->canvasSize());
    m_model = {};
    m_model.active = true;
    m_model.view = view;
    // The panel's frame, the canvas's shape, lands in the middle at seven tenths of the canvas.
    const double h = cs.height() * 0.7, w = h * cs.width() / cs.height();
    m_model.frame = QRectF((cs.width() - w) / 2.0, (cs.height() - h) / 2.0, w, h);
    m_model.centre = m_model.frame.center();
    m_model.height = h;
    m_model.dirty = true;
    updateCursorShape();
    update();
    return true;
}

void CanvasWidget::finishModelPlace(bool commit)
{
    if (!m_model.active)
        return;
    bool placed = false;
    if (commit) {
        // Only what lies on the canvas, at the canvas's own pixels.
        const QRect px = m_model.frame.toAlignedRect().intersected(QRect(QPoint(), m_engine->canvasSize()));
        if (px.isEmpty()) {
            emit toast(tr("The frame is off the canvas"));
            return;
        }
        const QImage image = m_model.view->render(windowOf(QRectF(px), m_model.centre, m_model.height, 16.0), px.size(), true);
        if (image.isNull()) {
            emit toast(tr("The model could not be drawn"));
            return;
        }
        placed = m_engine->addImageLayer(image, tr("3D reference"), px.topLeft(), tr("Place 3D Reference")) != 0;
    }
    m_model.active = false;
    m_model.view = nullptr;
    m_model.image = QImage();
    m_model.handle = -1;
    if (m_drag == Drag::Model)
        m_drag = Drag::None;
    updateCursorShape();
    update();
    emit modelPlaceFinished(placed);
}

void CanvasWidget::modelPlaceChanged()
{
    if (!m_model.active)
        return;
    m_model.dirty = true;
    update();
}

void CanvasWidget::modelPlaceFit()
{
    if (!m_model.active)
        return;
    m_model.view->fit();
    m_model.centre = m_model.frame.center();
    m_model.height = m_model.frame.height();
    modelPlaceChanged();
}

QVector<QPointF> CanvasWidget::modelHandlePositions() const
{
    QVector<QPointF> out;
    if (!m_model.active)
        return out;
    for (int h = 0; h < 8; ++h)
        out.push_back(canvasToWidget(framePoint(m_model.frame, h)));
    return out;
}

QVector<QPointF> CanvasWidget::modelJointPositions() const
{
    QVector<QPointF> out;
    if (!m_model.active)
        return out;
    for (const QPointF& c : m_model.view->handlePositions(windowOf(m_model.frame, m_model.centre, m_model.height, 16.0)))
        out.push_back(canvasToWidget(c));
    return out;
}

// What a press at that point takes hold of: a handle of the frame, its edge, the model, or nothing.
int CanvasWidget::modelHit(const QPointF& logical) const
{
    const QVector<QPointF> at = modelHandlePositions();
    for (int h : {0, 2, 4, 6, 1, 3, 5, 7}) {   // corners first: on a small frame they are what matters
        if (QLineF(at[h], logical).length() < 12.0)
            return h;
    }
    const QPointF c = widgetToCanvas(logical);
    const double tol = 10.0 * devicePixelRatioF() / m_zoom;   // ten widget pixels, in canvas pixels
    const QRectF& f = m_model.frame;
    if (f.adjusted(-tol, -tol, tol, tol).contains(c) && !f.adjusted(tol, tol, -tol, -tol).contains(c))
        return kFrameMove;
    if (f.contains(c))
        return kFrameInside;
    return -1;
}

void CanvasWidget::modelPress(const PointerInput& in)
{
    const int hit = modelHit(in.pos);
    if (hit < 0)
        return;
    ModelSession& s = m_model;
    s.handle = hit;
    s.startMouse = widgetToCanvas(in.pos);
    s.startFrame = s.frame;
    s.startCentre = s.centre;
    s.startHeight = s.height;
    if (hit == kFrameInside) {
        // Handles are as easy to hit as in the panel, whatever the zoom.
        const double radius = 16.0 * devicePixelRatioF() / m_zoom;
        s.view->press(s.startMouse, in.button, in.mods, windowOf(s.frame, s.centre, s.height, radius));
    }
    m_drag = Drag::Model;
    update();
}

void CanvasWidget::modelMove(const PointerInput& in)
{
    ModelSession& s = m_model;
    const QPointF m = widgetToCanvas(in.pos), d = m - s.startMouse;
    if (s.handle == kFrameInside) {
        s.view->move(m, windowOf(s.frame, s.centre, s.height, 16.0));
    } else if (s.handle == kFrameMove) {
        s.frame = s.startFrame.translated(d);
        s.centre = s.startCentre + d;
    } else if (s.handle % 2 == 0) {
        // A corner: the picture scales about the opposite one, the model with it.
        const QPointF anchor = framePoint(s.startFrame, (s.handle + 4) % 8);
        const QPointF d0 = framePoint(s.startFrame, s.handle) - anchor, v = m - anchor;
        const double len2 = d0.x() * d0.x() + d0.y() * d0.y();
        double k = len2 > 1e-9 ? (v.x() * d0.x() + v.y() * d0.y()) / len2 : 1.0;
        k = std::max(k, kMinFrame / std::max(1.0, std::min(s.startFrame.width(), s.startFrame.height())));
        s.frame = QRectF(anchor + (s.startFrame.topLeft() - anchor) * k, s.startFrame.size() * k);
        s.centre = anchor + (s.startCentre - anchor) * k;
        s.height = s.startHeight * k;
    } else {
        // A side: the frame shows more or less of the view, the model where it was.
        QRectF f = s.startFrame;
        switch (s.handle) {
        case 1: f.setTop(std::min(m.y(), f.bottom() - kMinFrame)); break;
        case 3: f.setRight(std::max(m.x(), f.left() + kMinFrame)); break;
        case 5: f.setBottom(std::max(m.y(), f.top() + kMinFrame)); break;
        default: f.setLeft(std::min(m.x(), f.right() - kMinFrame)); break;
        }
        s.frame = f;
    }
    s.dirty = true;
    update();
}

void CanvasWidget::modelRelease()
{
    if (m_model.active && m_model.handle == kFrameInside)
        m_model.view->release();
    m_model.handle = -1;
    m_model.dirty = true;
    update();
}

void CanvasWidget::renderModel()
{
    ModelSession& s = m_model;
    if (!s.active)
        return;
    // As many pixels as the frame shows on screen, within reason: the layer gets them all when applied.
    QSizeF want = s.frame.size() * std::min(1.0, m_zoom);
    if (want.width() > kLiveSide || want.height() > kLiveSide)
        want.scale(kLiveSide, kLiveSide, Qt::KeepAspectRatio);
    const QSize px(std::max(1, qRound(want.width())), std::max(1, qRound(want.height())));
    if (!s.dirty && s.image.size() == px)
        return;
    s.image = s.view->render(windowOf(s.frame, s.centre, s.height, 16.0), px, true);
    s.dirty = false;
}

void CanvasWidget::drawModelOverlay(QPainter& p)
{
    if (!m_model.active)
        return;
    const ModelSession& s = m_model;
    if (!s.image.isNull()) {
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const double dpr = devicePixelRatioF();
        p.setTransform(viewTransform() * QTransform::fromScale(1.0 / dpr, 1.0 / dpr));
        p.drawImage(s.frame, s.image);
        p.restore();
    }
    // The frame, drawn as the transform's box is: dark under light, so it reads on anything.
    QPolygonF box;
    for (int h : {0, 2, 4, 6})
        box << canvasToWidget(framePoint(s.frame, h));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 110), 3));
    p.drawPolygon(box);
    p.setPen(QPen(theme::accent, 1.5));
    p.drawPolygon(box);
    const QVector<QPointF> at = modelHandlePositions();
    for (int h = 0; h < 8; ++h) {
        p.setBrush(h == s.handle ? QColor(Qt::white) : theme::accent);
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(at[h], 5.5, 5.5);
    }
    if (s.view->mode() == ModelView::Mode::Pose) {
        // The joints to take hold of, as the panel shows them.
        const QVector<QPointF> joints = modelJointPositions();
        const int chosen = s.view->selectedHandle();
        for (int i = 0; i < joints.size(); ++i) {
            const bool moves = s.view->model()->handles[size_t(i)].moves;
            p.setPen(QPen(i == chosen ? QColor(Qt::white) : QColor(0, 0, 0, 120), 1.5));
            p.setBrush(i == chosen ? theme::accent : (moves ? QColor(52, 199, 89) : QColor(255, 255, 255, 200)));
            p.drawEllipse(joints[i], moves ? 7.5 : 6.0, moves ? 7.5 : 6.0);
        }
    }
}
