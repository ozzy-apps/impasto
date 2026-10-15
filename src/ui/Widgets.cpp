#include "ui/Widgets.h"

#include "core/Curve.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabletEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

// -------------------------------------------------------------- IconButton

IconButton::IconButton(const QString& icon, QWidget* parent, int iconSize)
    : QAbstractButton(parent), m_icon(icon), m_iconSize(iconSize)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void IconButton::setIconName(const QString& icon)
{
    m_icon = icon;
    update();
}

void IconButton::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    update();
}

QSize IconButton::sizeHint() const
{
    if (!text().isEmpty()) {
        const QFontMetrics fm(theme::font(14, QFont::Medium));
        return {fm.horizontalAdvance(text()) + 20, 34};
    }
    return {m_iconSize + 16, m_iconSize + 12};
}

void IconButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (isDown() || (m_hover && isEnabled())) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, isDown() ? 40 : 22));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
    }
    QColor c = m_active || isChecked() ? theme::accent : theme::text;
    if (!isEnabled())
        c = QColor(theme::textDim.red(), theme::textDim.green(), theme::textDim.blue(), 110);
    if (!text().isEmpty()) {
        p.setFont(theme::font(14, QFont::Medium));
        p.setPen(c);
        p.drawText(rect(), Qt::AlignCenter, text());
        return;
    }
    const QPixmap pm = theme::icon(m_icon, m_iconSize, c, devicePixelRatioF());
    p.drawPixmap(QPointF((width() - m_iconSize) / 2.0, (height() - m_iconSize) / 2.0), pm);
}

void IconButton::enterEvent(QEnterEvent*)
{
    m_hover = true;
    update();
}

void IconButton::leaveEvent(QEvent*)
{
    m_hover = false;
    update();
}

// ------------------------------------------------------------------ Slider

Slider::Slider(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Slider::setValue(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    if (v == m_value)
        return;
    m_value = v;
    update();
}

void Slider::setTrackColors(const QVector<QColor>& stops)
{
    m_stops = stops;
    update();
}

void Slider::setFromPos(qreal x)
{
    const qreal pad = 10.0;
    const float v = float(std::clamp((x - pad) / std::max(1.0, width() - 2 * pad), 0.0, 1.0));
    if (v == m_value)
        return;
    m_value = v;
    update();
    emit valueChanged(v);
}

void Slider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal pad = 10.0;
    const qreal cy = height() / 2.0;
    const qreal x0 = pad, x1 = width() - pad;
    const qreal kx = x0 + (x1 - x0) * m_value;
    p.setPen(Qt::NoPen);
    if (!m_stops.isEmpty()) {
        const QRectF track(x0 - 7, cy - 7, x1 - x0 + 14, 14);
        QLinearGradient g(x0, 0, x1, 0);
        for (int i = 0; i < m_stops.size(); ++i)
            g.setColorAt(m_stops.size() == 1 ? 0.0 : double(i) / (m_stops.size() - 1), m_stops[i]);
        p.setBrush(g);
        p.drawRoundedRect(track, 7, 7);
        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(track, 7, 7);
    } else {
        p.setBrush(theme::track);
        p.drawRoundedRect(QRectF(x0, cy - 2.5, x1 - x0, 5), 2.5, 2.5);
        p.setBrush(theme::accent);
        p.drawRoundedRect(QRectF(x0, cy - 2.5, kx - x0, 5), 2.5, 2.5);
    }
    p.setPen(QPen(QColor(0, 0, 0, 70), 1));
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(kx, cy), 8.5, 8.5);
}

void Slider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    emit pressed();
    setFromPos(e->position().x());
}

void Slider::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton)
        setFromPos(e->position().x());
}

void Slider::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        emit released();
}

void Slider::wheelEvent(QWheelEvent* e)
{
    const float v = std::clamp(m_value + float(e->angleDelta().y()) / 120.0f * 0.02f, 0.0f, 1.0f);
    if (v != m_value) {
        m_value = v;
        update();
        emit valueChanged(v);
    }
    e->accept();
}

// ----------------------------------------------------------------- VSlider

VSlider::VSlider(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
}

void VSlider::setValue(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    if (v == m_value)
        return;
    m_value = v;
    update();
}

void VSlider::setFromPos(qreal y)
{
    const qreal pad = 12.0;
    const float v = 1.0f - float(std::clamp((y - pad) / std::max(1.0, height() - 2 * pad), 0.0, 1.0));
    if (v == m_value)
        return;
    m_value = v;
    update();
    emit valueChanged(v);
}

void VSlider::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal pad = 12.0;
    const qreal cx = width() / 2.0;
    const qreal y0 = pad, y1 = height() - pad;
    const qreal ky = y1 - (y1 - y0) * m_value;
    p.setPen(Qt::NoPen);
    p.setBrush(theme::track);
    p.drawRoundedRect(QRectF(cx - 3, y0, 6, y1 - y0), 3, 3);
    p.setBrush(QColor(0xd8, 0xd8, 0xde));
    p.drawRoundedRect(QRectF(cx - 3, ky, 6, y1 - ky), 3, 3);
    p.setPen(QPen(QColor(0, 0, 0, 80), 1));
    p.setBrush(m_down ? theme::accent : QColor(Qt::white));
    p.drawRoundedRect(QRectF(cx - 12, ky - 6, 24, 12), 6, 6);
}

void VSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_down = true;
    emit pressed();
    setFromPos(e->position().y());
    update();
}

void VSlider::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton)
        setFromPos(e->position().y());
}

void VSlider::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_down = false;
    update();
    emit released();
}

void VSlider::wheelEvent(QWheelEvent* e)
{
    const float v = std::clamp(m_value + float(e->angleDelta().y()) / 120.0f * 0.02f, 0.0f, 1.0f);
    if (v != m_value) {
        m_value = v;
        update();
        emit valueChanged(v);
    }
    e->accept();
}

// ------------------------------------------------------------------- Panel

Panel::Panel(QWidget* parent, int radius) : QWidget(parent), m_radius(radius) {}

void Panel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // Extend past any edge that should sit flush with the window border.
    QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_flat & Qt::LeftEdge) r.setLeft(r.left() - m_radius - 2);
    if (m_flat & Qt::RightEdge) r.setRight(r.right() + m_radius + 2);
    if (m_flat & Qt::TopEdge) r.setTop(r.top() - m_radius - 2);
    if (m_flat & Qt::BottomEdge) r.setBottom(r.bottom() + m_radius + 2);
    p.setPen(QPen(theme::panelBorder, 1));
    p.setBrush(theme::bar);
    p.drawRoundedRect(r, m_radius, m_radius);
}

// ------------------------------------------------------------ DragScroller

namespace {

constexpr double kDragSlack = 9.0;   // travel that is still a tap; a pen wobbles as it lands

// Knows, at any time, whether a pen is pressed on the tablet.
class PenWatch : public QObject {
public:
    using QObject::QObject;
    bool down = false;

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::TabletPress)
            down = true;
        else if (e->type() == QEvent::TabletRelease || e->type() == QEvent::TabletLeaveProximity)
            down = false;
        return false;
    }
};
PenWatch* g_penWatch = nullptr;

} // namespace

void DragScroller::trackPen()
{
    if (g_penWatch)
        return;
    g_penWatch = new PenWatch(qApp);
    qApp->installEventFilter(g_penWatch);
}

bool DragScroller::penDown()
{
    return g_penWatch && g_penWatch->down;
}

DragScroller::DragScroller(QScrollArea* area) : QObject(area), m_area(area)
{
    m_clock.start();
    m_glide.setInterval(16);
    connect(&m_glide, &QTimer::timeout, this, [this] {
        QScrollBar* bar = m_area->verticalScrollBar();
        m_position -= m_velocity * 16.0;
        m_velocity *= 0.94;
        const double clamped = std::clamp(m_position, double(bar->minimum()), double(bar->maximum()));
        bar->setValue(int(std::lround(clamped)));
        if (std::abs(m_velocity) < 0.03 || clamped != m_position)
            m_glide.stop();
    });
}

DragScroller::DragScroller(QScrollArea* area, QWidget* content) : DragScroller(area)
{
    m_content = content;
    content->installEventFilter(this);
}

bool DragScroller::canScroll() const
{
    return m_area->verticalScrollBar()->maximum() > m_area->verticalScrollBar()->minimum();
}

void DragScroller::begin(const QPointF& globalPos)
{
    m_glide.stop();
    m_startValue = m_area->verticalScrollBar()->value();
    m_startY = globalPos.y();
    m_trail.clear();
    m_trail.push_back({m_clock.elapsed(), globalPos.y()});
}

void DragScroller::dragTo(const QPointF& globalPos)
{
    // Measured on the screen: the content itself moves under the pen as it scrolls.
    m_area->verticalScrollBar()->setValue(int(std::lround(m_startValue - (globalPos.y() - m_startY))));
    m_trail.push_back({m_clock.elapsed(), globalPos.y()});
    while (m_trail.size() > 2 && m_trail.last().ms - m_trail.first().ms > 90)
        m_trail.removeFirst();
}

void DragScroller::end()
{
    if (m_trail.size() < 2)
        return;
    const Sample& a = m_trail.first();
    const Sample& b = m_trail.last();
    // Coming to rest before lifting means no flick was meant.
    if (m_clock.elapsed() - b.ms > 60 || b.ms <= a.ms)
        return;
    m_velocity = std::clamp((b.y - a.y) / double(b.ms - a.ms), -6.0, 6.0);
    if (std::abs(m_velocity) < 0.25)
        return;
    m_position = m_area->verticalScrollBar()->value();
    m_glide.start();
}

void DragScroller::stop()
{
    m_glide.stop();
    m_state = State::Idle;
}

void DragScroller::replayTap()
{
    m_replaying = true;
    QMouseEvent press(QEvent::MouseButtonPress, m_pressLocal, m_pressGlobal, Qt::LeftButton, Qt::LeftButton, m_pressMods);
    QCoreApplication::sendEvent(m_content, &press);
    m_replaying = false;
}

bool DragScroller::eventFilter(QObject* obj, QEvent* e)
{
    if (obj != m_content || m_replaying)
        return false;
    switch (e->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(e);
        m_glide.stop();
        if (me->button() != Qt::LeftButton || !canScroll()) {
            m_state = State::Idle;
            return false;
        }
        m_state = State::Pressed;
        m_pressLocal = me->position();
        m_pressGlobal = me->globalPosition();
        m_pressMods = me->modifiers();
        begin(m_pressGlobal);
        return true;
    }
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(e);
        if (m_state == State::Idle || m_state == State::PassedOn)
            return false;
        if (m_state == State::Pressed) {
            const QPointF d = me->globalPosition() - m_pressGlobal;
            if (std::hypot(d.x(), d.y()) < kDragSlack)
                return true;
            if (std::abs(d.y()) < std::abs(d.x())) {
                // Sideways is not for us: the widget gets its press after all.
                m_state = State::PassedOn;
                replayTap();
                return false;
            }
            m_state = State::Scrolling;
        }
        dragTo(me->globalPosition());
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() != Qt::LeftButton || m_state == State::Idle)
            return false;
        const State was = m_state;
        m_state = State::Idle;
        if (was == State::PassedOn)
            return false;
        if (was == State::Scrolling) {
            end();
            return true;
        }
        // It was a tap: the widget gets the press it was kept from, then this release.
        replayTap();
        return false;
    }
    default:
        break;
    }
    return false;
}

// ----------------------------------------------------------------- Popover

namespace {
constexpr int kShadow = 14;
constexpr int kArrow = 9;
constexpr int kGrip = 14;         // strip along the top to drag the panel by
constexpr int kSnap = 14;         // how near an edge a dragged panel jumps to it
constexpr int kEdgeGap = 8;       // room left between a snapped panel and that edge
}

Popover::Popover(QWidget* host) : QWidget(host), m_host(host)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(kShadow, kShadow + kArrow + kGrip, kShadow, kShadow);
    setMouseTracking(true);
    hide();
}

void Popover::setContent(QWidget* content)
{
    m_content = content;
    layout()->addWidget(content);
}

QRect Popover::bodyRect() const
{
    return m_upward ? rect().adjusted(kShadow, kShadow, -kShadow, -kShadow - kArrow) : rect().adjusted(kShadow, kShadow + kArrow, -kShadow, -kShadow);
}

void Popover::setOpensUpward(bool upward)
{
    m_upward = upward;
    layout()->setContentsMargins(kShadow, kShadow + (upward ? 0 : kArrow) + kGrip, kShadow, kShadow + (upward ? kArrow : 0));
}

QRect Popover::gripRect() const
{
    const QRect body = bodyRect();
    // Presses on a panel's title row fall through to us, so that drags it as well.
    return QRect(body.left(), body.top(), body.width(), kGrip + 38);
}

QRect Popover::pinRect() const
{
    const QRect body = bodyRect();
    return QRect(body.right() - 30, body.top() + 1, 26, kGrip + 8);
}

void Popover::popup(QWidget* anchor)
{
    m_anchor = anchor;
    adjustSize();
    if (m_pinned)
        placePinned(m_pinnedPos, false);
    else
        reposition();
    show();
    raise();
    qApp->installEventFilter(this);
}

void Popover::reposition()
{
    if (m_pinned) {
        placePinned(m_pinnedPos, false);   // the window may have shrunk around it
        return;
    }
    if (!m_anchor)
        return;
    const QPoint a = m_anchor->mapTo(m_host, QPoint(m_anchor->width() / 2, m_upward ? 0 : m_anchor->height()));
    const int w = width();
    const int x = std::clamp(a.x() - w / 2, 4 - kShadow, std::max(4 - kShadow, m_host->width() - w + kShadow - 4));
    m_arrowX = a.x() - x;
    move(x, m_upward ? a.y() - height() + kShadow - 2 : a.y() - kShadow + 2);
    update();
}

// Puts the panel's body at `bodyTopLeft` in the host, kept inside the window
// and, while it is being dragged, pulled to an edge it comes close to.
void Popover::placePinned(QPoint bodyTopLeft, bool snap)
{
    const QSize body = bodyRect().size();
    const int left = kEdgeGap, top = m_topInset + kEdgeGap;
    const int right = std::max(left, m_host->width() - body.width() - kEdgeGap);
    const int bottom = std::max(top, m_host->height() - body.height() - kEdgeGap);
    if (snap) {
        if (std::abs(bodyTopLeft.x() - left) < kSnap)
            bodyTopLeft.setX(left);
        if (std::abs(bodyTopLeft.x() - right) < kSnap)
            bodyTopLeft.setX(right);
        if (std::abs(bodyTopLeft.y() - top) < kSnap)
            bodyTopLeft.setY(top);
        if (std::abs(bodyTopLeft.y() - bottom) < kSnap)
            bodyTopLeft.setY(bottom);
    }
    // Never further out than leaves the grip within reach.
    bodyTopLeft.setX(std::clamp(bodyTopLeft.x(), 60 - body.width(), std::max(60 - body.width(), m_host->width() - 60)));
    bodyTopLeft.setY(std::clamp(bodyTopLeft.y(), m_topInset + 2, std::max(m_topInset + 2, m_host->height() - 40)));
    m_pinnedPos = bodyTopLeft;
    move(bodyTopLeft - QPoint(kShadow, kShadow + kArrow));
    update();
}

void Popover::setPinnedPos(const QPoint& bodyTopLeft)
{
    m_pinnedPos = bodyTopLeft;
    if (m_pinned && isVisible())
        placePinned(bodyTopLeft, false);
}

void Popover::setPinned(bool pinned)
{
    if (m_pinned == pinned)
        return;
    m_pinned = pinned;
    if (pinned)
        m_pinnedPos = pos() + QPoint(kShadow, kShadow + kArrow);   // stays where it is
    else if (isVisible())
        reposition();   // back under its button
    update();
    emit pinnedChanged(pinned);
}

void Popover::dismiss()
{
    if (!m_pinned)
        hidePanel();
}

void Popover::hidePanel()
{
    if (!isVisible())
        return;
    qApp->removeEventFilter(this);
    hide();
    emit dismissed();
}

void Popover::mousePressEvent(QMouseEvent* e)
{
    e->accept();   // clicks on the padding must not fall through to the canvas
    if (e->button() != Qt::LeftButton)
        return;
    if (pinRect().contains(e->position().toPoint())) {
        setPinned(!m_pinned);
        return;
    }
    if (gripRect().contains(e->position().toPoint())) {
        m_gripDown = true;
        m_gripMoved = false;
        m_gripStart = e->globalPosition();
        m_bodyAtGrip = pos() + QPoint(kShadow, kShadow + kArrow);
    }
}

void Popover::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_gripDown) {
        const QPoint at = e->position().toPoint();
        setCursor(pinRect().contains(at) ? Qt::PointingHandCursor : gripRect().contains(at) ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    const QPointF d = e->globalPosition() - m_gripStart;
    if (!m_gripMoved && std::hypot(d.x(), d.y()) < 5.0)
        return;
    m_gripMoved = true;
    if (!m_pinned)
        setPinned(true);   // dragging it away is what tears it off
    placePinned(m_bodyAtGrip + d.toPoint(), true);
}

void Popover::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        m_gripDown = false;
}

bool Popover::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::TabletPress) {
        auto* w = qobject_cast<QWidget*>(obj);
        if (!w || w->window() != window())
            return false;   // one of our own menus, combo popups or dialogs
        // Decide by where the press landed, not by who is receiving it. A pen
        // press that a widget does not handle is offered to each of its parents
        // in turn before Qt replays it as a mouse press, so the receiver alone
        // would make a tap inside the panel look like a tap on the window.
        const QPointF global = e->type() == QEvent::TabletPress ? static_cast<QTabletEvent*>(e)->globalPosition()
                                                                : static_cast<QMouseEvent*>(e)->globalPosition();
        if (rect().contains(mapFromGlobal(global).toPoint())) {
            if (m_pinned && bodyRect().contains(mapFromGlobal(global).toPoint()))
                raise();   // the panel being used comes to the front
            return false;
        }
        if (m_pinned)
            return false;   // a pinned panel stays through whatever happens elsewhere
        if (m_anchor && m_anchor->rect().contains(m_anchor->mapFromGlobal(global).toPoint()))
            return false;   // the anchor button toggles us itself
        dismiss();
    } else if (e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && !m_pinned) {
        dismiss();
        return true;
    }
    return false;
}

void Popover::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(bodyRect());
    p.setPen(Qt::NoPen);
    for (int i = kShadow; i > 0; i -= 2) {
        p.setBrush(QColor(0, 0, 0, 5 + (kShadow - i)));
        p.drawRoundedRect(body.adjusted(-i, -i + 3, i, i + 3), 14 + i, 14 + i);
    }
    QPainterPath path;
    path.addRoundedRect(body, 14, 14);
    if (!m_pinned) {
        // Hanging from its button: an arrow says which one.
        const qreal ax = std::clamp(qreal(m_arrowX), body.left() + 22, body.right() - 22);
        QPainterPath arrow;
        const qreal edge = m_upward ? body.bottom() - 0.5 : body.top() + 0.5;
        const qreal tip = m_upward ? body.bottom() + kArrow : body.top() - kArrow;
        arrow.moveTo(ax - kArrow - 2, edge);
        arrow.lineTo(ax, tip);
        arrow.lineTo(ax + kArrow + 2, edge);
        arrow.closeSubpath();
        path = path.united(arrow);
    }
    p.setPen(QPen(theme::panelBorder, 1));
    p.setBrush(theme::panel);
    p.drawPath(path);

    // The grip to drag it by, and the pin.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, m_gripDown ? 120 : 55));
    p.drawRoundedRect(QRectF(body.center().x() - 20, body.top() + 7, 40, 4), 2, 2);
    const QRect pin = pinRect();
    p.drawPixmap(QPoint(pin.center().x() - 7, pin.center().y() - 7),
                 theme::icon(QStringLiteral("pin"), 15, m_pinned ? theme::accent : QColor(255, 255, 255, 110), devicePixelRatioF()));
}

// ------------------------------------------------------------------- Toast

Toast::Toast(QWidget* host) : QWidget(host), m_host(host)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_timer.setSingleShot(true);
    m_timer.setInterval(1500);
    connect(&m_timer, &QTimer::timeout, this, &QWidget::hide);
    hide();
}

void Toast::showMessage(const QString& text)
{
    if (text.isEmpty())
        return;
    m_text = text;
    // A long message wraps onto more lines and stays up long enough to be read.
    const QFontMetrics fm(theme::font(13, QFont::Medium));
    const int widest = std::max(200, std::min(560, m_host->width() - 48));
    const QRect text0 = fm.boundingRect(QRect(0, 0, widest, 2000), Qt::AlignCenter | Qt::TextWordWrap, text);
    resize(text0.width() + 36, std::max(34, text0.height() + 16));
    move((m_host->width() - width()) / 2, m_top);
    show();
    raise();
    update();
    m_timer.setInterval(std::clamp(1500 + int(text.size() - 40) * 45, 1500, 9000));
    m_timer.start();
}

void Toast::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x3a, 0x3a, 0x3e, 0xf0));
    p.drawRoundedRect(rect(), 17.0, 17.0);
    p.setFont(theme::font(13, QFont::Medium));
    p.setPen(theme::text);
    p.drawText(rect().adjusted(18, 8, -18, -8), Qt::AlignCenter | Qt::TextWordWrap, m_text);
}

// ------------------------------------------------------------- CurveEditor

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent), m_points(identity())
{
    setCursor(Qt::CrossCursor);
}

QVector<QPointF> CurveEditor::identity()
{
    return {{0.0, 0.0}, {0.33, 0.33}, {0.67, 0.67}, {1.0, 1.0}};
}

namespace {

// The hills of a histogram inside `r`, the tallest touching its top. A square
// root lifts the low steps, which are otherwise lost beside one tall spike.
QPainterPath histogramPath(const QVector<float>& bins, const QRectF& r)
{
    QPainterPath path;
    if (bins.size() < 2)
        return path;
    path.moveTo(r.bottomLeft());
    for (int i = 0; i < bins.size(); ++i) {
        const qreal x = r.left() + r.width() * i / (bins.size() - 1.0);
        path.lineTo(x, r.bottom() - r.height() * std::sqrt(std::clamp(double(bins[i]), 0.0, 1.0)));
    }
    path.lineTo(r.bottomRight());
    path.closeSubpath();
    return path;
}

} // namespace

void CurveEditor::setHistogram(const QVector<float>& bins, const QColor& colour)
{
    m_histogram = bins;
    m_histogramColour = colour;
    update();
}

HistogramView::HistogramView(QWidget* parent) : QWidget(parent)
{
    setFixedHeight(96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void HistogramView::setBins(const QVector<float>& bins)
{
    m_bins = bins;
    update();
}

void HistogramView::setMarkers(float black, float middle, float white)
{
    m_black = black;
    m_middle = middle;
    m_white = white;
    update();
}

void HistogramView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1f, 0x1f, 0x22));
    p.drawRoundedRect(rect(), 8, 8);
    const QRectF r = QRectF(rect()).adjusted(10, 8, -10, -8);
    p.setBrush(QColor(255, 255, 255, 70));
    p.drawPath(histogramPath(m_bins, r));
    auto marker = [&](float at, const QColor& colour) {
        const qreal x = r.left() + r.width() * std::clamp(at, 0.0f, 1.0f);
        p.setPen(QPen(QColor(0, 0, 0, 120), 3));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QPen(colour, 1.4));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    };
    marker(m_black, QColor(20, 20, 20));
    marker(m_middle, QColor(150, 150, 155));
    marker(m_white, QColor(Qt::white));
}

void CurveEditor::setPoints(const QVector<QPointF>& points)
{
    m_points = points.size() >= 2 ? points : identity();
    update();
}

float CurveEditor::evaluate(const QVector<QPointF>& p, float x)
{
    return curveValue(p, x);
}

QRectF CurveEditor::plot() const
{
    return QRectF(rect()).adjusted(10, 10, -10, -10);
}

QPointF CurveEditor::toWidget(const QPointF& p) const
{
    const QRectF r = plot();
    return {r.left() + p.x() * r.width(), r.bottom() - p.y() * r.height()};
}

void CurveEditor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1f, 0x1f, 0x22));
    p.drawRoundedRect(rect(), 8, 8);
    if (!m_histogram.isEmpty()) {
        p.setBrush(m_histogramColour);
        p.drawPath(histogramPath(m_histogram, r));
    }
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4.0, r.top()), QPointF(r.left() + r.width() * i / 4.0, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4.0), QPointF(r.right(), r.top() + r.height() * i / 4.0));
    }
    p.setPen(QPen(QColor(255, 255, 255, 50), 1, Qt::DashLine));
    p.drawLine(r.bottomLeft(), r.topRight());   // the untouched response, for reference

    QPolygonF curve;
    for (int i = 0; i <= 64; ++i) {
        const float x = float(i) / 64.0f;
        curve << toWidget(QPointF(x, evaluate(m_points, x)));
    }
    p.setPen(QPen(theme::accent, 2));
    p.drawPolyline(curve);
    for (int i = 0; i < m_points.size(); ++i) {
        p.setPen(QPen(Qt::white, 1.5));
        p.setBrush(i == m_grabbed ? QColor(Qt::white) : theme::accent);
        p.drawEllipse(toWidget(m_points[i]), 5.5, 5.5);
    }
}

void CurveEditor::mousePressEvent(QMouseEvent* e)
{
    m_grabbed = -1;
    double best = 16.0;
    for (int i = 0; i < m_points.size(); ++i) {
        const QPointF w = toWidget(m_points[i]);
        const double d = std::hypot(w.x() - e->position().x(), w.y() - e->position().y());
        if (d < best) {
            best = d;
            m_grabbed = i;
        }
    }
    // A press on the line itself, away from its points, puts a new point there.
    if (m_grabbed < 0 && m_points.size() < 12) {
        const QRectF r = plot();
        const double x = (e->position().x() - r.left()) / r.width();
        if (x > 0.0 && x < 1.0) {
            const double y = evaluate(m_points, float(x));
            int at = 0;
            while (at < m_points.size() && m_points[at].x() < x)
                ++at;
            const bool room = at > 0 && at < m_points.size() && x - m_points[at - 1].x() >= 0.04 && m_points[at].x() - x >= 0.04;
            if (room && std::abs(toWidget(QPointF(x, y)).y() - e->position().y()) < 18.0) {
                m_points.insert(at, QPointF(x, y));
                m_grabbed = at;
                emit changed();
            }
        }
    }
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e)
{
    if (m_grabbed < 0 || !(e->buttons() & Qt::LeftButton))
        return;
    const QRectF r = plot();
    // Pulled well clear of the plot, a point between the ends comes off the line.
    const bool inner = m_grabbed > 0 && m_grabbed < m_points.size() - 1;
    if (inner && (e->position().y() < r.top() - 36.0 || e->position().y() > r.bottom() + 36.0)) {
        m_points.removeAt(m_grabbed);
        m_grabbed = -1;
        update();
        emit changed();
        return;
    }
    double x = (e->position().x() - r.left()) / r.width();
    const double y = std::clamp((r.bottom() - e->position().y()) / r.height(), 0.0, 1.0);
    // The ends stay at the ends; inner points stay between their neighbours.
    if (m_grabbed == 0)
        x = 0.0;
    else if (m_grabbed == m_points.size() - 1)
        x = 1.0;
    else
        x = std::clamp(x, m_points[m_grabbed - 1].x() + 0.04, m_points[m_grabbed + 1].x() - 0.04);
    m_points[m_grabbed] = QPointF(x, y);
    update();
    emit changed();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*)
{
    m_grabbed = -1;
    update();
}

// -------------------------------------------------------------- SegmentBar

SegmentBar::SegmentBar(const QStringList& labels, QWidget* parent) : QWidget(parent), m_labels(labels)
{
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(28);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void SegmentBar::setCurrent(int index)
{
    index = std::clamp(index, 0, int(m_labels.size()) - 1);
    if (index == m_current)
        return;
    m_current = index;
    update();
}

// Each segment is as wide as its label, plus an equal share of what is left.
QRectF SegmentBar::segmentRect(int index) const
{
    const QFontMetricsF fm(theme::font(12, QFont::Medium));
    qreal text = 0.0;
    for (const QString& label : m_labels)
        text += fm.horizontalAdvance(label);
    const qreal pad = (width() - text) / std::max(1, int(m_labels.size()));
    qreal x = 0.0;
    for (int i = 0; i < m_labels.size(); ++i) {
        const qreal w = fm.horizontalAdvance(m_labels[i]) + pad;
        if (i == index)
            return QRectF(x, 0, w, height());
        x += w;
    }
    return {};
}

void SegmentBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1f, 0x1f, 0x22));
    p.drawRoundedRect(QRectF(rect()), 8, 8);
    p.setBrush(QColor(0x52, 0x52, 0x58));
    p.drawRoundedRect(segmentRect(m_current).adjusted(2, 2, -2, -2), 6, 6);
    p.setFont(theme::font(12, QFont::Medium));
    for (int i = 0; i < m_labels.size(); ++i) {
        p.setPen(i == m_current ? theme::text : theme::textDim);
        p.drawText(segmentRect(i), Qt::AlignCenter, m_labels[i]);
    }
}

void SegmentBar::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    for (int i = 0; i < m_labels.size(); ++i) {
        if (segmentRect(i).contains(e->position())) {
            if (i != m_current) {
                m_current = i;
                update();
                emit currentChanged(i);
            }
            return;
        }
    }
}

// -------------------------------------------------------------- SwatchGrid

SwatchGrid::SwatchGrid(int columns, QWidget* parent) : QWidget(parent), m_columns(columns)
{
    setCursor(Qt::PointingHandCursor);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void SwatchGrid::setColors(const QVector<QColor>& colors)
{
    m_colors = colors;
    updateGeometry();
    update();
}

int SwatchGrid::heightForWidth(int w) const
{
    const int rows = std::max(1, (int(m_colors.size()) + m_columns - 1) / m_columns);
    return int(std::ceil(double(w) / m_columns * rows));
}

QSize SwatchGrid::sizeHint() const
{
    return {260, heightForWidth(260)};
}

QRectF SwatchGrid::cellRect(int index) const
{
    const qreal cell = qreal(width()) / m_columns;
    return QRectF((index % m_columns) * cell, (index / m_columns) * cell, cell, cell);
}

void SwatchGrid::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_colors.size(); ++i) {
        const QRectF r = cellRect(i).adjusted(2.5, 2.5, -2.5, -2.5);
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.setBrush(m_colors[i]);
        p.drawRoundedRect(r, 5, 5);
    }
}

void SwatchGrid::mousePressEvent(QMouseEvent* e)
{
    for (int i = 0; i < m_colors.size(); ++i) {
        if (cellRect(i).contains(e->position())) {
            emit picked(m_colors[i]);
            return;
        }
    }
}
