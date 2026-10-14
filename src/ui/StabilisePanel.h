#pragma once

#include <QWidget>

class QLabel;
class Slider;
class ToolState;

// The smoothing of the brush in hand, without going into the Brush Lab: the
// same four settings as its Stabilisation section, for whichever brush the
// current tool holds, and the extra smoothing Preferences adds to every brush.
// A change is a change to the brush, kept as the Brush Lab keeps it.
class StabilisePanel : public QWidget {
    Q_OBJECT
public:
    explicit StabilisePanel(ToolState* tools, QWidget* parent = nullptr);

    // Smoothing added to every brush; the window passes it on to the engine.
    float everyBrush() const;
    void setEveryBrush(float amount);   // does not emit

signals:
    void everyBrushChanged(float amount);
    void labRequested();   // open the Brush Lab at its Stabilisation section

protected:
    void showEvent(QShowEvent*) override;

private:
    void sync();

    ToolState* m_tools;
    QLabel* m_brush;
    Slider* m_sliders[4];
    QLabel* m_values[4];
    Slider* m_every;
    QLabel* m_everyValue;
};
