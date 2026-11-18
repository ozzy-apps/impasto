#pragma once

#include "invoke/InvokeClient.h"
#include "ui/Widgets.h"

class CanvasEngine;
class CanvasWidget;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// The Invoke panel: a column along the left of the canvas, beside the side
// bar, in the manner of the other panels. It stays up while painting, so the
// inpaint mask can be painted with it open. Every number is a slider, for the pen.
class InvokePanel : public Panel {
    Q_OBJECT
public:
    InvokePanel(CanvasEngine* engine, CanvasWidget* canvas, QWidget* parent = nullptr);
    void setOverlayVisible(bool visible);
    static constexpr int kWidth = 340;

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
    void paintMask();
    void generate();
    void generateFromKeyboard();
    void receive(const QImage& result);
    void clearResult();
    void accept();
    void saveSettings() const;
    bool inpainting() const;
    QRect frame() const;
    QSize renderSize(const QSize& frameSize) const;

    CanvasEngine* m_engine;
    CanvasWidget* m_canvas;
    InvokeClient m_client;
    QLineEdit *m_address, *m_token, *m_seed;
    IconButton *m_connect, *m_selectionFrame, *m_fullFrame, *m_paintMask, *m_lastSeed;
    IconButton *m_preview, *m_discard, *m_close;
    QPushButton *m_generate, *m_cancel, *m_accept;
    SegmentBar* m_mode;
    QComboBox *m_model, *m_encoder, *m_vae, *m_scheduler;
    QPlainTextEdit *m_prompt, *m_negative;
    Setting m_strength, m_steps, m_guidance, m_size, m_feather;
    QLabel *m_status, *m_frameInfo;
    QWidget* m_resultRow;
    QRect m_frame, m_jobFrame;
    QImage m_jobMask, m_result;
    quint32 m_jobSeed = 0;
    bool m_hasLastSeed = false;
    quint32 m_lastSeedValue = 0;
    int m_maskLayer = 0;
    int m_layerBeforeMask = 0;
    bool m_overlayVisible = false;
    bool m_previewShown = true;
    bool m_connectionEdited = false;
};
