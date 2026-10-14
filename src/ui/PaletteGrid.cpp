#include "ui/PaletteGrid.h"

#include "color/Palette.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

namespace {
constexpr int kHoldMs = 450;       // how long the pen rests before a swatch comes up
constexpr double kSlack = 8.0;     // travel that is still a tap
}

PaletteGrid::PaletteGrid(ToolState* tools, QWidget* parent) : QWidget(parent), m_tools(tools)
{
    setCursor(Qt::PointingHandCursor);
    m_hold.setSingleShot(true);
    m_hold.setInterval(kHoldMs);
    connect(&m_hold, &QTimer::timeout, this, [this] {
        if (m_gesture != Gesture::Pressed || !colorAt(m_pressSlot).isValid())
            return;
        m_gesture = Gesture::Moving;
        m_lifted = true;
        update();
    });
    // Which swatch is the colour in use is marked on the grid.
    connect(m_tools, &ToolState::colorChanged, this, qOverload<>(&QWidget::update));
    refresh();
}

void PaletteGrid::setPalette(const QString& id)
{
    if (id == m_id)
        return;
    m_id = id;
    m_gesture = Gesture::None;
    m_hold.stop();
    m_lifted = m_carried = false;
    refresh();
}

void PaletteGrid::refresh()
{
    const Palette* p = PaletteLibrary::instance().find(m_id);
    const int rows = p ? p->rows() : Palette::kMinRows;
    setFixedSize(Palette::kColumns * kCell, rows * kCell);
    update();
}

QColor PaletteGrid::colorAt(int slot) const
{
    const Palette* p = PaletteLibrary::instance().find(m_id);
    return p && slot >= 0 && slot < p->colors.size() ? p->colors[slot] : QColor();
}

QRectF PaletteGrid::slotRect(int slot) const
{
    return QRectF((slot % Palette::kColumns) * kCell, (slot / Palette::kColumns) * kCell, kCell, kCell);
}

int PaletteGrid::slotAt(const QPointF& pos) const
{
    const Palette* p = PaletteLibrary::instance().find(m_id);
    if (!p || pos.x() < 0 || pos.y() < 0 || pos.x() >= Palette::kColumns * kCell)
        return -1;
    const int slot = int(pos.y()) / kCell * Palette::kColumns + int(pos.x()) / kCell;
    return slot < p->colors.size() ? slot : -1;
}

void PaletteGrid::paintEvent(QPaintEvent*)
{
    const Palette* pal = PaletteLibrary::instance().find(m_id);
    if (!pal)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool moving = m_gesture == Gesture::Moving;
    const int target = moving && m_carried ? slotAt(m_dragPos) : -1;
    const QColor current = m_tools->color();
    for (int i = 0; i < pal->colors.size(); ++i) {
        const QRectF r = slotRect(i).adjusted(2.5, 2.5, -2.5, -2.5);
        const QColor c = pal->colors[i];
        const bool inHand = moving && i == m_pressSlot;
        if (c.isValid() && !(inHand && m_carried)) {
            // Picked up but not yet carried off, a swatch stands a little proud of the rest.
            p.setPen(inHand ? QPen(QColor(255, 255, 255, 220), 1.5) : QPen(QColor(255, 255, 255, 30), 1));
            p.setBrush(c);
            p.drawRoundedRect(inHand ? r.adjusted(-2, -2, 2, 2) : r, 5, 5);
            if (c == current && !inHand) {
                // The colour in use: a ring that shows on light and on dark swatches.
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(0, 0, 0, 150), 1));
                p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 3.5, 3.5);
                p.setPen(QPen(Qt::white, 1.5));
                p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
            }
        } else {
            // An empty slot: somewhere to put a colour.
            p.setPen(QPen(QColor(255, 255, 255, 34), 1));
            p.setBrush(QColor(255, 255, 255, 8));
            p.drawRoundedRect(r, 5, 5);
        }
        if (i == target && i != m_pressSlot) {
            p.setPen(QPen(theme::accent, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(-1, -1, 1, 1), 6, 6);
        }
    }
    if (m_incoming >= 0 || (m_incomingColour.isValid() && rect().contains(m_incomingPos.toPoint()))) {
        // A swatch from another palette: where it would go, and the swatch itself under the pen.
        if (m_incoming >= 0) {
            p.setPen(QPen(theme::accent, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(slotRect(m_incoming).adjusted(1.5, 1.5, -1.5, -1.5), 6, 6);
        }
        const QRectF r(m_incomingPos.x() - kCell / 2.0 - 1, m_incomingPos.y() - kCell / 2.0 - 1, kCell + 2, kCell + 2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        p.drawRoundedRect(r.translated(0, 2), 7, 7);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
        p.setBrush(m_incomingColour);
        p.drawRoundedRect(r, 7, 7);
    }
    if (moving && m_carried && !m_over) {
        // The swatch in hand, under the pen.
        const QRectF r(m_dragPos.x() - kCell / 2.0 - 1, m_dragPos.y() - kCell / 2.0 - 1, kCell + 2, kCell + 2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 70));
        p.drawRoundedRect(r.translated(0, 2), 7, 7);
        p.setPen(QPen(QColor(255, 255, 255, 200), 1.5));
        p.setBrush(colorAt(m_pressSlot));
        p.drawRoundedRect(r, 7, 7);
    }
}

void PaletteGrid::mousePressEvent(QMouseEvent* e)
{
    if (m_scroller)
        m_scroller->stop();
    m_hold.stop();
    m_gesture = Gesture::None;
    m_lifted = m_carried = false;
    m_pressSlot = slotAt(e->position());
    if (e->button() != Qt::LeftButton || m_pressSlot < 0)
        return;
    m_gesture = Gesture::Pressed;
    m_pressGlobal = e->globalPosition();
    m_dragPos = e->position();
    m_pen = DragScroller::penDown();
    if (colorAt(m_pressSlot).isValid())
        m_hold.start();
}

void PaletteGrid::mouseMoveEvent(QMouseEvent* e)
{
    if (m_gesture == Gesture::None || !(e->buttons() & Qt::LeftButton))
        return;
    const QPointF d = e->globalPosition() - m_pressGlobal;
    if (m_gesture == Gesture::Pressed && std::hypot(d.x(), d.y()) > kSlack) {
        m_hold.stop();
        const bool scrolls = m_pen && m_scroller && m_scroller->canScroll();
        if (scrolls && std::abs(d.y()) >= std::abs(d.x())) {
            m_gesture = Gesture::Scrolling;
            m_scroller->begin(m_pressGlobal);
        } else if (!m_pen && colorAt(m_pressSlot).isValid()) {
            m_gesture = Gesture::Moving;   // with the mouse a drag carries it off at once
        } else if (slotAt(e->position()) != m_pressSlot) {
            // A pen that slides from one swatch onto the next was a careless
            // tap, not a wish to rearrange: palettes have no undo.
            m_gesture = Gesture::None;
        }
    }
    if (m_gesture == Gesture::Scrolling) {
        m_scroller->dragTo(e->globalPosition());
    } else if (m_gesture == Gesture::Moving) {
        m_dragPos = e->position();
        if (std::hypot(d.x(), d.y()) > kSlack)
            m_carried = true;
        // Over another palette: that grid shows where the copy would go.
        PaletteGrid* over = m_carried ? gridUnder(e->globalPosition()) : nullptr;
        if (m_over && m_over != over)
            m_over->showIncoming(-1, QColor(), QPointF());
        m_over = over;
        if (over) {
            const QPointF at = over->mapFromGlobal(e->globalPosition());
            over->showIncoming(over->slotAt(at), colorAt(m_pressSlot), at);
        }
        update();
    }
}

PaletteGrid* PaletteGrid::gridUnder(const QPointF& globalPos) const
{
    for (PaletteGrid* g : window()->findChildren<PaletteGrid*>()) {
        if (g == this || !g->isVisible() || g->m_id.isEmpty() || g->m_id == m_id)
            continue;
        const QPoint at = g->mapFromGlobal(globalPos.toPoint());
        if (g->rect().contains(at) && g->visibleRegion().contains(at))
            return g;
    }
    return nullptr;
}

void PaletteGrid::showIncoming(int slot, const QColor& colour, const QPointF& at)
{
    m_incoming = colour.isValid() ? std::max(slot, -1) : -1;
    m_incomingColour = colour;
    m_incomingPos = at;
    update();
}

void PaletteGrid::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_hold.stop();
    const Gesture was = m_gesture;
    const bool lifted = m_lifted, carried = m_carried;
    const int from = m_pressSlot, to = slotAt(e->position());
    PaletteGrid* over = m_over;
    m_over = nullptr;
    if (over)
        over->showIncoming(-1, QColor(), QPointF());
    m_gesture = Gesture::None;
    m_lifted = m_carried = false;
    update();
    if (was == Gesture::Moving && carried && over) {
        // Into another palette: a copy, so that nothing is lost on the way.
        PaletteLibrary& lib = PaletteLibrary::instance();
        const int slot = over->slotAt(over->mapFromGlobal(e->globalPosition()));
        if (lib.placeSwatch(over->paletteId(), slot >= 0 ? slot : int(lib.find(over->paletteId())->colors.size()), colorAt(from)) >= 0)
            lib.save();
        return;
    }
    if (was == Gesture::Scrolling) {
        m_scroller->end();
    } else if (was == Gesture::Pressed) {
        if (to == from)
            tap(from);
    } else if (was == Gesture::Moving) {
        PaletteLibrary& lib = PaletteLibrary::instance();
        if (carried && to >= 0 && to != from) {
            if (lib.moveSwatch(m_id, from, to))
                lib.save();
        } else if (to == from && lifted && !carried) {
            showSwatchMenu(from, e->globalPosition().toPoint());
        } else if (to == from && !lifted) {
            tap(from);   // set off and came back: it was a tap with a wobble
        }
    }
}

void PaletteGrid::tap(int slot)
{
    const QColor c = colorAt(slot);
    if (c.isValid()) {
        m_tools->setColor(c);
        return;
    }
    PaletteLibrary& lib = PaletteLibrary::instance();
    if (lib.setSwatch(m_id, slot, m_tools->color()))
        lib.save();
}

void PaletteGrid::contextMenuEvent(QContextMenuEvent* e)
{
    const int slot = slotAt(e->pos());
    if (slot >= 0)
        showSwatchMenu(slot, e->globalPos());
}

void PaletteGrid::showSwatchMenu(int slot, const QPoint& globalPos)
{
    PaletteLibrary& lib = PaletteLibrary::instance();
    const QString id = m_id;
    const bool filled = colorAt(slot).isValid();
    QMenu menu(window());
    menu.addAction(filled ? tr("Set to the Current Colour") : tr("Keep the Current Colour Here"), this, [=, this, &lib] {
        if (lib.setSwatch(id, slot, m_tools->color()))
            lib.save();
    });
    if (filled) {
        menu.addAction(tr("Remove"), this, [=, &lib] {
            if (lib.setSwatch(id, slot, QColor()))
                lib.save();
        });
    }
    menu.exec(globalPos);
}
