#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <functional>

class IconButton;
class QLabel;
class QLineEdit;
class QPushButton;
class SegmentBar;
class ValueField;

namespace palettemaker {
class PictureView;
class SwatchStrip;
}

// Makes a palette from a picture, filling the window the way Brush Lab does:
// the picture large, with a mark where each colour was found in it, the
// colours under it (a tap leaves one out), and down the side where the
// picture comes from, how many colours to take and the palette's name. Save
// keeps it with the other palettes; Save and Export also writes it to a file.
class PaletteMaker : public QWidget {
    Q_OBJECT
public:
    enum Source { Canvas = 0, Reference, File };

    explicit PaletteMaker(QWidget* host);

    std::function<QImage()> canvasImage;
    std::function<QImage()> referenceImage;

    void open(Source source = Canvas);
    void setSource(Source source);
    // A picture to take colours from, as a file would give it.
    void setPicture(const QImage& picture, const QString& name);
    bool loadFile(const QString& path, QString* error = nullptr);
    void setCount(int count);
    int count() const;
    void toggle(int index);              // leaves a colour out, or takes it back
    QVector<QColor> found() const { return m_colors; }
    QVector<QColor> kept() const;        // what Save keeps: found, less what was left out
    QVector<QPointF> marks() const { return m_marks; }   // where each was found, 0..1 across the picture
    QString name() const;
    void setName(const QString& name);

    QString save();                      // the new palette's id, or nothing; closes
    QString saveAndExport(const QString& path, QString* error = nullptr);   // .gpl or .swatches
    void cancel();
    QRect swatchRect(int index) const;   // in this widget's coordinates, for the pen test

signals:
    void saved(const QString& paletteId);
    void message(const QString& text);
    void closed();

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void extract();
    void refresh();

    QImage m_picture;
    QString m_pictureName;
    QVector<QColor> m_colors;
    QVector<QPointF> m_marks;
    QSet<int> m_left;                    // left out
    bool m_nameEdited = false;

    SegmentBar* m_source = nullptr;
    IconButton* m_chooseFile = nullptr;
    QLineEdit* m_name = nullptr;
    ValueField* m_count = nullptr;
    QLabel* m_summary = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_export = nullptr;
    palettemaker::PictureView* m_view = nullptr;
    palettemaker::SwatchStrip* m_strip = nullptr;
};
