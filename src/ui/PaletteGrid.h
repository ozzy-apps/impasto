#pragma once

#include <QColor>
#include <QTimer>
#include <QWidget>

class DragScroller;
class ToolState;

// One palette as a grid of swatches, ten to a row.
//  - A tap on a colour takes it; a tap on an empty slot keeps the current
//    colour there.
//  - Resting on a colour for a moment picks the swatch up: dragged to another
//    slot it moves there (changing places with whatever was in it), let go
//    where it was it offers to replace or remove it. The pen has to rest
//    first, so that a tap that slips never rearranges anything; the mouse
//    may drag a swatch off at once.
//  - Carried over to another palette's grid and let go there, a copy of it
//    goes into that palette: into the slot it was let go over, or the next
//    free one if that is taken.
//  - Where the grid is part of a list that scrolls, a drag with the pen
//    scrolls the list, as it does everywhere else.
class PaletteGrid : public QWidget {
    Q_OBJECT
public:
    static constexpr int kCell = 27;

    explicit PaletteGrid(ToolState* tools, QWidget* parent = nullptr);
    void setPalette(const QString& id);
    QString paletteId() const { return m_id; }
    void setScroller(DragScroller* scroller) { m_scroller = scroller; }
    void refresh();   // the palette changed: take its size again and repaint

    QRectF slotRect(int slot) const;
    int slotAt(const QPointF& pos) const;
    bool holding() const { return m_lifted; }   // a swatch has been picked up
    int incomingSlot() const { return m_incoming; }   // where a swatch from another palette would go, or -1

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    enum class Gesture { None, Pressed, Scrolling, Moving };
    QColor colorAt(int slot) const;
    void tap(int slot);
    void showSwatchMenu(int slot, const QPoint& globalPos);
    PaletteGrid* gridUnder(const QPointF& globalPos) const;   // another palette's grid
    void showIncoming(int slot, const QColor& colour, const QPointF& at);

    ToolState* m_tools;
    QString m_id;
    DragScroller* m_scroller = nullptr;
    Gesture m_gesture = Gesture::None;
    int m_pressSlot = -1;
    QPointF m_pressGlobal, m_dragPos;
    bool m_pen = false;           // this press came from the pen
    bool m_lifted = false;        // by resting on it, rather than by dragging off
    bool m_carried = false;       // the swatch in hand has left its slot
    PaletteGrid* m_over = nullptr;   // another palette's grid the swatch in hand is over
    // A swatch from another palette, over this grid: where it would go, what it is, where the pen is.
    int m_incoming = -1;
    QColor m_incomingColour;
    QPointF m_incomingPos;
    QTimer m_hold;
};
