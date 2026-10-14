#pragma once

#include <QDialog>
#include <QSize>
#include <QVector>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

// Asks what the new canvas should be. Its size can be given in pixels or as
// a size on paper at a resolution, the way a printer asks for it; either way
// the dialog says how many pixels that comes to and what this graphics card
// has room for at that size.
class NewCanvasDialog : public QDialog {
    Q_OBJECT
public:
    enum Unit { Pixels = 0, Millimetres, Centimetres, Inches };
    enum Background { White = 0, CurrentColour, Transparent };

    explicit NewCanvasDialog(const QSize& screenPixels, QWidget* parent = nullptr);

    QSize pixelSize() const { return m_px; }
    int resolution() const;                 // dots per inch
    Background background() const;
    bool fits() const;                      // within what this graphics card can hold

    Unit unit() const;
    void setUnit(Unit unit);
    void setResolution(int dpi);
    void setSize(double width, double height);   // in the unit that is showing
    double shownWidth() const;
    double shownHeight() const;
    QString roomText() const;
    int presetCount() const;
    QString presetText(int index) const;
    int currentPreset() const;              // presetCount() when the size is none of them
    void choosePreset(int index);

    static double pixelsPerUnit(Unit unit, int dpi);
    // The finest resolution at which a sheet this many inches across still
    // fits; 0 when not even a coarse one does.
    static int finestResolution(double widthInches, double heightInches);

    void accept() override;

private:
    struct Preset {
        QString name;
        QSize size;
        int dpi = 0;                        // set along with the size when not 0
        Unit unit = Pixels;                 // the unit such a size is usually given in
    };
    void showSize();
    void sizeEdited();
    void describe();
    void markPreset();

    QVector<Preset> m_presets;
    QSize m_px;
    QComboBox* m_preset = nullptr;
    QDoubleSpinBox* m_width = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QComboBox* m_unit = nullptr;
    QSpinBox* m_dpi = nullptr;
    QComboBox* m_background = nullptr;
    QLabel* m_room = nullptr;
};
