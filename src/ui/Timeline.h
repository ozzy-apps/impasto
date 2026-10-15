#pragma once

#include "ui/Widgets.h"

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QTimer>

class CanvasEngine;
class QLabel;

// The frames of an animation in a row, as Procreate's Animation Assist shows
// them: tap one to work on it, tap the one you are on for its options, rest
// on one a moment and drag to put it somewhere else, and drag the row itself
// to scroll it.
class FrameStrip : public QWidget {
    Q_OBJECT
public:
    explicit FrameStrip(CanvasEngine* engine, QWidget* parent = nullptr);
    QRectF cellRect(int index) const;   // in this widget, scrolled
    void scrollToFrame(int index);
    QSize sizeHint() const override { return {600, 96}; }

signals:
    void optionsRequested(int index, const QRect& cellInParent);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent*) override;

private:
    int cellWidth() const;
    int indexAt(const QPointF& pos) const;
    int dropSlot(qreal x) const;   // 0..frames: the gap a lifted frame would go into
    const QImage& thumbnail(int index, int id, quint64 key);
    void clampScroll();

    CanvasEngine* m_engine;
    struct Thumb {
        quint64 key = 0;
        QImage image;
    };
    QHash<int, Thumb> m_thumbs;
    qreal m_scroll = 0.0;
    // A press: a tap, a scroll, or (after resting) a frame being carried.
    int m_pressed = -1;
    QPointF m_pressPos, m_lastPos;
    bool m_moved = false, m_lifted = false;
    QTimer m_rest;
};

class TimelineBar : public Panel {
    Q_OBJECT
public:
    TimelineBar(CanvasEngine* engine, QWidget* host);

    bool playing() const { return m_timer.isActive(); }
    void play(bool on);
    FrameStrip* strip() const { return m_strip; }
    enum class Format { Gif, Png, Video, Sequence };
    // Renders every frame and writes it out; `scale` 1 is the canvas size.
    bool exportAnimation(Format format, const QString& path, double scale, QString* error);
    // The order the frames play in, each one as many times as it is held.
    QVector<int> playOrder() const;
    int preferredHeight() const { return 124; }

signals:
    void message(const QString& text);

private:
    void tick();
    void openSettings();
    void openFrameOptions(int index);
    void openExport();
    void syncPlayButton();
    QWidget* edgeAnchor(QWidget* button);

    CanvasEngine* m_engine;
    QWidget* m_host;
    FrameStrip* m_strip;
    IconButton* m_playBtn;
    IconButton* m_settingsBtn;
    IconButton* m_exportBtn;
    IconButton* m_addBtn;
    Popover* m_settingsPop = nullptr;
    Popover* m_framePop = nullptr;
    Popover* m_exportPop = nullptr;
    QTimer m_timer;
    QVector<int> m_sequence;
    int m_position = 0;
};
