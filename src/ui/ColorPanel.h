#pragma once

#include "color/Harmony.h"

#include <QColor>
#include <QImage>
#include <QWidget>
#include <functional>

class ChannelSliders;
class ColorDisc;
class ColorPair;
class ColorSquare;
class HarmonyWheel;
class PaletteGrid;
class QAbstractButton;
class QComboBox;
class QLineEdit;
class QScrollArea;
class QStackedWidget;
class SegmentBar;
class Slider;
class SwatchGrid;
class ToolState;

// The Colours panel. Four ways of choosing a colour (the disc, the classic
// square, harmonies, and plain numbers) share the space at the top; under
// them stay the colours used lately and the default palette. A fifth page
// holds all the palettes.
class ColorPanel : public QWidget {
    Q_OBJECT
public:
    enum Tab { Disc = 0, Classic, Harmonies, Value, Palettes };

    explicit ColorPanel(ToolState* tools, QWidget* parent = nullptr);

    int tab() const;
    void setTab(int tab);
    Harmony harmony() const;
    void setHarmony(Harmony mode);

    // Where "from the canvas" and "from the reference picture" get their pictures.
    std::function<QImage()> canvasImage;
    std::function<QImage()> referenceImage;

    // Each makes a palette, makes it the default and brings it into view.
    QString newPalette();
    QString newPaletteFrom(const QImage& picture, const QString& name);
    bool importPaletteFile(const QString& path);

    // The list on the Palettes page, for the pen test: where each palette's
    // name is (in the list's coordinates) and its grid of swatches.
    QWidget* paletteList() const;
    QRect paletteHeaderRect(int index) const;
    PaletteGrid* paletteGrid(int index) const;

signals:
    void message(const QString& text);

private:
    class PaletteList;
    friend class PaletteList;

    QWidget* buildPickerPage(QWidget* picker, ChannelSliders* sliders);
    QWidget* buildHarmonyPage();
    QWidget* buildValuePage();
    QWidget* buildPalettesPage();
    QWidget* buildFooter();

    void syncFromTools();
    void applyHsv(float h, float s, float v);
    void syncViews();
    void syncPair();
    void syncPalettes();
    void adopt(const QString& paletteId);
    void showNewMenu(const QPoint& globalPos);
    void showPaletteMenu(const QString& id, const QPoint& globalPos);
    void showSwitchMenu(const QPoint& globalPos);

    ToolState* m_tools;
    SegmentBar* m_tabs = nullptr;
    QStackedWidget* m_stack = nullptr;
    QWidget* m_footer = nullptr;
    QLineEdit* m_hex = nullptr;
    ColorPair* m_pair = nullptr;
    ColorPair* m_bigPair = nullptr;
    ColorDisc* m_disc = nullptr;
    ColorSquare* m_square = nullptr;
    HarmonyWheel* m_wheel = nullptr;
    Slider* m_brightness = nullptr;
    QComboBox* m_harmonyMode = nullptr;
    SwatchGrid* m_companions = nullptr;
    ChannelSliders* m_discSliders = nullptr;
    ChannelSliders* m_squareSliders = nullptr;
    ChannelSliders* m_valueSliders = nullptr;
    SwatchGrid* m_recent = nullptr;
    QAbstractButton* m_paletteName = nullptr;
    PaletteGrid* m_compact = nullptr;
    QScrollArea* m_compactScroll = nullptr;
    PaletteList* m_list = nullptr;
    QScrollArea* m_listScroll = nullptr;
    // Hue and saturation are tracked here because they are undefined for
    // greys and black; without this the sliders would snap back to zero.
    float m_h = 0.0f, m_s = 0.0f, m_v = 0.0f;
    bool m_updating = false;
};
