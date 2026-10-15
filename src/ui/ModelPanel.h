#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>

class IconButton;
class ModelView;
class QLabel;
class SegmentBar;
class Slider;
class ToolState;

// A 3D model to draw from, kept beside the canvas like the reference picture:
// the built-in figure, posed by its joints, or a model from a file. Turn it,
// light it, pick colours from it, or place what it shows on the canvas as a
// layer to draw over.
class ModelPanel : public QWidget {
    Q_OBJECT
public:
    explicit ModelPanel(ToolState* tools, QWidget* parent = nullptr);

    ModelView* view() const { return m_view; }
    bool openModel(const QString& path);
    void showFigure();
    void setCanvasSize(const QSize& size);
    QImage placement() const;   // the view as a layer the size of the canvas
    // Rigging a model that came without joints: the figure's skeleton fitted
    // into it, to be checked and then bound.
    bool startRig();
    bool finishRig(bool bind);

signals:
    void placeRequested(const QImage& image);
    void message(const QString& text);

protected:
    void showEvent(QShowEvent*) override;

private:
    void sync();
    void keep();   // into the settings, a moment after the last change
    void bindSavedRig();   // a model rigged before comes back rigged

    ToolState* m_tools;
    ModelView* m_view;
    QLabel* m_title;
    SegmentBar* m_modes;
    QWidget* m_poses;
    IconButton* m_twist;
    IconButton* m_rigBtn;
    QWidget* m_tuneRow;
    QWidget* m_rigRow;
    IconButton* m_rigMirror;
    std::vector<QVector3D> m_rigBefore;   // a re-rig cancelled goes back to these
    IconButton* m_hard;
    IconButton* m_floor;
    IconButton* m_pick;
    Slider* m_lens;
    QLabel* m_lensValue;
    QSize m_canvas;
    QTimer m_keep;
    bool m_restored = false;
};
