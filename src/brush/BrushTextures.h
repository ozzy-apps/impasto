#pragma once

#include <QImage>
#include <QString>
#include <QVector>

// Tip shapes and paper grains the artist brought in from image files. Each is
// kept as a 512x512 Grayscale8 PNG next to the brush settings, named after a
// hash of its pixels, so the same image is only ever stored once and a brush
// can refer to it by a name that never changes: "user:<hash>".
namespace brushtex {

struct Entry {
    QString name;    // what a preset stores in `tip` or `grain`
    QString label;   // the file it came from, for menus
    bool grain = false;
};

bool isUser(const QString& name);
QString directory();

// Turns an image file into a tip (white = paint; the shape is cropped,
// centred and, when it is dark on a light ground, inverted) or into a grain
// (stretched to fill, white = raised tooth). Returns its name, or nothing and
// an error.
QString importTip(const QString& imagePath, QString* error);
QString importGrain(const QString& imagePath, QString* error);
QString tipFromImage(const QImage& source, const QString& label, QString* error);
QString grainFromImage(const QImage& source, const QString& label, QString* error);

// Stores an image that is already in its final form (out of a brush file).
QString adopt(const QImage& map, const QString& label, bool grain);
// The negative of a stored image, stored in turn.
QString inverted(const QString& name);

QImage load(const QString& name);   // null when it is not there
QVector<Entry> list();              // everything stored, oldest first

} // namespace brushtex
