#pragma once

#include <QColor>
#include <QDialog>
#include <QSize>
#include <QSizeF>
#include <QVector>

class IconButton;
class QAbstractButton;
class QLabel;
class QPushButton;
class SegmentBar;
class ValueField;

namespace newcanvas {
class PresetList;
class Preview;
}

// Asks what the new canvas should be, in three columns: sizes to start from,
// the size itself (fields that change as they are dragged), and a picture of
// the sheet with its way up. The size can be given in pixels or as a size on
// paper at a resolution, the way a printer asks for it; either way the dialog
// says how many pixels that comes to and what this graphics card has room
// for at that size.
class NewCanvasDialog : public QDialog {
    Q_OBJECT
public:
    enum Unit { Pixels = 0, Millimetres, Centimetres, Inches };
    enum Background { White = 0, CurrentColour, Transparent };
    enum Aspect { Square = 0, FourThree, ThreeTwo, SixteenNine, Paper, AspectCount };

    explicit NewCanvasDialog(const QSize& screenPixels, QWidget* parent = nullptr);

    QSize pixelSize() const { return m_px; }
    int resolution() const { return m_dpi; }   // dots per inch
    Background background() const;
    void setBackground(Background background);
    void setCurrentColour(const QColor& colour);   // what "Colour" fills it with
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

    // Portrait and landscape: the same sheet turned.
    bool isPortrait() const { return m_px.height() > m_px.width(); }
    void setPortrait(bool portrait);        // turns the sheet if it is the other way up
    void swapOrientation();
    // While the shape is kept, changing one side changes the other with it.
    bool keepsShape() const { return m_keepShape; }
    void setKeepShape(bool keep);
    void chooseAspect(int aspect);          // also keeps that shape
    int currentAspect() const;              // AspectCount when it is none of them
    static double aspectRatio(int aspect);  // long side over short side

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
        QString group;
    };
    void showSize();
    void widthEdited();
    void heightEdited();
    void sizeChanged();
    void describe();
    void markPreset();
    void finishTyping();
    int clampSide(qint64 px) const;

    QVector<Preset> m_presets;
    QSize m_px;
    QSizeF m_paper;                         // the same size in inches, as it was given
    int m_dpi = 300;
    int m_preset = 0;
    bool m_keepShape = false;
    double m_ratio = 1.0;                   // width over height, while the shape is kept
    QColor m_colour = Qt::white;
    newcanvas::PresetList* m_list = nullptr;
    newcanvas::Preview* m_preview = nullptr;
    ValueField* m_width = nullptr;
    ValueField* m_height = nullptr;
    ValueField* m_dpiField = nullptr;
    QAbstractButton* m_lock = nullptr;
    SegmentBar* m_aspect = nullptr;
    SegmentBar* m_unit = nullptr;
    SegmentBar* m_background = nullptr;
    IconButton* m_portrait = nullptr;
    IconButton* m_landscape = nullptr;
    QLabel* m_pixels = nullptr;
    QLabel* m_room = nullptr;
    QPushButton* m_create = nullptr;
};
