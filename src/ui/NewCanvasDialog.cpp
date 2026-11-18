#include "ui/NewCanvasDialog.h"

#include "engine/CanvasEngine.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kSmallestSide = 16;

QString trimmed(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0')))
            s.chop(1);
        if (s.endsWith(QLatin1Char('.')))
            s.chop(1);
    }
    return s;
}

QLabel* heading(const QString& text, QWidget* parent)
{
    auto* h = new QLabel(text, parent);
    h->setFont(theme::font(12, QFont::DemiBold));
    h->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    return h;
}

// A sheet of `size` drawn as large as it fits in `box`.
QRectF fitted(const QSize& size, const QRectF& box)
{
    if (size.isEmpty())
        return box;
    const double s = std::min(box.width() / size.width(), box.height() / size.height());
    const QSizeF in(std::max(2.0, size.width() * s), std::max(2.0, size.height() * s));
    return QRectF(box.center().x() - in.width() / 2, box.center().y() - in.height() / 2, in.width(), in.height());
}

const char* kPrimaryStyle =
    "QPushButton { background: #0a84ff; color: white; border-radius: 9px; padding: 9px 22px; font-size: 14px; font-weight: 600; }"
    "QPushButton:hover { background: #2b95ff; }"
    "QPushButton:pressed { background: #0a6fd6; }"
    "QPushButton:disabled { background: #3a3a3e; color: #77777d; }";

const char* kSecondaryStyle =
    "QPushButton { background: #3a3a3e; color: #f2f2f7; border-radius: 9px; padding: 9px 22px; font-size: 14px; }"
    "QPushButton:hover { background: #46464b; }"
    "QPushButton:pressed { background: #505056; }";

} // namespace

namespace newcanvas {

// The sizes to start from, under headings, each with a little picture of its shape.
class PresetList : public QWidget {
public:
    struct Row {
        QString heading;   // a heading row when set
        int preset = -1;
        QString name, detail;
        QSize size;
    };
    std::function<void(int)> picked;

    explicit PresetList(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setRows(const QVector<Row>& rows)
    {
        m_rows = rows;
        setFixedHeight(top(int(m_rows.size())) + 4);
        update();
    }
    void setCurrent(int preset)
    {
        if (preset == m_current)
            return;
        m_current = preset;
        update();
    }
    void setTurned(bool turned)   // the chosen one is shown the other way up
    {
        m_turned = turned;
        update();
    }
    QRect presetRect(int preset) const
    {
        for (int i = 0; i < m_rows.size(); ++i)
            if (m_rows[i].heading.isEmpty() && m_rows[i].preset == preset)
                return rowRect(i);
        return {};
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < m_rows.size(); ++i) {
            const Row& row = m_rows[i];
            const QRect r = rowRect(i);
            if (!row.heading.isEmpty()) {
                p.setFont(theme::font(11, QFont::DemiBold));
                p.setPen(QColor(0x80, 0x80, 0x88));
                p.drawText(r.adjusted(10, 0, 0, -4), Qt::AlignLeft | Qt::AlignBottom, row.heading.toUpper());
                continue;
            }
            const bool chosen = row.preset == m_current, hover = i == m_hover;
            if (chosen || hover) {
                p.setPen(Qt::NoPen);
                p.setBrush(chosen ? theme::row : QColor(255, 255, 255, 14));
                p.drawRoundedRect(QRectF(r).adjusted(0, 1, 0, -1), 9, 9);
            }
            // Its shape, in a square of its own.
            const QSize shape = chosen && m_turned ? row.size.transposed() : row.size;
            const QRectF glyph = fitted(shape, QRectF(r.left() + 12, r.top() + 11, 24, 24));
            p.setBrush(chosen ? QBrush(QColor(0x0a, 0x84, 0xff, 50)) : QBrush(Qt::NoBrush));
            p.setPen(QPen(chosen ? theme::accent : QColor(0x8a, 0x8a, 0x92), 1.4));
            p.drawRoundedRect(glyph.adjusted(0.7, 0.7, -0.7, -0.7), 2, 2);

            p.setPen(theme::text);
            p.setFont(theme::font(13, chosen ? QFont::DemiBold : QFont::Normal));
            p.drawText(QRect(r.left() + 48, r.top() + 5, r.width() - 56, 20), Qt::AlignLeft | Qt::AlignVCenter, row.name);
            p.setPen(theme::textDim);
            p.setFont(theme::font(11));
            p.drawText(QRect(r.left() + 48, r.top() + 23, r.width() - 56, 18), Qt::AlignLeft | Qt::AlignVCenter, row.detail);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        const int i = rowAt(e->position().toPoint());
        if (i >= 0 && m_rows[i].heading.isEmpty() && picked)
            picked(m_rows[i].preset);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int i = rowAt(e->position().toPoint());
        const int h = i >= 0 && m_rows[i].heading.isEmpty() ? i : -1;
        if (h != m_hover) {
            m_hover = h;
            update();
        }
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = -1;
        update();
    }

private:
    static constexpr int kHeading = 28, kRow = 46;
    int top(int index) const
    {
        int y = 0;
        for (int i = 0; i < index && i < m_rows.size(); ++i)
            y += m_rows[i].heading.isEmpty() ? kRow : kHeading + (i == 0 ? 0 : 6);
        return y;
    }
    QRect rowRect(int i) const
    {
        const int extra = !m_rows[i].heading.isEmpty() && i > 0 ? 6 : 0;
        return QRect(0, top(i) + extra, width(), m_rows[i].heading.isEmpty() ? kRow : kHeading);
    }
    int rowAt(const QPoint& pos) const
    {
        for (int i = 0; i < m_rows.size(); ++i)
            if (rowRect(i).contains(pos))
                return i;
        return -1;
    }
    QVector<Row> m_rows;
    int m_current = -1, m_hover = -1;
    bool m_turned = false;
};

// The sheet, its way up and its background, with its sides measured.
class Preview : public QWidget {
public:
    explicit Preview(QWidget* parent) : QWidget(parent) { setFixedSize(320, 270); }
    void set(const QSize& px, NewCanvasDialog::Background background, const QColor& colour)
    {
        m_px = px;
        m_background = background;
        m_colour = colour;
        update();
    }
    QRectF sheetRect() const { return fitted(m_px, QRectF(rect()).adjusted(34, 26, -46, -40)); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(theme::panelBorder, 1));
        p.setBrush(QColor(0x1f, 0x1f, 0x22));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);

        const QRectF sheet = sheetRect();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawRoundedRect(sheet.translated(0, 3).adjusted(-1, -1, 1, 1), 3, 3);
        if (m_background == NewCanvasDialog::Transparent) {
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(sheet, 2, 2);
            p.setClipPath(clip);
            theme::drawChecker(p, sheet, 8);
            p.restore();
        } else {
            p.setBrush(m_background == NewCanvasDialog::CurrentColour ? m_colour : QColor(Qt::white));
            p.drawRoundedRect(sheet, 2, 2);
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 50), 1));
        p.drawRoundedRect(sheet.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);

        // The sides, measured along them.
        p.setPen(theme::textDim);
        p.setFont(theme::font(11, QFont::Medium));
        p.drawText(QRectF(sheet.left() - 40, sheet.bottom() + 6, sheet.width() + 80, 18), Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("%1 px").arg(m_px.width()));
        p.save();
        p.translate(sheet.right() + 8, sheet.center().y());
        p.rotate(90);
        p.drawText(QRectF(-80, -18, 160, 18), Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("%1 px").arg(m_px.height()));
        p.restore();
    }

private:
    QSize m_px;
    NewCanvasDialog::Background m_background = NewCanvasDialog::White;
    QColor m_colour = Qt::white;
};

// The bracket from the width to the height, with a lock in it: closed, the
// two keep their proportion.
class ShapeLock : public QAbstractButton {
public:
    explicit ShapeLock(QWidget* parent) : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedWidth(34);
        setMinimumHeight(84);   // the two fields and the space between them
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor c = isChecked() ? theme::accent : QColor(0x6a, 0x6a, 0x72);
        const qreal mx = width() / 2.0, cy = height() / 2.0;
        const qreal top = 19, bottom = height() - 19;   // the middles of the two fields
        p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF({QPointF(2, top), QPointF(mx, top), QPointF(mx, cy - 15)}));
        p.drawPolyline(QPolygonF({QPointF(mx, cy + 15), QPointF(mx, bottom), QPointF(2, bottom)}));
        const QRectF knob(mx - 14, cy - 14, 28, 28);
        p.setPen(Qt::NoPen);
        p.setBrush(isChecked() ? QColor(0x0a, 0x84, 0xff, 46) : isDown() ? QColor(255, 255, 255, 40) : QColor(0x1f, 0x1f, 0x22));
        p.drawRoundedRect(knob, 8, 8);
        const QPixmap pm = theme::icon(isChecked() ? QStringLiteral("lock") : QStringLiteral("unlock"), 18, isChecked() ? theme::accent : theme::text,
                                       devicePixelRatioF());
        p.drawPixmap(QPointF(mx - 9, cy - 9), pm);
    }
};

} // namespace newcanvas

using newcanvas::PresetList;
using newcanvas::Preview;
using newcanvas::ShapeLock;

double NewCanvasDialog::pixelsPerUnit(Unit unit, int dpi)
{
    switch (unit) {
    case Millimetres: return dpi / 25.4;
    case Centimetres: return dpi / 2.54;
    case Inches: return dpi;
    case Pixels: break;
    }
    return 1.0;
}

int NewCanvasDialog::finestResolution(double widthInches, double heightInches)
{
    if (widthInches <= 0.0 || heightInches <= 0.0)
        return 0;
    const double byArea = std::sqrt(double(CanvasEngine::maxCanvasPixels()) / (widthInches * heightInches));
    const double bySide = double(CanvasEngine::maxCanvasDimension()) / std::max(widthInches, heightInches);
    return int(std::floor(std::min(byArea, bySide)));
}

double NewCanvasDialog::aspectRatio(int aspect)
{
    switch (aspect) {
    case Square: return 1.0;
    case FourThree: return 4.0 / 3.0;
    case ThreeTwo: return 1.5;
    case SixteenNine: return 16.0 / 9.0;
    case Paper: return std::sqrt(2.0);
    default: break;
    }
    return 0.0;
}

NewCanvasDialog::NewCanvasDialog(const QSize& screenPixels, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("New Canvas"));
    setObjectName(QStringLiteral("new-canvas"));
    // The largest square this card holds, so the limit can be seen and picked rather than found by trying.
    const int largest = std::min(CanvasEngine::maxCanvasDimension(), int(std::floor(std::sqrt(double(CanvasEngine::maxCanvasPixels())))));
    const QString screen = tr("Screen"), square = tr("Square"), print = tr("Print"), limit = tr("This graphics card");
    m_presets = {
        {tr("Screen size"), screenPixels, 0, Pixels, screen},
        {tr("HD"), QSize(1920, 1080), 0, Pixels, screen},
        {tr("4K"), QSize(3840, 2160), 0, Pixels, screen},
        {tr("Square"), QSize(2048, 2048), 0, Pixels, square},
        {tr("Large Square"), QSize(4096, 4096), 0, Pixels, square},
        {tr("A4"), QSize(2480, 3508), 300, Millimetres, print},
        {tr("A3"), QSize(3508, 4961), 300, Millimetres, print},
        {tr("A2"), QSize(4961, 7016), 300, Millimetres, print},
        {tr("A1"), QSize(7016, 9933), 300, Millimetres, print},
        {tr("Poster 50 × 70 cm"), QSize(5906, 8268), 300, Centimetres, print},
        {tr("1 m square"), QSize(5906, 5906), 150, Centimetres, print},
        {tr("Comic page"), QSize(4125, 6262), 0, Pixels, print},
        {tr("Largest square"), QSize(largest, largest), 0, Pixels, limit},
    };

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 18);
    outer->setSpacing(16);
    auto* columns = new QHBoxLayout;
    columns->setSpacing(22);
    outer->addLayout(columns, 1);

    // ---- Sizes to start from.
    auto* left = new QVBoxLayout;
    left->setSpacing(8);
    left->addWidget(heading(tr("Start from"), this));
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("canvas-presets-scroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(250);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    m_list = new PresetList(scroll);
    m_list->setObjectName(QStringLiteral("canvas-presets"));
    QVector<PresetList::Row> rows;
    QString group;
    for (int i = 0; i < m_presets.size(); ++i) {
        const Preset& p = m_presets[i];
        if (p.group != group) {
            group = p.group;
            rows.push_back({group});
        }
        QString detail = QStringLiteral("%1 × %2 px").arg(p.size.width()).arg(p.size.height());
        if (p.dpi > 0) {
            const double per = pixelsPerUnit(p.unit, p.dpi);
            const int decimals = p.unit == Millimetres ? 0 : 1;
            detail = QStringLiteral("%1 × %2 %3 · %4 dpi")
                         .arg(trimmed(p.size.width() / per, decimals), trimmed(p.size.height() / per, decimals),
                              p.unit == Millimetres ? tr("mm") : tr("cm"))
                         .arg(p.dpi);
        }
        rows.push_back({QString(), i, p.name, detail, p.size});
    }
    m_list->setRows(rows);
    m_list->picked = [this](int i) { choosePreset(i); };
    scroll->setWidget(m_list);
    new DragScroller(scroll, m_list);
    left->addWidget(scroll, 1);
    columns->addLayout(left);

    // ---- The size.
    auto* middle = new QVBoxLayout;
    middle->setSpacing(8);
    middle->addWidget(heading(tr("Size"), this));
    auto* sides = new QGridLayout;
    sides->setHorizontalSpacing(4);
    sides->setVerticalSpacing(8);
    m_width = new ValueField(tr("Width"), this);
    m_width->setObjectName(QStringLiteral("canvas-width"));
    m_width->setToolTip(tr("Drag left or right to change it, or tap to type it in"));
    m_height = new ValueField(tr("Height"), this);
    m_height->setObjectName(QStringLiteral("canvas-height"));
    m_height->setToolTip(m_width->toolTip());
    auto* lock = new ShapeLock(this);
    lock->setObjectName(QStringLiteral("canvas-lock"));
    lock->setToolTip(tr("Keep the shape: changing one side changes the other with it"));
    m_lock = lock;
    sides->addWidget(m_width, 0, 0);
    sides->addWidget(m_height, 1, 0);
    sides->addWidget(lock, 0, 1, 2, 1);
    middle->addLayout(sides);

    auto* shapeRow = new QHBoxLayout;
    shapeRow->setSpacing(8);
    auto* shapeLabel = new QLabel(tr("Shape"), this);
    shapeLabel->setFont(theme::font(12));
    shapeLabel->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    shapeRow->addWidget(shapeLabel);
    m_aspect = new SegmentBar({QStringLiteral("1:1"), QStringLiteral("4:3"), QStringLiteral("3:2"), QStringLiteral("16:9"), tr("Paper")}, this);
    m_aspect->setObjectName(QStringLiteral("canvas-aspect"));
    m_aspect->setFixedHeight(32);
    m_aspect->setToolTip(tr("A shape to keep, the way up it is now. Paper is the A sizes' 1 : 1.414."));
    shapeRow->addWidget(m_aspect, 1);
    middle->addLayout(shapeRow);

    middle->addSpacing(6);
    middle->addWidget(heading(tr("Measured in"), this));
    m_unit = new SegmentBar({tr("Pixels"), tr("mm"), tr("cm"), tr("Inches")}, this);
    m_unit->setObjectName(QStringLiteral("canvas-unit"));
    m_unit->setFixedHeight(32);
    middle->addWidget(m_unit);
    m_dpiField = new ValueField(tr("Resolution"), this);
    m_dpiField->setObjectName(QStringLiteral("canvas-dpi"));
    m_dpiField->setRange(30, 1200);
    m_dpiField->setSuffix(tr("dpi"));
    m_dpiField->setDragStep(0.5, 1);
    m_dpiField->setStep(10);
    m_dpiField->setToolTip(tr("Dots per inch: how finely the picture is to be printed. 300 is usual for print seen close up, 150 for posters."));
    middle->addWidget(m_dpiField);

    middle->addSpacing(6);
    middle->addWidget(heading(tr("Background"), this));
    m_background = new SegmentBar({tr("White"), tr("Colour"), tr("Transparent")}, this);
    m_background->setObjectName(QStringLiteral("canvas-background"));
    m_background->setFixedHeight(32);
    m_background->setToolTip(tr("Colour is the colour chosen now"));
    middle->addWidget(m_background);
    middle->addStretch(1);
    auto* hint = new QLabel(tr("Drag a number left or right to change it, or tap it to type one in."), this);
    hint->setWordWrap(true);
    hint->setFont(theme::font(12));
    hint->setStyleSheet(QStringLiteral("color: #77777d;"));
    middle->addWidget(hint);
    auto* middleBox = new QWidget(this);
    middleBox->setLayout(middle);
    middleBox->setFixedWidth(300);
    middle->setContentsMargins(0, 0, 0, 0);
    columns->addWidget(middleBox);

    // ---- The sheet.
    auto* right = new QVBoxLayout;
    right->setSpacing(10);
    right->addWidget(heading(tr("Preview"), this));
    m_preview = new Preview(this);
    m_preview->setObjectName(QStringLiteral("canvas-preview"));
    right->addWidget(m_preview);
    auto* turn = new QHBoxLayout;
    turn->setSpacing(6);
    m_pixels = new QLabel(this);
    m_pixels->setObjectName(QStringLiteral("canvas-pixels"));
    m_pixels->setFont(theme::font(15, QFont::DemiBold));
    turn->addWidget(m_pixels, 1);
    m_portrait = new IconButton(QStringLiteral("portrait"), this, 22);
    m_portrait->setObjectName(QStringLiteral("canvas-portrait"));
    m_portrait->setToolTip(tr("Portrait"));
    m_portrait->setFixedSize(40, 36);
    m_landscape = new IconButton(QStringLiteral("landscape"), this, 22);
    m_landscape->setObjectName(QStringLiteral("canvas-landscape"));
    m_landscape->setToolTip(tr("Landscape"));
    m_landscape->setFixedSize(40, 36);
    turn->addWidget(m_portrait);
    turn->addWidget(m_landscape);
    right->addLayout(turn);
    // Like Procreate's "maximum layers": how much this size leaves room for.
    m_room = new QLabel(this);
    m_room->setObjectName(QStringLiteral("canvas-room"));
    m_room->setWordWrap(true);
    m_room->setFont(theme::font(12));
    m_room->setFixedWidth(320);
    m_room->setMinimumHeight(m_room->fontMetrics().lineSpacing() * 4);
    m_room->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    right->addWidget(m_room);
    right->addStretch(1);
    columns->addLayout(right);

    // ---- Create or not.
    auto* footer = new QHBoxLayout;
    footer->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("canvas-cancel"));
    cancel->setStyleSheet(QLatin1String(kSecondaryStyle));
    cancel->setAutoDefault(false);
    m_create = new QPushButton(tr("Create"), this);
    m_create->setObjectName(QStringLiteral("canvas-create"));
    m_create->setStyleSheet(QLatin1String(kPrimaryStyle));
    m_create->setDefault(true);
    footer->addWidget(cancel);
    footer->addWidget(m_create);
    outer->addLayout(footer);
    connect(m_create, &QPushButton::clicked, this, &NewCanvasDialog::accept);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    QSettings st;
    m_dpi = std::clamp(st.value(QStringLiteral("newCanvas/dpi"), 300).toInt(), 30, 1200);
    m_dpiField->setValue(m_dpi);
    m_unit->setCurrent(std::clamp(st.value(QStringLiteral("newCanvas/unit"), int(Pixels)).toInt(), int(Pixels), int(Inches)));
    m_background->setCurrent(std::clamp(st.value(QStringLiteral("newCanvas/background"), int(White)).toInt(), int(White), int(Transparent)));
    m_px = screenPixels;
    m_paper = QSizeF(m_px) / m_dpi;
    showSize();
    markPreset();
    describe();

    connect(m_width, &ValueField::valueChanged, this, &NewCanvasDialog::widthEdited);
    connect(m_height, &ValueField::valueChanged, this, &NewCanvasDialog::heightEdited);
    connect(lock, &QAbstractButton::toggled, this, [this](bool on) { setKeepShape(on); });
    connect(m_aspect, &SegmentBar::currentChanged, this, &NewCanvasDialog::chooseAspect);
    connect(m_portrait, &QAbstractButton::clicked, this, [this] { setPortrait(true); });
    connect(m_landscape, &QAbstractButton::clicked, this, [this] { setPortrait(false); });
    connect(m_background, &SegmentBar::currentChanged, this, [this] { describe(); });
    // Another unit says the same size differently.
    connect(m_unit, &SegmentBar::currentChanged, this, [this] {
        showSize();
        describe();
    });
    // Another resolution keeps a size on paper and changes the pixels; a size
    // in pixels stays as it is and comes out a different size on paper.
    connect(m_dpiField, &ValueField::valueChanged, this, [this](double v) { setResolution(int(std::lround(v))); });
    m_create->setFocus();
}

NewCanvasDialog::Background NewCanvasDialog::background() const
{
    return Background(std::max(0, m_background->current()));
}

void NewCanvasDialog::setBackground(Background background)
{
    m_background->setCurrent(int(background));
    describe();
}

void NewCanvasDialog::setCurrentColour(const QColor& colour)
{
    m_colour = colour.isValid() ? colour : QColor(Qt::white);
    describe();
}

bool NewCanvasDialog::fits() const
{
    return qint64(m_px.width()) * m_px.height() <= CanvasEngine::maxCanvasPixels() && m_px.width() <= CanvasEngine::maxCanvasDimension()
           && m_px.height() <= CanvasEngine::maxCanvasDimension();
}

NewCanvasDialog::Unit NewCanvasDialog::unit() const
{
    return Unit(std::max(0, m_unit->current()));
}

void NewCanvasDialog::setUnit(Unit unit)
{
    m_unit->setCurrent(int(unit));
    showSize();
    describe();
}

void NewCanvasDialog::setResolution(int dpi)
{
    dpi = std::clamp(dpi, 30, 1200);
    m_dpiField->setValue(dpi);
    if (dpi == m_dpi)
        return;
    m_dpi = dpi;
    if (unit() == Pixels) {
        m_paper = QSizeF(m_px) / m_dpi;
    } else {
        // The sheet as it was given, not as its pixels rounded it.
        m_px = QSize(clampSide(std::llround(m_paper.width() * m_dpi)), clampSide(std::llround(m_paper.height() * m_dpi)));
        if (m_keepShape)
            m_ratio = double(m_px.width()) / m_px.height();
    }
    showSize();   // the range follows the resolution
    sizeChanged();
}

int NewCanvasDialog::clampSide(qint64 px) const
{
    return int(std::clamp<qint64>(px, kSmallestSide, CanvasEngine::maxCanvasDimension()));
}

void NewCanvasDialog::setSize(double width, double height)
{
    const double per = pixelsPerUnit(unit(), m_dpi);
    m_px = QSize(clampSide(std::llround(width * per)), clampSide(std::llround(height * per)));
    m_paper = unit() == Pixels ? QSizeF(m_px) / m_dpi : QSizeF(width, height) * (per / m_dpi);
    if (m_keepShape)
        m_ratio = double(m_px.width()) / m_px.height();
    showSize();
    sizeChanged();
}

void NewCanvasDialog::widthEdited()
{
    const double per = pixelsPerUnit(unit(), m_dpi);
    int w = clampSide(std::llround(m_width->value() * per));
    int h = m_px.height();
    if (m_keepShape) {
        // The other side follows; where it would pass a limit, both stop there.
        h = clampSide(std::llround(w / m_ratio));
        w = clampSide(std::llround(h * m_ratio));
    }
    m_px = QSize(w, h);
    m_paper = QSizeF(unit() == Pixels || w != std::llround(m_width->value() * per) ? w / double(m_dpi) : m_width->value() * per / m_dpi,
                     m_px.height() / double(m_dpi));
    showSize();
    sizeChanged();
}

void NewCanvasDialog::heightEdited()
{
    const double per = pixelsPerUnit(unit(), m_dpi);
    int h = clampSide(std::llround(m_height->value() * per));
    int w = m_px.width();
    if (m_keepShape) {
        w = clampSide(std::llround(h * m_ratio));
        h = clampSide(std::llround(w / m_ratio));
    }
    m_px = QSize(w, h);
    m_paper = QSizeF(m_px.width() / double(m_dpi),
                     unit() == Pixels || h != std::llround(m_height->value() * per) ? h / double(m_dpi) : m_height->value() * per / m_dpi);
    showSize();
    sizeChanged();
}

void NewCanvasDialog::sizeChanged()
{
    markPreset();
    describe();
}

double NewCanvasDialog::shownWidth() const
{
    return m_width->value();
}

double NewCanvasDialog::shownHeight() const
{
    return m_height->value();
}

QString NewCanvasDialog::roomText() const
{
    return m_room->text();
}

int NewCanvasDialog::presetCount() const
{
    return int(m_presets.size());
}

QString NewCanvasDialog::presetText(int index) const
{
    if (index < 0 || index >= int(m_presets.size()))
        return tr("Custom");
    const Preset& p = m_presets[index];
    return QStringLiteral("%1   %2 × %3").arg(p.name).arg(p.size.width()).arg(p.size.height());
}

int NewCanvasDialog::currentPreset() const
{
    return m_preset;
}

void NewCanvasDialog::choosePreset(int index)
{
    if (index < 0 || index >= int(m_presets.size()))
        return;
    finishTyping();
    const Preset& p = m_presets[index];
    // A paper size brings its resolution with it, and shows in the measure it is known by.
    if (p.dpi > 0) {
        m_dpi = p.dpi;
        m_dpiField->setValue(m_dpi);
        m_unit->setCurrent(int(p.unit));
    } else {
        m_unit->setCurrent(int(Pixels));
    }
    m_px = p.size;
    m_paper = QSizeF(m_px) / m_dpi;
    if (m_keepShape)
        m_ratio = double(m_px.width()) / m_px.height();
    showSize();
    sizeChanged();
}

void NewCanvasDialog::setPortrait(bool portrait)
{
    if (m_px.width() != m_px.height() && isPortrait() != portrait)
        swapOrientation();
}

void NewCanvasDialog::swapOrientation()
{
    finishTyping();
    m_px.transpose();
    m_paper.transpose();
    m_ratio = 1.0 / m_ratio;
    showSize();
    sizeChanged();
}

void NewCanvasDialog::setKeepShape(bool keep)
{
    m_keepShape = keep;
    m_ratio = double(m_px.width()) / m_px.height();
    if (m_lock->isChecked() != keep) {
        const QSignalBlocker b(m_lock);
        m_lock->setChecked(keep);
    }
    m_lock->update();
}

void NewCanvasDialog::chooseAspect(int aspect)
{
    const double r = aspectRatio(aspect);
    if (r <= 0.0)
        return;
    finishTyping();
    // The width stays, the way up stays (a square turns landscape, except to
    // paper, which is mostly upright), and the height follows.
    const bool portrait = isPortrait() || (m_px.width() == m_px.height() && aspect == Paper);
    const double ratio = portrait ? 1.0 / r : r;   // width over height
    int w = m_px.width();
    int h = clampSide(std::llround(w / ratio));
    w = clampSide(std::llround(h * ratio));
    m_px = QSize(w, h);
    m_paper = QSizeF(m_px) / m_dpi;
    setKeepShape(true);
    m_ratio = ratio;
    showSize();
    sizeChanged();
}

int NewCanvasDialog::currentAspect() const
{
    const double r = double(std::max(m_px.width(), m_px.height())) / std::max(1, std::min(m_px.width(), m_px.height()));
    for (int a = 0; a < AspectCount; ++a) {
        // Within a pixel's rounding, at this size.
        const double tolerance = 1.0 / std::max(1, std::min(m_px.width(), m_px.height())) + 1e-4;
        if (std::abs(r - aspectRatio(a)) <= aspectRatio(a) * tolerance)
            return a;
    }
    return AspectCount;
}

// Puts the pixel size into the fields in the unit that is showing.
void NewCanvasDialog::showSize()
{
    const Unit u = unit();
    const double per = pixelsPerUnit(u, m_dpi);
    const int decimals = u == Pixels ? 0 : u == Millimetres ? 1 : u == Centimetres ? 2 : 3;
    const QString suffix = u == Pixels ? tr("px") : u == Millimetres ? tr("mm") : u == Centimetres ? tr("cm") : tr("in");
    // A pixel of dragging: 4 px, a millimetre, a tenth of a centimetre or a
    // twentieth of an inch at first, each landing on round numbers.
    const double drag = u == Pixels ? 4.0 : u == Millimetres ? 1.0 : u == Centimetres ? 0.1 : 0.05;
    const double snap = u == Pixels ? 8.0 : u == Millimetres ? 1.0 : u == Centimetres ? 0.1 : 0.05;
    const double step = u == Pixels ? 16.0 : u == Millimetres ? 5.0 : u == Centimetres ? 1.0 : 0.5;
    for (ValueField* f : {m_width, m_height}) {
        f->setDecimals(decimals);
        f->setSuffix(suffix);
        f->setDragStep(drag, snap);
        f->setStep(step);
        f->setRange(kSmallestSide / per, CanvasEngine::maxCanvasDimension() / per);
    }
    m_width->setValue(m_px.width() / per);
    m_height->setValue(m_px.height() / per);
}

void NewCanvasDialog::markPreset()
{
    int found = int(m_presets.size());
    for (int i = 0; i < int(m_presets.size()); ++i) {
        const Preset& p = m_presets[i];
        if ((p.size == m_px || p.size.transposed() == m_px) && (p.dpi == 0 || p.dpi == m_dpi)) {
            found = i;
            break;
        }
    }
    m_preset = found;
    m_list->setCurrent(found < int(m_presets.size()) ? found : -1);
    m_list->setTurned(found < int(m_presets.size()) && m_presets[found].size != m_px);
}

void NewCanvasDialog::describe()
{
    const qint64 pixels = qint64(m_px.width()) * m_px.height();
    const int dpi = m_dpi;
    const QString megapixels = QString::number(double(pixels) / 1e6, 'f', pixels < 10'000'000 ? 1 : 0);
    const double widthInches = double(m_px.width()) / dpi, heightInches = double(m_px.height()) / dpi;
    // The size on paper, when the fields do not show it already.
    const QString onPaper = tr("%1 × %2 cm at %3 dpi").arg(trimmed(widthInches * 2.54, 1), trimmed(heightInches * 2.54, 1)).arg(dpi);
    const QString lead = unit() == Pixels ? onPaper + QStringLiteral(" · ") : QString();
    const int layers = CanvasEngine::layerEstimate(m_px);
    QString text;
    if (!fits()) {
        const int finest = finestResolution(widthInches, heightInches);
        text = tr("%1%2 megapixels: more than this graphics card can hold (the limit here is %3).")
                   .arg(lead, megapixels).arg(CanvasEngine::maxCanvasPixels() / 1'000'000);
        if (finest >= 30)
            text += QLatin1Char(' ') + tr("A sheet this size fits at up to %1 dpi.").arg(finest);
    } else {
        // Where a layer only takes memory for the paint on it, the count is
        // for layers painted from edge to edge: the fewest there is room for.
        if (!CanvasEngine::sparseLayers())
            text = tr("%1%2 megapixels · room for about %3 layers in graphics memory").arg(lead, megapixels).arg(layers);
        else if (layers <= 1)
            text = tr("%1%2 megapixels · room for the paint of about one layer filled edge to edge; layers with less on them take less").arg(lead, megapixels);
        else
            text = tr("%1%2 megapixels · room for about %3 layers filled edge to edge; layers with less on them take less").arg(lead, megapixels).arg(layers);
    }
    m_room->setText(text);
    m_room->setStyleSheet(!fits() || layers < 3 ? QStringLiteral("color: #ff9f0a;") : QStringLiteral("color: #a0a0a8;"));
    m_pixels->setText(QStringLiteral("%1 × %2 px").arg(m_px.width()).arg(m_px.height()));
    m_create->setEnabled(fits());
    m_preview->set(m_px, background(), m_colour);
    m_portrait->setActive(isPortrait());
    m_landscape->setActive(m_px.width() > m_px.height());
    const int aspect = currentAspect();
    m_aspect->setCurrent(aspect < AspectCount ? aspect : -1);
}

void NewCanvasDialog::finishTyping()
{
    for (ValueField* f : {m_width, m_height, m_dpiField})
        f->finishEditing();
}

void NewCanvasDialog::accept()
{
    finishTyping();   // what is still being typed counts
    if (!fits())
        return;   // the line under the preview says why, and what would fit
    QSettings st;
    st.setValue(QStringLiteral("newCanvas/dpi"), m_dpi);
    st.setValue(QStringLiteral("newCanvas/unit"), int(unit()));
    st.setValue(QStringLiteral("newCanvas/background"), int(background()));
    QDialog::accept();
}
