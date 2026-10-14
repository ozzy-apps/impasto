#include "ui/BrushLab.h"

#include "brush/BrushTextures.h"
#include "brush/TextureGen.h"
#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cmath>

namespace {

constexpr int kLeftWidth = 236;
constexpr int kMiddleWidth = 424;
constexpr int kContentWidth = 380;      // what a page has to lay things out in
constexpr int kHeaderHeight = 56;
constexpr int kSectionRow = 40;
constexpr int kPickColumns = 4;
constexpr int kPickCell = kContentWidth / kPickColumns;
constexpr int kPickRow = kPickCell + 16;
constexpr double kSlack = 8.0;          // travel that is still a tap

const QColor kLabBackground(0x20, 0x20, 0x23);
const QColor kLabSide(0x26, 0x26, 0x29);
const QColor kPadPaper(0x16, 0x16, 0x18);

struct SectionInfo {
    const char* title;
    const char* icon;
};
const SectionInfo kSections[BrushLab::SectionCount] = {
    {QT_TRANSLATE_NOOP("BrushLab", "Stroke path"), "stroke-path"},
    {QT_TRANSLATE_NOOP("BrushLab", "Stabilisation"), "stabilise"},
    {QT_TRANSLATE_NOOP("BrushLab", "Taper"), "taper"},
    {QT_TRANSLATE_NOOP("BrushLab", "Shape"), "tip-shape"},
    {QT_TRANSLATE_NOOP("BrushLab", "Grain"), "grain"},
    {QT_TRANSLATE_NOOP("BrushLab", "Rendering"), "rendering"},
    {QT_TRANSLATE_NOOP("BrushLab", "Wet mix"), "wet-mix"},
    {QT_TRANSLATE_NOOP("BrushLab", "Colour dynamics"), "color-dynamics"},
    {QT_TRANSLATE_NOOP("BrushLab", "Dynamics"), "dynamics"},
    {QT_TRANSLATE_NOOP("BrushLab", "Pen"), "pen"},
    {QT_TRANSLATE_NOOP("BrushLab", "Properties"), "properties"},
    {QT_TRANSLATE_NOOP("BrushLab", "About this brush"), "info"},
};

QLabel* dimLabel(const QString& text, QWidget* parent, int px = 12)
{
    auto* l = new QLabel(text, parent);
    l->setFont(theme::font(px, QFont::Medium));
    l->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    return l;
}

// The round tip a brush has when it uses no picture: soft or hard, squashed, turned.
QImage roundTip(float hardness, float roundness, float degrees)
{
    constexpr int n = 256;
    QImage img(n, n, QImage::Format_Grayscale8);
    const double a = -degrees * 3.14159265358979 / 180.0, ca = std::cos(a), sa = std::sin(a);
    const double squash = std::clamp(double(roundness), 0.05, 1.0);
    const double inner = std::clamp(double(hardness), 0.0, 0.98);
    for (int y = 0; y < n; ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < n; ++x) {
            const double u = (x + 0.5) / n * 2.0 - 1.0, v = (y + 0.5) / n * 2.0 - 1.0;
            const double along = (u * ca - v * sa) / 0.9, across = (u * sa + v * ca) / (0.9 * squash);
            const double d = std::hypot(along, across);
            const double t = std::clamp((1.0 - d) / (1.0 - inner), 0.0, 1.0);
            row[x] = uchar(std::lround(t * t * (3.0 - 2.0 * t) * 255.0));
        }
    }
    return img;
}

// The picture behind a tip or a grain name, whoever made it. Kept, since the
// shipped ones are worked out pixel by pixel.
QImage textureImage(const QString& name, bool grain)
{
    static QHash<QString, QImage> cache;
    if (name.isEmpty())
        return grain ? QImage() : roundTip(0.85f, 1.0f, 0.0f);
    const QString key = (grain ? QStringLiteral("g:") : QStringLiteral("t:")) + name;
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();
    QImage img = brushtex::isUser(name) ? brushtex::load(name) : (grain ? texgen::grain(name) : texgen::tip(name));
    if (!brushtex::isUser(name) || !img.isNull())
        cache.insert(key, img);
    return img;
}

// Something for the smudge tool to drag about and the eraser to bite into.
QImage padGround(const QSize& size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const QColor inks[5] = {QColor(0xff, 0x45, 0x3a), QColor(0xff, 0xd6, 0x0a), QColor(0x30, 0xd1, 0x58), QColor(0x0a, 0x84, 0xff), QColor(0xf2, 0xf2, 0xf7)};
    const double band = size.height() / 7.0;
    for (int i = 0; i < 5; ++i) {
        p.setBrush(inks[i]);
        p.drawRoundedRect(QRectF(size.width() * 0.08, band * (1.0 + i), size.width() * 0.84, band * 0.72), band * 0.2, band * 0.2);
    }
    return img;
}

} // namespace

// ------------------------------------------------------------- SectionList

class BrushLab::SectionList : public QWidget {
public:
    explicit SectionList(QWidget* parent) : QWidget(parent)
    {
        setFixedHeight(SectionCount * kSectionRow);
        setCursor(Qt::PointingHandCursor);
    }
    int current() const { return m_current; }
    void setCurrent(int index)
    {
        m_current = std::clamp(index, 0, int(SectionCount) - 1);
        update();
    }
    QRect rowRect(int index) const { return QRect(0, index * kSectionRow, width(), kSectionRow); }
    std::function<void(int)> onChanged;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(theme::font(13, QFont::Medium));
        for (int i = 0; i < SectionCount; ++i) {
            const QRectF r = QRectF(rowRect(i)).adjusted(0, 2, 0, -2);
            const bool on = i == m_current;
            if (on) {
                p.setPen(Qt::NoPen);
                p.setBrush(theme::accent);
                p.drawRoundedRect(r, 10, 10);
            }
            const QColor ink = on ? QColor(Qt::white) : theme::textDim;
            p.drawPixmap(QPointF(r.left() + 12, r.center().y() - 9), theme::icon(QLatin1String(kSections[i].icon), 18, ink, devicePixelRatioF()));
            p.setPen(on ? QColor(Qt::white) : theme::text);
            p.drawText(r.adjusted(42, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, QCoreApplication::translate("BrushLab", kSections[i].title));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const int i = int(e->position().y()) / kSectionRow;
        if (e->button() != Qt::LeftButton || i < 0 || i >= SectionCount || i == m_current)
            return;
        m_current = i;
        update();
        if (onChanged)
            onChanged(i);
    }

private:
    int m_current = 0;
};

// ----------------------------------------------------------- TexturePicker

// Every tip shape, or every grain, as a wall of small pictures. The last
// tile brings in a picture of your own.
class BrushLab::TexturePicker : public QWidget {
public:
    struct Item {
        QString name, title;
    };
    TexturePicker(bool grain, QWidget* parent) : QWidget(parent), m_grain(grain)
    {
        setCursor(Qt::PointingHandCursor);
        setItems({});
    }
    void setItems(const QVector<Item>& items)
    {
        m_items = items;
        const int tiles = int(m_items.size()) + 1;
        setFixedSize(kContentWidth, ((tiles + kPickColumns - 1) / kPickColumns) * kPickRow);
        update();
    }
    void setCurrent(const QString& name)
    {
        m_current = name;
        update();
    }
    void setScroller(DragScroller* scroller) { m_scroller = scroller; }
    int count() const { return int(m_items.size()); }
    QString nameAt(int index) const { return index >= 0 && index < m_items.size() ? m_items[index].name : QString(); }
    QRect cellRect(int index) const { return QRect((index % kPickColumns) * kPickCell, (index / kPickColumns) * kPickRow, kPickCell, kPickRow); }
    std::function<void(const QString&)> onPicked;
    std::function<void()> onImport;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const qreal dpr = devicePixelRatioF();
        const int side = kPickCell - 14;
        int made = 0;
        bool more = false;
        for (int i = 0; i <= m_items.size(); ++i) {
            const QRect cell = cellRect(i);
            const QRectF tile(cell.left() + 7, cell.top() + 4, side, side);
            const bool import = i == m_items.size();
            const bool on = !import && m_items[i].name == m_current;
            p.setPen(on ? QPen(theme::accent, 2.5) : QPen(QColor(255, 255, 255, 22), 1));
            p.setBrush(QColor(0x14, 0x14, 0x16));
            p.drawRoundedRect(tile, 9, 9);
            QString title;
            if (import) {
                p.drawPixmap(QPointF(tile.center().x() - 11, tile.center().y() - 11), theme::icon(QStringLiteral("plus"), 22, theme::textDim, dpr));
                title = tr("Import…");
            } else {
                title = m_items[i].title;
                const QString key = m_items[i].name;
                auto it = m_thumbs.find(key);
                // A few at a time: the shipped pictures take a moment each to work out.
                if (it == m_thumbs.end() && made < 3) {
                    const QImage full = textureImage(key, m_grain);
                    QPixmap thumb;
                    if (!full.isNull()) {
                        thumb = QPixmap::fromImage(full.scaled(QSize(side - 8, side - 8) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                        thumb.setDevicePixelRatio(dpr);
                    }
                    it = m_thumbs.insert(key, thumb);
                    ++made;
                } else if (it == m_thumbs.end()) {
                    more = true;
                }
                if (it != m_thumbs.end() && !it.value().isNull()) {
                    QPainterPath clip;
                    clip.addRoundedRect(tile.adjusted(4, 4, -4, -4), 6, 6);
                    p.save();
                    p.setClipPath(clip);
                    p.drawPixmap(tile.topLeft() + QPointF(4, 4), it.value());
                    p.restore();
                } else if (key.isEmpty() && m_grain) {
                    p.setPen(theme::textDim);
                    p.setFont(theme::font(20));
                    p.drawText(tile, Qt::AlignCenter, QStringLiteral("∅"));
                }
            }
            p.setFont(theme::font(11, on ? QFont::DemiBold : QFont::Normal));
            p.setPen(on ? theme::text : theme::textDim);
            const QRectF caption(cell.left() + 2, tile.bottom() + 2, cell.width() - 4, 16);
            p.drawText(caption, Qt::AlignHCenter | Qt::AlignTop, QFontMetrics(p.font()).elidedText(title, Qt::ElideRight, int(caption.width())));
        }
        if (more)
            QTimer::singleShot(0, this, qOverload<>(&QWidget::update));
    }

    // A tap chooses; a drag with the pen scrolls the page this is on.
    void mousePressEvent(QMouseEvent* e) override
    {
        if (m_scroller)
            m_scroller->stop();
        m_down = e->button() == Qt::LeftButton;
        m_scrolling = false;
        m_pressGlobal = e->globalPosition();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_down || !(e->buttons() & Qt::LeftButton))
            return;
        const QPointF d = e->globalPosition() - m_pressGlobal;
        if (!m_scrolling && std::hypot(d.x(), d.y()) > kSlack && m_scroller && m_scroller->canScroll()) {
            m_scrolling = true;
            m_scroller->begin(m_pressGlobal);
        }
        if (m_scrolling)
            m_scroller->dragTo(e->globalPosition());
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !m_down)
            return;
        m_down = false;
        if (m_scrolling) {
            m_scrolling = false;
            m_scroller->end();
            return;
        }
        const QPointF d = e->globalPosition() - m_pressGlobal;
        if (std::hypot(d.x(), d.y()) > kSlack * 2)
            return;
        for (int i = 0; i <= m_items.size(); ++i) {
            if (!cellRect(i).contains(e->position().toPoint()))
                continue;
            if (i == m_items.size()) {
                if (onImport)
                    onImport();
            } else if (onPicked) {
                onPicked(m_items[i].name);
            }
            return;
        }
    }

private:
    bool m_grain;
    QVector<Item> m_items;
    QString m_current;
    QHash<QString, QPixmap> m_thumbs;
    DragScroller* m_scroller = nullptr;
    bool m_down = false, m_scrolling = false;
    QPointF m_pressGlobal;
};

// The tip shape or the grain in use, large.
class BrushLab::TexturePreview : public QWidget {
public:
    explicit TexturePreview(QWidget* parent) : QWidget(parent) { setFixedSize(kContentWidth, 224); }
    void setImage(const QImage& image, const QString& caption)
    {
        m_image = image;
        m_caption = caption;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(QColor(255, 255, 255, 22), 1));
        p.setBrush(QColor(0x14, 0x14, 0x16));
        p.drawRoundedRect(frame, 12, 12);
        const QRectF square(frame.center().x() - 100, frame.center().y() - 100, 200, 200);
        if (!m_image.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(square, 8, 8);
            p.save();
            p.setClipPath(clip);
            p.drawImage(square, m_image);
            p.restore();
        } else {
            p.setPen(theme::textDim);
            p.setFont(theme::font(13));
            p.drawText(frame, Qt::AlignCenter, tr("No grain: the paint goes on smooth"));
        }
        p.setFont(theme::font(11, QFont::Medium));
        p.setPen(theme::textDim);
        p.drawText(frame.adjusted(12, 8, -12, -8), Qt::AlignTop | Qt::AlignLeft, m_caption);
    }

private:
    QImage m_image;
    QString m_caption;
};

// ---------------------------------------------------------------- BrushLab

const std::vector<BrushLab::Row>& BrushLab::rows()
{
    using P = BrushPreset;
    static const std::vector<Row> table = {
        {StrokePath, "spacing", nullptr, QT_TR_NOOP("Spacing"), QT_TR_NOOP("Gap between stamps, as a share of the brush size"), &P::spacing, 0.01f, 1.0f, 2.0f, Unit::Percent},
        {StrokePath, "scatter", nullptr, QT_TR_NOOP("Jitter"), QT_TR_NOOP("Stamps thrown off the line"), &P::scatter, 0.0f, 2.0f, 2.0f, Unit::Percent},

        {Stabilisation, "streamline", nullptr, QT_TR_NOOP("StreamLine"), QT_TR_NOOP("The line glides after the pen: smoother, with a little lag"), &P::streamline, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Stabilisation, "stabilization", nullptr, QT_TR_NOOP("Stabilisation"), QT_TR_NOOP("Averages the path; the faster the stroke, the more it is smoothed"), &P::stabilization, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Stabilisation, "stringLength", nullptr, QT_TR_NOOP("Pull string"), QT_TR_NOOP("The line only follows once the pen pulls a string taut: slow, exact lines with crisp corners"), &P::stringLength, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Stabilisation, "pressureSmoothing", nullptr, QT_TR_NOOP("Weight smoothing"), QT_TR_NOOP("Steadies the line weight against small changes in pressure"), &P::pressureSmoothing, 0.0f, 1.0f, 1.0f, Unit::Percent},

        {Taper, "taper", nullptr, QT_TR_NOOP("Start"), QT_TR_NOOP("How long the stroke takes to reach full width, in brush sizes"), &P::taper, 0.0f, 6.0f, 1.0f, Unit::Times},
        {Taper, "taperEnd", nullptr, QT_TR_NOOP("End"), QT_TR_NOOP("Length of the point the stroke runs out to when the pen lifts"), &P::taperEnd, 0.0f, 6.0f, 1.0f, Unit::Times},
        {Taper, "taperTip", nullptr, QT_TR_NOOP("Tip size"), QT_TR_NOOP("How fine the tapered point gets"), &P::taperTip, 0.0f, 1.0f, 1.0f, Unit::Percent},

        {Shape, "angle", QT_TR_NOOP("Shape properties"), QT_TR_NOOP("Rotation"), QT_TR_NOOP("Rotation of the tip"), &P::angle, 0.0f, 180.0f, 1.0f, Unit::Degrees},
        {Shape, "angleJitter", nullptr, QT_TR_NOOP("Rotation jitter"), QT_TR_NOOP("Random rotation per stamp"), &P::angleJitter, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Shape, "roundness", nullptr, QT_TR_NOOP("Roundness"), QT_TR_NOOP("Squash the tip into an oval"), &P::roundness, 0.05f, 1.0f, 1.0f, Unit::Percent},
        {Shape, "hardness", nullptr, QT_TR_NOOP("Hardness"), QT_TR_NOOP("Edge softness of the round tip"), &P::hardness, 0.0f, 1.0f, 1.0f, Unit::Percent},

        {Grain, "grainDepth", nullptr, QT_TR_NOOP("Depth"), QT_TR_NOOP("How strongly the texture shows"), &P::grainDepth, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Grain, "grainScale", nullptr, QT_TR_NOOP("Scale"), QT_TR_NOOP("Size of the texture"), &P::grainScale, 0.2f, 4.0f, 2.0f, Unit::Times},

        {Rendering, "flow", nullptr, QT_TR_NOOP("Flow"), QT_TR_NOOP("Paint laid down per stamp"), &P::flow, 0.01f, 1.0f, 2.0f, Unit::Percent},
        {Rendering, "opacity", nullptr, QT_TR_NOOP("Opacity"), QT_TR_NOOP("The opacity a stroke has when this brush is picked up"), &P::opacity, 0.05f, 1.0f, 1.0f, Unit::Percent},
        {Rendering, "wetEdges", nullptr, QT_TR_NOOP("Wet edges"), QT_TR_NOOP("Pigment pooling at the rim, as in a watercolour wash"), &P::wetEdges, 0.0f, 0.9f, 1.0f, Unit::Percent},

        {WetMix, "wetMix", nullptr, QT_TR_NOOP("Pull"), QT_TR_NOOP("How far the brush drags the paint that is already there. At nothing the brush is dry: it lays its own colour down and leaves the rest be."), &P::wetMix, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {WetMix, "wetCharge", nullptr, QT_TR_NOOP("Charge"), QT_TR_NOOP("How much of what it lays down is its own paint; the rest is what it picked up on the way"), &P::wetCharge, 0.02f, 1.0f, 1.0f, Unit::Percent},

        {ColourDynamics, "hueJitter", QT_TR_NOOP("Stamp colour jitter"), QT_TR_NOOP("Hue"), QT_TR_NOOP("Random colour shift per stamp"), &P::hueJitter, 0.0f, 0.5f, 2.0f, Unit::Percent},
        {ColourDynamics, "satJitter", nullptr, QT_TR_NOOP("Saturation"), QT_TR_NOOP("Random saturation per stamp"), &P::satJitter, 0.0f, 1.0f, 2.0f, Unit::Percent},
        {ColourDynamics, "valJitter", nullptr, QT_TR_NOOP("Brightness"), QT_TR_NOOP("Random brightness per stamp"), &P::valJitter, 0.0f, 1.0f, 2.0f, Unit::Percent},

        {Dynamics, "speedSize", QT_TR_NOOP("Speed"), QT_TR_NOOP("Size"), QT_TR_NOOP("Above zero a fast stroke thins out; below zero it thickens"), &P::speedSize, -1.0f, 1.0f, 1.0f, Unit::Percent},
        {Dynamics, "speedFlow", nullptr, QT_TR_NOOP("Flow"), QT_TR_NOOP("A fast stroke runs dry"), &P::speedFlow, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Dynamics, "sizeJitter", QT_TR_NOOP("Jitter"), QT_TR_NOOP("Size"), QT_TR_NOOP("Random size per stamp"), &P::sizeJitter, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Dynamics, "flowJitter", nullptr, QT_TR_NOOP("Flow"), QT_TR_NOOP("Random paint amount per stamp"), &P::flowJitter, 0.0f, 1.0f, 1.0f, Unit::Percent},

        {Pen, "pressureSize", nullptr, QT_TR_NOOP("Size"), QT_TR_NOOP("How much pressing harder widens the stroke"), &P::pressureSize, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Pen, "pressureFlow", nullptr, QT_TR_NOOP("Flow"), QT_TR_NOOP("How much pressing harder adds paint"), &P::pressureFlow, 0.0f, 1.0f, 1.0f, Unit::Percent},
        {Pen, "tiltSize", QT_TR_NOOP("Tilt"), QT_TR_NOOP("Size"), QT_TR_NOOP("Extra width when the pen leans over"), &P::tiltSize, 0.0f, 3.0f, 1.0f, Unit::Times},
        {Pen, "tiltFlow", nullptr, QT_TR_NOOP("Flow"), QT_TR_NOOP("Paint lost when the pen leans over"), &P::tiltFlow, 0.0f, 1.0f, 1.0f, Unit::Percent},

        {Properties, "minSize", QT_TR_NOOP("Size limits"), QT_TR_NOOP("Smallest"), QT_TR_NOOP("Where the size slider starts"), &P::minSize, 0.5f, 50.0f, 2.0f, Unit::Pixels},
        {Properties, "maxSize", nullptr, QT_TR_NOOP("Largest"), QT_TR_NOOP("Where the size slider ends"), &P::maxSize, 10.0f, 1000.0f, 2.0f, Unit::Pixels},
    };
    return table;
}

QString BrushLab::format(const Row& r, float v)
{
    switch (r.unit) {
    case Unit::Percent: return QStringLiteral("%1%").arg(qRound(v * 100.0f));
    case Unit::Degrees: return QStringLiteral("%1°").arg(qRound(v));
    case Unit::Pixels: return v < 10.0f ? tr("%1 px").arg(QString::number(v, 'f', 1)) : tr("%1 px").arg(qRound(v));
    case Unit::Times: return QStringLiteral("%1×").arg(QString::number(v, 'f', 2));
    case Unit::Number: break;
    }
    return QString::number(v, 'f', 2);
}

BrushLab::BrushLab(CanvasEngine* engine, ToolState* tools, QWidget* host) : QWidget(host), m_engine(engine), m_tools(tools)
{
    setObjectName(QStringLiteral("brush-lab"));
    m_padTools = new ToolState(this);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---- left: the sections
    auto* left = new QWidget(this);
    left->setFixedWidth(kLeftWidth);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(14, 18, 12, 16);
    ll->setSpacing(2);
    auto* title = new QLabel(tr("Brush Lab"), left);
    title->setFont(theme::font(22, QFont::DemiBold));
    ll->addWidget(title);
    m_brushName = dimLabel(QString(), left, 13);
    m_brushName->setObjectName(QStringLiteral("lab-brush-name"));
    ll->addWidget(m_brushName);
    ll->addSpacing(14);
    m_sections = new SectionList(left);
    m_sections->setObjectName(QStringLiteral("lab-sections"));
    ll->addWidget(m_sections);
    ll->addStretch();
    m_strokePreview = new QLabel(left);
    m_strokePreview->setFixedHeight(80);
    m_strokePreview->setStyleSheet(QStringLiteral("background: #141416; border-radius: 10px;"));
    m_strokePreview->setToolTip(tr("One stroke of this brush, from a light touch to a firm one and back"));
    ll->addWidget(m_strokePreview);
    outer->addWidget(left);

    // ---- middle: the section that is open
    auto* middle = new QWidget(this);
    middle->setFixedWidth(kMiddleWidth);
    auto* ml = new QVBoxLayout(middle);
    ml->setContentsMargins(18, 20, 4, 8);
    ml->setSpacing(10);
    m_pageTitle = new QLabel(middle);
    m_pageTitle->setFont(theme::font(18, QFont::DemiBold));
    ml->addWidget(m_pageTitle);
    m_stack = new QStackedWidget(middle);
    ml->addWidget(m_stack, 1);
    outer->addWidget(middle);
    buildPages();

    outer->addWidget(buildPadSide(), 1);

    m_sections->onChanged = [this](int index) { setSection(index); };
    m_redraw.setSingleShot(true);
    m_redraw.setInterval(35);
    connect(&m_redraw, &QTimer::timeout, this, &BrushLab::redrawPad);
    m_resizePad.setSingleShot(true);
    m_resizePad.setInterval(120);
    connect(&m_resizePad, &QTimer::timeout, this, [this] { syncPadDocument(false); });
    m_disarm.setSingleShot(true);
    m_disarm.setInterval(3500);
    connect(&m_disarm, &QTimer::timeout, this, [this] { armDelete(false); });
    // Duplicating or deleting changes which brush the tool holds.
    connect(m_tools, &ToolState::brushChanged, this, [this] {
        if (m_applying || !isVisible())
            return;
        if (!BrushLibrary::instance().find(m_id)) {
            finish();   // the brush was deleted from under the lab
            return;
        }
        if (m_tools->preset().id != m_id)
            adopt(m_tools->preset().id);
        else
            load();
    });
    setSection(StrokePath);
    hide();
}

BrushLab::~BrushLab()
{
    // The pad's viewport draws from the pad's engine: it goes first.
    delete m_pad;
    m_pad = nullptr;
}

const BrushPreset& BrushLab::preset() const
{
    const BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset* p = lib.find(m_id);
    return p ? *p : lib.fallback();
}

QVBoxLayout* BrushLab::page(Section section) const
{
    return m_pages[size_t(section)];
}

QLabel* BrushLab::heading(Section section, const QString& text)
{
    QVBoxLayout* l = page(section);
    auto* h = new QLabel(text, l->parentWidget());
    h->setFont(theme::font(15, QFont::DemiBold));
    h->setContentsMargins(0, l->count() > 0 ? 14 : 0, 0, 2);
    l->addWidget(h);
    return h;
}

// A setting as the lab shows it: its name and its value on one line, and a
// slider the full width of the column underneath.
Slider* BrushLab::addSlider(Section section, const QString& label, const QString& hint, const QString& name, QLabel** value)
{
    QVBoxLayout* l = page(section);
    auto* box = new QWidget(l->parentWidget());
    box->setFixedWidth(kContentWidth);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 3, 0, 3);
    v->setSpacing(1);
    auto* top = new QHBoxLayout;
    auto* title = new QLabel(label, box);
    title->setFont(theme::font(13));
    title->setToolTip(hint);
    top->addWidget(title);
    top->addStretch();
    auto* pill = new QLabel(box);
    pill->setFont(theme::font(12, QFont::Medium));
    pill->setAlignment(Qt::AlignCenter);
    pill->setMinimumWidth(52);
    pill->setStyleSheet(QStringLiteral("background: #141416; border-radius: 9px; padding: 2px 9px;"));
    top->addWidget(pill);
    v->addLayout(top);
    auto* slider = new Slider(box);
    slider->setObjectName(name);
    slider->setToolTip(hint);
    v->addWidget(slider);
    l->addWidget(box);
    *value = pill;
    return slider;
}

void BrushLab::buildPages()
{
    for (int i = 0; i < SectionCount; ++i) {
        auto* scroll = new QScrollArea(m_stack);
        auto* body = new QWidget(scroll);
        auto* l = new QVBoxLayout(body);
        l->setContentsMargins(0, 2, 14, 24);
        l->setSpacing(9);
        scroll->setWidget(body);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->viewport()->setAutoFillBackground(false);
        body->setAutoFillBackground(false);
        auto* scroller = new DragScroller(scroll, body);   // press on the names and drag, for the pen
        scroll->setProperty("scroller", QVariant::fromValue(static_cast<QObject*>(scroller)));
        scroll->setObjectName(QStringLiteral("lab-page-%1").arg(i));
        m_stack->addWidget(scroll);
        m_pages.push_back(l);
    }
    buildShape();
    buildGrain();
    buildPen();

    for (const Row& r : rows()) {
        if (r.heading)
            heading(r.section, tr(r.heading));
        QLabel* value = nullptr;
        Slider* slider = addSlider(r.section, tr(r.label), tr(r.hint), QStringLiteral("lab-%1").arg(QLatin1String(r.key)), &value);
        m_sliders.push_back(slider);
        m_values.push_back(value);
        const int index = int(m_sliders.size()) - 1;
        connect(slider, &Slider::valueChanged, this, [this, index](float t) { edited(index, t); });
    }

    m_count = addSlider(StrokePath, tr("Stamps per step"), tr("How many stamps land at each step; with jitter this makes sprays and foliage"),
                        QStringLiteral("lab-count"), &m_countValue);
    connect(m_count, &Slider::valueChanged, this, [this](float t) {
        const int count = 1 + qRound(t * 15.0f);
        m_countValue->setText(QString::number(count));
        change([&](BrushPreset& p) { p.count = count; });
    });

    page(Rendering)->addSpacing(10);
    page(Rendering)->addWidget(dimLabel(tr("Where stamps overlap within one stroke"), page(Rendering)->parentWidget()));
    m_buildup = new SegmentBar({tr("Paint builds up"), tr("Levels off at the strongest")}, page(Rendering)->parentWidget());
    m_buildup->setObjectName(QStringLiteral("lab-buildup"));
    m_buildup->setFixedWidth(kContentWidth);
    page(Rendering)->addWidget(m_buildup);
    connect(m_buildup, &SegmentBar::currentChanged, this, [this](int index) { change([&](BrushPreset& p) { p.buildup = BrushPreset::Buildup(index); }); });

    // How the paint meets what the layer already holds.
    heading(Rendering, tr("Blend mode"));
    m_blend = new QComboBox(page(Rendering)->parentWidget());
    m_blend->setObjectName(QStringLiteral("lab-blend"));
    m_blend->setFocusPolicy(Qt::NoFocus);
    m_blend->setFixedWidth(kContentWidth);
    m_blend->setFont(theme::font(13));
    m_blend->setToolTip(tr("Normal lays the paint on top. Any other mode blends it with the paint already on the layer, the way a layer in that mode would; "
                           "on bare canvas the paint goes down as it is. Multiply suits markers and washes, Add and Screen suit light."));
    for (const BlendModeInfo& mode : blendModes())
        m_blend->addItem(tr(mode.name));
    page(Rendering)->addWidget(m_blend);
    connect(m_blend, &QComboBox::activated, this, [this](int index) { change([&](BrushPreset& p) { p.blend = BlendMode(index); }); });

    {
        auto* note = dimLabel(tr("A wet brush works straight into the paint on the layer, dragging it along and mixing it into its own. "
                                 "Grain, wet edges and the blend mode belong to dry brushes and are left out."),
                              page(WetMix)->parentWidget());
        note->setWordWrap(true);
        note->setFixedWidth(kContentWidth);
        page(WetMix)->addSpacing(8);
        page(WetMix)->addWidget(note);
    }

    // The walls of pictures to choose from, under everything that has a number.
    {
        auto* line = new QHBoxLayout;
        auto* h = new QLabel(tr("Shapes"), page(Shape)->parentWidget());
        h->setFont(theme::font(15, QFont::DemiBold));
        h->setContentsMargins(0, 14, 0, 2);
        line->addWidget(h);
        line->addStretch();
        line->addWidget(m_invert, 0, Qt::AlignBottom);
        page(Shape)->addLayout(line);
        page(Shape)->addWidget(m_tipPicker);
        heading(Grain, tr("Grains"));
        page(Grain)->addWidget(m_grainPicker);
    }

    buildAbout();
    for (QVBoxLayout* l : m_pages)
        l->addStretch();
}

void BrushLab::buildShape()
{
    QVBoxLayout* l = page(Shape);
    QWidget* body = l->parentWidget();
    auto* scroller = qobject_cast<DragScroller*>(m_stack->widget(Shape)->property("scroller").value<QObject*>());
    heading(Shape, tr("Shape source"));
    m_tipPreview = new TexturePreview(body);
    l->addWidget(m_tipPreview);
    m_tipPicker = new TexturePicker(false, body);   // added to the page after its sliders
    m_tipPicker->setObjectName(QStringLiteral("lab-tips"));
    m_tipPicker->setScroller(scroller);
    m_tipPicker->onPicked = [this](const QString& name) { pickTexture(name, false); };
    m_tipPicker->onImport = [this] { importTexture(false); };
    m_invert = new QPushButton(tr("Invert"), body);
    m_invert->setFocusPolicy(Qt::NoFocus);
    m_invert->setToolTip(tr("Swap what is paint and what is not in a shape of your own"));
    connect(m_invert, &QPushButton::clicked, this, [this] {
        const QString flipped = brushtex::inverted(preset().tip);
        if (!flipped.isEmpty())
            change([&](BrushPreset& p) { p.tip = flipped; }, true);
    });

    heading(Shape, tr("Direction"));
    m_follow = new SegmentBar({tr("Stays as set"), tr("Follows the stroke"), tr("Follows the pen")}, body);
    m_follow->setObjectName(QStringLiteral("lab-follow"));
    m_follow->setFixedWidth(kContentWidth);
    m_follow->setToolTip(tr("Whether the shape keeps its angle, turns with the line, or turns with the way the pen leans"));
    l->addWidget(m_follow);
    connect(m_follow, &SegmentBar::currentChanged, this, [this](int index) { change([&](BrushPreset& p) { p.angleSource = BrushPreset::AngleSource(index); }); });
}

void BrushLab::buildGrain()
{
    QVBoxLayout* l = page(Grain);
    QWidget* body = l->parentWidget();
    auto* scroller = qobject_cast<DragScroller*>(m_stack->widget(Grain)->property("scroller").value<QObject*>());
    heading(Grain, tr("Grain source"));
    m_grainPreview = new TexturePreview(body);
    l->addWidget(m_grainPreview);
    m_grainPicker = new TexturePicker(true, body);   // added to the page after its sliders
    m_grainPicker->setObjectName(QStringLiteral("lab-grains"));
    m_grainPicker->setScroller(scroller);
    m_grainPicker->onPicked = [this](const QString& name) { pickTexture(name, true); };
    m_grainPicker->onImport = [this] { importTexture(true); };

    heading(Grain, tr("Grain behaviour"));
    m_grainMoving = new SegmentBar({tr("Fixed to the canvas"), tr("Rolls with each stamp")}, body);
    m_grainMoving->setObjectName(QStringLiteral("lab-grain-moving"));
    m_grainMoving->setFixedWidth(kContentWidth);
    m_grainMoving->setToolTip(tr("Fixed, the texture stays put like real paper; rolling, every stamp carries its own"));
    l->addWidget(m_grainMoving);
    l->addSpacing(6);
    m_grainMode = new SegmentBar({tr("Off"), tr("Even"), tr("Paper tooth"), tr("Edge only")}, body);
    m_grainMode->setObjectName(QStringLiteral("lab-grain-mode"));
    m_grainMode->setFixedWidth(kContentWidth);
    m_grainMode->setToolTip(tr("Even: the texture shows all over. Paper tooth: it fills in as paint builds. Edge only: it frays the soft rim and leaves the core solid."));
    l->addWidget(m_grainMode);
    l->addSpacing(6);
    connect(m_grainMoving, &SegmentBar::currentChanged, this, [this](int index) { change([&](BrushPreset& p) { p.grainMoving = index == 1; }); });
    connect(m_grainMode, &SegmentBar::currentChanged, this, [this](int index) { change([&](BrushPreset& p) { p.grainMode = BrushPreset::GrainMode(index); }); });
}

void BrushLab::buildPen()
{
    QVBoxLayout* l = page(Pen);
    QWidget* body = l->parentWidget();
    heading(Pen, tr("Pressure"));
    m_curve = new CurveEditor(body);
    m_curve->setObjectName(QStringLiteral("lab-pressure-curve"));
    m_curve->setFixedSize(kContentWidth, 300);
    m_curve->setToolTip(tr("From how hard the pen presses (across) to how hard this brush takes it to press (up). "
                           "Drag a point; press on the line for another; pull one well clear to remove it."));
    l->addWidget(m_curve);
    auto* line = new QHBoxLayout;
    line->addWidget(dimLabel(tr("Light touch on the left, full pressure on the right"), body, 11));
    line->addStretch();
    auto* straight = new QPushButton(tr("Straighten"), body);
    straight->setObjectName(QStringLiteral("lab-curve-straight"));
    straight->setFocusPolicy(Qt::NoFocus);
    straight->setToolTip(tr("Back to a straight line: the brush takes the pressure as it comes"));
    line->addWidget(straight);
    l->addLayout(line);
    l->addSpacing(4);
    connect(m_curve, &CurveEditor::changed, this, [this] {
        change([&](BrushPreset& p) {
            p.pressureCurve = m_curve->points();
            p.pressureGamma = 1.0f;   // the curve says all of it now
        });
    });
    connect(straight, &QPushButton::clicked, this, [this] {
        change([](BrushPreset& p) {
            p.pressureCurve.clear();
            p.pressureGamma = 1.0f;
        }, true);
    });
}

void BrushLab::buildAbout()
{
    QVBoxLayout* l = page(About);
    QWidget* body = l->parentWidget();
    m_aboutPreview = new QLabel(body);
    m_aboutPreview->setFixedSize(kContentWidth, 110);
    m_aboutPreview->setStyleSheet(QStringLiteral("background: #141416; border-radius: 12px;"));
    l->addWidget(m_aboutPreview);
    l->addSpacing(10);
    l->addWidget(dimLabel(tr("Name"), body));
    m_name = new QLineEdit(body);
    m_name->setObjectName(QStringLiteral("lab-name"));
    m_name->setFont(theme::font(15, QFont::DemiBold));
    m_name->setMaxLength(60);
    m_name->setFixedWidth(kContentWidth);
    l->addWidget(m_name);
    m_set = dimLabel(QString(), body);
    m_set->setWordWrap(true);
    m_set->setFixedWidth(kContentWidth);
    l->addWidget(m_set);
    l->addSpacing(14);
    auto* buttons = new QHBoxLayout;
    m_duplicate = new QPushButton(tr("Duplicate"), body);
    m_duplicate->setObjectName(QStringLiteral("lab-duplicate"));
    m_duplicate->setFocusPolicy(Qt::NoFocus);
    m_duplicate->setToolTip(tr("Make a copy of this brush that is yours to rename and reshape"));
    buttons->addWidget(m_duplicate);
    m_reset = new QPushButton(tr("Reset to How It Shipped"), body);
    m_reset->setObjectName(QStringLiteral("lab-reset"));
    m_reset->setFocusPolicy(Qt::NoFocus);
    buttons->addWidget(m_reset);
    m_delete = new QPushButton(tr("Delete"), body);
    m_delete->setObjectName(QStringLiteral("lab-delete"));
    m_delete->setFocusPolicy(Qt::NoFocus);
    buttons->addWidget(m_delete);
    buttons->addStretch();
    l->addLayout(buttons);

    connect(m_duplicate, &QPushButton::clicked, this, &BrushLab::duplicateRequested);
    connect(m_reset, &QPushButton::clicked, this, [this] {
        BrushLibrary::instance().reset(m_id);
        m_applying = true;
        m_tools->notifyBrushEdited();
        m_padTools->notifyBrushEdited();
        m_applying = false;
        load();
        m_redraw.start();
    });
    // Deleting cannot be undone, so it takes a second press to mean it.
    connect(m_delete, &QPushButton::clicked, this, [this] {
        if (!m_deleteArmed) {
            armDelete(true);
            return;
        }
        armDelete(false);
        emit deleteRequested();
    });
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        BrushLibrary& lib = BrushLibrary::instance();
        const BrushPreset& p = preset();
        if (p.custom && m_name->text().trimmed() != p.name && lib.rename(p.id, m_name->text())) {
            lib.save();
            m_applying = true;
            m_tools->notifyBrushEdited();
            m_applying = false;
        }
        m_name->setText(preset().name);
        m_name->clearFocus();
        m_brushName->setText(preset().name);
    });
}

QWidget* BrushLab::buildPadSide()
{
    auto* side = new QWidget(this);
    auto* l = new QVBoxLayout(side);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto* header = new QWidget(side);
    header->setFixedHeight(kHeaderHeight);
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(16, 0, 16, 0);
    hl->setSpacing(8);
    m_padMenu = new IconButton(QString(), header);
    m_padMenu->setText(tr("Drawing Pad"));
    m_padMenu->setObjectName(QStringLiteral("lab-pad-menu"));
    m_padMenu->setToolTip(tr("Clear the pad, or try the brush in your own colour on your canvas's paper"));
    hl->addWidget(m_padMenu);
    auto* clear = new IconButton(QString(), header);
    clear->setText(tr("Clear"));
    clear->setObjectName(QStringLiteral("lab-pad-clear"));
    clear->setToolTip(tr("Wipe the pad"));
    hl->addWidget(clear);
    hl->addSpacing(12);
    hl->addWidget(dimLabel(tr("Size"), header));
    m_padSize = new Slider(header);
    m_padSize->setObjectName(QStringLiteral("lab-pad-size"));
    m_padSize->setFixedWidth(170);
    m_padSize->setToolTip(tr("The size to try the brush at"));
    hl->addWidget(m_padSize);
    m_padSizeValue = new QLabel(header);
    m_padSizeValue->setFont(theme::font(12, QFont::Medium));
    m_padSizeValue->setFixedWidth(60);
    hl->addWidget(m_padSizeValue);
    hl->addStretch();
    auto* cancel = new QPushButton(tr("Cancel"), header);
    cancel->setObjectName(QStringLiteral("lab-cancel"));
    cancel->setFocusPolicy(Qt::NoFocus);
    cancel->setToolTip(tr("Put the brush back as it was when the lab opened"));
    hl->addWidget(cancel);
    auto* done = new QPushButton(tr("Done"), header);
    done->setObjectName(QStringLiteral("lab-done"));
    done->setFocusPolicy(Qt::NoFocus);
    done->setDefault(true);
    done->setToolTip(tr("Keep the brush as it is now (Esc)"));
    hl->addWidget(done);
    l->addWidget(header);

    m_padHolder = new QWidget(side);
    auto* grid = new QGridLayout(m_padHolder);
    grid->setContentsMargins(0, 0, 0, 0);
    m_padHint = new QLabel(tr("Draw here to try the brush"), m_padHolder);
    m_padHint->setFont(theme::font(15));
    m_padHint->setStyleSheet(QStringLiteral("color: rgba(160, 160, 168, 150); background: transparent;"));
    m_padHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    grid->addWidget(m_padHint, 0, 0, Qt::AlignCenter);
    l->addWidget(m_padHolder, 1);

    connect(m_padMenu, &QAbstractButton::clicked, this, &BrushLab::showPadMenu);
    connect(clear, &QAbstractButton::clicked, this, &BrushLab::clearPad);
    connect(cancel, &QPushButton::clicked, this, &BrushLab::cancel);
    connect(done, &QPushButton::clicked, this, &BrushLab::done);
    connect(m_padSize, &Slider::valueChanged, this, [this](float t) { m_padTools->setSizeFraction(t); });
    connect(m_padTools, &ToolState::sizeChanged, this, [this] {
        m_padSize->setValue(m_padTools->sizeFraction());
        const float px = m_padTools->size();
        m_padSizeValue->setText(tr("%1 px").arg(QString::number(px, 'f', px < 10.0f ? 1 : 0)));
    });
    return side;
}

// ------------------------------------------------------------------ the pad

bool BrushLab::ensurePad()
{
    if (m_pad)
        return true;
    m_padEngine = new CanvasEngine(this);
    QString error;
    if (!m_padEngine->initialize(&error)) {
        delete m_padEngine;
        m_padEngine = nullptr;
        m_padHint->setText(tr("The drawing pad could not be started.\n%1").arg(error));
        return false;
    }
    m_padEngine->setStrokeJournal(true);
    m_pad = new CanvasWidget(m_padEngine, m_padTools, m_padHolder);
    m_pad->setObjectName(QStringLiteral("lab-pad"));
    m_pad->setPadMode(true);
    m_pad->setQuickShape(false, 600);   // a pause on the pad is a pause, not a shape
    static_cast<QGridLayout*>(m_padHolder->layout())->addWidget(m_pad, 0, 0);
    m_padHint->raise();
    m_pad->installEventFilter(this);
    connect(m_padEngine, &CanvasEngine::layersChanged, this, [this] { m_padHint->setVisible(m_padEngine->journalSize() == 0); });
    return true;
}

// The pad's canvas is as large as the pad is on screen, pixel for pixel.
void BrushLab::syncPadDocument(bool force)
{
    if (!m_pad)
        return;
    const QSize px = (QSizeF(m_pad->size()) * m_pad->devicePixelRatioF()).toSize();
    if (px.width() < 32 || px.height() < 32 || (!force && px == m_padEngine->canvasSize()))
        return;
    const StrokeTool tool = m_padTools->tool();
    m_padEngine->newDocument(px, m_padCanvasColours ? m_canvasPaper : kPadPaper);
    m_padEngine->setJournalGround(tool == StrokeTool::Brush ? QImage() : padGround(px));
    m_padEngine->redrawJournal(preset());
    m_padHint->setVisible(m_padEngine->journalSize() == 0);
}

void BrushLab::redrawPad()
{
    refreshPreview();
    if (m_pad)
        m_padEngine->redrawJournal(preset());
}

void BrushLab::redrawPadNow()
{
    m_redraw.stop();
    redrawPad();
}

void BrushLab::clearPad()
{
    if (!m_pad)
        return;
    m_padEngine->clearJournal();
    m_padEngine->redrawJournal(preset());
    m_padHint->setVisible(true);
}

void BrushLab::setPadCanvasColours(bool on)
{
    m_padCanvasColours = on;
    m_padTools->setColor(on ? m_tools->color() : QColor(Qt::white));
    if (m_pad) {
        m_padEngine->clearJournal();   // what was drawn was drawn in the other colours
        syncPadDocument(true);
    }
}

void BrushLab::showPadMenu()
{
    QMenu menu(window());
    menu.addAction(tr("Clear the Pad"), this, &BrushLab::clearPad);
    menu.addSeparator();
    QAction* dark = menu.addAction(tr("White on Dark"), this, [this] { setPadCanvasColours(false); });
    dark->setCheckable(true);
    dark->setChecked(!m_padCanvasColours);
    QAction* own = menu.addAction(tr("My Colour on My Canvas's Paper"), this, [this] { setPadCanvasColours(true); });
    own->setCheckable(true);
    own->setChecked(m_padCanvasColours);
    menu.exec(m_padMenu->mapToGlobal(QPoint(0, m_padMenu->height() + 4)));
}

bool BrushLab::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_pad && e->type() == QEvent::Resize && isVisible())
        m_resizePad.start();
    return QWidget::eventFilter(obj, e);
}

// --------------------------------------------------------- opening, closing

void BrushLab::open(const QVector<QPointF>& penCurve)
{
    const bool pad = ensurePad();
    m_padTools->setTool(m_tools->tool());
    if (pad)
        m_pad->setPressureCurve(penCurve);
    m_padTools->setColor(m_padCanvasColours ? m_tools->color() : QColor(Qt::white));
    adopt(m_tools->preset().id);
    if (pad)
        m_padEngine->clearJournal();
    show();
    raise();
    if (pad) {
        // Once the lab has been laid out and the pad knows its size.
        syncPadDocument(true);
        QTimer::singleShot(0, this, [this] { syncPadDocument(false); });
        m_pad->setFocus();
    }
}

// Makes `brushId` the brush being worked on: this is the state Cancel returns to.
void BrushLab::adopt(const QString& brushId)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    m_id = brushId;
    m_before = preset();
    m_wasModified = lib.isModified(m_id);
    m_padTools->setPreset(m_id);
    m_padTools->setSize(m_tools->size());
    m_padTools->setOpacity(m_tools->opacity());
    emit m_padTools->sizeChanged();   // the readout, even where the size is the same as before
    load();
    m_redraw.start();
}

void BrushLab::done()
{
    BrushLibrary::instance().save();
    if (m_tools->preset().id == m_id)
        m_tools->setSize(m_padTools->size());   // the size tried on the pad is the one to go on with
    finish();
}

void BrushLab::cancel()
{
    BrushLibrary& lib = BrushLibrary::instance();
    if (lib.find(m_id)) {
        if (!m_before.custom && !m_wasModified)
            lib.reset(m_id);
        else
            lib.update(m_before);
        if (m_before.custom && preset().name != m_before.name)
            lib.rename(m_id, m_before.name);
        lib.save();
        m_applying = true;
        m_tools->notifyBrushEdited();
        m_applying = false;
    }
    finish();
}

void BrushLab::finish()
{
    if (!isVisible())
        return;
    m_redraw.stop();
    armDelete(false);
    hide();
    emit closed();
}

void BrushLab::armDelete(bool armed)
{
    m_deleteArmed = armed;
    m_delete->setText(armed ? tr("Really delete?") : tr("Delete"));
    m_delete->setToolTip(armed ? tr("Press again to delete this brush for good") : tr("Delete this brush"));
    m_delete->setStyleSheet(armed ? QStringLiteral("QPushButton { background: #c9342c; color: white; }") : QString());
    if (armed)
        m_disarm.start();
    else
        m_disarm.stop();
}

int BrushLab::section() const
{
    return m_sections->current();
}

void BrushLab::setSection(int section)
{
    section = std::clamp(section, 0, int(SectionCount) - 1);
    m_sections->setCurrent(section);
    m_stack->setCurrentIndex(section);
    m_pageTitle->setText(tr(kSections[section].title));
}

QWidget* BrushLab::tipPicker() const
{
    return m_tipPicker;
}

QRect BrushLab::tipTileRect(const QString& name) const
{
    for (int i = 0; i < m_tipPicker->count(); ++i) {
        if (m_tipPicker->nameAt(i) == name)
            return m_tipPicker->cellRect(i);
    }
    return {};
}

QWidget* BrushLab::sectionList() const
{
    return m_sections;
}

QRect BrushLab::sectionRect(int section) const
{
    return m_sections->rowRect(section);
}

// ---------------------------------------------------------------- settings

void BrushLab::load()
{
    const BrushPreset& p = preset();
    const BrushLibrary& lib = BrushLibrary::instance();
    m_brushName->setText(m_brushName->fontMetrics().elidedText(p.name, Qt::ElideRight, kLeftWidth - 30));
    const auto& table = rows();
    for (size_t i = 0; i < table.size(); ++i) {
        const Row& r = table[i];
        const float t = std::clamp((p.*r.member - r.min) / (r.max - r.min), 0.0f, 1.0f);
        m_sliders[i]->setValue(std::pow(t, 1.0f / r.curve));
        m_values[i]->setText(format(r, p.*r.member));
        // Hardness belongs to the round tip; depth and scale to a grain that is on.
        if (r.member == &BrushPreset::hardness)
            m_sliders[i]->parentWidget()->setEnabled(p.tip.isEmpty());
        if (r.section == Grain)
            m_sliders[i]->parentWidget()->setEnabled(!p.grain.isEmpty() && p.grainMode != BrushPreset::GrainMode::Off);
    }
    m_count->setValue(float(p.count - 1) / 15.0f);
    m_countValue->setText(QString::number(p.count));

    // The pictures to choose from: the round tip or no grain, what ships, then what was brought in.
    const QVector<brushtex::Entry> imported = brushtex::list();
    auto items = [&](bool grain, const QString& current) {
        QVector<TexturePicker::Item> out;
        out.push_back({QString(), grain ? tr("None") : tr("Round")});
        for (const QString& name : grain ? texgen::grainNames() : texgen::tipNames())
            out.push_back({name, grain ? texgen::grainTitle(name) : texgen::tipTitle(name)});
        bool found = current.isEmpty() || (grain ? texgen::grainNames() : texgen::tipNames()).contains(current);
        for (const brushtex::Entry& e : imported) {
            if (e.grain != grain)
                continue;
            out.push_back({e.name, e.label});
            found = found || e.name == current;
        }
        if (!found)
            out.push_back({current, tr("Missing")});
        return out;
    };
    m_tipPicker->setItems(items(false, p.tip));
    m_tipPicker->setCurrent(p.tip);
    m_grainPicker->setItems(items(true, p.grain));
    m_grainPicker->setCurrent(p.grain);
    m_tipPreview->setImage(p.tip.isEmpty() ? roundTip(p.hardness, p.roundness, p.angle) : textureImage(p.tip, false),
                           p.tip.isEmpty() ? tr("Round") : brushtex::isUser(p.tip) ? tr("Your own shape") : texgen::tipTitle(p.tip));
    m_grainPreview->setImage(textureImage(p.grain, true),
                             p.grain.isEmpty() ? QString() : brushtex::isUser(p.grain) ? tr("Your own grain") : texgen::grainTitle(p.grain));
    m_invert->setEnabled(brushtex::isUser(p.tip));
    m_follow->setCurrent(int(p.angleSource));
    m_buildup->setCurrent(int(p.buildup));
    {
        const QSignalBlocker quiet(m_blend);
        m_blend->setCurrentIndex(int(p.blend));
    }
    m_grainMode->setCurrent(int(p.grainMode));
    m_grainMoving->setCurrent(p.grainMoving ? 1 : 0);

    // The curve the brush has; or, for a brush that only says how firm it is, that as a curve.
    QVector<QPointF> curve = p.pressureCurve;
    if (curve.size() < 2) {
        for (int i = 0; i <= 4; ++i)
            curve.push_back(QPointF(i / 4.0, std::pow(i / 4.0, double(p.pressureGamma))));
    }
    m_curve->setPoints(curve);

    m_name->setText(p.name);
    m_name->setReadOnly(!p.custom);
    m_name->setToolTip(p.custom ? tr("The name of your brush") : tr("A shipped brush keeps its name; duplicate it to make it yours"));
    m_set->setText(p.custom ? tr("One of your own brushes, in the set “%1”.").arg(p.category)
                            : tr("A brush that came with Impasto, in the set “%1”. Changes to it are kept, and can always be undone with the reset.")
                                  .arg(p.category));
    m_reset->setVisible(!p.custom);
    m_reset->setEnabled(lib.isModified(p.id));
    m_delete->setVisible(p.custom);
    refreshPreview();
}

void BrushLab::refreshPreview()
{
    const qreal dpr = devicePixelRatioF();
    for (QLabel* label : {m_strokePreview, m_aboutPreview}) {
        const QSize size(std::max(64, label->width() - 12), label->height() - 8);
        QImage img = m_engine->brushPreview(preset(), size * dpr, Qt::white, float(dpr));
        img.setDevicePixelRatio(dpr);
        label->setAlignment(Qt::AlignCenter);
        label->setPixmap(QPixmap::fromImage(img));
    }
    m_reset->setEnabled(BrushLibrary::instance().isModified(m_id));
}

// Applies an edit to the brush and brings everything that shows it up to date.
void BrushLab::change(const std::function<void(BrushPreset&)>& edit, bool reload)
{
    BrushLibrary& lib = BrushLibrary::instance();
    if (!lib.find(m_id))
        return;
    BrushPreset p = preset();
    edit(p);
    lib.update(p);
    m_applying = true;
    m_tools->notifyBrushEdited();
    m_padTools->notifyBrushEdited();
    m_applying = false;
    if (reload)
        load();
    m_redraw.start();
}

void BrushLab::edited(int index, float t)
{
    const Row& r = rows()[size_t(index)];
    bool pushed = false;
    change([&](BrushPreset& p) {
        p.*r.member = r.min + (r.max - r.min) * std::pow(t, r.curve);
        // The two ends of the size range push each other rather than cross.
        if (r.member == &BrushPreset::minSize && p.maxSize < p.minSize) {
            p.maxSize = p.minSize;
            pushed = true;
        } else if (r.member == &BrushPreset::maxSize && p.minSize > p.maxSize) {
            p.minSize = p.maxSize;
            pushed = true;
        }
        if (r.member == &BrushPreset::minSize || r.member == &BrushPreset::maxSize)
            p.size = std::clamp(p.size, p.minSize, p.maxSize);
    });
    const BrushPreset& p = preset();
    m_values[size_t(index)]->setText(format(r, p.*r.member));
    // The picture of the round tip follows its hardness, roundness and angle.
    if (r.section == Shape && p.tip.isEmpty())
        m_tipPreview->setImage(roundTip(p.hardness, p.roundness, p.angle), tr("Round"));
    if (pushed)
        load();
}

void BrushLab::pickTexture(const QString& name, bool grain)
{
    change([&](BrushPreset& p) {
        if (!grain) {
            p.tip = name;
            return;
        }
        p.grain = name;
        // A texture that is switched off would look like nothing happened.
        if (name.isEmpty())
            p.grainMode = BrushPreset::GrainMode::Off;
        else if (p.grainMode == BrushPreset::GrainMode::Off)
            p.grainMode = BrushPreset::GrainMode::Multiply;
    }, true);
}

void BrushLab::importTexture(bool grain)
{
    const QString path = QFileDialog::getOpenFileName(window(), grain ? tr("Import a grain") : tr("Import a tip shape"), QString(),
                                                      tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)"));
    if (path.isEmpty())
        return;
    QString error;
    const QString name = grain ? brushtex::importGrain(path, &error) : brushtex::importTip(path, &error);
    if (name.isEmpty()) {
        if (!error.isEmpty())
            QMessageBox::warning(window(), tr("Import"), error);
        return;
    }
    pickTexture(name, grain);
}

void BrushLab::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kLabBackground);
    p.fillRect(QRect(0, 0, kLeftWidth, height()), kLabSide);
    p.fillRect(QRect(kLeftWidth + kMiddleWidth, 0, width(), kHeaderHeight), kLabSide);
    p.fillRect(QRect(kLeftWidth + kMiddleWidth, kHeaderHeight, width(), height()), kPadPaper);
    p.setPen(QColor(255, 255, 255, 18));
    p.drawLine(kLeftWidth, 0, kLeftWidth, height());
    p.drawLine(kLeftWidth + kMiddleWidth, 0, kLeftWidth + kMiddleWidth, height());
}

void BrushLab::mousePressEvent(QMouseEvent* e)
{
    e->accept();   // nothing behind the lab is to be reached through it
}
