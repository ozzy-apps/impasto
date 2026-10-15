#include "ui/ShortcutSheet.h"

#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QCloseEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {

constexpr int kColumnWidth = 400;   // a column is added for every this much width
constexpr int kHeaderH = 34;

// The line at the top of a section: an arrow, its name, and how much is in it.
class Header : public QWidget {
public:
    Header(const QString& title, int count, QWidget* parent) : QWidget(parent), m_title(title), m_count(count)
    {
        setFixedHeight(kHeaderH);
        setCursor(Qt::PointingHandCursor);
    }
    void setFolded(bool folded)
    {
        m_folded = folded;
        update();
    }
    std::function<void()> onPressed;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, m_hover ? 26 : 14));
        p.drawRoundedRect(QRectF(rect()).adjusted(0, 2, 0, -2), 8, 8);
        p.drawPixmap(QPointF(9, (height() - 14) / 2.0),
                     theme::icon(m_folded ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"), 14, theme::textDim, devicePixelRatioF()));
        p.setFont(theme::font(13, QFont::DemiBold));
        p.setPen(theme::text);
        p.drawText(rect().adjusted(30, 0, -44, 0), Qt::AlignVCenter | Qt::AlignLeft, m_title);
        p.setFont(theme::font(11));
        p.setPen(theme::textDim);
        p.drawText(rect().adjusted(0, 0, -12, 0), Qt::AlignVCenter | Qt::AlignRight, QString::number(m_count));
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && onPressed)
            onPressed();
    }
    void enterEvent(QEnterEvent*) override
    {
        m_hover = true;
        update();
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = false;
        update();
    }

private:
    QString m_title;
    int m_count;
    bool m_folded = false, m_hover = false;
};

} // namespace

// One section: its header, and under it what it holds.
class ShortcutSheet::Box : public QWidget {
public:
    Box(const Section& section, QWidget* parent) : QWidget(parent), m_title(section.title)
    {
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 6);
        l->setSpacing(2);
        header = new Header(section.title, int(section.rows.size()), this);
        l->addWidget(header);
        body = new QWidget(this);
        auto* grid = new QGridLayout(body);
        grid->setContentsMargins(12, 4, 8, 4);
        grid->setHorizontalSpacing(14);
        grid->setVerticalSpacing(7);
        grid->setColumnStretch(1, 1);
        for (int i = 0; i < section.rows.size(); ++i) {
            auto* what = new QLabel(section.rows[i].what, body);
            what->setFont(theme::font(12));
            what->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
            what->setWordWrap(true);
            what->setFixedWidth(150);
            what->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            auto* keys = new QLabel(section.rows[i].keys, body);
            keys->setFont(theme::font(12, QFont::DemiBold));
            keys->setWordWrap(true);
            keys->setAlignment(Qt::AlignTop | Qt::AlignLeft);
            grid->addWidget(what, i, 0);
            grid->addWidget(keys, i, 1);
            rows.push_back({what, keys});
        }
        l->addWidget(body);
    }

    void setFolded(bool on)
    {
        folded = on;
        refresh();
    }

    // Leaves only the lines that mention `text`; while something is being
    // looked for, a folded section opens to show what was found in it.
    void filter(const QString& text)
    {
        m_filter = text.trimmed();
        refresh();
    }

    int shownRows() const
    {
        int n = 0;
        for (const auto& row : rows)
            n += !row.first->isHidden() && !body->isHidden();
        return n;
    }

    int weight() const { return 2 + int(rows.size()); }   // how much of a column it takes, roughly

    Header* header = nullptr;
    QWidget* body = nullptr;
    QVector<QPair<QLabel*, QLabel*>> rows;
    bool folded = false;

private:
    void refresh()
    {
        int matches = 0;
        for (const auto& row : rows) {
            const bool match = m_filter.isEmpty() || row.first->text().contains(m_filter, Qt::CaseInsensitive)
                               || row.second->text().contains(m_filter, Qt::CaseInsensitive)
                               || m_title.contains(m_filter, Qt::CaseInsensitive);
            row.first->setVisible(match);
            row.second->setVisible(match);
            matches += match;
        }
        const bool searching = !m_filter.isEmpty();
        setVisible(!searching || matches > 0);
        body->setVisible(searching ? matches > 0 : !folded);
        header->setFolded(searching ? false : folded);
    }

    QString m_title;
    QString m_filter;
};

QVector<ShortcutSheet::Section> ShortcutSheet::sections()
{
    return {
        {tr("Tools"),
         {{tr("Paint / Smudge / Erase"), QStringLiteral("B / S / E")},
          {tr("Brush library"), tr("tap the active tool again")},
          {tr("Brush size"), QStringLiteral("[  ]   ·   Alt + scroll")},
          {tr("Brush size by dragging"), tr("Shift + drag sideways (in Liquify, its brush)")},
          {tr("Opacity"), QStringLiteral("{  }")},
          {tr("Eyedropper"), tr("hold Alt · right-click · I")},
          {tr("Fill"), tr("G then tap · or drag the colour chip onto the canvas")},
          {tr("Fill threshold"), tr("slide sideways before lifting · rest the chip to do the same")},
          {tr("Gradient"), tr("Shift+G, then drag on the canvas")},
          {tr("Add text"), tr("T, tap the canvas, type · Ctrl+Enter when done")},
          {tr("Liquify"), QStringLiteral("W")}}},
        {tr("Colour"),
         {{tr("Colours"), QStringLiteral("C")},
          {tr("Swap the current and the previous colour"), QStringLiteral("X")},
          {tr("Keep a colour in the palette"), tr("tap an empty slot")},
          {tr("Move, replace or remove a swatch"), tr("rest on it a moment, then drag or let go")}}},
        {tr("Canvas and view"),
         {{tr("Pan"), tr("Space + drag · middle-drag")},
          {tr("Zoom"), tr("scroll · Ctrl+Space + drag · Ctrl +/−")},
          {tr("Rotate view"), tr("Shift+Space + drag · 4 / 6 · 5 resets")},
          {tr("Fit / actual size / mirror"), QStringLiteral("Ctrl+0 / Ctrl+1 / M")},
          {tr("Hide interface / fullscreen"), QStringLiteral("Tab / F11")},
          {tr("Crop or extend the canvas / resize the image"), tr("the wrench menu")}}},
        {tr("Layers"),
         {{tr("Layers"), QStringLiteral("L")},
          {tr("New / duplicate / merge layer"), QStringLiteral("Ctrl+Shift+N / Ctrl+J / Ctrl+E")},
          {tr("Group / ungroup"), QStringLiteral("Ctrl+G / Ctrl+Shift+G")},
          {tr("Clear layer"), QStringLiteral("Delete")},
          {tr("Move a layer in a long list (pen)"), tr("rest on it a moment, then drag")}}},
        {tr("Selecting and editing"),
         {{tr("Undo / Redo"), QStringLiteral("Ctrl+Z / Ctrl+Shift+Z")},
          {tr("Select an area"), tr("Q · Alt + drag removes")},
          {tr("Automatic selection"), tr("tap an area · slide sideways for the threshold")},
          {tr("Select all / deselect / invert"), QStringLiteral("Ctrl+A / Ctrl+D / Ctrl+Shift+I")},
          {tr("Copy / cut / paste"), QStringLiteral("Ctrl+C / Ctrl+X / Ctrl+V")},
          {tr("Transform"), tr("Ctrl+T · Enter applies, Esc cancels")}}},
        {tr("Shapes and guides"),
         {{tr("QuickShape"), tr("rest the pen at the end of a stroke")},
          {tr("Perfect circle / square / 15° line"), tr("hold Shift while shaping")},
          {tr("Shape handles without snapping"), tr("hold Alt")},
          {tr("Add a point to a shape"), tr("Ctrl + click its line")},
          {tr("Remove a point"), tr("Ctrl + click it · Delete removes the chosen one")},
          {tr("Smooth point / sharp corner"), tr("Alt + click the point")},
          {tr("Symmetry, perspective and grid"), tr("the mirror button in the top bar")},
          {tr("Move / turn the symmetry"), tr("drag its blue point / its green point · Shift turns in steps")}}},
        {tr("Adjustments"),
         {{tr("Hue, Saturation, Brightness"), QStringLiteral("Ctrl+U")},
          {tr("Levels / Curves"), QStringLiteral("Ctrl+L / Ctrl+M")},
          {tr("Other adjustments and filters"), tr("the wand button in the top bar")}}},
        {tr("Files"),
         {{tr("New / open / save / save as"), QStringLiteral("Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S")},
          {tr("Export (picture, PSD, OpenRaster) / insert a picture"), QStringLiteral("Ctrl+Shift+E / Ctrl+I")},
          {tr("Gallery"), tr("the Gallery button · Esc returns to the canvas")},
          {tr("Quit"), QStringLiteral("Ctrl+Q")}}},
        {tr("Panels and the pen"),
         {{tr("Reference picture"), QStringLiteral("R")},
          {tr("Stabilisation of the brush in hand"), QStringLiteral("Y")},
          {tr("3D reference: a figure to pose, or a model"), QStringLiteral("Shift+R")},
          {tr("Keep a panel open"), tr("drag it away by its top edge, or press its pin")},
          {tr("Scroll a list with the pen"), tr("press and drag it · a flick glides on")}}},
    };
}

ShortcutSheet::ShortcutSheet(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Keyboard Shortcuts"));
    setModal(false);
    setSizeGripEnabled(true);
    setMinimumSize(420, 320);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 8, 12);
    outer->setSpacing(10);
    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 6, 0);
    m_find = new QLineEdit(this);
    m_find->setObjectName(QStringLiteral("shortcut-find"));
    m_find->setPlaceholderText(tr("Find a shortcut…"));
    m_find->setClearButtonEnabled(true);
    m_find->setFont(theme::font(13));
    top->addWidget(m_find, 1);
    auto textButton = [&](const QString& label, const QString& tip) {
        auto* b = new IconButton(QString(), this);
        b->setText(label);
        b->setToolTip(tip);
        top->addWidget(b);
        return b;
    };
    IconButton* foldAll = textButton(tr("Fold all"), tr("Shut every section"));
    IconButton* openAll = textButton(tr("Open all"), tr("Open every section"));
    outer->addLayout(top);

    m_content = new QWidget(this);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_content);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_content->setAutoFillBackground(false);
    outer->addWidget(m_scroll, 1);
    new DragScroller(m_scroll, m_content);   // the text scrolls under the pen

    QSettings st;
    const QStringList folded = st.value(QStringLiteral("shortcuts/folded")).toStringList();
    const QVector<Section> all = sections();
    for (int i = 0; i < all.size(); ++i) {
        auto* box = new Box(all[i], m_content);
        box->setFolded(folded.contains(all[i].title));
        box->header->onPressed = [this, i] {
            if (m_find->text().trimmed().isEmpty())
                setFolded(i, !isFolded(i));
        };
        m_boxes.push_back(box);
    }
    connect(m_find, &QLineEdit::textChanged, this, &ShortcutSheet::setFilter);
    connect(foldAll, &QAbstractButton::clicked, this, [this] {
        for (int i = 0; i < m_boxes.size(); ++i)
            setFolded(i, true);
    });
    connect(openAll, &QAbstractButton::clicked, this, [this] {
        for (int i = 0; i < m_boxes.size(); ++i)
            setFolded(i, false);
    });

    resize(880, 640);
    if (st.contains(QStringLiteral("shortcuts/geometry")))
        restoreGeometry(st.value(QStringLiteral("shortcuts/geometry")).toByteArray());
    arrange();
}

bool ShortcutSheet::isFolded(int section) const
{
    return section >= 0 && section < m_boxes.size() && m_boxes[section]->folded;
}

void ShortcutSheet::setFolded(int section, bool folded)
{
    if (section < 0 || section >= m_boxes.size())
        return;
    m_boxes[section]->setFolded(folded);
    QStringList shut;
    const QVector<Section> all = sections();
    for (int i = 0; i < m_boxes.size(); ++i) {
        if (m_boxes[i]->folded)
            shut << all[i].title;
    }
    QSettings().setValue(QStringLiteral("shortcuts/folded"), shut);
}

QWidget* ShortcutSheet::sectionHeader(int section) const
{
    return section >= 0 && section < m_boxes.size() ? m_boxes[section]->header : nullptr;
}

int ShortcutSheet::visibleRows() const
{
    int n = 0;
    for (const Box* box : m_boxes)
        n += box->isHidden() ? 0 : box->shownRows();
    return n;
}

void ShortcutSheet::setFilter(const QString& text)
{
    for (Box* box : m_boxes)
        box->filter(text);
}

// As many columns as the window has room for, each section going to
// whichever column is shortest so far.
void ShortcutSheet::arrange()
{
    const int columns = std::clamp(width() / kColumnWidth, 1, 3);
    if (columns == m_columns)
        return;
    m_columns = columns;
    delete m_content->layout();
    auto* row = new QHBoxLayout(m_content);
    row->setContentsMargins(0, 0, 10, 0);
    row->setSpacing(14);
    QVector<QVBoxLayout*> stacks;
    QVector<int> filled(columns, 0);
    for (int c = 0; c < columns; ++c) {
        auto* stack = new QVBoxLayout;
        stack->setSpacing(2);
        row->addLayout(stack, 1);
        stacks.push_back(stack);
    }
    for (Box* box : m_boxes) {
        const int shortest = int(std::min_element(filled.begin(), filled.end()) - filled.begin());
        stacks[shortest]->addWidget(box);
        filled[shortest] += box->weight();
    }
    for (QVBoxLayout* stack : stacks)
        stack->addStretch();
}

void ShortcutSheet::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
    arrange();
}

void ShortcutSheet::closeEvent(QCloseEvent* e)
{
    QSettings().setValue(QStringLiteral("shortcuts/geometry"), saveGeometry());
    QDialog::closeEvent(e);
}
