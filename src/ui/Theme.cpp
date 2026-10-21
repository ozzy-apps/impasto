#include "ui/Theme.h"

#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QWidget>

namespace theme {

namespace {

// 24x24 stroke icons. Kept as SVG fragments so they stay crisp at any scale.
const QHash<QString, QString>& iconBodies()
{
    static const QHash<QString, QString> bodies = {
        {QStringLiteral("actions"),
         QStringLiteral("<path d='M20.26 6.13A4 4 0 1 1 17.87 3.74L16.9 7.1Z'/>"
                        "<path d='M13.6 10.4L5 19' stroke-width='3'/>")},
        {QStringLiteral("adjust"),
         QStringLiteral("<path d='M4.5 19.5L14 10' stroke-width='2.2'/>"
                        "<path d='M17.5 3.5v3.4M15.8 5.2h3.4M20 10.6v2.4M18.8 11.8h2.4M10.6 3.8v2.4M9.4 5h2.4'/>")},
        {QStringLiteral("transform"), QStringLiteral("<path d='M5.5 3.5l13.2 6.3-5.5 2.1-2.1 5.5z'/>")},
        {QStringLiteral("select"),
         QStringLiteral("<path d='M4 8V5.500A1.500 1.500 0 0 1 5.500 4H8M11 4h2M16 4h2.500A1.500 1.500 0 0 1 20 5.500V8M20 11v2"
                        "M20 16v2.500a1.500 1.500 0 0 1-1.500 1.500H16M13 20h-2M8 20H5.500A1.500 1.500 0 0 1 4 18.500V16M4 13v-2'/>")},
        {QStringLiteral("folder"),
         QStringLiteral("<path d='M3.500 7.500v10a2 2 0 0 0 2 2h13a2 2 0 0 0 2-2v-8a2 2 0 0 0-2-2h-6.500l-2-2.500H5.500a2 2 0 0 0-2 2z'/>")},
        {QStringLiteral("chevron-right"), QStringLiteral("<path d='M9.500 6l6 6-6 6' stroke-width='2'/>")},
        {QStringLiteral("chevron-down"), QStringLiteral("<path d='M6 9.500l6 6 6-6' stroke-width='2'/>")},
        {QStringLiteral("sliders"),
         QStringLiteral("<path d='M4 7h8M18 7h2M4 12h2M12 12h8M4 17h10M20 17h0'/>"
                        "<circle cx='15' cy='7' r='2.300'/><circle cx='9' cy='12' r='2.300'/><circle cx='17' cy='17' r='2.300'/>")},
        {QStringLiteral("brush"),
         QStringLiteral("<path d='M20.6 3.4a1.6 1.6 0 0 1 0 2.3l-8.4 8.4-2.3-2.3 8.4-8.4a1.6 1.6 0 0 1 2.3 0z'/>"
                        "<path d='M9.4 12.6c-2.3-.4-4.3 1.2-4.6 3.4-.2 1.5-.9 2.5-2 3.2 3.800 1.5 8.300.5 9.100-3.200.2-1.100-.4-2.200-2.500-3.400z'/>")},
        {QStringLiteral("smudge"),
         QStringLiteral("<path d='M9 12V5.200a1.600 1.600 0 0 1 3.200 0v5.100l4.900 1c1.100.2 1.900 1.200 1.900 2.300V16a5 5 0 0 1-5 5h-1.700a5 5 0 0 1-4-2l-3-4.100a1.500 1.500 0 0 1 2.300-1.900L9 14.500'/>")},
        {QStringLiteral("eraser"),
         QStringLiteral("<path d='M3.9 15.100L13 6a2 2 0 0 1 2.800 0l4.200 4.200a2 2 0 0 1 0 2.800L13 20H8.700l-4.800-4.900z'/>"
                        "<path d='M9.400 9.600l7 7M13 20h7.500'/>")},
        {QStringLiteral("layers"),
         QStringLiteral("<rect x='3.500' y='8.500' width='12' height='12' rx='2'/>"
                        "<path d='M8.500 8.500v-3a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-3'/>")},
        {QStringLiteral("undo"), QStringLiteral("<path d='M8 5L3.500 9.500 8 14'/><path d='M3.500 9.500H14a6 6 0 0 1 0 12h-3'/>")},
        {QStringLiteral("redo"), QStringLiteral("<path d='M16 5l4.500 4.500L16 14'/><path d='M20.500 9.500H10a6 6 0 0 0 0 12h3'/>")},
        {QStringLiteral("plus"), QStringLiteral("<path d='M12 5v14M5 12h14'/>")},
        {QStringLiteral("check"), QStringLiteral("<path d='M5 12.500l4.500 4.500L19 7.500' stroke-width='2.2'/>")},
        {QStringLiteral("close"), QStringLiteral("<path d='M6 6l12 12M18 6L6 18'/>")},
        {QStringLiteral("eyedropper"),
         QStringLiteral("<path d='M13 7.500l3.500 3.500-8 8H5v-3.500z'/>"
                        "<path d='M14.800 5.700l1.300-1.300a2.300 2.300 0 0 1 3.300 3.300l-1.300 1.300M12 6.500l5.500 5.500'/>")},
        {QStringLiteral("symmetry"), QStringLiteral("<path d='M12 3v18M8.500 7.500 4 12l4.500 4.500zM15.500 7.500 20 12l-4.500 4.500z'/>")},
        {QStringLiteral("picture"),
         QStringLiteral("<rect x='4' y='5' width='16' height='14' rx='2'/><path d='M4.500 16l4.500-4.500 3.500 3.500 2.500-2.500 4.500 4.500'/><circle cx='15.300' cy='9.300' r='1.200'/>")},
        {QStringLiteral("pin"), QStringLiteral("<path d='M9 4h6l-1 6 3.500 3v2h-11v-2l3.500-3-1-6zM12 15v5.500'/>")},
        {QStringLiteral("lock"),
         QStringLiteral("<rect x='5.500' y='11' width='13' height='9.500' rx='2'/><path d='M8.500 11V8a3.500 3.500 0 0 1 7 0v3'/>")},
        {QStringLiteral("clip"), QStringLiteral("<path d='M7 5v8a3 3 0 0 0 3 3h7M14 12.500l3.500 3.500-3.500 3.500'/>")},
        {QStringLiteral("more"),
         QStringLiteral("<circle cx='5.500' cy='12' r='1.300' fill='currentColor'/><circle cx='12' cy='12' r='1.300' fill='currentColor'/>"
                        "<circle cx='18.500' cy='12' r='1.300' fill='currentColor'/>")},
        {QStringLiteral("flip-h"),
         QStringLiteral("<path d='M12 3.500v17' stroke-dasharray='2.500 2.500'/><path d='M8.500 7L3.500 12l5 5zM15.500 7l5 5-5 5z'/>")},
        {QStringLiteral("flip-v"),
         QStringLiteral("<path d='M3.500 12h17' stroke-dasharray='2.500 2.500'/><path d='M7 8.500L12 3.500l5 5zM7 15.500l5 5 5-5z'/>")},
        {QStringLiteral("rotate"), QStringLiteral("<path d='M19.500 12a7.500 7.500 0 1 1-2.600-5.700'/><path d='M17.500 2.500v4.300h-4.300'/>")},
        {QStringLiteral("fit"),
         QStringLiteral("<path d='M4 9V5.500A1.500 1.500 0 0 1 5.500 4H9M15 4h3.500A1.500 1.500 0 0 1 20 5.500V9M20 15v3.500a1.500 1.500 0 0 1-1.500 1.500H15M9 20H5.500A1.500 1.500 0 0 1 4 18.500V15'/>")},
        {QStringLiteral("reset"), QStringLiteral("<path d='M4.500 12a7.500 7.500 0 1 0 2.600-5.700'/><path d='M6.500 2.500v4.300h4.300'/>")},
        {QStringLiteral("save"),
         QStringLiteral("<path d='M12 4v10.500M7.500 10.500l4.500 4.500 4.500-4.500'/>"
                        "<path d='M5 16.500v2a1.500 1.500 0 0 0 1.500 1.500h11a1.500 1.500 0 0 0 1.500-1.500v-2'/>")},
        {QStringLiteral("trash"),
         QStringLiteral("<path d='M4.500 7h15M9.500 7V4.500h5V7M6.500 7l.800 11.600a1.500 1.500 0 0 0 1.500 1.400h6.400a1.500 1.500 0 0 0 1.500-1.400L17.500 7"
                        "M10.200 10.500v6M13.800 10.500v6'/>")},
        {QStringLiteral("help"),
         QStringLiteral("<circle cx='12' cy='12' r='8.500'/><path d='M9.500 9.700a2.500 2.500 0 1 1 3.800 2.100c-.900.500-1.300 1-1.300 1.900'/>"
                        "<circle cx='12' cy='16.700' r='.700' fill='currentColor'/>")},
        {QStringLiteral("info"),
         QStringLiteral("<circle cx='12' cy='12' r='8.500'/><path d='M12 11v5.500'/><circle cx='12' cy='7.800' r='.700' fill='currentColor'/>")},
        {QStringLiteral("warning"),
         QStringLiteral("<path d='M12 4.200L20.500 19h-17z'/><path d='M12 10v4.500'/><circle cx='12' cy='16.700' r='.600' fill='currentColor'/>")},
        // The kinds of gradient.
        {QStringLiteral("gradient-linear"), QStringLiteral("<rect x='3.500' y='3.500' width='17' height='17' rx='2.500'/><path d='M7 12h10M14 9l3 3-3 3'/>")},
        {QStringLiteral("gradient-radial"), QStringLiteral("<rect x='3.500' y='3.500' width='17' height='17' rx='2.500'/><circle cx='12' cy='12' r='1.800'/><circle cx='12' cy='12' r='5.200'/>")},
        {QStringLiteral("gradient-angle"), QStringLiteral("<rect x='3.500' y='3.500' width='17' height='17' rx='2.500'/><path d='M12 12h5.500'/><path d='M17.300 10.500A5.500 5.500 0 1 0 15.900 15.900'/><path d='M14.400 17.100l1.600-1.300-1.300-1.600'/>")},
        {QStringLiteral("gradient-reflected"), QStringLiteral("<rect x='3.500' y='3.500' width='17' height='17' rx='2.500'/><path d='M12 7.500v9M10.500 12H6.500M8.300 10.200L6.500 12l1.800 1.800M13.500 12h4M15.700 10.200l1.800 1.800-1.800 1.800'/>")},
        {QStringLiteral("gradient-diamond"), QStringLiteral("<rect x='3.500' y='3.500' width='17' height='17' rx='2.500'/><path d='M12 6.500l5.500 5.500-5.500 5.500-5.500-5.500z'/><path d='M12 10l2 2-2 2-2-2z'/>")},
        // Points of a shape: one more on the line, one fewer, and a point that bends against one that turns.
        {QStringLiteral("point-add"),
         QStringLiteral("<path d='M3 19.500c2.200-5.200 4-7.300 5.600-8.300M12.400 9.600c2-.500 4.300-.300 7.100.900'/><circle cx='10.400' cy='10.600' r='2.200'/><path d='M17.500 2.500v6M14.500 5.500h6'/>")},
        // A shape's ends: joined by the run to come, or a closed shape cut and parted.
        {QStringLiteral("shape-close"), QStringLiteral("<path d='M6.500 17A7 7 0 1 1 17.500 17'/><path d='M9 18.600h1.600M13.400 18.600H15'/>")},
        {QStringLiteral("shape-open"), QStringLiteral("<path d='M18.500 14A7 7 0 1 1 18.500 10'/><path d='M18.500 14l2.300 1.400M18.500 10l2.300-1.400'/>")},
        {QStringLiteral("point-remove"),
         QStringLiteral("<path d='M3 19.500c2.200-5.200 4-7.300 5.600-8.300M12.400 9.600c2-.500 4.300-.300 7.100.900'/><circle cx='10.400' cy='10.600' r='2.200'/><path d='M14.500 5.500h6'/>")},
        {QStringLiteral("point-smooth"),
         QStringLiteral("<path d='M3.500 18.500c1-6.500 3.500-9.500 6.300-10.500M14.200 8c2.800 1 5.300 4 6.300 10.500'/><circle cx='12' cy='7.500' r='2.200'/><path d='M4.500 7.500h5.300M14.200 7.500h5.300'/>")},
        {QStringLiteral("point-corner"),
         QStringLiteral("<path d='M3.500 19l6.900-9.600M13.600 9.400L20.500 19'/><rect x='9.900' y='5.200' width='4.200' height='4.200'/>")},
        // Sections of the brush lab.
        {QStringLiteral("stroke-path"),
         QStringLiteral("<path d='M5.500 17.500C5.500 9 18.500 15 18.500 6.500'/><circle cx='5.500' cy='18' r='1.500'/><circle cx='18.500' cy='6' r='1.500'/>")},
        // Playing an animation, the animation itself, and a 3D model to draw from.
        {QStringLiteral("play"), QStringLiteral("<path d='M8 5.500v13l10.500-6.500z'/>")},
        {QStringLiteral("pause"), QStringLiteral("<path d='M8.500 5.500v13M15.500 5.500v13'/>")},
        {QStringLiteral("film"), QStringLiteral("<rect x='3.500' y='5.500' width='17' height='13' rx='2'/><path d='M7.500 5.500v13M16.500 5.500v13M3.500 12h4M16.500 12h4'/>")},
        {QStringLiteral("cube"), QStringLiteral("<path d='M12 3.500l7.500 4.200v8.600L12 20.500l-7.500-4.200V7.700z'/><path d='M4.500 7.700L12 12l7.500-4.300M12 12v8.500'/>")},
        {QStringLiteral("stabilise"), QStringLiteral("<path d='M3.500 9c1.800-3.500 3.200 3.500 5 0s3.200 3.500 5 0 3.200 3.500 5 0 2 0 2 0'/><path d='M3.500 16.500h17'/>")},
        {QStringLiteral("taper"), QStringLiteral("<path d='M3 12c5.500-3.800 12-3.800 18 0-6 3.800-12.500 3.800-18 0z'/>")},
        {QStringLiteral("tip-shape"),
         QStringLiteral("<path d='M12 3.500c2 0 2.500 2.300 4.300 3 1.900.700 3.900-.200 4.200 2 .300 2-1.700 2.700-2 4.500-.300 1.900 1.300 3.700-.200 5-1.600 1.300-3.200-.300-5 0"
                        "-1.900.300-3 2.300-4.900 1.400-1.800-.900-.900-2.900-2-4.400-1.100-1.500-3.300-1.700-3-3.700.300-2 2.500-1.600 3.600-3 1.200-1.500 1.200-4.800 5-4.800z'/>")},
        {QStringLiteral("grain"),
         QStringLiteral("<rect x='4' y='4' width='16' height='16' rx='2.500'/>"
                        "<circle cx='8.500' cy='8.500' r='.900' fill='currentColor'/><circle cx='13.500' cy='9.500' r='.900' fill='currentColor'/>"
                        "<circle cx='10' cy='13' r='.900' fill='currentColor'/><circle cx='15.500' cy='14.500' r='.900' fill='currentColor'/>"
                        "<circle cx='8' cy='16.500' r='.900' fill='currentColor'/>")},
        {QStringLiteral("rendering"),
         QStringLiteral("<path d='M12 3.500c3.700 4.400 5.800 7.300 5.800 10a5.800 5.800 0 0 1-11.600 0c0-2.700 2.100-5.600 5.800-10z'/><path d='M9 14a3 3 0 0 0 3 3'/>")},
        {QStringLiteral("wet-mix"),
         QStringLiteral("<path d='M12 3c2.800 3.300 4.400 5.500 4.400 7.500a4.400 4.400 0 0 1-8.800 0c0-2 1.600-4.200 4.400-7.500z'/>"
                        "<path d='M3.500 18c1.400-1.100 2.800-1.100 4.200 0s2.900 1.100 4.300 0 2.900-1.100 4.300 0 2.800 1.100 4.200 0'/>")},
        {QStringLiteral("color-dynamics"),
         QStringLiteral("<circle cx='9' cy='9.500' r='4.700'/><circle cx='15' cy='9.500' r='4.700'/><circle cx='12' cy='14.700' r='4.700'/>")},
        {QStringLiteral("dynamics"),
         QStringLiteral("<path d='M4.500 17a7.500 7.500 0 1 1 15 0'/><path d='M12 17l4.200-5.500'/><circle cx='12' cy='17' r='1.200' fill='currentColor'/>")},
        {QStringLiteral("pen"),
         QStringLiteral("<path d='M4 20l1.200-4.700L16.700 3.800a2.100 2.100 0 0 1 3 3L8.200 18.300z'/><path d='M14.700 5.800l3 3'/>")},
        {QStringLiteral("properties"),
         QStringLiteral("<path d='M9 7h11M9 12h11M9 17h11'/><circle cx='4.800' cy='7' r='.900' fill='currentColor'/>"
                        "<circle cx='4.800' cy='12' r='.900' fill='currentColor'/><circle cx='4.800' cy='17' r='.900' fill='currentColor'/>")},
        {QStringLiteral("pad"),
         QStringLiteral("<path d='M4 17c2.500-6 4.500 2 7-3s3.500-7 9-8'/><path d='M4 20.500h16'/>")},
        {QStringLiteral("link"),
         QStringLiteral("<path d='M10 13.500a4 4 0 0 0 5.700 0l3-3a4 4 0 0 0-5.700-5.700l-1 1'/>"
                        "<path d='M14 10.500a4 4 0 0 0-5.700 0l-3 3a4 4 0 0 0 5.700 5.700l1-1'/>")},
    };
    return bodies;
}

} // namespace

QFont font(int pixelSize, QFont::Weight weight)
{
    QFont f = QApplication::font();
    f.setPixelSize(pixelSize);
    f.setWeight(weight);
    return f;
}

QPixmap icon(const QString& name, int size, const QColor& color, qreal dpr)
{
    static QHash<QString, QPixmap> cache;
    const QString key = QStringLiteral("%1/%2/%3/%4").arg(name).arg(size).arg(color.rgba(), 0, 16).arg(dpr);
    const auto hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();

    const QString hex = color.name(QColor::HexRgb);
    QString body = iconBodies().value(name);
    body.replace(QStringLiteral("currentColor"), hex);
    const QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
                                       "stroke='%1' stroke-width='1.700' stroke-linecap='round' stroke-linejoin='round'>%2</svg>")
                            .arg(hex, body);
    const int px = qRound(size * dpr);
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QSvgRenderer renderer(svg.toUtf8());
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(color.alphaF());
    renderer.render(&p, QRectF(0, 0, px, px));
    p.end();
    pm.setDevicePixelRatio(dpr);
    cache.insert(key, pm);
    return pm;
}

void drawChecker(QPainter& p, const QRectF& r, int cell)
{
    p.save();
    p.setClipRect(r, Qt::IntersectClip);
    p.fillRect(r, QColor(0xd6, 0xd6, 0xd6));
    const QColor dark(0xa8, 0xa8, 0xa8);
    for (int y = 0; y * cell < r.height(); ++y) {
        for (int x = (y & 1); x * cell < r.width(); x += 2)
            p.fillRect(QRectF(r.left() + x * cell, r.top() + y * cell, cell, cell), dark);
    }
    p.restore();
}

namespace {

// An icon from the app's own set, as buttons want it: in the text colour,
// dimmed when the button is not available, sharp on dense screens too.
QIcon lineIcon(const QString& name)
{
    QIcon out;
    for (qreal dpr : {1.0, 2.0}) {
        out.addPixmap(icon(name, 16, text, dpr), QIcon::Normal);
        out.addPixmap(icon(name, 16, QColor(textDim.red(), textDim.green(), textDim.blue(), 130), dpr), QIcon::Disabled);
    }
    return out;
}

// Fusion, except that the buttons of dialogs carry icons drawn like every
// other icon here, rather than whatever the desktop's icon theme supplies.
class AppStyle : public QProxyStyle {
public:
    AppStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int styleHint(StyleHint which, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr, QStyleHintReturn* data = nullptr) const override
    {
        if (which == SH_DialogButtonBox_ButtonsHaveIcons)
            return 1;
        return QProxyStyle::styleHint(which, option, widget, data);
    }

    QIcon standardIcon(StandardPixmap which, const QStyleOption* option = nullptr, const QWidget* widget = nullptr) const override
    {
        switch (which) {
        case SP_DialogOkButton:
        case SP_DialogApplyButton:
        case SP_DialogYesButton:
        case SP_DialogYesToAllButton:
            return lineIcon(QStringLiteral("check"));
        case SP_DialogCancelButton:
        case SP_DialogCloseButton:
        case SP_DialogNoButton:
        case SP_DialogNoToAllButton:
        case SP_DialogAbortButton:
        case SP_DialogIgnoreButton:
            return lineIcon(QStringLiteral("close"));
        case SP_DialogSaveButton:
        case SP_DialogSaveAllButton:
            return lineIcon(QStringLiteral("save"));
        case SP_DialogDiscardButton:
            return lineIcon(QStringLiteral("trash"));
        case SP_DialogOpenButton:
            return lineIcon(QStringLiteral("folder"));
        case SP_DialogResetButton:
        case SP_DialogRetryButton:
            return lineIcon(QStringLiteral("reset"));
        case SP_DialogHelpButton:
            return lineIcon(QStringLiteral("help"));
        default:
            return QProxyStyle::standardIcon(which, option, widget);
        }
    }

    QPixmap standardPixmap(StandardPixmap which, const QStyleOption* option = nullptr, const QWidget* widget = nullptr) const override
    {
        // The pictures beside a message: the same line icons, large.
        const qreal dpr = widget ? widget->devicePixelRatioF() : qApp->devicePixelRatio();
        switch (which) {
        case SP_MessageBoxInformation:
            return icon(QStringLiteral("info"), 40, accent, dpr);
        case SP_MessageBoxQuestion:
            return icon(QStringLiteral("help"), 40, accent, dpr);
        case SP_MessageBoxWarning:
            return icon(QStringLiteral("warning"), 40, QColor(0xff, 0x9f, 0x0a), dpr);
        case SP_MessageBoxCritical:
            return icon(QStringLiteral("warning"), 40, QColor(0xff, 0x45, 0x3a), dpr);
        default:
            return QProxyStyle::standardPixmap(which, option, widget);
        }
    }
};

} // namespace

void apply(QApplication& app)
{
    app.setStyle(new AppStyle);

    QPalette pal;
    pal.setColor(QPalette::Window, QColor(0x2a, 0x2a, 0x2d));
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, QColor(0x1f, 0x1f, 0x22));
    pal.setColor(QPalette::AlternateBase, QColor(0x2a, 0x2a, 0x2d));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, QColor(0x3a, 0x3a, 0x3e));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, QColor(0x3a, 0x3a, 0x3e));
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::PlaceholderText, textDim);
    pal.setColor(QPalette::Disabled, QPalette::Text, textDim);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, textDim);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, textDim);
    app.setPalette(pal);

    app.setStyleSheet(QStringLiteral(R"(
        QToolTip { color: #f2f2f7; background: #3a3a3e; border: 1px solid #55555a; padding: 4px 6px; border-radius: 4px; }
        QMenu { background: #2e2e31; color: #f2f2f7; border: 1px solid #48484d; border-radius: 10px; padding: 6px; }
        QMenu::item { padding: 7px 28px 7px 14px; border-radius: 6px; font-size: 13px; }
        QMenu::item:selected { background: #0a84ff; color: white; }
        QMenu::item:disabled { color: #77777d; }
        QMenu::separator { height: 1px; background: #48484d; margin: 5px 8px; }
        QMenu::indicator { width: 14px; height: 14px; left: 6px; }
        QScrollArea { background: transparent; border: none; }
        QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
        QScrollBar::handle:vertical { background: #5a5a60; border-radius: 2px; min-height: 28px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
        QLineEdit { background: #1f1f22; color: #f2f2f7; border: 1px solid #48484d; border-radius: 6px; padding: 4px 8px;
                    selection-background-color: #0a84ff; }
        QLineEdit:focus { border-color: #0a84ff; }
        QComboBox { background: #3a3a3e; color: #f2f2f7; border: 1px solid #48484d; border-radius: 6px; padding: 4px 10px; }
        QComboBox::drop-down { border: none; width: 18px; }
        QComboBox QAbstractItemView { background: #2e2e31; color: #f2f2f7; border: 1px solid #48484d;
                                      selection-background-color: #0a84ff; outline: none; }
        QSpinBox { background: #1f1f22; color: #f2f2f7; border: 1px solid #48484d; border-radius: 6px; padding: 4px 6px; }
        QPushButton { background: #3a3a3e; color: #f2f2f7; border: none; border-radius: 7px; padding: 7px 16px; font-size: 13px; }
        QPushButton:hover { background: #46464b; }
        QPushButton:pressed { background: #505056; }
        QPushButton:default { background: #0a84ff; color: white; }
        QPushButton:default:hover { background: #2b95ff; }
        QDialog, QMessageBox { background: #2a2a2d; }
        QLabel { color: #f2f2f7; }
    )"));
}

} // namespace theme
