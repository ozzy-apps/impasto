#include "ui/ColorPickers.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSpinBox>
#include <cmath>

namespace {

constexpr qreal kRing = 24.0;
constexpr qreal kGap = 10.0;
constexpr double kPi = 3.14159265358979323846;

// Maps the unit disc onto the unit square (and back) without the corners being
// crushed, so the whole saturation/brightness range stays reachable.
QPointF discToSquare(double u, double v)
{
    const double u2 = u * u, v2 = v * v, r2 = 2.0 * std::sqrt(2.0);
    const double x = 0.5 * std::sqrt(std::max(0.0, 2.0 + u2 - v2 + r2 * u)) - 0.5 * std::sqrt(std::max(0.0, 2.0 + u2 - v2 - r2 * u));
    const double y = 0.5 * std::sqrt(std::max(0.0, 2.0 - u2 + v2 + r2 * v)) - 0.5 * std::sqrt(std::max(0.0, 2.0 - u2 + v2 - r2 * v));
    return {std::clamp(x, -1.0, 1.0), std::clamp(y, -1.0, 1.0)};
}

QPointF squareToDisc(double x, double y)
{
    return {x * std::sqrt(1.0 - y * y / 2.0), y * std::sqrt(1.0 - x * x / 2.0)};
}

// The round mark that shows where a colour is, filled with that colour.
void drawMark(QPainter& p, const QPointF& at, const QColor& fill, qreal r)
{
    p.setPen(QPen(QColor(0, 0, 0, 110), 1));
    p.setBrush(Qt::white);
    p.drawEllipse(at, r + 2.5, r + 2.5);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(at, r, r);
}

} // namespace

// --------------------------------------------------------------- ColorDisc

ColorDisc::ColorDisc(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::CrossCursor);
}

qreal ColorDisc::outerRadius() const
{
    return std::min(width(), height()) / 2.0 - 3.0;
}

qreal ColorDisc::discRadius() const
{
    return outerRadius() - kRing - kGap;
}

void ColorDisc::setHsv(float h, float s, float v)
{
    m_h = h;
    m_s = s;
    m_v = v;
    update();
}

void ColorDisc::resizeEvent(QResizeEvent*)
{
    m_discHue = -1.0f;
}

void ColorDisc::rebuildDisc()
{
    const qreal dpr = devicePixelRatioF();
    const int d = int(std::ceil(discRadius() * 2 * dpr));
    if (d <= 0)
        return;
    m_disc = QImage(d, d, QImage::Format_RGB32);
    const double r = d / 2.0;
    for (int y = 0; y < d; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(m_disc.scanLine(y));
        for (int x = 0; x < d; ++x) {
            double u = (x + 0.5 - r) / r, v = (y + 0.5 - r) / r;
            const double len = std::hypot(u, v);
            if (len > 1.0) {
                u /= len;
                v /= len;
            }
            const QPointF sq = discToSquare(u, v);
            row[x] = QColor::fromHsvF(m_h, float((sq.x() + 1.0) / 2.0), float((1.0 - sq.y()) / 2.0)).rgb();
        }
    }
    m_disc.setDevicePixelRatio(dpr);
    m_discHue = m_h;
}

void ColorDisc::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, height() / 2.0);
    const qreal ro = outerRadius(), rm = ro - kRing / 2.0, rd = discRadius();

    QConicalGradient ring(c, 0);
    for (int i = 0; i <= 12; ++i)
        ring.setColorAt(i / 12.0, QColor::fromHsvF(float(i % 12) / 12.0f, 1.0f, 1.0f));
    p.setPen(QPen(QBrush(ring), kRing));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, rm, rm);

    if (m_discHue != m_h || m_disc.isNull())
        rebuildDisc();
    QPainterPath clip;
    clip.addEllipse(c, rd, rd);
    p.save();
    p.setClipPath(clip);
    p.drawImage(QPointF(c.x() - rd, c.y() - rd), m_disc);
    p.restore();

    const double a = m_h * 2.0 * kPi;
    drawMark(p, c + QPointF(std::cos(a), -std::sin(a)) * rm, QColor::fromHsvF(m_h, 1.0f, 1.0f), kRing / 2.0 - 4.0);
    const QPointF d = squareToDisc(m_s * 2.0 - 1.0, 1.0 - m_v * 2.0);
    drawMark(p, c + d * rd, QColor::fromHsvF(m_h, m_s, m_v), 8.0);
}

void ColorDisc::handle(const QPointF& pos)
{
    const QPointF c(width() / 2.0, height() / 2.0);
    const QPointF d = pos - c;
    if (m_grab == Grab::Ring) {
        double a = std::atan2(-d.y(), d.x()) / (2.0 * kPi);
        if (a < 0.0)
            a += 1.0;
        m_h = float(a);
    } else if (m_grab == Grab::Disc) {
        const qreal rd = discRadius();
        double u = d.x() / rd, v = d.y() / rd;
        const double len = std::hypot(u, v);
        if (len > 1.0) {
            u /= len;
            v /= len;
        }
        const QPointF sq = discToSquare(u, v);
        m_s = float((sq.x() + 1.0) / 2.0);
        m_v = float((1.0 - sq.y()) / 2.0);
    }
    update();
    emit hsvChanged(m_h, m_s, m_v);
}

void ColorDisc::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const QPointF c(width() / 2.0, height() / 2.0);
    const qreal dist = std::hypot(e->position().x() - c.x(), e->position().y() - c.y());
    m_grab = dist <= discRadius() + kGap / 2.0 ? Grab::Disc : Grab::Ring;
    handle(e->position());
}

void ColorDisc::mouseMoveEvent(QMouseEvent* e)
{
    if (m_grab != Grab::None && (e->buttons() & Qt::LeftButton))
        handle(e->position());
}

void ColorDisc::mouseReleaseEvent(QMouseEvent*)
{
    m_grab = Grab::None;
}

// ------------------------------------------------------------- ColorSquare

ColorSquare::ColorSquare(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::CrossCursor);
}

QRectF ColorSquare::field() const
{
    return QRectF(rect()).adjusted(6, 6, -6, -6);
}

void ColorSquare::setHsv(float h, float s, float v)
{
    m_h = h;
    m_s = s;
    m_v = v;
    update();
}

void ColorSquare::resizeEvent(QResizeEvent*)
{
    m_imageHue = -1.0f;
}

void ColorSquare::rebuild()
{
    const qreal dpr = devicePixelRatioF();
    const QRectF f = field();
    const int w = int(std::ceil(f.width() * dpr)), h = int(std::ceil(f.height() * dpr));
    if (w <= 1 || h <= 1)
        return;
    m_image = QImage(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(m_image.scanLine(y));
        const float v = 1.0f - float(y) / float(h - 1);
        for (int x = 0; x < w; ++x)
            row[x] = QColor::fromHsvF(m_h, float(x) / float(w - 1), v).rgb();
    }
    m_image.setDevicePixelRatio(dpr);
    m_imageHue = m_h;
}

void ColorSquare::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_imageHue != m_h || m_image.isNull())
        rebuild();
    const QRectF f = field();
    QPainterPath clip;
    clip.addRoundedRect(f, 8, 8);
    p.save();
    p.setClipPath(clip);
    p.drawImage(f.topLeft(), m_image);
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 36), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(f, 8, 8);
    drawMark(p, QPointF(f.left() + m_s * f.width(), f.bottom() - m_v * f.height()), QColor::fromHsvF(m_h, m_s, m_v), 8.0);
}

void ColorSquare::handle(const QPointF& pos)
{
    const QRectF f = field();
    m_s = float(std::clamp((pos.x() - f.left()) / f.width(), 0.0, 1.0));
    m_v = float(std::clamp((f.bottom() - pos.y()) / f.height(), 0.0, 1.0));
    update();
    emit hsvChanged(m_h, m_s, m_v);
}

void ColorSquare::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        handle(e->position());
}

void ColorSquare::mouseMoveEvent(QMouseEvent* e)
{
    if (e->buttons() & Qt::LeftButton)
        handle(e->position());
}

// ------------------------------------------------------------ HarmonyWheel

HarmonyWheel::HarmonyWheel(QWidget* parent) : QWidget(parent)
{
    setCursor(Qt::CrossCursor);
}

qreal HarmonyWheel::radius() const
{
    return std::min(width(), height()) / 2.0 - 13.0;   // room for a mark on the rim
}

float HarmonyWheel::offsetOf(int index) const
{
    const QVector<float> offsets = harmonyOffsets(m_mode);
    return index > 0 && index <= offsets.size() ? offsets[index - 1] : 0.0f;
}

void HarmonyWheel::setHsv(float h, float s, float v)
{
    m_h = h;
    m_s = s;
    m_v = v;
    update();
}

void HarmonyWheel::setHarmony(Harmony mode)
{
    m_mode = mode;
    m_grab = -1;
    update();
}

QPointF HarmonyWheel::markPos(int index) const
{
    const QPointF c(width() / 2.0, height() / 2.0);
    const double a = wrapHue(m_h + offsetOf(index)) * 2.0 * kPi;
    return c + QPointF(std::cos(a), -std::sin(a)) * (m_s * radius());
}

void HarmonyWheel::resizeEvent(QResizeEvent*)
{
    m_wheel = QImage();
}

void HarmonyWheel::rebuild()
{
    const qreal dpr = devicePixelRatioF();
    const int d = int(std::ceil(radius() * 2 * dpr));
    if (d <= 0)
        return;
    m_wheel = QImage(d, d, QImage::Format_RGB32);
    const double r = d / 2.0;
    for (int y = 0; y < d; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(m_wheel.scanLine(y));
        for (int x = 0; x < d; ++x) {
            const double u = (x + 0.5 - r) / r, v = (y + 0.5 - r) / r;
            double hue = std::atan2(-v, u) / (2.0 * kPi);
            if (hue < 0.0)
                hue += 1.0;
            row[x] = QColor::fromHsvF(float(std::min(hue, 0.9999)), float(std::min(1.0, std::hypot(u, v))), 1.0f).rgb();
        }
    }
    m_wheel.setDevicePixelRatio(dpr);
}

void HarmonyWheel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_wheel.isNull())
        rebuild();
    const QPointF c(width() / 2.0, height() / 2.0);
    const qreal r = radius();
    QPainterPath clip;
    clip.addEllipse(c, r, r);
    p.save();
    p.setClipPath(clip);
    p.drawImage(QPointF(c.x() - r, c.y() - r), m_wheel);
    // The wheel is drawn at full brightness and dimmed to the brightness in use.
    p.fillRect(rect(), QColor(0, 0, 0, qRound((1.0f - m_v) * 255.0f)));
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 36), 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, r, r);

    const int marks = int(harmonyOffsets(m_mode).size()) + 1;
    p.setPen(QPen(QColor(255, 255, 255, 120), 1));
    for (int i = 0; i < marks; ++i)
        p.drawLine(c, markPos(i));
    for (int i = marks - 1; i >= 0; --i)   // the colour itself last, on top
        drawMark(p, markPos(i), QColor::fromHsvF(wrapHue(m_h + offsetOf(i)), m_s, m_v), i == 0 ? 9.5 : 6.5);
}

void HarmonyWheel::handle(const QPointF& pos)
{
    const QPointF d = pos - QPointF(width() / 2.0, height() / 2.0);
    const double len = std::hypot(d.x(), d.y());
    m_s = float(std::min(1.0, len / radius()));
    if (len > 0.5) {
        // Whichever mark is held, the colour itself sits its offset behind it.
        const double hue = std::atan2(-d.y(), d.x()) / (2.0 * kPi);
        m_h = std::min(wrapHue(float(hue) - offsetOf(m_grab)), 0.9999f);
    }
    update();
    emit hsvChanged(m_h, m_s, m_v);
}

void HarmonyWheel::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    // A press on a companion's mark takes hold of that one; anywhere else
    // moves the colour itself there.
    m_grab = 0;
    double best = 14.0;
    const int marks = int(harmonyOffsets(m_mode).size()) + 1;
    bool onMark = false;
    for (int i = 0; i < marks; ++i) {
        const QPointF m = markPos(i);
        const double dist = std::hypot(m.x() - e->position().x(), m.y() - e->position().y());
        if (dist < best) {
            best = dist;
            m_grab = i;
            onMark = true;
        }
    }
    if (!onMark || m_grab == 0)
        handle(e->position());
}

void HarmonyWheel::mouseMoveEvent(QMouseEvent* e)
{
    if (m_grab >= 0 && (e->buttons() & Qt::LeftButton))
        handle(e->position());
}

void HarmonyWheel::mouseReleaseEvent(QMouseEvent*)
{
    m_grab = -1;
}

// ---------------------------------------------------------- ChannelSliders

ChannelSliders::ChannelSliders(Rows rows, QWidget* parent) : QWidget(parent)
{
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);
    const bool all = rows == Rows::All;
    const QString labels[6] = {tr("H"), tr("S"), tr("B"), tr("R"), tr("G"), tr("B")};
    const char* const names[6] = {"hue", "saturation", "brightness", "red", "green", "blue"};
    const int maxima[6] = {360, 100, 100, 255, 255, 255};
    int line = 0;
    for (int row = 0; row < (all ? 6 : 3); ++row, ++line) {
        if (row == 3)
            grid->setRowMinimumHeight(line++, 10);   // a breath between the two ways of saying it
        auto* label = new QLabel(labels[row], this);
        label->setFont(theme::font(12, QFont::Medium));
        label->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
        grid->addWidget(label, line, 0);
        auto* s = new Slider(this);
        s->setObjectName(QStringLiteral("color-%1").arg(QLatin1String(names[row])));
        grid->addWidget(s, line, 1);
        m_sliders.push_back(s);
        if (row < 3)
            connect(s, &Slider::valueChanged, this, [this, row](float v) { fromHsbRow(row, v); });
        else
            connect(s, &Slider::valueChanged, this, [this, row](float v) { fromRgbRow(row - 3, qRound(v * 255.0f)); });
        if (!all)
            continue;
        auto* n = new QSpinBox(this);
        n->setObjectName(QStringLiteral("color-%1-number").arg(QLatin1String(names[row])));
        n->setButtonSymbols(QAbstractSpinBox::NoButtons);
        n->setRange(0, maxima[row]);
        n->setAlignment(Qt::AlignCenter);
        n->setFixedWidth(50);
        n->setFont(theme::font(12));
        if (row == 0)
            n->setSuffix(QStringLiteral("°"));
        else if (row < 3)
            n->setSuffix(QStringLiteral("%"));
        grid->addWidget(n, line, 2);
        m_numbers.push_back(n);
        // Once a number is entered the keys go back to being shortcuts.
        connect(n, &QSpinBox::editingFinished, n, [n] { n->clearFocus(); });
        connect(n, &QSpinBox::valueChanged, this, [this, row, maxima](int value) {
            if (row < 3)
                fromHsbRow(row, float(value) / float(maxima[row]));
            else
                fromRgbRow(row - 3, value);
        });
    }
    QVector<QColor> hues;
    for (int i = 0; i <= 12; ++i)
        hues.push_back(QColor::fromHsvF(float(i % 12) / 12.0f, 1.0f, 1.0f));
    m_sliders[0]->setTrackColors(hues);
    sync();
}

void ChannelSliders::setHsv(float h, float s, float v)
{
    m_h = h;
    m_s = s;
    m_v = v;
    sync();
}

void ChannelSliders::fromHsbRow(int row, float value)
{
    if (row == 0)
        m_h = std::clamp(value, 0.0f, 0.9999f);
    else if (row == 1)
        m_s = std::clamp(value, 0.0f, 1.0f);
    else
        m_v = std::clamp(value, 0.0f, 1.0f);
    sync();
    emit hsvChanged(m_h, m_s, m_v);
}

void ChannelSliders::fromRgbRow(int row, int value)
{
    QColor c = QColor::fromHsvF(m_h, m_s, m_v).toRgb();
    value = std::clamp(value, 0, 255);
    if (row == 0)
        c.setRed(value);
    else if (row == 1)
        c.setGreen(value);
    else
        c.setBlue(value);
    emit rgbChanged(c);   // comes back as hue, saturation and brightness through setHsv
}

void ChannelSliders::sync()
{
    const QColor c = QColor::fromHsvF(m_h, m_s, m_v).toRgb();
    m_sliders[0]->setValue(m_h);
    m_sliders[1]->setValue(m_s);
    m_sliders[2]->setValue(m_v);
    m_sliders[1]->setTrackColors({QColor::fromHsvF(m_h, 0.0f, m_v), QColor::fromHsvF(m_h, 1.0f, m_v)});
    m_sliders[2]->setTrackColors({QColor(Qt::black), QColor::fromHsvF(m_h, m_s, 1.0f)});
    if (m_sliders.size() < 6)
        return;
    m_sliders[3]->setValue(float(c.redF()));
    m_sliders[4]->setValue(float(c.greenF()));
    m_sliders[5]->setValue(float(c.blueF()));
    m_sliders[3]->setTrackColors({QColor(0, c.green(), c.blue()), QColor(255, c.green(), c.blue())});
    m_sliders[4]->setTrackColors({QColor(c.red(), 0, c.blue()), QColor(c.red(), 255, c.blue())});
    m_sliders[5]->setTrackColors({QColor(c.red(), c.green(), 0), QColor(c.red(), c.green(), 255)});
    const int values[6] = {qRound(m_h * 360.0f) % 360, qRound(m_s * 100.0f), qRound(m_v * 100.0f), c.red(), c.green(), c.blue()};
    for (int i = 0; i < m_numbers.size(); ++i) {
        // A number being typed is left alone unless it has really changed.
        if (m_numbers[i]->value() == values[i])
            continue;
        const QSignalBlocker quiet(m_numbers[i]);
        m_numbers[i]->setValue(values[i]);
    }
}

// --------------------------------------------------------------- ColorPair

ColorPair::ColorPair(QWidget* parent) : QAbstractButton(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void ColorPair::setColors(const QColor& current, const QColor& previous)
{
    m_current = current;
    m_previous = previous;
    update();
}

void ColorPair::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath shape;
    shape.addRoundedRect(r, 6, 6);
    p.save();
    p.setClipPath(shape);
    p.fillRect(r, m_current);
    // The previous colour takes the right half; with none yet, the chip is all one colour.
    if (m_previous.isValid())
        p.fillRect(QRectF(r.center().x(), r.top(), r.width() / 2.0 + 1, r.height()), m_previous);
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, isDown() ? 150 : 70), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r, 6, 6);
}
