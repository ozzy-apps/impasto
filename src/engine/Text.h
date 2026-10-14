#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVector>

// Text as it was typed and set: enough to set it again. A layer that holds
// text keeps this for as long as its pixels are exactly that text, so the
// words, the font and the colour can still be changed afterwards.
struct TextInfo {
    QString text;
    QString family;
    int pixelSize = 48;
    bool bold = false, italic = false;
    int alignment = Qt::AlignLeft;   // Qt::AlignLeft, Qt::AlignHCenter or Qt::AlignRight
    // Room between letters, as a share of the size (0.1 adds a tenth of it
    // after every letter; below 0 they close up), and between lines, as a
    // multiple of what the font itself leaves.
    double letterSpacing = 0.0;
    double lineSpacing = 1.0;
    QColor color = Qt::black;
    QPointF anchor;      // the top of the first line, at its left, middle or right depending on the alignment
    QTransform matrix;   // from where it was typed to where it has since been moved, sized and turned
    bool operator==(const TextInfo&) const = default;

    QFont font() const;
    void setFont(const QFont& font);
};

struct TextLine {
    QString text;
    int start = 0;   // where the line begins in the whole text
    double x = 0, top = 0, baseline = 0, width = 0;
};

namespace text {

// Where every line sits, before the matrix is applied. `lineHeight` is how
// tall each line's box is: the font's own, whatever the line spacing.
QVector<TextLine> layout(const TextInfo& info, QRectF* bounds = nullptr, double* lineHeight = nullptr);
// The box round all of it, as it lies on the canvas.
QPolygonF outline(const TextInfo& info);
// The letters as pixels: the part of them that falls on the canvas, and where that part starts.
QImage render(const TextInfo& info, const QSize& canvas, QPoint* topLeft);

} // namespace text
