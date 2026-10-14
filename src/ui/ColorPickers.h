#pragma once

#include "color/Harmony.h"

#include <QAbstractButton>
#include <QColor>
#include <QImage>
#include <QWidget>

class QSpinBox;
class Slider;

// Procreate-style colour disc: a hue ring around a disc that spans saturation
// (left to right) and brightness (bottom to top).
class ColorDisc : public QWidget {
    Q_OBJECT
public:
    explicit ColorDisc(QWidget* parent = nullptr);
    void setHsv(float h, float s, float v);   // does not emit
    QSize sizeHint() const override { return {250, 250}; }

signals:
    void hsvChanged(float h, float s, float v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent*) override;

private:
    enum class Grab { None, Ring, Disc };
    void handle(const QPointF& pos);
    void rebuildDisc();
    qreal outerRadius() const;
    qreal discRadius() const;

    float m_h = 0.0f, m_s = 0.0f, m_v = 0.0f;
    Grab m_grab = Grab::None;
    QImage m_disc;
    float m_discHue = -1.0f;
};

// The classic picker: one hue as a square, saturation from left to right and
// brightness from bottom to top, with its corners where one expects them
// (white top left, the full colour top right, black along the bottom).
class ColorSquare : public QWidget {
    Q_OBJECT
public:
    explicit ColorSquare(QWidget* parent = nullptr);
    void setHsv(float h, float s, float v);   // does not emit
    QRectF field() const;                     // the square itself, in widget coordinates
    QSize sizeHint() const override { return {250, 250}; }

signals:
    void hsvChanged(float h, float s, float v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void handle(const QPointF& pos);
    void rebuild();

    float m_h = 0.0f, m_s = 0.0f, m_v = 0.0f;
    QImage m_image;
    float m_imageHue = -1.0f;
};

// Hue around the circle and saturation outwards, with a mark for the colour
// and one for each colour that goes with it. Dragging any of them turns the
// whole set; brightness has a slider of its own beside this.
class HarmonyWheel : public QWidget {
    Q_OBJECT
public:
    explicit HarmonyWheel(QWidget* parent = nullptr);
    void setHsv(float h, float s, float v);   // does not emit
    void setHarmony(Harmony mode);
    Harmony harmony() const { return m_mode; }
    // Where a mark is, in widget coordinates: 0 is the colour itself, the rest its companions.
    QPointF markPos(int index) const;
    QSize sizeHint() const override { return {250, 250}; }

signals:
    void hsvChanged(float h, float s, float v);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void handle(const QPointF& pos);
    void rebuild();
    qreal radius() const;
    float offsetOf(int index) const;

    float m_h = 0.0f, m_s = 0.0f, m_v = 0.0f;
    Harmony m_mode = Harmony::Complementary;
    int m_grab = -1;
    QImage m_wheel;
};

// Sliders for hue, saturation and brightness, each track showing what the
// slider would give. With `Rows::All` red, green and blue follow, and every
// value can also be read and typed as a number.
class ChannelSliders : public QWidget {
    Q_OBJECT
public:
    enum class Rows { Hsb, All };
    explicit ChannelSliders(Rows rows, QWidget* parent = nullptr);
    void setHsv(float h, float s, float v);   // does not emit
    Slider* slider(int row) const { return m_sliders.value(row); }   // H S B, then R G B

signals:
    void hsvChanged(float h, float s, float v);
    void rgbChanged(const QColor& color);

private:
    void fromHsbRow(int row, float value);
    void fromRgbRow(int row, int value);
    void sync();

    QVector<Slider*> m_sliders;
    QVector<QSpinBox*> m_numbers;
    float m_h = 0.0f, m_s = 0.0f, m_v = 0.0f;
};

// The current colour and the previous one, side by side. A tap swaps them.
class ColorPair : public QAbstractButton {
    Q_OBJECT
public:
    explicit ColorPair(QWidget* parent = nullptr);
    void setColors(const QColor& current, const QColor& previous);
    QSize sizeHint() const override { return {46, 24}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QColor m_current, m_previous;
};
