// Renders an SVG to a square PNG: impasto-svg2png in.svg out.png size
// (run with -platform offscreen where there is no display).
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 4)
        return 1;
    QSvgRenderer svg(args[1]);
    const int size = args[3].toInt();
    if (!svg.isValid() || size <= 0)
        return 1;
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    svg.render(&p);
    p.end();
    return image.save(args[2]) ? 0 : 1;
}
