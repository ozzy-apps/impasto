#include "ui/LayerPanel.h"

#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>

namespace {
constexpr int kRowH = 66;
constexpr int kThumbW = 72;
constexpr int kThumbH = 50;
constexpr int kMaskW = 40;
constexpr int kIndent = 16;   // per level of nesting
}

// Custom-painted rows. Row 0 is the top of the stack; members of a group are
// indented under its header, and hidden while it is folded shut. The final row
// is the background colour.
class LayerPanel::List : public QWidget {
public:
    explicit List(LayerPanel* panel) : QWidget(panel), m_panel(panel)
    {
        setMouseTracking(true);
        // Resting the pen on a layer picks it up.
        m_hold.setSingleShot(true);
        m_hold.setInterval(320);
        connect(&m_hold, &QTimer::timeout, this, [this] {
            if (m_gesture == Gesture::Pressed)
                pickUp();
        });
        // Carrying a layer to the edge of the panel brings more of the list into view.
        m_edgeScroll.setInterval(30);
        connect(&m_edgeScroll, &QTimer::timeout, this, [this] {
            QScrollArea* area = m_panel->m_scroll;
            const QPoint inView = area->viewport()->mapFromGlobal(m_lastGlobal.toPoint());
            const int step = inView.y() < 28 ? -10 : inView.y() > area->viewport()->height() - 28 ? 10 : 0;
            if (m_gesture != Gesture::Moving || step == 0) {
                m_edgeScroll.stop();
                return;
            }
            area->verticalScrollBar()->setValue(area->verticalScrollBar()->value() + step);
            m_drop = dropAt(mapFromGlobal(m_lastGlobal));
            update();
        });
    }

    void setLayers(const QVector<LayerInfo>& bottomToTop)
    {
        m_rows.clear();
        int foldedBelow = -1;   // depth of a folded group whose members are being skipped
        for (auto it = bottomToTop.crbegin(); it != bottomToTop.crend(); ++it) {
            if (foldedBelow >= 0 && it->depth > foldedBelow)
                continue;
            foldedBelow = (it->isGroup && it->collapsed) ? it->depth : -1;
            m_rows.push_back(*it);
        }
        // Drop cached thumbnails of layers that no longer exist.
        auto prune = [&](QHash<int, Thumb>& cache) {
            for (auto it = cache.begin(); it != cache.end();) {
                bool alive = false;
                for (const LayerInfo& l : bottomToTop)
                    alive = alive || l.id == it.key();
                it = alive ? std::next(it) : cache.erase(it);
            }
        };
        prune(m_thumbs);
        prune(m_maskThumbs);
        setFixedHeight((int(m_rows.size()) + 1) * kRowH);
        update();
    }

    // Where a part of a row is, in this widget's coordinates (used by tests).
    QRectF partRect(int id, const QString& part) const
    {
        for (int row = 0; row < m_rows.size(); ++row) {
            if (m_rows[row].id != id)
                continue;
            if (part == QLatin1String("eye"))
                return eyeRect(row);
            if (part == QLatin1String("mask"))
                return maskRect(row);
            if (part == QLatin1String("chevron"))
                return chevronRect(row);
            if (part == QLatin1String("row"))
                return QRectF(0, row * kRowH, width(), kRowH);
            return thumbRect(row);
        }
        return {};
    }

protected:
    struct Thumb {
        quint64 revision = 0;
        QPixmap pixmap;
    };
    struct Drop {
        bool valid = false;
        int parent = 0;     // group to land in, 0 = top level
        int above = 0;      // sibling that ends up directly above, 0 = top of that parent
        int depth = 0;
        int gap = -1;       // row gap the indicator line is drawn in
        int intoRow = -1;   // or: the group header being dropped onto
    };

    qreal indent(int row) const { return m_rows[row].depth * kIndent; }
    qreal rowMid(int row) const { return row * kRowH + (kRowH - 6) / 2.0; }
    QRectF eyeRect(int row) const { return QRectF(width() - 40, rowMid(row) - 12, 24, 24); }
    QRectF codeRect(int row) const { return QRectF(width() - 84, rowMid(row) - 13, 36, 26); }
    QRectF chevronRect(int row) const { return QRectF(indent(row) + 4, rowMid(row) - 11, 22, 22); }
    QRectF thumbRect(int row) const
    {
        const qreal x = indent(row) + 8 + (m_rows[row].clipping ? 18 : 0) + (m_rows[row].isGroup ? 22 : 0);
        return QRectF(x, rowMid(row) - kThumbH / 2.0, m_rows[row].isGroup ? 34 : kThumbW, kThumbH);
    }
    QRectF maskRect(int row) const
    {
        const QRectF t = thumbRect(row);
        return QRectF(t.right() + 6, t.top(), kMaskW, kThumbH);
    }

    const QPixmap& thumbnail(const LayerInfo& l, bool mask)
    {
        Thumb& t = (mask ? m_maskThumbs : m_thumbs)[l.id];
        const quint64 revision = mask ? l.maskRevision : l.revision;
        if (t.pixmap.isNull() || t.revision != revision) {
            t.pixmap = QPixmap::fromImage(m_panel->m_engine->layerThumbnail(l.id, int(kThumbW * 2 * devicePixelRatioF()), mask));
            t.revision = revision;
        }
        return t.pixmap;
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        CanvasEngine* e = m_panel->m_engine;
        const int activeId = e->activeLayerId();
        const bool onMask = e->maskEditing();
        const QSizeF canvas = e->canvasSize();
        const qreal dpr = devicePixelRatioF();

        auto drawEye = [&](const QRectF& r, bool on, bool selected) {
            p.setPen(QPen(selected ? QColor(255, 255, 255, 200) : QColor(255, 255, 255, 70), 1.4));
            p.setBrush(on ? (selected ? QColor(255, 255, 255, 60) : QColor(255, 255, 255, 28)) : Qt::NoBrush);
            p.drawRoundedRect(r, 6, 6);
            if (on)
                p.drawPixmap(r.topLeft() + QPointF(4, 4), theme::icon(QStringLiteral("check"), 16, Qt::white, dpr));
        };
        // A thumbnail letterboxed to the canvas shape; `target` marks what the brush paints on.
        auto drawThumb = [&](const QRectF& slot, const QPixmap& pm, bool checker, const QColor& backing, bool faded, bool target) {
            const QSizeF ts = canvas.scaled(slot.size(), Qt::KeepAspectRatio);
            const QRectF r(slot.center().x() - ts.width() / 2, slot.center().y() - ts.height() / 2, ts.width(), ts.height());
            QPainterPath clip;
            clip.addRoundedRect(r, 4, 4);
            p.save();
            p.setClipPath(clip);
            if (checker)
                theme::drawChecker(p, r, 5);
            else
                p.fillRect(r, backing);
            p.setOpacity(faded ? 0.35 : 1.0);
            p.drawPixmap(r, pm, QRectF());
            p.restore();
            p.setPen(target ? QPen(Qt::white, 2) : QPen(QColor(0, 0, 0, 80), 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r, 4, 4);
        };

        for (int row = 0; row < m_rows.size(); ++row) {
            const LayerInfo& l = m_rows[row];
            const QRectF r(indent(row), row * kRowH, width() - 4 - indent(row), kRowH - 6);
            const bool sel = l.id == activeId;
            const QColor ink = sel ? QColor(Qt::white) : theme::textDim;
            p.setPen((m_dragging && m_drop.intoRow == row) || (m_lifted && row == m_pressRow) ? QPen(Qt::white, 2) : Qt::NoPen);
            p.setBrush(sel ? theme::accent : QColor(255, 255, 255, row == m_hoverRow ? 26 : 14));
            p.drawRoundedRect(r, 9, 9);

            const QRectF thumb = thumbRect(row);
            if (l.isGroup) {
                const QRectF c = chevronRect(row);
                p.drawPixmap(c.topLeft() + QPointF(3, 3),
                             theme::icon(l.collapsed ? QStringLiteral("chevron-right") : QStringLiteral("chevron-down"), 16, ink, dpr));
                p.setOpacity(l.visible ? 1.0 : 0.4);
                p.drawPixmap(QPointF(thumb.left() + 4, thumb.center().y() - 13),
                             theme::icon(QStringLiteral("folder"), 26, sel ? QColor(Qt::white) : theme::text, dpr));
                p.setOpacity(1.0);
            } else {
                if (l.clipping)
                    p.drawPixmap(QPointF(indent(row) + 8, r.center().y() - 8), theme::icon(QStringLiteral("clip"), 16, ink, dpr));
                drawThumb(thumb, thumbnail(l, false), l.alphaLock || !e->backgroundVisible(), e->backgroundColor(), !l.visible,
                          sel && l.hasMask && !onMask);
            }
            qreal textX = thumb.right() + 12;
            if (l.hasMask) {
                const QRectF m = maskRect(row);
                drawThumb(m, thumbnail(l, true), false, Qt::black, !l.maskEnabled, sel && onMask);
                if (!l.maskEnabled) {
                    // A struck-through mask is switched off.
                    p.setPen(QPen(QColor(0xff, 0x45, 0x3a), 2));
                    p.drawLine(m.center() + QPointF(-9, -9), m.center() + QPointF(9, 9));
                }
                textX = m.right() + 10;
            }

            p.setFont(theme::font(13, QFont::Medium));
            p.setPen(sel ? QColor(Qt::white) : (l.visible ? theme::text : theme::textDim));
            const QRectF nameRect(textX, r.top(), codeRect(row).left() - textX - 6, r.height());
            const QString name = p.fontMetrics().elidedText(l.name, Qt::ElideRight, std::max(10, int(nameRect.width()) - (l.alphaLock ? 18 : 0)));
            p.drawText(l.reference || l.isText ? nameRect.adjusted(0, -8, 0, -8) : nameRect, Qt::AlignVCenter | Qt::AlignLeft, name);
            if (l.reference || l.isText) {
                // Like Procreate, the layer that fills follow says so under its
                // name; and so does a layer whose text can still be retyped.
                p.save();
                p.setFont(theme::font(10));
                p.setPen(sel ? QColor(255, 255, 255, 200) : theme::accent);
                p.drawText(nameRect.adjusted(0, 9, 0, 9), Qt::AlignVCenter | Qt::AlignLeft,
                           l.reference && l.isText ? tr("Reference · Text") : l.reference ? tr("Reference") : tr("Text"));
                p.restore();
            }
            if (l.alphaLock) {
                const qreal lx = textX + p.fontMetrics().horizontalAdvance(name) + 6;
                p.drawPixmap(QPointF(lx, r.center().y() - (l.reference ? 15 : 7)), theme::icon(QStringLiteral("lock"), 14, ink, dpr));
            }

            // Blend badge with opacity underneath, like Procreate's "N".
            p.setFont(theme::font(13, QFont::DemiBold));
            p.setPen(ink);
            const QRectF cr = codeRect(row);
            p.drawText(cr.adjusted(0, -5, 0, -5), Qt::AlignCenter, QString::fromLatin1(blendModeInfo(l.blend).code));
            if (l.opacity < 0.995f) {
                p.setFont(theme::font(9));
                p.drawText(cr.adjusted(0, 15, 0, 9), Qt::AlignCenter, QStringLiteral("%1%").arg(qRound(l.opacity * 100)));
            }
            drawEye(eyeRect(row), l.visible, sel);
        }

        // Background colour row.
        const int row = int(m_rows.size());
        const QRectF r(0, row * kRowH, width() - 4, kRowH - 6);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, row == m_hoverRow ? 26 : 14));
        p.drawRoundedRect(r, 9, 9);
        const QRectF sw(r.left() + 8, r.top() + (r.height() - kThumbH) / 2.0, kThumbW, kThumbH);
        p.setPen(QPen(QColor(0, 0, 0, 80), 1));
        p.setBrush(e->backgroundColor());
        p.drawRoundedRect(sw, 4, 4);
        p.setFont(theme::font(13, QFont::Medium));
        p.setPen(theme::text);
        p.drawText(QRectF(sw.right() + 12, r.top(), 160, r.height()), Qt::AlignVCenter | Qt::AlignLeft, m_panel->tr("Background colour"));
        const QRectF bgEye(width() - 40, row * kRowH + (kRowH - 6) / 2.0 - 12, 24, 24);
        drawEye(bgEye, e->backgroundVisible(), false);

        if (m_dragging && m_drop.valid && m_drop.gap >= 0) {
            // The line starts at the level the layer would land on.
            const qreal y = m_drop.gap * kRowH - 3;
            p.setPen(QPen(Qt::white, 2.5, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(8 + m_drop.depth * kIndent, y), QPointF(width() - 12, y));
        }
    }

    // Where a dragged layer would go if released at `pos`. Between two rows the
    // level is ambiguous at the foot of a group (inside it, or after it): how
    // far right the pointer is decides, as in a file tree.
    Drop dropAt(const QPointF& pos) const
    {
        Drop d;
        const int n = int(m_rows.size());
        if (n == 0)
            return d;
        const int row = std::clamp(int(pos.y()) / kRowH, 0, n - 1);
        const double within = (pos.y() - row * kRowH) / kRowH;
        if (m_rows[row].isGroup && within > 0.3 && within < 0.7 && pos.y() < n * kRowH) {
            d.valid = true;
            d.parent = m_rows[row].id;
            d.depth = m_rows[row].depth + 1;
            d.intoRow = row;
            return d;
        }
        const int gap = std::clamp(int((pos.y() + kRowH / 2.0) / kRowH), 0, n);
        const LayerInfo* above = gap > 0 ? &m_rows[gap - 1] : nullptr;
        const LayerInfo* below = gap < n ? &m_rows[gap] : nullptr;
        const int shallow = below ? below->depth : 0;
        const bool openGroupAbove = above && above->isGroup && !above->collapsed;
        const int deep = std::max(shallow, above ? above->depth + (openGroupAbove ? 1 : 0) : 0);
        d.valid = true;
        d.gap = gap;
        d.depth = std::clamp(int((pos.x() - 8) / kIndent), shallow, deep);
        if (!above)
            return d;   // very top of the stack
        if (openGroupAbove && d.depth == above->depth + 1) {
            d.parent = above->id;   // first thing inside the group above
            return d;
        }
        // Climb from the row above to its ancestor at the chosen level; the
        // dragged layer goes directly underneath that.
        LayerInfo sibling = *above;
        while (sibling.depth > d.depth && sibling.parent != 0)
            sibling = m_panel->m_engine->layerInfo(sibling.parent);
        d.parent = sibling.parent;
        d.above = sibling.id;
        return d;
    }

    // A press does nothing yet: it may become a tap, a scroll or a move.
    //  - Tap: the row's action (eye, arrow, select, mask) happens on release.
    //  - Mouse: dragging moves the layer, as it always has.
    //  - Pen, when the list is longer than the panel: dragging scrolls, the
    //    way a finger would; resting on a layer for a moment picks it up, and
    //    then dragging moves it.
    void mousePressEvent(QMouseEvent* e) override
    {
        const int row = int(e->position().y()) / kRowH;
        m_scroller->stop();
        m_hold.stop();
        m_pressRow = -1;
        m_gesture = Gesture::None;
        if (e->button() != Qt::LeftButton || row < 0 || row > m_rows.size())
            return;
        m_pressRow = row;
        m_pressPos = e->position();
        m_pressGlobal = e->globalPosition();
        m_gesture = Gesture::Pressed;
        m_scrollFirst = DragScroller::penDown() && m_scroller->canScroll();
        if (m_scrollFirst && row < m_rows.size())
            m_hold.start();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int row = int(e->position().y()) / kRowH;
        if (row != m_hoverRow) {
            m_hoverRow = row;
            update();
        }
        if (m_gesture == Gesture::None || !(e->buttons() & Qt::LeftButton))
            return;
        const QPointF d = e->globalPosition() - m_pressGlobal;
        if (m_gesture == Gesture::Pressed && std::hypot(d.x(), d.y()) > 8) {
            m_hold.stop();
            if (m_scrollFirst) {
                m_gesture = Gesture::Scrolling;
                m_scroller->begin(m_pressGlobal);
            } else if (m_pressRow < m_rows.size()) {
                pickUp();
            } else {
                m_gesture = Gesture::None;
            }
        }
        if (m_gesture == Gesture::Scrolling) {
            m_scroller->dragTo(e->globalPosition());
        } else if (m_gesture == Gesture::Moving) {
            m_dragging = true;
            m_drop = dropAt(e->position());
            m_lastGlobal = e->globalPosition();
            if (!m_edgeScroll.isActive())
                m_edgeScroll.start();
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        m_hold.stop();
        m_edgeScroll.stop();
        const Gesture was = m_gesture;
        m_gesture = Gesture::None;
        if (was == Gesture::Pressed && !m_lifted)
            tap(m_pressRow, m_pressPos);
        else if (was == Gesture::Scrolling)
            m_scroller->end();
        else if (was == Gesture::Moving && m_dragging && m_pressRow >= 0 && m_pressRow < m_rows.size() && m_drop.valid)
            m_panel->m_engine->moveLayer(m_rows[m_pressRow].id, m_drop.parent, m_drop.above);
        m_pressRow = -1;
        m_dragging = false;
        m_lifted = false;
        m_drop = {};
        update();
    }

    // The layer under the pen is in hand: it becomes the active one and follows the pen.
    void pickUp()
    {
        if (m_pressRow < 0 || m_pressRow >= m_rows.size())
            return;
        m_panel->m_engine->setActiveLayer(m_rows[m_pressRow].id);
        m_gesture = Gesture::Moving;
        m_lifted = true;
        update();
    }

    void tap(int row, const QPointF& at)
    {
        CanvasEngine* eng = m_panel->m_engine;
        if (row < 0 || row > m_rows.size())
            return;
        if (row == m_rows.size()) {
            const QRectF bgEye(width() - 40, row * kRowH + (kRowH - 6) / 2.0 - 12, 24, 24);
            if (bgEye.adjusted(-6, -6, 6, 6).contains(at)) {
                eng->setBackgroundVisible(!eng->backgroundVisible());
            } else {
                const QColor c = QColorDialog::getColor(eng->backgroundColor(), window(), m_panel->tr("Background colour"));
                if (c.isValid())
                    eng->setBackgroundColor(c);
            }
            return;
        }
        const LayerInfo l = m_rows[row];
        if (eyeRect(row).adjusted(-6, -6, 6, 6).contains(at)) {
            eng->setLayerVisible(l.id, !l.visible);
            return;
        }
        if (l.isGroup && chevronRect(row).adjusted(-4, -8, 4, 8).contains(at)) {
            eng->setLayerCollapsed(l.id, !l.collapsed);
            return;
        }
        // Tapping the mask thumbnail paints on the mask; anywhere else on the
        // row goes back to the layer itself.
        eng->setActiveLayer(l.id);
        eng->setMaskEditing(l.hasMask && maskRect(row).adjusted(-3, -3, 3, 3).contains(at));
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        const int row = int(e->position().y()) / kRowH;
        if (row >= 0 && row < m_rows.size() && !eyeRect(row).contains(e->position()))
            m_panel->renameLayer(m_rows[row].id);
    }

    void contextMenuEvent(QContextMenuEvent* e) override
    {
        const int row = e->pos().y() / kRowH;
        if (row < 0 || row >= m_rows.size())
            return;
        m_panel->m_engine->setActiveLayer(m_rows[row].id);
        m_panel->showLayerMenu(m_rows[row].id, e->globalPos());
    }

    void leaveEvent(QEvent*) override
    {
        m_hoverRow = -1;
        update();
    }

private:
    LayerPanel* m_panel;
    QVector<LayerInfo> m_rows;   // what is on show, top of the stack first
    QHash<int, Thumb> m_thumbs, m_maskThumbs;
    int m_hoverRow = -1;
    enum class Gesture { None, Pressed, Scrolling, Moving };
    int m_pressRow = -1;
    QPointF m_pressPos, m_pressGlobal;
    Gesture m_gesture = Gesture::None;
    bool m_scrollFirst = false;   // this press came from the pen on a list that scrolls
    bool m_lifted = false;        // a layer has been picked up
    bool m_dragging = false;
    Drop m_drop;
    QTimer m_hold, m_edgeScroll;
    QPointF m_lastGlobal;

public:
    DragScroller* m_scroller = nullptr;
};

LayerPanel::LayerPanel(CanvasEngine* engine, ToolState* tools, QWidget* parent)
    : QWidget(parent), m_engine(engine), m_tools(tools)
{
    setFixedSize(372, 560);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 10, 12);
    outer->setSpacing(10);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("Layers"), this);
    title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(title);
    header->addStretch();
    auto* menuBtn = new IconButton(QStringLiteral("more"), this, 20);
    menuBtn->setToolTip(tr("Layer actions"));
    menuBtn->setObjectName(QStringLiteral("layer-menu"));
    header->addWidget(menuBtn);
    auto* groupBtn = new IconButton(QStringLiteral("folder"), this, 20);
    groupBtn->setToolTip(tr("New group"));
    groupBtn->setObjectName(QStringLiteral("layer-add-group"));
    header->addWidget(groupBtn);
    auto* addBtn = new IconButton(QStringLiteral("plus"), this, 20);
    addBtn->setToolTip(tr("New layer (Ctrl+Shift+N)"));
    addBtn->setObjectName(QStringLiteral("layer-add"));
    header->addWidget(addBtn);
    outer->addLayout(header);

    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    m_blend = new QComboBox(this);
    m_blend->setFocusPolicy(Qt::NoFocus);
    m_blend->setMaxVisibleItems(30);
    QString group;
    for (const BlendModeInfo& info : blendModes()) {
        if (QLatin1String(info.group) != group && info.mode != BlendMode::Normal) {
            if (m_blend->count() > 0)
                m_blend->insertSeparator(m_blend->count());
            group = QLatin1String(info.group);
        }
        m_blend->addItem(tr(info.name), int(info.mode));
    }
    m_blend->setFixedWidth(130);
    controls->addWidget(m_blend);
    m_opacity = new Slider(this);
    m_opacity->setObjectName(QStringLiteral("layer-opacity"));
    controls->addWidget(m_opacity, 1);
    m_opacityValue = new QLabel(this);
    m_opacityValue->setFixedWidth(38);
    m_opacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_opacityValue->setFont(theme::font(12));
    controls->addWidget(m_opacityValue);
    outer->addLayout(controls);

    m_list = new List(this);
    m_list->setObjectName(QStringLiteral("layer-list"));
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_list);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_list->setAutoFillBackground(false);
    outer->addWidget(m_scroll, 1);
    m_list->m_scroller = new DragScroller(m_scroll);

    connect(addBtn, &QAbstractButton::clicked, this, [this] { m_engine->addLayer(); });
    connect(groupBtn, &QAbstractButton::clicked, this, [this] { m_engine->addGroup(); });
    connect(menuBtn, &QAbstractButton::clicked, this, [this, menuBtn] {
        showLayerMenu(m_engine->activeLayerId(), menuBtn->mapToGlobal(QPoint(0, menuBtn->height())));
    });
    connect(m_blend, &QComboBox::activated, this, [this](int index) {
        if (!m_updating)
            m_engine->setLayerBlend(m_engine->activeLayerId(), BlendMode(m_blend->itemData(index).toInt()));
    });
    connect(m_opacity, &Slider::valueChanged, this, [this](float v) {
        if (!m_updating)
            m_engine->setLayerOpacity(m_engine->activeLayerId(), v);
    });
    connect(m_engine, &CanvasEngine::layersChanged, this, &LayerPanel::refresh);
    connect(m_engine, &CanvasEngine::documentChanged, this, &LayerPanel::refresh);
    refresh();
}

void LayerPanel::showEvent(QShowEvent*)
{
    refresh();
}

void LayerPanel::refresh()
{
    if (!isVisible() || !m_engine->hasDocument())
        return;   // thumbnails cost a GPU readback; skip while hidden
    m_updating = true;
    m_list->setLayers(m_engine->layers());
    const LayerInfo active = m_engine->layerInfo(m_engine->activeLayerId());
    m_blend->setCurrentIndex(m_blend->findData(int(active.blend)));
    m_opacity->setValue(active.opacity);
    m_opacityValue->setText(QStringLiteral("%1%").arg(qRound(active.opacity * 100)));
    m_updating = false;
}

QRectF LayerPanel::rowPart(int layerId, const QString& part) const
{
    return m_list->partRect(layerId, part);
}

void LayerPanel::renameLayer(int layerId)
{
    const LayerInfo info = m_engine->layerInfo(layerId);
    bool ok = false;
    const QString name = QInputDialog::getText(window(), info.isGroup ? tr("Rename Group") : tr("Rename Layer"), tr("Name"),
                                               QLineEdit::Normal, info.name, &ok);
    if (ok && !name.trimmed().isEmpty())
        m_engine->setLayerName(layerId, name.trimmed());
}

void LayerPanel::showLayerMenu(int layerId, const QPoint& globalPos)
{
    const LayerInfo info = m_engine->layerInfo(layerId);
    if (info.id == 0)
        return;
    const QVector<LayerInfo> all = m_engine->layers();
    // "Below" means the next entry down in the same group.
    int index = 0;
    for (int i = 0; i < all.size(); ++i) {
        if (all[i].id == layerId)
            index = i;
    }
    const bool siblingBelow = index > 0 && all[index - 1].parent == info.parent;
    const bool plainBelow = siblingBelow && !all[index - 1].isGroup;
    int plainLayers = 0;
    for (const LayerInfo& l : all)
        plainLayers += !l.isGroup;

    QMenu menu(window());
    menu.addAction(tr("Rename…"), this, [=, this] { renameLayer(layerId); });
    menu.addAction(tr("Duplicate"), this, [=, this] { m_engine->duplicateLayer(layerId); });
    menu.addSeparator();
    if (info.isGroup) {
        menu.addAction(tr("Ungroup"), this, [=, this] { m_engine->ungroupLayer(layerId); });
        menu.addAction(tr("Flatten Group"), this, [=, this] { m_engine->flattenGroup(layerId); });
        menu.addAction(tr("Group"), this, [=, this] { m_engine->groupLayer(layerId); });
        menu.addSeparator();
        // What the group shows, put together, is what fills on other layers stop at.
        QAction* groupReference = menu.addAction(tr("Reference"), this, [=, this] { m_engine->setReferenceLayer(info.reference ? 0 : layerId); });
        groupReference->setCheckable(true);
        groupReference->setChecked(info.reference);
        groupReference->setToolTip(tr("Fills and automatic selections on other layers stop at the lines of everything in this group"));
        menu.addSeparator();
        // A mask on a group hides part of everything in it.
        if (!info.hasMask) {
            menu.addAction(m_engine->hasSelection() ? tr("Add Mask from Selection") : tr("Add Mask"), this,
                           [=, this] { m_engine->addLayerMask(layerId); });
        } else {
            QAction* enabled = menu.addAction(tr("Mask Enabled"), this, [=, this] { m_engine->setLayerMaskEnabled(layerId, !info.maskEnabled); });
            enabled->setCheckable(true);
            enabled->setChecked(info.maskEnabled);
            menu.addAction(tr("Invert Mask"), this, [=, this] { m_engine->invertLayerMask(layerId); });
            menu.addAction(tr("Delete Mask"), this, [=, this] { m_engine->removeLayerMask(layerId); });
            if (m_engine->maskEditing() && layerId == m_engine->activeLayerId()) {
                menu.addAction(tr("Fill Mask with Colour"), this, [=, this] { m_engine->fillLayer(layerId, m_tools->color()); });
                menu.addAction(tr("Clear Mask"), this, [=, this] { m_engine->clearLayer(layerId); });
            }
        }
        menu.addSeparator();
        menu.addAction(tr("Delete Group"), this, [=, this] { m_engine->removeLayer(layerId); });
        menu.exec(globalPos);
        return;
    }

    if (info.isText) {
        menu.addAction(tr("Edit Text"), this, [=, this] { emit editTextRequested(layerId); });
        menu.addSeparator();
    }
    QAction* lock = menu.addAction(tr("Alpha Lock"), this, [=, this] { m_engine->setLayerAlphaLock(layerId, !info.alphaLock); });
    lock->setCheckable(true);
    lock->setChecked(info.alphaLock);
    QAction* clip = menu.addAction(tr("Clipping Mask"), this, [=, this] { m_engine->setLayerClipping(layerId, !info.clipping); });
    clip->setCheckable(true);
    clip->setChecked(info.clipping);
    clip->setEnabled(siblingBelow);
    QAction* reference = menu.addAction(tr("Reference"), this, [=, this] { m_engine->setReferenceLayer(info.reference ? 0 : layerId); });
    reference->setCheckable(true);
    reference->setChecked(info.reference);
    reference->setToolTip(tr("Fills and automatic selections on other layers stop at this layer's lines"));
    menu.addAction(tr("Select Contents"), this, [=, this] { m_engine->selectLayerContents(layerId); });
    menu.addSeparator();
    if (!info.hasMask) {
        menu.addAction(m_engine->hasSelection() ? tr("Add Mask from Selection") : tr("Add Mask"), this,
                       [=, this] { m_engine->addLayerMask(layerId); });
    } else {
        QAction* enabled = menu.addAction(tr("Mask Enabled"), this, [=, this] { m_engine->setLayerMaskEnabled(layerId, !info.maskEnabled); });
        enabled->setCheckable(true);
        enabled->setChecked(info.maskEnabled);
        menu.addAction(tr("Invert Mask"), this, [=, this] { m_engine->invertLayerMask(layerId); });
        menu.addAction(tr("Apply Mask"), this, [=, this] { m_engine->applyLayerMask(layerId); });
        menu.addAction(tr("Delete Mask"), this, [=, this] { m_engine->removeLayerMask(layerId); });
    }
    menu.addSeparator();
    const bool onMask = m_engine->maskEditing() && layerId == m_engine->activeLayerId();
    menu.addAction(onMask ? tr("Fill Mask with Colour") : tr("Fill with Colour"), this, [=, this] { m_engine->fillLayer(layerId, m_tools->color()); });
    menu.addAction(onMask ? tr("Clear Mask") : tr("Clear"), this, [=, this] { m_engine->clearLayer(layerId); });
    menu.addAction(tr("Flip Horizontal"), this, [=, this] { m_engine->flipLayer(layerId, true); });
    menu.addAction(tr("Flip Vertical"), this, [=, this] { m_engine->flipLayer(layerId, false); });
    menu.addSeparator();
    menu.addAction(tr("Group"), this, [=, this] { m_engine->groupLayer(layerId); });
    QAction* merge = menu.addAction(tr("Merge Down"), this, [=, this] { m_engine->mergeDown(layerId); });
    merge->setEnabled(plainBelow);
    QAction* del = menu.addAction(tr("Delete"), this, [=, this] { m_engine->removeLayer(layerId); });
    del->setEnabled(plainLayers > 1);
    menu.exec(globalPos);
}
