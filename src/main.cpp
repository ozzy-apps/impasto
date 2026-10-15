#include "SelfTest.h"
#include "brush/BrushPreset.h"
#include "color/Palette.h"
#include "engine/CanvasEngine.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QIcon>
#include <QMessageBox>
#include <QSurfaceFormat>

// The app was called Sketchbook before it was Impasto. The first time
// Impasto starts, it takes over the settings, brushes, palettes, imported
// textures and unsaved work kept under the old name, as a copy: the old
// folder is left as it was. Artworks stay in the gallery folder they are in.
static void carryOverFromSketchbook()
{
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QDir from(config + QStringLiteral("/Sketchbook"));
    const QString to = config + QStringLiteral("/Impasto");
    if (QDir(to).exists() || !from.exists())
        return;
    QDirIterator it(from.absolutePath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        QString relative = from.relativeFilePath(source);
        if (relative == QLatin1String("Sketchbook.conf"))
            relative = QStringLiteral("Impasto.conf");
        else if (relative.startsWith(QLatin1String("Sketchbook/")))
            relative = QStringLiteral("Impasto/") + relative.mid(11);
        const QString target = to + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::copy(source, target);
    }
    // The artworks made so far stay where they are, and the gallery goes on showing them there.
    QSettings st;
    const QString oldGallery = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/Sketchbook");
    if (!st.contains(QStringLiteral("gallery/folder")) && QDir(oldGallery).exists())
        st.setValue(QStringLiteral("gallery/folder"), oldGallery);
}

int main(int argc, char** argv)
{
    QSurfaceFormat fmt;
    fmt.setVersion(4, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    // The engine renders in its own context and the viewport samples the result.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Impasto"));
    QApplication::setOrganizationName(QStringLiteral("Impasto"));
    QApplication::setApplicationVersion(QStringLiteral(IMPASTO_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("io.github.ozzy_apps.Impasto"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));

    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QLatin1String("--selftest"))
        return runSelfTest(args[2]);
    if (args.size() >= 3 && args[1] == QLatin1String("--brush-sheet"))
        return runBrushSheet(args[2]);
    if (args.size() >= 3 && args[1] == QLatin1String("--brush-showcase"))
        return runBrushShowcase(args[2], args.mid(3));
    if (args.size() >= 3 && args[1] == QLatin1String("--interchange-sample"))
        return runInterchangeSample(args[2]);
    if (args.size() >= 4 && args[1] == QLatin1String("--interchange-open"))
        return runInterchangeOpen(args[2], args[3]);
    if (args.size() >= 5 && args[1] == QLatin1String("--make-figure"))
        return runMakeFigure(args[2], args[3], args[4], args.value(5));
    if (args.size() >= 4 && args[1] == QLatin1String("--rig-check"))
        return runRigCheck(args[2], args[3]);
    if (args.size() >= 2 && args[1] == QLatin1String("--devices"))
        return runDeviceReport();
    if (args.size() >= 2 && args[1] == QLatin1String("--bench")) {
        // --bench 8000x8000 times one canvas size instead of the usual three.
        const QStringList wh = args.value(2).toLower().split(QLatin1Char('x'));
        return runBenchmark(wh.size() == 2 ? QSize(wh[0].toInt(), wh[1].toInt()) : QSize());
    }
    if (args.size() >= 3 && args[1] == QLatin1String("--ui-shot"))
        return runUiShot(args[2]);

    carryOverFromSketchbook();   // not for the test runs above: they keep to folders of their own
    theme::apply(app);
    BrushLibrary::instance().load();   // the artist's own brush tweaks
    PaletteLibrary::instance().load();

    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Impasto"), error);
        return 1;
    }

    MainWindow window(&engine);
    QString file;
    for (int i = 1; i < args.size(); ++i) {
        if (!args[i].startsWith(QLatin1Char('-')) && QFileInfo::exists(args[i]))
            file = args[i];
    }
    const bool opened = !file.isEmpty() && window.openPath(file);
    if (!opened)
        window.newDefaultCanvas();
    window.showMaximized();
    // Work a crashed session left behind comes before anything else.
    if (!opened && !window.offerRecovery())
        window.showGalleryAtStart();
    return app.exec();
}
