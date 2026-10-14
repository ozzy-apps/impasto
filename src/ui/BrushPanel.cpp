#include "ui/BrushPanel.h"

#include "brush/BrushPreset.h"
#include "brush/BrushTextures.h"
#include "brush/TextureGen.h"
#include "engine/CanvasEngine.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <functional>

namespace {
constexpr int kRowH = 84;
constexpr int kCatH = 40;
}

class BrushPanel::CategoryList : public QWidget {
public:
    explicit CategoryList(BrushPanel* panel) : QWidget(panel), m_panel(panel)
    {
        m_names = BrushLibrary::instance().categories();
        setFixedHeight(std::max(1, int(m_names.size())) * kCatH);
        setCursor(Qt::PointingHandCursor);
    }
    QString current() const { return m_names.value(m_current); }
    // The artist may have started a set, or emptied one.
    void reload()
    {
        const QString was = current();
        m_names = BrushLibrary::instance().categories();
        m_current = std::max(0, int(m_names.indexOf(was)));
        setFixedHeight(std::max(1, int(m_names.size())) * kCatH);
        update();
    }
    void setCurrent(const QString& name)
    {
        const int i = m_names.indexOf(name);
        if (i >= 0 && i != m_current) {
            m_current = i;
            update();
        }
    }
    std::function<void()> onChanged;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(theme::font(13, QFont::Medium));
        for (int i = 0; i < m_names.size(); ++i) {
            const QRectF r(0, i * kCatH, width() - 6, kCatH - 4);
            if (i == m_current) {
                p.setPen(Qt::NoPen);
                p.setBrush(theme::accent);
                p.drawRoundedRect(r, 8, 8);
            }
            p.setPen(i == m_current ? QColor(Qt::white) : theme::text);
            p.drawText(r.adjusted(12, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, m_names[i]);
        }
    }
    // A tap picks a set; a drag scrolls the column when it is longer than the panel.
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
        if (!m_scrolling && std::abs(e->globalPosition().y() - m_pressGlobal.y()) > 8.0 && m_scroller && m_scroller->canScroll()) {
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
        const int i = int(e->position().y()) / kCatH;
        if (i >= 0 && i < m_names.size() && i != m_current) {
            m_current = i;
            update();
            if (onChanged)
                onChanged();
        }
    }

public:
    DragScroller* m_scroller = nullptr;

private:
    BrushPanel* m_panel;
    QStringList m_names;
    int m_current = 0;
    bool m_down = false, m_scrolling = false;
    QPointF m_pressGlobal;
};

class BrushPanel::BrushList : public QWidget {
public:
    explicit BrushList(BrushPanel* panel) : QWidget(panel), m_panel(panel)
    {
        setCursor(Qt::PointingHandCursor);
        // Resting on a brush picks it up, to be put elsewhere in its set.
        m_hold.setSingleShot(true);
        m_hold.setInterval(350);
        connect(&m_hold, &QTimer::timeout, this, [this] {
            if (m_gesture != Gesture::Pressed)
                return;
            m_gesture = Gesture::Moving;
            m_lifted = true;
            m_drop = m_pressRow;
            update();
        });
        // Carried to the top or bottom of what is in view, the list scrolls along.
        m_edge.setInterval(16);
        connect(&m_edge, &QTimer::timeout, this, [this] {
            auto* area = m_scroller ? qobject_cast<QScrollArea*>(m_scroller->parent()) : nullptr;
            if (m_gesture != Gesture::Moving || !area)
                return;
            const int y = area->viewport()->mapFromGlobal(m_lastGlobal.toPoint()).y();
            const int step = y < 34 ? -9 : y > area->viewport()->height() - 34 ? 9 : 0;
            if (step == 0)
                return;
            area->verticalScrollBar()->setValue(area->verticalScrollBar()->value() + step);
            m_drop = rowAt(mapFromGlobal(m_lastGlobal.toPoint()).y());
            update();
        });
    }

    void setCategory(const QString& category)
    {
        m_category = category;
        m_items = BrushLibrary::instance().inCategory(category);
        setFixedHeight(std::max(1, int(m_items.size())) * kRowH);
        update();
    }
    void clear()
    {
        m_items.clear();
        update();
    }
    int rowOf(const QString& id) const
    {
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items[i]->id == id)
                return i;
        }
        return -1;
    }
    std::function<void(const QPoint& globalPos)> onMenu;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QString selected = m_panel->m_tools->preset().id;
        const qreal dpr = devicePixelRatioF();
        if (m_cacheRevision != BrushLibrary::instance().revision()) {
            m_cache.clear();   // a brush was edited; its stroke looks different now
            m_cacheRevision = BrushLibrary::instance().revision();
        }
        for (int i = 0; i < m_items.size(); ++i) {
            const BrushPreset& b = *m_items[i];
            const QRectF r(0, i * kRowH, width() - 4, kRowH - 6);
            const bool sel = b.id == selected;
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? theme::accent : QColor(255, 255, 255, 14));
            p.drawRoundedRect(r, 9, 9);

            const QSize previewSize(int(r.width()) - 16, int(r.height()) - 20);
            const QString key = QStringLiteral("%1/%2x%3@%4").arg(b.id).arg(previewSize.width()).arg(previewSize.height()).arg(dpr);
            auto it = m_cache.find(key);
            if (it == m_cache.end()) {
                QImage img = m_panel->m_engine->brushPreview(b, previewSize * dpr, Qt::white, float(dpr));
                img.setDevicePixelRatio(dpr);
                it = m_cache.insert(key, QPixmap::fromImage(img));
            }
            p.drawPixmap(QPointF(r.left() + 8, r.top() + 16), it.value());

            p.setFont(theme::font(12, QFont::Medium));
            p.setPen(sel ? QColor(Qt::white) : theme::textDim);
            p.drawText(r.adjusted(12, 7, -8, 0), Qt::AlignTop | Qt::AlignLeft, b.name);
            if (b.custom) {
                // Your own brushes carry a mark, so they stand out from what shipped.
                p.setFont(theme::font(10));
                p.setPen(sel ? QColor(255, 255, 255, 200) : theme::accent);
                p.drawText(r.adjusted(12, 8, -12, 0), Qt::AlignTop | Qt::AlignRight, tr("yours"));
            }
            if (m_gesture == Gesture::Moving && i == m_pressRow) {
                // The brush in hand.
                p.setPen(QPen(Qt::white, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 9, 9);
            }
        }
        if (m_gesture == Gesture::Moving && m_drop >= 0 && m_drop != m_pressRow) {
            // Where it would go: above the row it is over when carried up, below it when carried down.
            const qreal y = m_drop > m_pressRow ? (m_drop + 1) * kRowH - 3 : m_drop * kRowH - 3;
            p.setPen(Qt::NoPen);
            p.setBrush(theme::accent);
            p.drawRoundedRect(QRectF(4, y - 2, width() - 12, 4), 2, 2);
        }
    }

    int rowAt(int y) const { return std::clamp(y / kRowH, 0, std::max(0, int(m_items.size()) - 1)); }

    // A press does nothing yet: it may become a tap, a scroll or a move.
    //  - Tap: the brush is chosen, on release.
    //  - Mouse: dragging moves the brush within its set.
    //  - Pen, when the list is longer than the panel: dragging scrolls;
    //    resting on a brush for a moment picks it up, and then dragging moves it.
    void mousePressEvent(QMouseEvent* e) override
    {
        if (m_scroller)
            m_scroller->stop();
        m_hold.stop();
        m_gesture = Gesture::None;
        m_lifted = false;
        m_drop = -1;
        m_pressRow = int(e->position().y()) / kRowH;
        if (e->button() != Qt::LeftButton || m_pressRow < 0 || m_pressRow >= m_items.size())
            return;
        m_gesture = Gesture::Pressed;
        m_pressGlobal = m_lastGlobal = e->globalPosition();
        m_scrollFirst = DragScroller::penDown() && m_scroller && m_scroller->canScroll();
        m_hold.start();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (m_gesture == Gesture::None || !(e->buttons() & Qt::LeftButton))
            return;
        const QPointF d = e->globalPosition() - m_pressGlobal;
        if (m_gesture == Gesture::Pressed && std::hypot(d.x(), d.y()) > 8.0) {
            m_hold.stop();
            if (m_scrollFirst) {
                m_gesture = Gesture::Scrolling;
                m_scroller->begin(m_pressGlobal);
            } else {
                m_gesture = Gesture::Moving;
            }
        }
        if (m_gesture == Gesture::Scrolling) {
            m_scroller->dragTo(e->globalPosition());
        } else if (m_gesture == Gesture::Moving) {
            m_lastGlobal = e->globalPosition();
            m_drop = rowAt(int(e->position().y()));
            if (!m_edge.isActive())
                m_edge.start();
            update();
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_hold.stop();
        m_edge.stop();
        const Gesture was = m_gesture;
        const int row = m_pressRow, drop = m_drop;
        m_gesture = Gesture::None;
        m_lifted = false;
        m_drop = -1;
        if (was == Gesture::Scrolling) {
            m_scroller->end();
        } else if (row >= 0 && row < m_items.size() && was == Gesture::Moving && drop >= 0 && drop != row) {
            BrushLibrary& lib = BrushLibrary::instance();
            if (lib.moveInCategory(m_items[row]->id, drop)) {
                lib.save();
                setCategory(m_category);
            }
        } else if (row >= 0 && row < m_items.size() && was != Gesture::None) {
            // Let go where it was pressed: that is choosing it.
            m_panel->m_tools->setPreset(m_items[row]->id);
            emit m_panel->brushChosen();
        }
        update();
    }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        const int i = e->pos().y() / kRowH;
        if (i < 0 || i >= m_items.size())
            return;
        m_panel->m_tools->setPreset(m_items[i]->id);
        update();
        if (onMenu)
            onMenu(e->globalPos());
    }

private:
    BrushPanel* m_panel;
    QString m_category;
    QVector<const BrushPreset*> m_items;
    QHash<QString, QPixmap> m_cache;
    int m_cacheRevision = -1;
    enum class Gesture { None, Pressed, Scrolling, Moving };
    Gesture m_gesture = Gesture::None;
    int m_pressRow = -1, m_drop = -1;
    bool m_scrollFirst = false;   // this press came from the pen on a list that scrolls
    bool m_lifted = false;        // picked up by resting on it
    QPointF m_pressGlobal, m_lastGlobal;
    QTimer m_hold, m_edge;

public:
    DragScroller* m_scroller = nullptr;
};

BrushPanel::BrushPanel(CanvasEngine* engine, ToolState* tools, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_tools(tools)
{
    setFixedSize(540, 560);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 10, 12);
    outer->setSpacing(10);

    auto* header = new QHBoxLayout;
    m_title = new QLabel(this);
    m_title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(m_title);
    header->addStretch();
    m_menuBtn = new IconButton(QStringLiteral("more"), this, 20);
    m_menuBtn->setObjectName(QStringLiteral("brush-menu"));
    m_menuBtn->setToolTip(tr("Duplicate, rename, export or import brushes"));
    header->addWidget(m_menuBtn);
    m_newBtn = new IconButton(QStringLiteral("plus"), this, 20);
    m_newBtn->setObjectName(QStringLiteral("brush-new"));
    m_newBtn->setToolTip(tr("New brush in this set"));
    header->addWidget(m_newBtn);
    m_editBtn = new IconButton(QStringLiteral("sliders"), this, 20);
    m_editBtn->setObjectName(QStringLiteral("brush-edit"));
    m_editBtn->setToolTip(tr("Brush Lab: everything about the current brush, with a pad to try it on"));
    header->addWidget(m_editBtn);
    outer->addLayout(header);

    m_libraryPage = new QWidget(this);
    auto* row = new QHBoxLayout(m_libraryPage);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    // The sets, in a column of their own that scrolls once there are more than fit.
    m_categories = new CategoryList(this);
    m_categories->setObjectName(QStringLiteral("brush-categories"));
    auto* sets = new QScrollArea(this);
    sets->setObjectName(QStringLiteral("brush-category-scroll"));
    sets->setWidget(m_categories);
    sets->setWidgetResizable(true);
    sets->setFixedWidth(138);
    sets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sets->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sets->setFrameShape(QFrame::NoFrame);
    sets->viewport()->setAutoFillBackground(false);
    m_categories->setAutoFillBackground(false);
    m_categories->m_scroller = new DragScroller(sets);
    row->addWidget(sets);

    m_brushes = new BrushList(this);
    m_brushes->setObjectName(QStringLiteral("brush-list"));
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_brushes);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_brushes->setAutoFillBackground(false);
    row->addWidget(m_scroll, 1);
    m_brushes->m_scroller = new DragScroller(m_scroll);   // the list tells taps, scrolls and moves apart itself
    outer->addWidget(m_libraryPage, 1);

    connect(m_editBtn, &QAbstractButton::clicked, this, &BrushPanel::labRequested);
    connect(m_newBtn, &QAbstractButton::clicked, this, &BrushPanel::newBrush);
    connect(m_menuBtn, &QAbstractButton::clicked, this,
            [this] { showBrushMenu(m_menuBtn->mapToGlobal(QPoint(0, m_menuBtn->height() + 4))); });

    m_categories->onChanged = [this] {
        m_brushes->setCategory(m_categories->current());
        m_scroll->verticalScrollBar()->setValue(0);
    };
    m_brushes->onMenu = [this](const QPoint& globalPos) { showBrushMenu(globalPos); };
    connect(m_tools, &ToolState::brushChanged, this, &BrushPanel::sync);
    connect(m_tools, &ToolState::toolChanged, this, &BrushPanel::sync);
    m_brushes->setCategory(m_categories->current());
    sync();
}

// Sets and brushes may have come or gone: show the library as it is now, on
// the set of the current brush.
void BrushPanel::reloadLibrary()
{
    const BrushPreset& p = m_tools->preset();
    m_categories->reload();
    m_categories->setCurrent(p.category);
    m_brushes->setCategory(p.category);
    const int row = m_brushes->rowOf(p.id);
    if (row >= 0)
        m_scroll->ensureVisible(0, row * kRowH + kRowH / 2, 0, kRowH);
    sync();
}

void BrushPanel::newBrush()
{
    BrushLibrary& lib = BrushLibrary::instance();
    const QString id = lib.createBrush(m_categories->current());
    lib.save();
    m_tools->setPreset(id);
    reloadLibrary();
    emit labRequested();   // a new brush is there to be shaped
}

void BrushPanel::duplicateBrush()
{
    BrushLibrary& lib = BrushLibrary::instance();
    const QString id = lib.duplicate(m_tools->preset().id);
    if (id.isEmpty())
        return;
    lib.save();
    m_tools->setPreset(id);
    reloadLibrary();
}

void BrushPanel::deleteBrush()
{
    BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset& p = m_tools->preset();
    if (!p.custom)
        return;
    // Move on to the brush that sat below it in its set, or the one above.
    const QString id = p.id;
    const QVector<const BrushPreset*> set = lib.inCategory(p.category);
    QString next;
    for (int i = 0; i < set.size(); ++i) {
        if (set[i]->id == id)
            next = i + 1 < set.size() ? set[i + 1]->id : i > 0 ? set[i - 1]->id : QString();
    }
    m_brushes->clear();   // it holds pointers into the library
    if (!lib.remove(id))
        return;
    lib.save();
    m_tools->brushRemoved(id, next.isEmpty() ? QStringLiteral("round-brush") : next);
    reloadLibrary();
}

bool BrushPanel::importBrushFile(const QString& path)
{
    BrushLibrary& lib = BrushLibrary::instance();
    QString error;
    const QString id = lib.importBrush(path, &error);
    if (id.isEmpty()) {
        QMessageBox::warning(window(), tr("Import Brush"), error);
        return false;
    }
    lib.save();
    m_tools->setPreset(id);
    reloadLibrary();
    return true;
}

void BrushPanel::showBrushMenu(const QPoint& globalPos)
{
    BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset& p = m_tools->preset();
    const QString id = p.id, name = p.name, category = p.category;
    const bool custom = p.custom;

    QMenu menu(window());
    menu.addAction(tr("Duplicate “%1”").arg(name), this, &BrushPanel::duplicateBrush);
    QAction* rename = menu.addAction(tr("Rename…"), this, [=, this, &lib] {
        bool ok = false;
        const QString wanted = QInputDialog::getText(window(), tr("Rename Brush"), tr("Name"), QLineEdit::Normal, name, &ok);
        if (ok && lib.rename(id, wanted)) {
            lib.save();
            m_tools->notifyBrushEdited();
            reloadLibrary();
        }
    });
    rename->setEnabled(custom);
    QMenu* sets = menu.addMenu(tr("Move to Set"));
    sets->setEnabled(custom);
    auto moveTo = [=, this, &lib](const QString& set) {
        if (lib.moveToCategory(id, set)) {
            lib.save();
            m_tools->notifyBrushEdited();
            reloadLibrary();
        }
    };
    for (const QString& set : lib.categories()) {
        QAction* a = sets->addAction(set, this, [=] { moveTo(set); });
        a->setCheckable(true);
        a->setChecked(set == category);
    }
    sets->addSeparator();
    sets->addAction(tr("New Set…"), this, [=, this] {
        bool ok = false;
        const QString set = QInputDialog::getText(window(), tr("New Set"), tr("Name of the set"), QLineEdit::Normal, tr("My Brushes"), &ok);
        if (ok)
            moveTo(set);
    });
    if (!custom)
        menu.addAction(tr("Shipped brushes stay in their set: duplicate first"))->setEnabled(false);
    menu.addSeparator();
    menu.addAction(tr("Export…"), this, [=, this, &lib] {
        const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                      .filePath(name + QLatin1Char('.') + QLatin1String(BrushLibrary::fileExtension()));
        QString path = QFileDialog::getSaveFileName(window(), tr("Export Brush"), suggested,
                                                    tr("Impasto brush (*.%1)").arg(QLatin1String(BrushLibrary::fileExtension())));
        if (path.isEmpty())
            return;
        if (QFileInfo(path).suffix().isEmpty())
            path += QLatin1Char('.') + QLatin1String(BrushLibrary::fileExtension());
        QString error;
        if (!lib.exportBrush(id, path, &error))
            QMessageBox::warning(window(), tr("Export Brush"), error);
    });
    menu.addAction(tr("Import Brushes…"), this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(window(), tr("Import Brushes"),
                                                                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                                tr("Impasto brushes (*.%1 *.%2)").arg(QLatin1String(BrushLibrary::fileExtension()), QLatin1String(BrushLibrary::oldFileExtension())));
        for (const QString& path : paths)
            importBrushFile(path);
    });
    menu.addSeparator();
    if (custom) {
        menu.addAction(tr("Delete “%1”").arg(name), this, &BrushPanel::deleteBrush);
    } else {
        QAction* reset = menu.addAction(tr("Reset to How It Shipped"), this, [=, this, &lib] {
            lib.reset(id);
            lib.save();
            m_tools->notifyBrushEdited();
        });
        reset->setEnabled(lib.isModified(id));
    }
    menu.exec(globalPos);
}

void BrushPanel::hideEvent(QHideEvent*)
{
    BrushLibrary::instance().save();
}

void BrushPanel::sync()
{
    const StrokeTool tool = m_tools->tool();
    m_title->setText(tool == StrokeTool::Smudge ? tr("Smudge") : tool == StrokeTool::Eraser ? tr("Eraser") : tr("Brush Library"));
    if (!isVisible()) {
        m_brushes->update();
        return;
    }
    const BrushPreset& p = m_tools->preset();
    if (m_categories->current() != p.category) {
        m_categories->reload();
        m_categories->setCurrent(p.category);
        m_brushes->setCategory(p.category);
    }
    m_brushes->update();
}

void BrushPanel::showEvent(QShowEvent*)
{
    // Open on the set that contains the current brush and scroll it into view.
    reloadLibrary();
}
