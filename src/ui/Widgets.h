#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QVector>
#include <QWidget>

// Flat toolbar button showing a line icon or a text label. `active` tints it
// with the accent colour, the way Procreate marks the current tool.
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconButton(const QString& icon, QWidget* parent = nullptr, int iconSize = 22);
    void setIconName(const QString& icon);
    void setActive(bool active);
    bool isActive() const { return m_active; }
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QString m_icon;
    int m_iconSize;
    bool m_active = false;
    bool m_hover = false;
};

// Horizontal slider with a 0..1 value. An optional gradient turns the track
// into a colour ramp for the hue/saturation/brightness sliders.
class Slider : public QWidget {
    Q_OBJECT
public:
    explicit Slider(QWidget* parent = nullptr);
    float value() const { return m_value; }
    void setValue(float v);   // does not emit
    void setTrackColors(const QVector<QColor>& stops);
    QSize sizeHint() const override { return {160, 24}; }

signals:
    void valueChanged(float v);
    void pressed();
    void released();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void setFromPos(qreal x);
    float m_value = 0.0f;
    QVector<QColor> m_stops;
};

// Tall vertical slider for the sidebar (brush size, opacity).
class VSlider : public QWidget {
    Q_OBJECT
public:
    explicit VSlider(QWidget* parent = nullptr);
    float value() const { return m_value; }
    void setValue(float v);   // does not emit
    QSize sizeHint() const override { return {36, 170}; }

signals:
    void valueChanged(float v);
    void pressed();
    void released();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void setFromPos(qreal y);
    float m_value = 0.0f;
    bool m_down = false;
};

// Rounded translucent panel used for the floating bars.
class Panel : public QWidget {
    Q_OBJECT
public:
    explicit Panel(QWidget* parent = nullptr, int radius = 12);
    void setFlatEdges(Qt::Edges edges) { m_flat = edges; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int m_radius;
    Qt::Edges m_flat;
};

// Floating panel anchored under a toolbar button, with a little arrow pointing
// at it. Lives inside the main window (not a separate top-level window) and
// closes when the user clicks or draws anywhere else.
class QScrollArea;

// Scrolling by pressing on a list and dragging it, as a finger would on a
// touch screen. It is there for the pen, which has no wheel; a flick keeps
// gliding for a moment.
class DragScroller : public QObject {
    Q_OBJECT
public:
    // Watches `content`, the widget inside `area`. While there is something to
    // scroll, a press is held back until it is clear whether it starts a drag;
    // if it turns out to be a tap, the widget receives it then. Controls inside
    // the content that take presses themselves (sliders, buttons) are left alone.
    DragScroller(QScrollArea* area, QWidget* content);
    // For a widget that tells taps and drags apart itself and only wants the
    // scrolling done.
    explicit DragScroller(QScrollArea* area);

    bool canScroll() const;
    void begin(const QPointF& globalPos);
    void dragTo(const QPointF& globalPos);
    void end();    // let go: glides on if it was moving
    void stop();

    // Whether the pointer that is down right now is a pen. Call trackPen once
    // at start-up for this to be known.
    static void trackPen();
    static bool penDown();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    enum class State { Idle, Pressed, Scrolling, PassedOn };
    void replayTap();

    QScrollArea* m_area;
    QWidget* m_content = nullptr;
    State m_state = State::Idle;
    bool m_replaying = false;
    QPointF m_pressLocal, m_pressGlobal;
    Qt::KeyboardModifiers m_pressMods;
    double m_startValue = 0.0, m_startY = 0.0;
    struct Sample {
        qint64 ms;
        double y;
    };
    QVector<Sample> m_trail;
    QElapsedTimer m_clock;
    QTimer m_glide;
    double m_velocity = 0.0;   // px per ms, of the content
    double m_position = 0.0;
};

// A panel that hangs from a button in the top bar. Pressing anywhere else
// puts it away, unless it has been pinned: dragging it by the grip along its
// top edge (or pressing the pin) tears it off, and then it stays wherever it
// is put, open, while painting goes on.
class Popover : public QWidget {
    Q_OBJECT
public:
    explicit Popover(QWidget* host);
    void setContent(QWidget* content);
    void popup(QWidget* anchor);
    void dismiss();     // put away, unless pinned
    void hidePanel();   // put away, pinned or not
    void reposition();

    bool isPinned() const { return m_pinned; }
    void setPinned(bool pinned);
    // Where the panel's body sits in the host while pinned.
    QPoint pinnedPos() const { return m_pinnedPos; }
    void setPinnedPos(const QPoint& bodyTopLeft);
    void setTopInset(int px) { m_topInset = px; }   // a pinned panel stays below this (the top bar)
    // Opens above its button rather than below: for buttons along the bottom of the window.
    void setOpensUpward(bool upward);
    QRect gripRect() const;                          // in this widget's coordinates
    QRect pinRect() const;

signals:
    void dismissed();
    void pinnedChanged(bool pinned);

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QRect bodyRect() const;
    void placePinned(QPoint bodyTopLeft, bool snap);

    QWidget* m_host;
    QWidget* m_content = nullptr;
    QPointer<QWidget> m_anchor;
    int m_arrowX = 0;
    bool m_pinned = false;
    QPoint m_pinnedPos;
    int m_topInset = 0;
    bool m_upward = false;
    bool m_gripDown = false, m_gripMoved = false;
    QPointF m_gripStart;      // global
    QPoint m_bodyAtGrip;
};

// Brief status message near the top of the canvas ("Undo Brush Stroke").
class Toast : public QWidget {
    Q_OBJECT
public:
    explicit Toast(QWidget* host);
    void showMessage(const QString& text);
    void setTop(int y) { m_top = y; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QWidget* m_host;
    QString m_text;
    QTimer m_timer;
    int m_top = 64;
};

// A response curve from 0..1 to 0..1 with draggable points, used for the pen
// pressure curve. Interpolation is monotone, so the curve never doubles back.
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);
    QVector<QPointF> points() const { return m_points; }
    void setPoints(const QVector<QPointF>& points);
    // Drawn behind the curve: how much of the picture has each tone (256 steps, 0..1).
    void setHistogram(const QVector<float>& bins, const QColor& colour = QColor(255, 255, 255, 46));
    const QVector<float>& histogram() const { return m_histogram; }
    static QVector<QPointF> identity();
    static float evaluate(const QVector<QPointF>& points, float x);
    QSize sizeHint() const override { return {220, 170}; }

signals:
    void changed();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    QPointF toWidget(const QPointF& p) const;
    QVector<QPointF> m_points;
    int m_grabbed = -1;
    QVector<float> m_histogram;
    QColor m_histogramColour;
};

// How the tones of a picture are spread, as a hill range: dark on the left,
// light on the right. Upright lines mark the tones an adjustment turns into
// black, middle grey and white.
class HistogramView : public QWidget {
public:
    explicit HistogramView(QWidget* parent = nullptr);
    void setBins(const QVector<float>& bins);
    const QVector<float>& bins() const { return m_bins; }
    void setMarkers(float black, float middle, float white);
    QSize sizeHint() const override { return {300, 96}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<float> m_bins;
    float m_black = 0.0f, m_middle = 0.5f, m_white = 1.0f;
};

// A row of labelled segments of which one is chosen, for switching between
// the pages of a panel.
class SegmentBar : public QWidget {
    Q_OBJECT
public:
    explicit SegmentBar(const QStringList& labels, QWidget* parent = nullptr);
    int current() const { return m_current; }
    void setCurrent(int index);   // does not emit
    QRectF segmentRect(int index) const;
    QSize sizeHint() const override { return {260, 28}; }

signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    QStringList m_labels;
    int m_current = 0;
};

// Grid of colour chips.
class SwatchGrid : public QWidget {
    Q_OBJECT
public:
    explicit SwatchGrid(int columns, QWidget* parent = nullptr);
    void setColors(const QVector<QColor>& colors);
    const QVector<QColor>& colors() const { return m_colors; }
    QRectF cellRect(int index) const;
    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

signals:
    void picked(const QColor& color);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    int m_columns;
    QVector<QColor> m_colors;
};
