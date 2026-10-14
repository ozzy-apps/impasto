#pragma once

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QString>

class QApplication;
class QPainter;

namespace theme {

inline const QColor workspace(0x1a, 0x1a, 0x1c);
inline const QColor bar(0x26, 0x26, 0x28, 0xf2);
inline const QColor panel(0x2a, 0x2a, 0x2d, 0xfa);
inline const QColor panelBorder(0xff, 0xff, 0xff, 0x1c);
inline const QColor row(0x3a, 0x3a, 0x3e);
inline const QColor rowHover(0x44, 0x44, 0x49);
inline const QColor track(0x4a, 0x4a, 0x50);
inline const QColor text(0xf2, 0xf2, 0xf7);
inline const QColor textDim(0xa0, 0xa0, 0xa8);
inline const QColor accent(0x0a, 0x84, 0xff);

void apply(QApplication& app);
QFont font(int pixelSize, QFont::Weight weight = QFont::Normal);

// Line icons drawn from inline SVG; `name` is one of the keys in Theme.cpp.
QPixmap icon(const QString& name, int size, const QColor& color, qreal dpr);

// Checkerboard used behind transparent thumbnails and swatches.
void drawChecker(QPainter& p, const QRectF& r, int cell = 6);

} // namespace theme
