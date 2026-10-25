#pragma once

#include <QSize>

#include <QString>

// Headless checks that drive the engine without a window. Used from the
// command line: `impasto --selftest out.png` and `--brush-sheet out.png`.
int runSelfTest(const QString& outPath);
int runBrushSheet(const QString& outPath);
// Strokes at working size on paper, for judging how brushes really look.
int runBrushShowcase(const QString& outPath, const QStringList& brushIds);
// A document with a bit of everything, written as OpenRaster and as 8- and 16-bit PSD.
int runInterchangeSample(const QString& dir);
// Opens a PSD or OpenRaster file and writes the picture it makes.
int runInterchangeOpen(const QString& path, const QString& outPng);
int runBenchmark(const QSize& only = QSize());   // one size (W x H) on its own, or the usual three
// Lists the pointing devices Qt registered and whether they report pressure/tilt.
int runDeviceReport();
// Opens the real window offscreen-style, paints a little and grabs screenshots
// of the interface (prefix-main.png, prefix-brushes.png, ...).
int runUiShot(const QString& outPrefix);
// Paints sample documents with the engine, captures the running UI, and checks
// the gallery About menu, version, links, and both dialog dismissal paths.
int runProductShots(const QString& outputDir);
int runRigCheck(const QString& modelPath, const QString& outPrefix);
int runMakeFigure(const QString& modelPath, const QString& outGlb, const QString& name, const QString& settingsFile);
