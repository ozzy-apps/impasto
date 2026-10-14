#pragma once

#include "brush/BrushPreset.h"

#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <vector>

class CanvasEngine;
class CanvasWidget;
class CurveEditor;
class IconButton;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class SegmentBar;
class Slider;
class ToolState;

// The brush lab: everything that defines the current brush, over the whole
// window. Its settings are sorted into sections down the left; the middle
// holds the section that is open, with room for a proper pressure curve and
// large pictures of the tip shape and the grain; and the rest is a drawing
// pad to try the brush on. What is drawn there is drawn again whenever a
// setting changes, so the same strokes show what the change did.
//
// Changes take effect at once, as they always have. Done keeps them; Cancel
// puts the brush back as it was when the lab opened.
class BrushLab : public QWidget {
    Q_OBJECT
public:
    enum Section { StrokePath = 0, Stabilisation, Taper, Shape, Grain, Rendering, WetMix, ColourDynamics, Dynamics, Pen, Properties, About, SectionCount };

    BrushLab(CanvasEngine* engine, ToolState* tools, QWidget* host);
    ~BrushLab() override;

    // Opens on the brush of the current tool. `penCurve` is the pen's own
    // pressure curve from Preferences, so that the pad feels like the canvas.
    void open(const QVector<QPointF>& penCurve = {});
    void done();
    void cancel();
    QString brushId() const { return m_id; }

    int section() const;
    void setSection(int section);
    QWidget* sectionList() const;
    QRect sectionRect(int section) const;   // in the list's coordinates
    // The wall of tip shapes on the Shape page, and where one of them is on it.
    QWidget* tipPicker() const;
    QRect tipTileRect(const QString& name) const;

    // The drawing pad.
    CanvasWidget* pad() const { return m_pad; }
    CanvasEngine* padEngine() const { return m_padEngine; }
    ToolState* padTools() const { return m_padTools; }
    void clearPad();
    // The canvas's own paper and the current colour, instead of white on dark.
    void setPadCanvasColours(bool on);
    void setCanvasPaper(const QColor& paper) { m_canvasPaper = paper; }
    void redrawPadNow();   // without waiting for the hand to leave the slider

signals:
    void closed();
    void duplicateRequested();
    void deleteRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    class SectionList;
    class TexturePicker;
    class TexturePreview;
    enum class Unit { Percent, Number, Degrees, Pixels, Times };
    struct Row {
        Section section;
        const char* key;
        const char* heading;   // a caption above this row, or nothing
        const char* label;
        const char* hint;
        float BrushPreset::*member;
        float min, max;
        float curve;   // >1 gives the low end of the slider finer steps
        Unit unit;
    };
    static const std::vector<Row>& rows();
    static QString format(const Row& r, float v);

    const BrushPreset& preset() const;
    void buildPages();
    QWidget* buildPadSide();
    QVBoxLayout* page(Section section) const;
    QLabel* heading(Section section, const QString& text);
    Slider* addSlider(Section section, const QString& label, const QString& hint, const QString& name, QLabel** value);
    void buildShape();
    void buildGrain();
    void buildPen();
    void buildAbout();

    void load();
    void change(const std::function<void(BrushPreset&)>& edit, bool reload = false);
    void edited(int index, float t);
    void pickTexture(const QString& name, bool grain);
    void importTexture(bool grain);
    void refreshPreview();
    void adopt(const QString& brushId);
    void finish();
    void armDelete(bool armed);

    bool ensurePad();
    void syncPadDocument(bool force);
    void redrawPad();
    void showPadMenu();

    CanvasEngine* m_engine;
    ToolState* m_tools;
    QString m_id;
    BrushPreset m_before;          // the brush as it was when the lab opened
    bool m_wasModified = false;
    bool m_applying = false;

    SectionList* m_sections = nullptr;
    QLabel* m_brushName = nullptr;
    QLabel* m_strokePreview = nullptr;
    QLabel* m_pageTitle = nullptr;
    QStackedWidget* m_stack = nullptr;
    std::vector<QVBoxLayout*> m_pages;
    std::vector<Slider*> m_sliders;
    std::vector<QLabel*> m_values;
    Slider* m_count = nullptr;
    QLabel* m_countValue = nullptr;
    TexturePicker* m_tipPicker = nullptr;
    TexturePreview* m_tipPreview = nullptr;
    QPushButton* m_invert = nullptr;
    SegmentBar* m_follow = nullptr;
    TexturePicker* m_grainPicker = nullptr;
    TexturePreview* m_grainPreview = nullptr;
    SegmentBar* m_grainMode = nullptr;
    SegmentBar* m_grainMoving = nullptr;
    SegmentBar* m_buildup = nullptr;
    QComboBox* m_blend = nullptr;
    CurveEditor* m_curve = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_set = nullptr;
    QLabel* m_aboutPreview = nullptr;
    QPushButton* m_duplicate = nullptr;
    QPushButton* m_reset = nullptr;
    QPushButton* m_delete = nullptr;
    QTimer m_disarm;
    bool m_deleteArmed = false;

    QWidget* m_padHolder = nullptr;
    QLabel* m_padHint = nullptr;
    Slider* m_padSize = nullptr;
    QLabel* m_padSizeValue = nullptr;
    IconButton* m_padMenu = nullptr;
    CanvasEngine* m_padEngine = nullptr;
    CanvasWidget* m_pad = nullptr;
    ToolState* m_padTools = nullptr;
    QTimer m_redraw, m_resizePad;
    bool m_padCanvasColours = false;
    QColor m_canvasPaper = QColor(250, 247, 240);
};
