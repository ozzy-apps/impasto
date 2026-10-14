#include "engine/Text.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

QFont TextInfo::font() const
{
    QFont f(family);
    f.setPixelSize(std::max(1, pixelSize));
    f.setBold(bold);
    f.setItalic(italic);
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setHintingPreference(QFont::PreferNoHinting);   // letter shapes as designed, at any size
    if (letterSpacing != 0.0)
        f.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing * std::max(1, pixelSize));
    return f;
}

void TextInfo::setFont(const QFont& font)
{
    family = font.family();
    pixelSize = std::max(1, font.pixelSize());
    bold = font.bold();
    italic = font.italic();
}

namespace text {

QVector<TextLine> layout(const TextInfo& info, QRectF* bounds, double* lineHeight)
{
    const QFontMetricsF metrics(info.font());
    // Line spacing sets how far apart the lines are, not how tall each is:
    // a line's box is always the font's own height, set close or far apart.
    const double height = metrics.lineSpacing();
    const double step = height * std::clamp(info.lineSpacing, 0.1, 10.0);
    // The room after the last letter of a line is no part of its width.
    const double trailing = info.letterSpacing * std::max(1, info.pixelSize);
    QVector<TextLine> lines;
    QRectF box;
    int start = 0;
    const QStringList parts = info.text.split(QLatin1Char('\n'));
    for (int i = 0; i < parts.size(); ++i) {
        TextLine line;
        line.text = parts[i];
        line.start = start;
        line.width = line.text.isEmpty() ? 0.0 : std::max(0.0, metrics.horizontalAdvance(line.text) - trailing);
        line.x = info.alignment & Qt::AlignHCenter ? info.anchor.x() - line.width / 2.0
                 : info.alignment & Qt::AlignRight ? info.anchor.x() - line.width
                                                   : info.anchor.x();
        line.top = info.anchor.y() + i * step;
        line.baseline = line.top + metrics.ascent();
        box |= QRectF(line.x, line.top, std::max(line.width, 1.0), height);
        lines.push_back(line);
        start += int(parts[i].size()) + 1;
    }
    if (bounds)
        *bounds = box;
    if (lineHeight)
        *lineHeight = height;
    return lines;
}

QPolygonF outline(const TextInfo& info)
{
    QRectF bounds;
    layout(info, &bounds);
    return info.matrix.map(QPolygonF(bounds));
}

QImage render(const TextInfo& info, const QSize& canvas, QPoint* topLeft)
{
    QRectF bounds;
    const QVector<TextLine> lines = layout(info, &bounds);
    const double pad = std::ceil(info.pixelSize * 0.05) + 2.0;   // the soft edge of every letter
    // Heavy, ornate and accented letters can reach past the height the font
    // gives a line too: the letters themselves decide what is kept, so that
    // nothing of them is ever cut off.
    const QFont font = info.font();
    QPainterPath ink;
    for (const TextLine& line : lines)
        ink.addText(QPointF(line.x, line.baseline), font, line.text);
    const QRectF reach = bounds | ink.boundingRect();
    const QRect covered = info.matrix.mapRect(reach.adjusted(-pad, -pad, pad, pad)).toAlignedRect() & QRect(QPoint(0, 0), canvas);
    if (covered.isEmpty() || info.text.trimmed().isEmpty())
        return {};
    QImage image(covered.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    p.translate(-covered.topLeft());
    p.setTransform(info.matrix, true);
    p.setFont(font);
    p.setPen(info.color);
    for (const TextLine& line : lines)
        p.drawText(QPointF(line.x, line.baseline), line.text);
    p.end();
    if (topLeft)
        *topLeft = covered.topLeft();
    return image;
}

} // namespace text
