#include "ui/ColorPanel.h"

#include "color/Palette.h"
#include "ui/ColorPickers.h"
#include "ui/PaletteGrid.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace {

constexpr int kWidth = 300;
constexpr int kSide = 14;                              // margin left and right of the content
constexpr int kGridWidth = Palette::kColumns * PaletteGrid::kCell;
constexpr int kCompactRows = 4;                        // of the default palette, before it scrolls
constexpr int kHeaderH = 26;                           // a palette's name in the list
constexpr int kBlockGap = 10;                          // between one palette and the next

QLabel* captionLabel(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setFont(theme::font(11, QFont::Medium));
    l->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    return l;
}

// A caption that can be pressed: the name of the default palette, with an
// arrow that says there are others to choose from.
class CaptionButton : public QAbstractButton {
public:
    explicit CaptionButton(QWidget* parent) : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
    }
    void setCaption(const QString& text)
    {
        setText(text);
        updateGeometry();
        update();
    }
    QSize sizeHint() const override { return {std::min(250, QFontMetrics(theme::font(11, QFont::Medium)).horizontalAdvance(text()) + 20), 20}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QFont f = theme::font(11, QFont::Medium);
        const QFontMetrics fm(f);
        const QColor c = isDown() || underMouse() ? theme::text : theme::textDim;
        const QString shown = fm.elidedText(text(), Qt::ElideRight, width() - 18);
        p.setFont(f);
        p.setPen(c);
        p.drawText(QRect(0, 0, width() - 16, height()), Qt::AlignLeft | Qt::AlignVCenter, shown);
        p.drawPixmap(QPointF(fm.horizontalAdvance(shown) + 4, (height() - 12) / 2.0 + 1),
                     theme::icon(QStringLiteral("chevron-down"), 12, c, devicePixelRatioF()));
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }
};

QString pictureFilter()
{
    return QObject::tr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)");
}

} // namespace

// ------------------------------------------------------------- PaletteList

// Every palette, one under the other: its name, then its swatches. A tap on a
// name makes that palette the default; the dots at the end of the row open
// its menu.
class ColorPanel::PaletteList : public QWidget {
public:
    PaletteList(ColorPanel* panel, ToolState* tools) : QWidget(panel), m_panel(panel), m_tools(tools) { setMouseTracking(true); }

    void setScroller(DragScroller* scroller)
    {
        m_scroller = scroller;
        for (PaletteGrid* g : m_grids)
            g->setScroller(scroller);
    }

    // The grids are kept and handed other palettes as the list changes, so
    // none is ever taken away from under a gesture.
    void relayout()
    {
        const QVector<Palette>& all = PaletteLibrary::instance().palettes();
        while (m_grids.size() < all.size()) {
            auto* g = new PaletteGrid(m_tools, this);
            g->setScroller(m_scroller);
            m_grids.push_back(g);
        }
        m_tops.clear();
        int y = 0;
        for (int i = 0; i < m_grids.size(); ++i) {
            PaletteGrid* g = m_grids[i];
            if (i >= all.size()) {
                g->hide();
                continue;
            }
            m_tops.push_back(y);
            g->setPalette(all[i].id);
            g->refresh();
            g->move(0, y + kHeaderH);
            g->show();
            y += kHeaderH + g->height() + kBlockGap;
        }
        setFixedSize(kGridWidth, std::max(1, y - kBlockGap + 2));
        update();
    }

    QRect headerRect(int index) const
    {
        return index >= 0 && index < m_tops.size() ? QRect(0, m_tops[index], width(), kHeaderH) : QRect();
    }
    QRect moreRect(int index) const
    {
        const QRect h = headerRect(index);
        return h.isNull() ? h : QRect(h.right() - 27, h.top(), 28, h.height());
    }
    PaletteGrid* grid(int index) const { return index >= 0 && index < m_tops.size() ? m_grids[index] : nullptr; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const PaletteLibrary& lib = PaletteLibrary::instance();
        const QVector<Palette>& all = lib.palettes();
        const QFont f = theme::font(13, QFont::Medium);
        const QFontMetrics fm(f);
        p.setFont(f);
        for (int i = 0; i < all.size() && i < m_tops.size(); ++i) {
            const QRect h = headerRect(i);
            const bool isDefault = all[i].id == lib.currentId();
            const QString name = fm.elidedText(all[i].name, Qt::ElideRight, h.width() - 60);
            p.setPen(isDefault ? theme::accent : theme::text);
            p.drawText(h.adjusted(3, 0, -34, 0), Qt::AlignLeft | Qt::AlignVCenter, name);
            if (isDefault) {
                p.drawPixmap(QPointF(3 + fm.horizontalAdvance(name) + 6, h.center().y() - 6),
                             theme::icon(QStringLiteral("check"), 13, theme::accent, devicePixelRatioF()));
            }
            const QRect more = moreRect(i);
            p.drawPixmap(QPointF(more.center().x() - 8, more.center().y() - 8),
                         theme::icon(QStringLiteral("more"), 17, i == m_hoverMore ? theme::text : theme::textDim, devicePixelRatioF()));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        PaletteLibrary& lib = PaletteLibrary::instance();
        const QPoint at = e->position().toPoint();
        for (int i = 0; i < m_tops.size() && i < lib.palettes().size(); ++i) {
            if (!headerRect(i).contains(at))
                continue;
            const QString id = lib.palettes()[i].id;
            if (e->button() == Qt::RightButton || moreRect(i).contains(at)) {
                // Once this press has been dealt with: a menu has an event loop of its own.
                const QPoint global = e->globalPosition().toPoint();
                QTimer::singleShot(0, m_panel, [panel = m_panel, id, global] { panel->showPaletteMenu(id, global); });
            } else if (e->button() == Qt::LeftButton && id != lib.currentId()) {
                lib.setCurrent(id);
                lib.save();
            }
            return;
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        int over = -1;
        for (int i = 0; i < m_tops.size(); ++i) {
            if (moreRect(i).contains(e->position().toPoint()))
                over = i;
        }
        if (over != m_hoverMore) {
            m_hoverMore = over;
            update();
        }
    }

    void leaveEvent(QEvent*) override
    {
        m_hoverMore = -1;
        update();
    }

private:
    ColorPanel* m_panel;
    ToolState* m_tools;
    DragScroller* m_scroller = nullptr;
    QVector<PaletteGrid*> m_grids;
    QVector<int> m_tops;   // where each palette's name starts
    int m_hoverMore = -1;
};

// -------------------------------------------------------------- ColorPanel

ColorPanel::ColorPanel(ToolState* tools, QWidget* parent) : QWidget(parent), m_tools(tools)
{
    setFixedSize(kWidth, 628);
    // The right margin is given row by row, so that the lists can keep their
    // scroll bar in it.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kSide, 12, 0, 14);
    outer->setSpacing(8);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, kSide, 0);
    auto* title = new QLabel(tr("Colours"), this);
    title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(title);
    header->addStretch();
    m_pair = new ColorPair(this);
    m_pair->setObjectName(QStringLiteral("color-pair"));
    m_pair->setFixedSize(46, 24);
    m_pair->setToolTip(tr("The current colour and the previous one: tap to swap them (X)"));
    header->addWidget(m_pair);
    m_hex = new QLineEdit(this);
    m_hex->setObjectName(QStringLiteral("color-hex"));
    m_hex->setFixedWidth(92);
    m_hex->setMaxLength(7);
    m_hex->setAlignment(Qt::AlignCenter);
    m_hex->setFont(theme::font(12));
    header->addWidget(m_hex);
    outer->addLayout(header);

    auto* tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(0, 0, kSide, 0);
    m_tabs = new SegmentBar({tr("Disc"), tr("Classic"), tr("Harmony"), tr("Value"), tr("Palettes")}, this);
    m_tabs->setObjectName(QStringLiteral("color-tabs"));
    tabRow->addWidget(m_tabs);
    outer->addLayout(tabRow);

    m_disc = new ColorDisc(this);
    m_square = new ColorSquare(this);
    m_square->setObjectName(QStringLiteral("color-square"));
    m_discSliders = new ChannelSliders(ChannelSliders::Rows::Hsb, this);
    m_squareSliders = new ChannelSliders(ChannelSliders::Rows::Hsb, this);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildPickerPage(m_disc, m_discSliders));
    m_stack->addWidget(buildPickerPage(m_square, m_squareSliders));
    m_stack->addWidget(buildHarmonyPage());
    m_stack->addWidget(buildValuePage());
    m_stack->addWidget(buildPalettesPage());
    outer->addWidget(m_stack, 1);
    m_footer = buildFooter();
    outer->addWidget(m_footer);

    connect(m_tabs, &SegmentBar::currentChanged, this, &ColorPanel::setTab);
    connect(m_disc, &ColorDisc::hsvChanged, this, &ColorPanel::applyHsv);
    connect(m_square, &ColorSquare::hsvChanged, this, &ColorPanel::applyHsv);
    connect(m_wheel, &HarmonyWheel::hsvChanged, this, &ColorPanel::applyHsv);
    for (ChannelSliders* s : {m_discSliders, m_squareSliders, m_valueSliders}) {
        connect(s, &ChannelSliders::hsvChanged, this, &ColorPanel::applyHsv);
        connect(s, &ChannelSliders::rgbChanged, m_tools, &ToolState::setColor);
    }
    connect(m_brightness, &Slider::valueChanged, this, [this](float v) { applyHsv(m_h, m_s, v); });
    connect(m_harmonyMode, &QComboBox::currentIndexChanged, this, [this](int index) { setHarmony(Harmony(index)); });
    connect(m_companions, &SwatchGrid::picked, m_tools, &ToolState::setColor);
    connect(m_hex, &QLineEdit::editingFinished, this, [this] {
        QString text = m_hex->text().trimmed();
        if (!text.startsWith(QLatin1Char('#')))
            text.prepend(QLatin1Char('#'));
        const QColor c(text);
        if (c.isValid())
            m_tools->setColor(c);
        else
            syncFromTools();
    });
    for (ColorPair* pair : {m_pair, m_bigPair})
        connect(pair, &QAbstractButton::clicked, m_tools, &ToolState::swapColor);
    connect(m_recent, &SwatchGrid::picked, m_tools, &ToolState::setColor);
    connect(m_tools, &ToolState::colorChanged, this, &ColorPanel::syncFromTools);
    connect(m_tools, &ToolState::recentColorsChanged, this, [this] {
        m_recent->setColors(m_tools->recentColors());
        syncPair();
    });
    connect(&PaletteLibrary::instance(), &PaletteLibrary::changed, this, &ColorPanel::syncPalettes);

    QSettings st;
    m_recent->setColors(m_tools->recentColors());
    setHarmony(Harmony(std::clamp(st.value(QStringLiteral("colors/harmony"), 0).toInt(), 0, int(Harmony::Count) - 1)));
    setTab(std::clamp(st.value(QStringLiteral("colors/tab"), 0).toInt(), 0, int(Palettes)));
    syncPalettes();
    syncFromTools();
}

// A picker with the three sliders under it: the disc, and the classic square.
QWidget* ColorPanel::buildPickerPage(QWidget* picker, ChannelSliders* sliders)
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, kSide, 0);
    l->setSpacing(8);
    picker->setFixedSize(250, 250);
    l->addWidget(picker, 0, Qt::AlignHCenter);
    l->addWidget(sliders);
    l->addStretch();
    return page;
}

QWidget* ColorPanel::buildHarmonyPage()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, kSide, 0);
    l->setSpacing(8);
    m_wheel = new HarmonyWheel(page);
    m_wheel->setObjectName(QStringLiteral("harmony-wheel"));
    m_wheel->setFixedSize(250, 250);
    l->addWidget(m_wheel, 0, Qt::AlignHCenter);

    auto* brightness = new QHBoxLayout;
    brightness->setSpacing(6);
    auto* b = new QLabel(tr("B"), page);
    b->setFont(theme::font(12, QFont::Medium));
    b->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    brightness->addWidget(b);
    m_brightness = new Slider(page);
    m_brightness->setObjectName(QStringLiteral("harmony-brightness"));
    brightness->addWidget(m_brightness, 1);
    l->addLayout(brightness);

    // The colour and the ones that go with it, to be tapped; and which kind of harmony.
    auto* row = new QHBoxLayout;
    m_companions = new SwatchGrid(4, page);
    m_companions->setObjectName(QStringLiteral("harmony-colors"));
    m_companions->setFixedSize(4 * PaletteGrid::kCell, PaletteGrid::kCell);
    row->addWidget(m_companions);
    row->addStretch();
    m_harmonyMode = new QComboBox(page);
    m_harmonyMode->setObjectName(QStringLiteral("harmony-mode"));
    m_harmonyMode->setFont(theme::font(12));
    for (int i = 0; i < int(Harmony::Count); ++i)
        m_harmonyMode->addItem(tr(harmonyName(Harmony(i))));
    row->addWidget(m_harmonyMode);
    l->addLayout(row);
    l->addStretch();
    return page;
}

QWidget* ColorPanel::buildValuePage()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, kSide, 0);
    l->setSpacing(6);
    m_bigPair = new ColorPair(page);
    m_bigPair->setObjectName(QStringLiteral("color-pair-large"));
    m_bigPair->setFixedHeight(52);
    m_bigPair->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_bigPair->setToolTip(tr("Tap to swap the current colour and the previous one (X)"));
    l->addWidget(m_bigPair);
    auto* names = new QHBoxLayout;
    names->addWidget(captionLabel(tr("Current"), page));
    names->addStretch();
    names->addWidget(captionLabel(tr("Previous · tap to swap"), page));
    l->addLayout(names);
    l->addSpacing(8);
    m_valueSliders = new ChannelSliders(ChannelSliders::Rows::All, page);
    l->addWidget(m_valueSliders);
    l->addStretch();
    return page;
}

QWidget* ColorPanel::buildPalettesPage()
{
    auto* page = new QWidget(this);
    auto* l = new QVBoxLayout(page);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, kSide - 6, 0);
    top->addWidget(captionLabel(tr("Tap a name to keep it under the pickers"), page));
    top->addStretch();
    auto* add = new IconButton(QStringLiteral("plus"), page, 18);
    add->setObjectName(QStringLiteral("palette-new"));
    add->setToolTip(tr("New palette: empty, from a picture or the canvas, or from a file"));
    top->addWidget(add);
    l->addLayout(top);

    m_list = new PaletteList(this, m_tools);
    m_list->setObjectName(QStringLiteral("palette-list"));
    m_listScroll = new QScrollArea(page);
    m_listScroll->setWidget(m_list);
    m_listScroll->setWidgetResizable(false);
    m_listScroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_listScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listScroll->setFrameShape(QFrame::NoFrame);
    m_listScroll->viewport()->setAutoFillBackground(false);
    m_list->setAutoFillBackground(false);
    l->addWidget(m_listScroll, 1);
    // Names and gaps scroll by being dragged; the grids tell a drag from a tap themselves.
    m_list->setScroller(new DragScroller(m_listScroll, m_list));
    connect(add, &QAbstractButton::clicked, this, [this, add] { showNewMenu(add->mapToGlobal(QPoint(add->width() - 240, add->height() + 4))); });
    return page;
}

// What stays under the pickers: the colours used lately, and the default palette.
QWidget* ColorPanel::buildFooter()
{
    auto* footer = new QWidget(this);
    auto* l = new QVBoxLayout(footer);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    l->addWidget(captionLabel(tr("History"), footer));
    auto* history = new QHBoxLayout;
    history->setContentsMargins(0, 0, kSide, 0);
    m_recent = new SwatchGrid(10, footer);
    m_recent->setObjectName(QStringLiteral("color-history"));
    history->addWidget(m_recent);
    l->addLayout(history);
    l->addSpacing(2);

    auto* nameRow = new QHBoxLayout;
    nameRow->setContentsMargins(0, 0, kSide, 0);
    auto* name = new CaptionButton(footer);
    name->setObjectName(QStringLiteral("palette-current"));
    name->setToolTip(tr("The default palette. Tap for another; tap an empty slot below to keep the current colour"));
    m_paletteName = name;
    nameRow->addWidget(name);
    nameRow->addStretch();
    l->addLayout(nameRow);

    m_compact = new PaletteGrid(m_tools, footer);
    m_compact->setObjectName(QStringLiteral("palette-compact"));
    m_compactScroll = new QScrollArea(footer);
    m_compactScroll->setWidget(m_compact);
    m_compactScroll->setWidgetResizable(false);
    m_compactScroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_compactScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_compactScroll->setFrameShape(QFrame::NoFrame);
    m_compactScroll->viewport()->setAutoFillBackground(false);
    m_compact->setAutoFillBackground(false);
    m_compactScroll->setFixedHeight(kCompactRows * PaletteGrid::kCell);
    m_compact->setScroller(new DragScroller(m_compactScroll));
    l->addWidget(m_compactScroll);
    connect(name, &QAbstractButton::clicked, this, [this, name] { showSwitchMenu(name->mapToGlobal(QPoint(0, name->height() + 2))); });
    return footer;
}

int ColorPanel::tab() const
{
    return m_stack->currentIndex();
}

void ColorPanel::setTab(int tab)
{
    tab = std::clamp(tab, 0, int(Palettes));
    m_tabs->setCurrent(tab);
    m_stack->setCurrentIndex(tab);
    m_footer->setVisible(tab != Palettes);   // the page of palettes takes the room of the one palette
    QSettings().setValue(QStringLiteral("colors/tab"), tab);
}

Harmony ColorPanel::harmony() const
{
    return m_wheel->harmony();
}

void ColorPanel::setHarmony(Harmony mode)
{
    m_wheel->setHarmony(mode);
    {
        const QSignalBlocker quiet(m_harmonyMode);
        m_harmonyMode->setCurrentIndex(int(mode));
    }
    QSettings().setValue(QStringLiteral("colors/harmony"), int(mode));
    syncViews();
}

void ColorPanel::applyHsv(float h, float s, float v)
{
    m_h = h;
    m_s = s;
    m_v = v;
    m_updating = true;
    m_tools->setColor(QColor::fromHsvF(h, s, v));
    m_updating = false;
    syncViews();
}

void ColorPanel::syncFromTools()
{
    if (m_updating)
        return;
    const QColor c = m_tools->color();
    float h = 0, s = 0, v = 0;
    c.getHsvF(&h, &s, &v);
    // Keep the previous hue/saturation where the colour itself can't define them.
    if (h >= 0.0f && s > 0.0f)
        m_h = h;
    if (v > 0.0f)
        m_s = s;
    m_v = v;
    syncViews();
}

void ColorPanel::syncViews()
{
    m_disc->setHsv(m_h, m_s, m_v);
    m_square->setHsv(m_h, m_s, m_v);
    m_wheel->setHsv(m_h, m_s, m_v);
    for (ChannelSliders* s : {m_discSliders, m_squareSliders, m_valueSliders})
        s->setHsv(m_h, m_s, m_v);
    m_brightness->setValue(m_v);
    m_brightness->setTrackColors({QColor(Qt::black), QColor::fromHsvF(m_h, m_s, 1.0f)});
    QVector<QColor> family = {QColor::fromHsvF(m_h, m_s, m_v)};
    family += harmonyColors(m_wheel->harmony(), m_h, m_s, m_v);
    m_companions->setColors(family);
    m_hex->setText(m_tools->color().name());
    syncPair();
}

void ColorPanel::syncPair()
{
    for (ColorPair* pair : {m_pair, m_bigPair})
        pair->setColors(m_tools->color(), m_tools->previousColor());
}

// ----------------------------------------------------------------- palettes

void ColorPanel::syncPalettes()
{
    const PaletteLibrary& lib = PaletteLibrary::instance();
    static_cast<CaptionButton*>(m_paletteName)->setCaption(lib.current().name);
    m_compact->setPalette(lib.currentId());
    m_compact->refresh();
    m_list->relayout();
}

QWidget* ColorPanel::paletteList() const
{
    return m_list;
}

QRect ColorPanel::paletteHeaderRect(int index) const
{
    return m_list->headerRect(index);
}

PaletteGrid* ColorPanel::paletteGrid(int index) const
{
    return m_list->grid(index);
}

void ColorPanel::adopt(const QString& paletteId)
{
    PaletteLibrary& lib = PaletteLibrary::instance();
    lib.setCurrent(paletteId);
    lib.save();
    const QRect header = m_list->headerRect(lib.indexOf(paletteId));
    if (!header.isNull())
        m_listScroll->verticalScrollBar()->setValue(header.top());
    m_compactScroll->verticalScrollBar()->setValue(0);
}

QString ColorPanel::newPalette()
{
    const QString id = PaletteLibrary::instance().create(tr("New Palette"));
    adopt(id);
    return id;
}

QString ColorPanel::newPaletteFrom(const QImage& picture, const QString& name)
{
    const QVector<QColor> colors = PaletteLibrary::fromImage(picture);
    if (colors.isEmpty()) {
        emit message(tr("There are no colours there to take"));
        return {};
    }
    const QString id = PaletteLibrary::instance().create(name, colors);
    adopt(id);
    emit message(colors.size() == 1 ? tr("1 colour taken") : tr("%1 colours taken").arg(colors.size()));
    return id;
}

bool ColorPanel::importPaletteFile(const QString& path)
{
    QString error;
    const QString id = PaletteLibrary::instance().importFile(path, &error);
    if (id.isEmpty()) {
        QMessageBox::warning(window(), tr("Import Palette"), error);
        return false;
    }
    adopt(id);
    return true;
}

void ColorPanel::showNewMenu(const QPoint& globalPos)
{
    QMenu menu(window());
    menu.addAction(tr("New Palette"), this, &ColorPanel::newPalette);
    menu.addAction(tr("From the Canvas"), this, [this] {
        if (canvasImage)
            newPaletteFrom(canvasImage(), tr("From the Canvas"));
    });
    QAction* fromReference = menu.addAction(tr("From the Reference Picture"), this, [this] {
        if (referenceImage)
            newPaletteFrom(referenceImage(), tr("From the Reference"));
    });
    fromReference->setEnabled(referenceImage && !referenceImage().isNull());
    menu.addAction(tr("From a Picture File…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(window(), tr("Colours from a Picture"),
                                                          QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), pictureFilter());
        if (path.isEmpty())
            return;
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QImage picture = reader.read();
        if (picture.isNull())
            QMessageBox::warning(window(), tr("Colours from a Picture"), tr("%1 could not be read as a picture.").arg(QFileInfo(path).fileName()));
        else
            newPaletteFrom(picture, QFileInfo(path).completeBaseName());
    });
    menu.addSeparator();
    menu.addAction(tr("Import Palettes…"), this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(window(), tr("Import Palettes"),
                                                                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                                tr("Palettes (*.gpl *.hex *.txt *.ase *.swatches);;All files (*)"));
        for (const QString& path : paths)
            importPaletteFile(path);
    });
    menu.addAction(tr("Add the Starter Palettes Again"), this, [] {
        PaletteLibrary& lib = PaletteLibrary::instance();
        lib.addStarters();
        lib.save();
    });
    menu.exec(globalPos);
}

void ColorPanel::showPaletteMenu(const QString& id, const QPoint& globalPos)
{
    PaletteLibrary& lib = PaletteLibrary::instance();
    const Palette* p = lib.find(id);
    if (!p)
        return;
    const QString name = p->name;
    const bool hasColours = p->filled() > 0;

    QMenu menu(window());
    QAction* def = menu.addAction(tr("Keep Under the Pickers"), this, [=, &lib] {
        lib.setCurrent(id);
        lib.save();
    });
    def->setCheckable(true);
    def->setChecked(id == lib.currentId());
    menu.addSeparator();
    menu.addAction(tr("Rename…"), this, [=, this, &lib] {
        bool ok = false;
        const QString wanted = QInputDialog::getText(window(), tr("Rename Palette"), tr("Name"), QLineEdit::Normal, name, &ok);
        if (ok && lib.rename(id, wanted))
            lib.save();
    });
    menu.addAction(tr("Duplicate"), this, [=, &lib] {
        if (!lib.duplicate(id).isEmpty())
            lib.save();
    });
    QAction* exportIt = menu.addAction(tr("Export…"), this, [=, this, &lib] {
        const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(name + QStringLiteral(".gpl"));
        const QString gpl = tr("GIMP palette (*.gpl)"), swatches = tr("Procreate palette (*.swatches)");
        QString filter = gpl;
        QString path = QFileDialog::getSaveFileName(window(), tr("Export Palette"), suggested, gpl + QStringLiteral(";;") + swatches, &filter);
        if (path.isEmpty())
            return;
        if (QFileInfo(path).suffix().isEmpty())
            path += filter == swatches ? QStringLiteral(".swatches") : QStringLiteral(".gpl");
        QString error;
        const bool ok = QFileInfo(path).suffix().toLower() == QLatin1String("swatches") ? lib.exportSwatches(id, path, &error) : lib.exportGpl(id, path, &error);
        if (!ok)
            QMessageBox::warning(window(), tr("Export Palette"), error);
    });
    exportIt->setEnabled(hasColours);
    menu.addSeparator();
    menu.addAction(tr("Delete “%1”").arg(name), this, [=, this, &lib] {
        // An empty palette goes without a word; one with colours in it asks first.
        if (hasColours
            && QMessageBox::question(window(), tr("Delete Palette"), tr("Delete the palette “%1” and its colours? This cannot be undone.").arg(name),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
                   != QMessageBox::Yes)
            return;
        if (lib.remove(id))
            lib.save();
    });
    menu.exec(globalPos);
}

// The palettes by name, to change which one is under the pickers without leaving them.
void ColorPanel::showSwitchMenu(const QPoint& globalPos)
{
    PaletteLibrary& lib = PaletteLibrary::instance();
    QMenu menu(window());
    for (const Palette& p : lib.palettes()) {
        const QString id = p.id;
        QAction* a = menu.addAction(p.name, this, [id, &lib] {
            lib.setCurrent(id);
            lib.save();
        });
        a->setCheckable(true);
        a->setChecked(id == lib.currentId());
    }
    menu.addSeparator();
    menu.addAction(tr("All Palettes…"), this, [this] { setTab(Palettes); });
    menu.exec(globalPos);
}
