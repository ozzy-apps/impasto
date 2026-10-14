#pragma once

#include <QImage>
#include <QWidget>

class IconButton;
class QLabel;
class Slider;
class ToolState;

// A picture to draw from, kept beside the canvas: pan it, zoom it, turn it
// over, and pick colours out of it. It is never part of the artwork.
class ReferencePanel : public QWidget {
    Q_OBJECT
public:
    explicit ReferencePanel(ToolState* tools, QWidget* parent = nullptr);

    bool loadImage(const QString& path);
    void setImage(const QImage& image, const QString& label);
    // Takes the picture away, and forgets it: it does not come back at the next start.
    void clearImage();
    bool hasImage() const { return !m_image.isNull(); }
    const QImage& image() const { return m_image; }
    // What is under a point of the view, for picking and for tests.
    QColor colorAt(const QPointF& viewPos) const;
    QRect viewRect() const;          // the picture area, in this widget's coordinates
    double viewScale() const { return m_scale; }
    QPointF viewCentre() const { return m_centre; }   // image px shown in the middle of the view

protected:
    void showEvent(QShowEvent*) override;

private:
    class View;
    friend class View;

    void openImage();
    void pasteImage();
    void fit();
    void setScale(double scale, const QPointF& aboutViewPos);
    void syncZoom();
    void syncButtons();

    ToolState* m_tools;
    View* m_view;
    QLabel* m_title;
    Slider* m_zoom;
    IconButton* m_pick;
    IconButton* m_remove = nullptr;
    IconButton* m_flip = nullptr;
    IconButton* m_fit = nullptr;
    QImage m_image;
    double m_scale = 1.0;       // view px per image px
    double m_fitScale = 1.0;
    QPointF m_centre;
    bool m_mirrored = false;
    bool m_picking = false;
    bool m_restored = false;
};
