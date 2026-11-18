#pragma once

#include "invoke/InvokeClient.h"
#include "ui/Widgets.h"

#include <QHash>
#include <QTimer>

class CanvasEngine;
class CanvasWidget;
struct LayerInfo;
namespace invokeui {
class GuideRow;
}
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// The Invoke panel: a column along the left of the canvas, beside the side
// bar, in the manner of the other panels. It stays up while painting, so the
// masks can be painted with it open. Every number is a slider, for the pen.
//
// Masks and guides are layers of the picture tagged with what they tell
// Invoke (LayerInfo::invoke): an inpaint mask (where to regenerate), a region
// (a prompt of its own for where it is painted) or a control (lines for a
// ControlNet-LLLite adapter to follow). Generate leaves masks and regions out
// of the picture it sends.
class InvokePanel : public Panel {
    Q_OBJECT
public:
    InvokePanel(CanvasEngine* engine, CanvasWidget* canvas, QWidget* parent = nullptr);
    void setOverlayVisible(bool visible);
    static constexpr int kWidth = 340;

    enum class Guide { None, Inpaint, Region, Control };
    static Guide guideOf(const LayerInfo& layer);
    int addGuide(Guide kind);   // a new layer on top, tagged; its id, 0 when it could not be made

signals:
    void paintMaskRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void paintEvent(QPaintEvent*) override;

private:
    // A number set with a slider, its value shown beside its name.
    struct Setting {
        Slider* slider = nullptr;
        QLabel* pill = nullptr;
        QWidget* row = nullptr;
        double min = 0, max = 1, step = 1, value = 0;
        int decimals = 0;
        QString suffix;
    };
    QWidget* addSetting(Setting& s, const QString& label, const QString& tip, const char* name);
    void setSetting(Setting& s, double value);

    void loadModels(const QJsonArray& models);
    void refresh();
    void refreshOverlay();
    void resetCanvas();
    void resetFrame();
    void paintGuide(int id);
    void refreshGuides();
    void storeGuide(int id, const QJsonObject& invoke, bool now);
    void flushGuides();
    void generate();
    void generateFromKeyboard();
    void receive(const QImage& result);
    void clearResult();
    void accept();
    void saveSettings() const;
    bool hasInpaintMask() const;
    QRect frame() const;
    QSize renderSize(const QSize& frameSize) const;

    CanvasEngine* m_engine;
    CanvasWidget* m_canvas;
    InvokeClient m_client;
    QLineEdit *m_address, *m_token, *m_seed;
    IconButton *m_connect, *m_selectionFrame, *m_fullFrame, *m_lastSeed;
    IconButton *m_addInpaint, *m_addRegion, *m_addControl;
    IconButton *m_preview, *m_discard, *m_close;
    QPushButton *m_generate, *m_cancel, *m_accept;
    QComboBox *m_model, *m_encoder, *m_vae, *m_scheduler, *m_inpaintAdapter;
    QPlainTextEdit *m_prompt, *m_negative;
    Setting m_strength, m_steps, m_guidance, m_size, m_feather;
    QLabel *m_status, *m_frameInfo;
    QWidget* m_resultRow;
    QWidget* m_guideList;
    QLabel* m_guideHint;
    QVector<invokeui::GuideRow*> m_rows;
    QJsonArray m_controlModels;              // the Anima ControlNet-LLLite adapters on the server
    QHash<int, QJsonObject> m_pendingGuides; // typed but not yet written to the layer
    QTimer m_guideTimer;
    QRect m_frame, m_jobFrame;
    QImage m_jobMask, m_result;
    quint32 m_jobSeed = 0;
    bool m_hasLastSeed = false;
    quint32 m_lastSeedValue = 0;
    bool m_overlayVisible = false;
    bool m_previewShown = true;
    bool m_connectionEdited = false;
};
