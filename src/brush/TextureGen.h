#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

// Procedural brush tips and paper grains. Generated at startup so the app ships
// without binary assets; every image is 512x512 Grayscale8.
namespace texgen {

constexpr int kSize = 512;

QStringList tipNames();
QStringList grainNames();
// What a tip or a grain is called in the brush lab.
QString tipTitle(const QString& name);
QString grainTitle(const QString& name);

QImage tip(const QString& name);     // white = full coverage
QImage grain(const QString& name);   // tileable, white = raised tooth

} // namespace texgen
