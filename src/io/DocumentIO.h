#pragma once

#include <QImage>
#include <QSize>
#include <QString>

class CanvasEngine;

// Native documents (.imp) keep every layer at full 16-bit float precision.
// Layout: magic, version, JSON header, PNG preview, then per layer a
// zlib-compressed block holding only its non-empty rectangle and, from
// version 2 on, a block for its mask (8-bit grey, empty when it has none).
namespace docio {

inline const char* const kExtension = "imp";
// What documents were called while the app was Sketchbook: the very same
// format, and they open as they always did.
inline const char* const kOldExtension = "sbk";
inline bool isDocumentPath(const QString& path)
{
    return path.endsWith(QLatin1Char('.') + QLatin1String(kExtension), Qt::CaseInsensitive)
           || path.endsWith(QLatin1Char('.') + QLatin1String(kOldExtension), Qt::CaseInsensitive);
}

bool save(CanvasEngine& engine, const QString& path, QString* error);
bool load(CanvasEngine& engine, const QString& path, QString* error);

// What a document is, without loading it: read from its header and the small
// picture stored beside it. Quick enough to do for a whole folder.
struct Summary {
    bool ok = false;
    QSize size;
    int layers = 0;
    QImage preview;   // at most 512 px on its longer side
};
Summary peek(const QString& path);

// Flattened export; the format follows the file extension (png, jpg, webp...).
bool exportImage(CanvasEngine& engine, const QString& path, QString* error);

// Reads an image file honouring EXIF orientation.
QImage readImage(const QString& path, QString* error);

} // namespace docio
