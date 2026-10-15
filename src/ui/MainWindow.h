#pragma once

#include "engine/CanvasEngine.h"

#include <QElapsedTimer>
#include <QFont>
#include <QMainWindow>
#include <QVector>

class BrushLab;
class BrushPanel;
class CanvasWidget;
class ColorPanel;
class Gallery;
class IconButton;
class LayerPanel;
class Panel;
class Popover;
class ReferencePanel;
class StabilisePanel;
class TimelineBar;
class ModelPanel;
class ShortcutSheet;
class QAbstractButton;
class QLabel;
class Toast;
class ToolState;
class VSlider;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(CanvasEngine* engine, QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openPath(const QString& path);
    void newDefaultCanvas();
    // An untitled canvas is given a name in the gallery folder without asking.
    bool saveDocument();
    QString documentPath() const { return m_path; }
    // The wall of artworks, over the whole window.
    void showGallery();
    void hideGallery();
    void showGalleryAtStart();   // only if there is something on it, and the artist wants that
    Gallery* gallery() const { return m_gallery; }
    ReferencePanel* referencePanel() const { return m_refPanel; }
    ModelPanel* modelPanel() const { return m_modelPanel; }
    // Everything about the current brush, over the whole window, with a pad to try it on.
    void showBrushLab();
    BrushLab* brushLab() const { return m_brushLab; }

    // Animation Assist: the canvas shows a frame at a time, with the timeline along the bottom.
    void setAnimationAssist(bool on);
    TimelineBar* timeline() const { return m_timeline; }

    // Text: set in a font and put on a layer of its own, then handed to the
    // transform tool to be placed. Once placed it is pixels like any other.
    bool placeText(const QString& text, const QFont& font, Qt::Alignment alignment, const QColor& color);

    // Crash recovery: unsaved work is copied to a file of its own every few
    // minutes, and offered back the next time the app starts if this session
    // never got to close properly.
    bool saveRecoveryNow();
    // Copies left behind by sessions that are no longer running (newest first).
    static QStringList leftoverRecoveries(bool includeOwn = false);
    bool recover(const QString& recoveryFile);
    bool offerRecovery();   // asks; true if something was brought back
    // Opens a panel or edit mode by name; used by the --ui-shot screenshot run.
    void showForScreenshot(const QString& what);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void buildTopBar();
    void buildSideBar();
    void buildPopovers();
    void buildShortcuts();
    void layoutOverlays();
    void syncToolButtons();
    void syncSliders();
    void updateTitle();
    void setInterfaceVisible(bool visible);

    void toolButtonClicked(StrokeTool tool, IconButton* button);
    void togglePopover(Popover* popover, QWidget* anchor);
    void closePopovers();
    void setPanelsSuppressed(bool suppressed);   // pinned panels step aside for the gallery or a hidden interface
    void restorePanels();
    void dropRecovery();
    QString recoveryFile() const;
    void savePanels() const;
    void showBubble(QWidget* beside, const QString& text);

    void showActionsMenu();
    void showAdjustMenu();
    bool newCanvas();
    bool openDocument();
    bool saveDocumentAs();
    void exportImage();
    void insertImage();
    bool maybeSave();
    void showShortcuts();
    void showAbout();

    void startTransform();
    void startCrop();
    void addText();
    void startLiquify();
    void resizeImage();
    void startAdjustment(Adjustment type);
    void showShapeBar();
    void showSelectBar(bool show);
    void showGuideBar(bool show);
    void showGradientBar(bool show);
    void showTextBar(bool show);
    void copySelection(bool cut);
    void paste();
    void showPreferences();
    void applyPreferences();
    void finishAdjustment(bool commit);
    void endEditBar();
    void setEditingUi(bool on);
    bool editing() const;

    CanvasEngine* m_engine;
    ToolState* m_tools;
    QWidget* m_host = nullptr;
    CanvasWidget* m_canvas = nullptr;

    Panel* m_topBar = nullptr;
    Panel* m_sideBar = nullptr;
    Panel* m_editBar = nullptr;
    Panel* m_selectBar = nullptr;
    Panel* m_guideBar = nullptr;
    Panel* m_gradientBar = nullptr;
    Panel* m_textBar = nullptr;
    Panel* m_shapePill = nullptr;       // "Edit Circle" offer after a QuickShape
    IconButton* m_shapePillBtn = nullptr;
    Toast* m_toast = nullptr;
    Gallery* m_gallery = nullptr;
    QLabel* m_bubble = nullptr;
    IconButton* m_fileBtn = nullptr;
    IconButton* m_actionsBtn = nullptr;
    IconButton* m_adjustBtn = nullptr;
    IconButton* m_transformBtn = nullptr;
    IconButton* m_guidesBtn = nullptr;
    IconButton* m_steadyBtn = nullptr;
    IconButton* m_modelBtn = nullptr;
    IconButton* m_selectBtn = nullptr;
    IconButton* m_toolBtn[3] = {nullptr, nullptr, nullptr};
    IconButton* m_layersBtn = nullptr;
    QAbstractButton* m_colorBtn = nullptr;
    IconButton* m_pickBtn = nullptr;
    IconButton* m_undoBtn = nullptr;
    IconButton* m_redoBtn = nullptr;
    VSlider* m_sizeSlider = nullptr;
    VSlider* m_opacitySlider = nullptr;

    Popover* m_brushPop = nullptr;
    Popover* m_layerPop = nullptr;
    Popover* m_colorPop = nullptr;
    Popover* m_refPop = nullptr;
    Popover* m_steadyPop = nullptr;
    Popover* m_modelPop = nullptr;
    ModelPanel* m_modelPanel = nullptr;
    ReferencePanel* m_refPanel = nullptr;
    StabilisePanel* m_steadyPanel = nullptr;
    TimelineBar* m_timeline = nullptr;
    void syncTimeline();
    IconButton* m_refBtn = nullptr;
    QWidget* anchorOf(Popover* popover) const;
    BrushPanel* m_brushPanel = nullptr;
    BrushLab* m_brushLab = nullptr;
    ColorPanel* m_colorPanel = nullptr;
    ShortcutSheet* m_shortcuts = nullptr;
    QVector<Popover*> m_suppressed;

    QString m_path;
    QString m_recoveryId;
    quint64 m_recoverySerial = 0;   // history serial the recovery copy is up to date with
    QElapsedTimer m_sinceRecovery;
    Guides::Perspective m_lastPerspective = Guides::Perspective::Off;
    QString m_nameHint;     // what an untitled canvas should be called when it is first saved
    bool m_adjusting = false;
    bool m_adjustTouched = false;   // whether anything about the adjustment has been set
    AdjustParams m_adjust;
    bool m_interfaceVisible = true;
    QImage m_copied;        // what Copy put on the clipboard, and where it came from
    QPoint m_copiedOrigin;
};
