#pragma once

#include <QString>
#include <QStringList>

class CanvasEngine;

namespace layered {
struct Document;
}

// Layered files other programs read and write: Photoshop (PSD, and PSB for
// sides past 30,000 pixels) and OpenRaster (ORA: Krita, GIMP, MyPaint).
// Layers keep their names, order, groups, visibility, opacity and blend
// modes. What a format has no room for is said in `notes`, in words for the
// artist: OpenRaster has no masks and no clipping, so those are baked into
// the pixels; text arrives as pixels in either.
namespace interchange {

bool canOpen(const QString& path);   // by its extension

// Opens a layered file as a new document in place of the open one.
bool open(CanvasEngine& engine, const QString& path, QString* error, QStringList* notes);

// `bits`: 8 or 16 a channel. 8 is what every program reads; 16 keeps the
// smooth gradations Impasto paints with.
bool exportPsd(CanvasEngine& engine, const QString& path, int bits, QString* error, QStringList* notes);
bool exportOra(CanvasEngine& engine, const QString& path, QString* error, QStringList* notes);

bool readPsd(const QString& path, layered::Document* doc, QString* error);
bool readOra(const QString& path, layered::Document* doc, QString* error);

} // namespace interchange
