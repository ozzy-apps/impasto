#include "SelfTest.h"

#include "brush/BrushPreset.h"
#include "brush/BrushTextures.h"
#include "brush/TextureGen.h"
#include "color/Harmony.h"
#include "color/Palette.h"
#include "engine/CanvasEngine.h"
#include "gfx/Gl.h"
#include "io/AnimationExport.h"
#include "io/DocumentIO.h"
#include "io/Interchange.h"
#include "io/Layered.h"
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include "io/Zip.h"
#include "shape/Perspective.h"
#include "shape/QuickShape.h"
#include "model/Figures.h"
#include "model/Model3D.h"
#include "ui/BrushLab.h"
#include "ui/CanvasWidget.h"
#include "ui/ColorPanel.h"
#include "ui/ColorPickers.h"
#include "ui/PaletteGrid.h"
#include "ui/Gallery.h"
#include "ui/LayerPanel.h"
#include "ui/NewCanvasDialog.h"
#include "ui/ReferencePanel.h"
#include "ui/ShortcutSheet.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Timeline.h"
#include "ui/ModelPanel.h"
#include "ui/ModelView.h"

#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QMenu>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QShortcut>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFloat16>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScreen>
#include <QScrollBar>
#include <QSpinBox>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QProcess>
#include <QPushButton>
#include <QTabletEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QWindow>
#include <QWheelEvent>
#include <QOpenGLWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <cmath>
#include <numbers>
#include <functional>
#include <csignal>
#include <cstdlib>
#if defined(Q_OS_LINUX)
#include <execinfo.h>
#include <unistd.h>
#endif

namespace {

void wave(CanvasEngine& e, StrokeTool tool, const BrushPreset& preset, float size, float opacity,
          const QColor& color, QPointF from, QPointF to, float amplitude, float tilt = 0.0f)
{
    constexpr int n = 90;
    for (int i = 0; i <= n; ++i) {
        const float t = float(i) / float(n);
        StrokeSample s;
        s.pos = from + (to - from) * t + QPointF(0, std::sin(t * 9.0f) * amplitude);
        s.pressure = 0.08f + 0.92f * std::pow(std::sin(t * 3.14159f), 0.7f);
        s.tiltX = tilt;
        s.time = i * 0.008;
        if (i == 0)
            e.beginStroke(tool, preset, size, opacity, color, s);
        else
            e.strokeTo(s);
    }
    e.endStroke();
}


// ---- synthetic hand-drawn strokes for the shape recogniser

constexpr double kTau = 6.283185307179586;

std::vector<QPointF> densify(const std::vector<QPointF>& v, bool closed, double step = 3.0)
{
    std::vector<QPointF> out;
    const size_t n = closed ? v.size() : v.size() - 1;
    for (size_t k = 0; k < n; ++k) {
        const QPointF a = v[k], b = v[(k + 1) % v.size()];
        const int m = std::max(1, int(std::hypot(b.x() - a.x(), b.y() - a.y()) / step));
        for (int i = 0; i < m; ++i)
            out.push_back(a + (b - a) * (double(i) / m));
    }
    out.push_back(closed ? v.front() : v.back());
    return out;
}

// Rounds corners the way a moving hand does.
std::vector<QPointF> soften(const std::vector<QPointF>& p, int window)
{
    std::vector<QPointF> out(p.size());
    const int n = int(p.size());
    for (int i = 0; i < n; ++i) {
        QPointF sum;
        int count = 0;
        for (int d = -window; d <= window; ++d) {
            const int j = i + d;
            if (j < 0 || j >= n)
                continue;
            sum += p[size_t(j)];
            ++count;
        }
        out[size_t(i)] = sum / double(count);
    }
    return out;
}

// Slow sideways drift, like an unsteady line.
std::vector<QPointF> wobble(const std::vector<QPointF>& p, double amplitude, double phase)
{
    std::vector<QPointF> out = p;
    double travelled = 0.0;
    for (size_t i = 1; i + 1 < p.size(); ++i) {
        const QPointF d = p[i + 1] - p[i - 1];
        const double len = std::hypot(d.x(), d.y());
        travelled += std::hypot(p[i].x() - p[i - 1].x(), p[i].y() - p[i - 1].y());
        if (len < 1e-9)
            continue;
        const QPointF normal(-d.y() / len, d.x() / len);
        const double w = std::sin(travelled * 0.021 + phase) + 0.6 * std::sin(travelled * 0.057 + phase * 2.3);
        out[i] = p[i] + normal * (amplitude * w);
    }
    return out;
}

std::vector<QPointF> ellipseStroke(QPointF c, double rx, double ry, double rotDeg, double sweepDeg, double startDeg = 20.0)
{
    std::vector<QPointF> out;
    const double rot = rotDeg * kTau / 360.0;
    for (double a = 0.0; a <= sweepDeg; a += 2.0) {
        const double t = (startDeg + a) * kTau / 360.0;
        const double x = rx * std::cos(t), y = ry * std::sin(t);
        out.emplace_back(c.x() + x * std::cos(rot) - y * std::sin(rot), c.y() + x * std::sin(rot) + y * std::cos(rot));
    }
    return out;
}

std::vector<QPointF> boxCorners(QPointF c, double w, double h, double rotDeg)
{
    const double rot = rotDeg * kTau / 360.0;
    std::vector<QPointF> out;
    for (const QPointF& q : {QPointF(-w / 2, -h / 2), QPointF(w / 2, -h / 2), QPointF(w / 2, h / 2), QPointF(-w / 2, h / 2)})
        out.emplace_back(c.x() + q.x() * std::cos(rot) - q.y() * std::sin(rot), c.y() + q.x() * std::sin(rot) + q.y() * std::cos(rot));
    return out;
}

// A sine wave running to the right: `bends` half-periods of it, optionally
// growing in height along the way, the way a hand loosens up.
std::vector<QPointF> waveStroke(QPointF from, double length, double height, double periods, double grow = 0.0)
{
    std::vector<QPointF> out;
    const int n = int(length * (1.0 + periods)) / 3;
    for (int i = 0; i <= n; ++i) {
        const double t = double(i) / n;
        out.emplace_back(from.x() + length * t, from.y() - std::sin(t * periods * kTau) * height * (1.0 + grow * t));
    }
    return out;
}

// The squiggle people draw to fill a space: upright runs joined by half-circles,
// alternately over the top and under the bottom.
std::vector<QPointF> meanderStroke(QPointF from, double radius, double run, int bends)
{
    std::vector<QPointF> out;
    QPointF at = from;
    for (int b = 0; b < bends; ++b) {
        const double up = b % 2 == 0 ? -1.0 : 1.0;   // y runs downwards
        for (double d = 0.0; d < run; d += 3.0)
            out.emplace_back(at.x(), at.y() + up * d);
        at.ry() += up * run;
        const QPointF centre(at.x() + radius, at.y());
        const int steps = int(radius * 3.14159 / 3.0);
        for (int i = 0; i <= steps; ++i) {
            const double a = 3.14159265 * i / steps;
            out.emplace_back(centre.x() - radius * std::cos(a), centre.y() + up * radius * std::sin(a));
        }
        at.rx() += 2.0 * radius;
    }
    return out;
}

std::vector<QPointF> spiralStroke(QPointF c, double fromRadius, double toRadius, double sweepDeg)
{
    std::vector<QPointF> out;
    const int n = int(sweepDeg * 1.2);
    for (int i = 0; i <= n; ++i) {
        const double t = double(i) / n, a = t * sweepDeg * kTau / 360.0, r = fromRadius + (toRadius - fromRadius) * t;
        out.emplace_back(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
    }
    return out;
}

// Starts a closed stroke part-way along its first side, as people tend to.
std::vector<QPointF> closedFrom(std::vector<QPointF> v, double along)
{
    const QPointF start = v[0] + (v[1] - v[0]) * along;
    v.push_back(v[0]);
    v[0] = start;
    v.push_back(start);
    return v;
}

// The pen held at the end of the stroke, waiting for the shape to snap: it
// never stays quite still, and every wander of a pixel or two is one more
// point on the stroke.
std::vector<QPointF> resting(std::vector<QPointF> stroke, double wander, quint32 seed, int points = 14)
{
    QPointF at = stroke.back();
    const QPointF home = at;
    quint32 s = seed * 2654435761u + 12345u;
    auto next = [&] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return double(s & 0xffffu) / 65535.0 * 2.0 - 1.0;
    };
    for (int i = 0; i < points; ++i) {
        at += QPointF(next(), next()) * (wander * 0.8);
        // Never far from where it came to rest.
        const QPointF off = at - home;
        const double farthest = std::hypot(off.x(), off.y());
        if (farthest > wander)
            at = home + off * (wander / farthest);
        stroke.push_back(at);
    }
    return stroke;
}

// A hand is never quite steady: every point lands a little off where it was aimed.
std::vector<QPointF> shaky(std::vector<QPointF> stroke, double amount, quint32 seed)
{
    quint32 s = seed * 2246822519u + 977u;
    auto next = [&] {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return double(s & 0xffffu) / 65535.0 * 2.0 - 1.0;
    };
    for (QPointF& p : stroke)
        p += QPointF(next(), next()) * amount;
    return stroke;
}

int runShapeTests()
{
    using Kind = QuickShape::Kind;
    struct Case {
        const char* what;
        std::vector<QPointF> stroke;
        Kind kind;
        const char* name;   // expected name, or nullptr when only the kind matters
        double rest = 0.0;  // how far the pen wandered while it waited at the end, if it did
    };
    // A fast hand leaves few points: every `step`-th one of a finely drawn stroke, and always its last.
    auto sparse = [](const std::vector<QPointF>& p, int step) {
        std::vector<QPointF> out;
        for (size_t i = 0; i < p.size(); i += size_t(step))
            out.push_back(p[i]);
        if (out.back() != p.back())
            out.push_back(p.back());
        return out;
    };
    auto hand = [](const std::vector<QPointF>& p, double amp, double phase, int smooth = 4) {
        return wobble(soften(p, smooth), amp, phase);
    };
    const QPointF c(600, 500);
    const std::vector<Case> cases = {
        {"circle, overshooting its start", hand(ellipseStroke(c, 200, 200, 0, 385), 4, 0.3), Kind::Ellipse, "Circle"},
        {"circle with a gap", hand(ellipseStroke(c, 180, 180, 0, 338), 4, 1.1), Kind::Ellipse, "Circle"},
        {"small circle", hand(ellipseStroke(c, 22, 21, 0, 365), 0.8, 0.2, 1), Kind::Ellipse, "Circle"},
        {"ellipse, tilted 30°", hand(ellipseStroke(c, 300, 140, 30, 362), 5, 2.0), Kind::Ellipse, "Ellipse"},
        {"flat ellipse", hand(ellipseStroke(c, 320, 90, 0, 360), 3, 0.7), Kind::Ellipse, "Ellipse"},
        {"rectangle", hand(densify(closedFrom(boxCorners(c, 420, 250, 0), 0.4), false), 4, 0.5, 6), Kind::Rectangle, "Rectangle"},
        {"rectangle, tilted 25°", hand(densify(closedFrom(boxCorners(c, 380, 200, 25), 0.6), false), 4, 1.7, 6), Kind::Rectangle, "Rectangle"},
        {"near-square", hand(densify(closedFrom(boxCorners(c, 300, 288, 2), 0.5), false), 3, 2.6, 6), Kind::Rectangle, "Square"},
        {"triangle", hand(densify(closedFrom({QPointF(400, 700), QPointF(820, 690), QPointF(590, 300)}, 0.3), false), 4, 0.9, 6), Kind::Polygon, "Triangle"},
        {"slanted quadrilateral", hand(densify(closedFrom({QPointF(300, 300), QPointF(760, 360), QPointF(900, 700), QPointF(380, 640)}, 0.5), false), 4, 1.4, 6), Kind::Polygon, "Quadrilateral"},
        {"pentagon", hand(densify(closedFrom({QPointF(600, 250), QPointF(840, 430), QPointF(750, 720), QPointF(450, 720), QPointF(360, 430)}, 0.5), false), 3, 2.2, 5), Kind::Polygon, "Polygon"},
        {"line", hand(densify({QPointF(200, 300), QPointF(900, 620)}, false), 5, 0.4), Kind::Line, nullptr},
        {"line with a hook at the end", hand(densify({QPointF(200, 600), QPointF(880, 380), QPointF(872, 392)}, false), 3, 1.9, 2), Kind::Line, nullptr},
        {"shallow arc", hand(ellipseStroke(c, 400, 400, 0, 70, 235), 3, 0.6), Kind::Arc, nullptr},
        {"half-circle arc", hand(ellipseStroke(c, 220, 220, 0, 185, 180), 4, 1.2), Kind::Arc, nullptr},
        {"S-bend", hand(waveStroke(QPointF(250, 500), 700, 150, 1.0), 3, 0.5), Kind::Curve, nullptr},
        {"meander, seven round bends", hand(meanderStroke(QPointF(200, 600), 60, 90, 7), 3, 1.3), Kind::Curve, nullptr},
        {"meander with tighter bends", hand(meanderStroke(QPointF(200, 600), 34, 150, 6), 2, 0.4), Kind::Curve, nullptr},
        {"lazy wave, three periods", hand(waveStroke(QPointF(150, 500), 900, 45, 3.0, 0.4), 3, 1.9), Kind::Curve, nullptr},
        {"tightening spiral", hand(spiralStroke(c, 260, 70, 560), 3, 2.1), Kind::Curve, nullptr},
        // The same kinds of stroke as a pen really ends them: resting where it stopped, never quite still.
        {"S-bend, pen resting at the end", resting(hand(waveStroke(QPointF(250, 500), 700, 150, 1.0), 3, 0.5), 5.0, 1), Kind::Curve, nullptr, 5.0},
        {"meander, pen resting", resting(hand(meanderStroke(QPointF(200, 600), 60, 90, 7), 3, 1.3), 7.0, 2), Kind::Curve, nullptr, 7.0},
        {"lazy wave, pen resting, zoomed out", resting(hand(waveStroke(QPointF(150, 500), 900, 45, 3.0, 0.4), 3, 1.9), 11.0, 3, 20), Kind::Curve, nullptr, 11.0},
        {"spiral, pen resting", resting(hand(spiralStroke(c, 260, 70, 560), 3, 2.1), 6.0, 4), Kind::Curve, nullptr, 6.0},
        {"shallow arc, pen resting", resting(hand(ellipseStroke(c, 400, 400, 0, 70, 235), 3, 0.6), 6.0, 5), Kind::Arc, nullptr, 6.0},
        {"half-circle arc, pen resting", resting(hand(ellipseStroke(c, 220, 220, 0, 185, 180), 4, 1.2), 8.0, 6), Kind::Arc, nullptr, 8.0},
        {"line, pen resting", resting(hand(densify({QPointF(200, 300), QPointF(900, 620)}, false), 5, 0.4), 7.0, 7), Kind::Line, nullptr, 7.0},
        {"small S-bend, pen resting", resting(hand(waveStroke(QPointF(400, 500), 160, 40, 1.0), 1, 0.5, 2), 4.0, 8), Kind::Curve, nullptr, 4.0},
        {"circle, pen resting", resting(hand(ellipseStroke(c, 200, 200, 0, 372), 4, 0.3), 6.0, 9), Kind::Ellipse, "Circle", 6.0},
        {"rectangle, pen resting", resting(hand(densify(closedFrom(boxCorners(c, 420, 250, 0), 0.4), false), 4, 0.5, 6), 6.0, 10), Kind::Rectangle, "Rectangle", 6.0},
        {"L shape, pen resting", resting(hand(densify({QPointF(300, 250), QPointF(310, 700), QPointF(800, 690)}, false), 4, 0.8, 5), 6.0, 11), Kind::Polyline, nullptr, 6.0},
        // Small on the canvas, and drawn with an ordinary unsteady hand.
        {"small S-bend", hand(waveStroke(QPointF(400, 500), 160, 40, 1.0), 1, 0.5, 2), Kind::Curve, nullptr},
        {"small S-bend, shaky hand", shaky(hand(waveStroke(QPointF(400, 500), 200, 36, 1.0), 1, 0.9, 2), 0.8, 21), Kind::Curve, nullptr},
        {"S-bend, shaky hand, pen resting", resting(shaky(hand(waveStroke(QPointF(250, 500), 700, 150, 1.0), 3, 0.5), 1.2, 22), 5.0, 13), Kind::Curve, nullptr, 5.0},
        {"long gentle wave, shaky hand", shaky(hand(waveStroke(QPointF(100, 500), 1400, 60, 2.0), 3, 0.2), 1.5, 23), Kind::Curve, nullptr},
        {"small hook (a J), shaky hand", shaky(hand(spiralStroke(QPointF(500, 500), 60, 44, 200), 1, 0.4, 2), 0.7, 24), Kind::Arc, nullptr},
        {"small zigzag, shaky hand", shaky(hand(densify({QPointF(400, 520), QPointF(440, 440), QPointF(480, 525), QPointF(520, 445), QPointF(560, 520)}, false), 1, 0.7, 2), 0.7, 25), Kind::Polyline, nullptr},
        {"small L, shaky hand", shaky(hand(densify({QPointF(400, 400), QPointF(404, 500), QPointF(500, 498)}, false), 1, 0.8, 2), 0.8, 26), Kind::Polyline, nullptr},
        {"S-bend drawn fast, a point every 20 px", sparse(hand(waveStroke(QPointF(250, 500), 700, 150, 1.0), 3, 0.5), 7), Kind::Curve, nullptr},
        {"meander drawn fast, pen resting", resting(sparse(hand(meanderStroke(QPointF(200, 600), 60, 90, 7), 3, 1.3), 5), 6.0, 12), Kind::Curve, nullptr, 6.0},
        {"zigzag with rounded corners", hand(densify({QPointF(200, 600), QPointF(400, 300), QPointF(600, 620), QPointF(800, 310), QPointF(1000, 600)}, false), 3, 0.7, 12), Kind::Polyline, nullptr},
        {"L shape", hand(densify({QPointF(300, 250), QPointF(310, 700), QPointF(800, 690)}, false), 4, 0.8, 5), Kind::Polyline, nullptr},
        {"zigzag", hand(densify({QPointF(200, 600), QPointF(400, 300), QPointF(600, 620), QPointF(800, 310), QPointF(1000, 600)}, false), 4, 2.4, 5), Kind::Polyline, nullptr},
    };

    int failures = 0;
    for (const Case& t : cases) {
        // As the viewport does it: what the pen wandered through while it waited is left out.
        const QuickShape got = QuickShape::recognize(t.rest > 0.0 ? QuickShape::withoutRest(t.stroke, 2.0 * t.rest) : t.stroke);
        const bool ok = got.kind == t.kind && (!t.name || got.name() == QLatin1String(t.name));
        failures += !ok;
        QString detail = got.name();
        if (got.kind == Kind::Polyline || got.kind == Kind::Polygon)
            detail += QStringLiteral(" (%1 points)").arg(got.pts.size());
        if (got.kind == Kind::Curve) {
            // How far the clean curve strays from the stroke it replaces, at worst.
            const std::vector<QPointF> line = QuickShape::smoothThrough(got.pts, 2.0);
            double worst = 0.0, minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
            for (const QPointF& q : t.stroke) {
                double nearest = 1e18;
                for (const QPointF& on : line)
                    nearest = std::min(nearest, std::hypot(q.x() - on.x(), q.y() - on.y()));
                worst = std::max(worst, nearest);
                minX = std::min(minX, q.x()), maxX = std::max(maxX, q.x());
                minY = std::min(minY, q.y()), maxY = std::max(maxY, q.y());
            }
            const double size = std::hypot(maxX - minX, maxY - minY);
            detail += QStringLiteral(" (%1 points, within %2 px of the stroke)").arg(got.pts.size()).arg(worst, 0, 'f', 1);
            if (worst > 0.02 * size + 4.0 || got.pts.size() > 28) {
                ++failures;
                detail += QStringLiteral("  <- strays too far");
            }
        }
        if (got.kind == Kind::Ellipse || got.kind == Kind::Rectangle)
            detail += QStringLiteral(" (%1 x %2, %3°)").arg(got.rx * 2, 0, 'f', 0).arg(got.ry * 2, 0, 'f', 0).arg(got.rot, 0, 'f', 1);
        qInfo().noquote() << QStringLiteral("  %1 %2 -> %3").arg(ok ? "ok    " : "FAILED").arg(QString::fromUtf8(t.what), -32).arg(detail);
    }

    // A near-horizontal line straightens; a 20-degree one is left alone.
    QuickShape flat = QuickShape::recognize(densify({QPointF(100, 400), QPointF(900, 412)}, false));
    QuickShape slanted = QuickShape::recognize(densify({QPointF(100, 400), QPointF(900, 690)}, false));
    const bool snapOk = flat.kind == Kind::Line && std::abs(flat.pts[1].y() - flat.pts[0].y()) < 1e-6
                        && slanted.kind == Kind::Line && std::abs(slanted.pts[1].y() - 690.0) < 2.0;
    failures += !snapOk;
    qInfo().noquote() << QStringLiteral("  %1 line angle snapping").arg(snapOk ? "ok    " : "FAILED");

    // ---- Paths: any shape as points that can be added, removed, and made
    // smooth or sharp. Turning a shape into one must not move its line.
    auto outline = [](const QuickShape& q, double step = 1.0) {
        std::vector<QPointF> out;
        for (const PathPoint& pt : q.path(step))
            out.push_back(pt.pos);
        return out;
    };
    // How far the two lines stray from each other at most.
    auto gap = [](const std::vector<QPointF>& a, const std::vector<QPointF>& b) {
        auto oneWay = [](const std::vector<QPointF>& from, const std::vector<QPointF>& to) {
            double worst = 0.0;
            for (const QPointF& q : from) {
                double nearest = std::numeric_limits<double>::max();
                for (size_t i = 0; i + 1 < to.size(); ++i) {
                    const QPointF ab = to[i + 1] - to[i], aq = q - to[i];
                    const double len2 = ab.x() * ab.x() + ab.y() * ab.y();
                    const double f = len2 > 1e-12 ? std::clamp((aq.x() * ab.x() + aq.y() * ab.y()) / len2, 0.0, 1.0) : 0.0;
                    nearest = std::min(nearest, std::hypot(aq.x() - ab.x() * f, aq.y() - ab.y() * f));
                }
                worst = std::max(worst, nearest);
            }
            return worst;
        };
        return std::max(oneWay(a, b), oneWay(b, a));
    };
    auto corners = [](const QuickShape& q) {
        int n = 0;
        for (const PathPoint& pt : q.path(1.0))
            n += pt.corner;
        return n;
    };
    auto pathCheck = [&](const char* what, bool ok, const QString& detail = {}) {
        failures += !ok;
        qInfo().noquote() << QStringLiteral("  %1 %2%3").arg(ok ? "ok    " : "FAILED").arg(QString::fromUtf8(what), -32).arg(detail.isEmpty() ? QString() : QStringLiteral(" -> ") + detail);
    };

    QuickShape curve;
    curve.kind = Kind::Curve;
    curve.pts = {QPointF(200, 600), QPointF(330, 380), QPointF(520, 560), QPointF(640, 300), QPointF(900, 420), QPointF(980, 700)};
    const QuickShape curvePath = curve.toPath();
    const double curveGap = gap(outline(curve), outline(curvePath));
    pathCheck("curve as a path", curvePath.kind == Kind::Path && curvePath.nodes.size() == 6 && !curvePath.closed() && curveGap < 0.02
                                     && curvePath.name() == QLatin1String("Curve"),
              QStringLiteral("the same line to within %1 px").arg(curveGap, 0, 'f', 4));

    QuickShape pentagon;
    pentagon.kind = Kind::Polygon;
    pentagon.pts = {QPointF(600, 250), QPointF(840, 430), QPointF(750, 720), QPointF(450, 720), QPointF(360, 430)};
    QuickShape box;
    box.kind = Kind::Rectangle;
    box.center = QPointF(600, 500);
    box.rx = 210;
    box.ry = 120;
    box.rot = 25;
    QuickShape zigzag;
    zigzag.kind = Kind::Polyline;
    zigzag.pts = {QPointF(200, 600), QPointF(400, 300), QPointF(600, 600), QPointF(800, 300)};
    bool sharpSame = true;
    for (const QuickShape* q : {&pentagon, &box, &zigzag}) {
        const QuickShape asPath = q->toPath();
        // The same line, starting at the same place, with the same corners kept sharp.
        const std::vector<PathPoint> a = q->path(2.0), b = asPath.path(2.0);
        const bool same = asPath.closed() == q->closed() && asPath.name() == (q == &box ? QStringLiteral("Quadrilateral") : q->name())
                          && gap(outline(*q), outline(asPath)) < 1e-6 && corners(*q) == corners(asPath)
                          && std::hypot(a.front().pos.x() - b.front().pos.x(), a.front().pos.y() - b.front().pos.y()) < 1e-6;
        sharpSame = sharpSame && same;
    }
    pathCheck("polygon, rectangle, polyline", sharpSame, QStringLiteral("the same outline from the same start, corners still sharp"));

    QuickShape oval;
    oval.kind = Kind::Ellipse;
    oval.center = QPointF(700, 500);
    oval.rx = 420;
    oval.ry = 230;
    oval.rot = 30;
    const QuickShape ovalPath = oval.toPath();
    const double ovalGap = gap(outline(oval), outline(ovalPath));
    QuickShape bow;
    bow.kind = Kind::Arc;
    bow.pts = {QPointF(300, 600), QPointF(420, 310), QPointF(900, 640)};
    const QuickShape bowPath = bow.toPath();
    const double bowGap = gap(outline(bow), outline(bowPath));
    bool middleKept = false;
    for (const QuickShape::Node& n : bowPath.nodes)
        middleKept = middleKept || std::hypot(n.pos.x() - 420, n.pos.y() - 310) < 1e-6;
    pathCheck("ellipse and arc as paths", ovalPath.nodes.size() == 4 && ovalPath.closed() && ovalGap < 0.15 && bowGap < 0.15 && middleKept
                                              && !bowPath.closed() && ovalPath.name() == QLatin1String("Shape"),
              QStringLiteral("round to within %1 px and %2 px, the arc in %3 points").arg(ovalGap, 0, 'f', 3).arg(bowGap, 0, 'f', 3).arg(bowPath.nodes.size()));

    // Adding a point. Where the handles are the shape's own the line does not
    // move at all; on a line through free points it gives a little.
    QuickShape ovalPlus = ovalPath;
    const QuickShape::Hit onOval = ovalPlus.nearest(oval.center + QPointF(300, 300));
    const int ovalAdded = ovalPlus.insertNode(onOval.segment, onOval.t);
    const double ovalPlusGap = gap(outline(ovalPath), outline(ovalPlus));
    QuickShape pentagonPlus = pentagon.toPath();
    const QuickShape::Hit onSide = pentagonPlus.nearest(QPointF(725, 335));
    const int sideAdded = pentagonPlus.insertNode(onSide.segment, onSide.t);
    const double sideGap = gap(outline(pentagon), outline(pentagonPlus));
    QuickShape curvePlus = curvePath;
    const QuickShape::Hit onCurve = curvePlus.nearest(QPointF(580, 430));
    const int curveAdded = curvePlus.insertNode(onCurve.segment, onCurve.t);
    const double curvePlusGap = gap(outline(curvePath), outline(curvePlus));
    pathCheck("a point added", ovalAdded >= 0 && ovalPlus.nodes.size() == 5 && ovalPlusGap < 0.01 && onOval.distance < 80.0
                                   && sideAdded == 1 && pentagonPlus.nodes.size() == 6 && !pentagonPlus.nodes[1].smooth && sideGap < 1e-6
                                   && corners(pentagonPlus) == 6
                                   && curveAdded == 3 && curvePlus.nodes[3].smooth && curvePlusGap < 0.01,
              QStringLiteral("the line moves %1 px on an ellipse, %2 px on a polygon's side, %3 px on a curve").arg(ovalPlusGap, 0, 'f', 4).arg(sideGap, 0, 'f', 4).arg(curvePlusGap, 0, 'f', 4));

    // Taking it out again, and the least a shape can be made of.
    QuickShape curveBack = curvePlus;
    const bool removed = curveBack.removeNode(size_t(curveAdded));
    QuickShape triangle = pentagon.toPath();
    const bool toTriangle = triangle.removeNode(0) && triangle.removeNode(0);
    QuickShape two = zigzag.toPath();
    const bool toTwo = two.removeNode(1) && two.removeNode(1);
    // A point that has since been moved leaves a line through the others, not a kink or a loop.
    QuickShape pulled = curvePlus;
    pulled.nodes[size_t(curveAdded)].pos += QPointF(40, -120);
    const bool pulledOut = pulled.removeNode(size_t(curveAdded));
    const double pulledGap = gap(outline(curvePath), outline(pulled));
    QuickShape ovalBack = ovalPlus;
    const bool ovalRemoved = ovalBack.removeNode(size_t(ovalAdded));
    pathCheck("a point removed", removed && gap(outline(curvePath), outline(curveBack)) < 0.01 && ovalRemoved && gap(outline(ovalPath), outline(ovalBack)) < 0.01
                                     && pulledOut && pulledGap < 25.0 && toTriangle && triangle.name() == QLatin1String("Triangle")
                                     && !triangle.removeNode(0) && triangle.nodes.size() == 3 && toTwo && two.name() == QLatin1String("Line")
                                     && !two.removeNode(0) && two.nodes.size() == 2,
              QStringLiteral("back to the line it was; a point moved first, then removed, leaves it %1 px off; a closed shape keeps three points, a line two").arg(pulledGap, 0, 'f', 1));

    // Smooth against sharp, and a handle moved by hand.
    QuickShape rounded = pentagon.toPath();
    rounded.setSmooth(1, true);
    const std::vector<QPointF> roundedLine = outline(rounded);
    // Just outside the corner that was there, the smooth point's bend passes; straight sides would not.
    const QPointF beyond = pentagon.pts[1] + QPointF(-3, 14);
    double nearBeyond = std::numeric_limits<double>::max();
    for (const QPointF& q : roundedLine)
        nearBeyond = std::min(nearBeyond, std::hypot(q.x() - beyond.x(), q.y() - beyond.y()));
    QuickShape sharpAgain = rounded;
    sharpAgain.setSmooth(1, false);
    QuickShape kinked = curvePath;
    kinked.setSmooth(2, false);
    QuickShape handled = curvePath;
    const QPointF node = handled.nodes[2].pos, inBefore = handled.handleIn(2) - node;
    handled.setHandle(2, true, node + QPointF(90, 60));
    const QPointF inAfter = handled.handleIn(2) - node, outAfter = handled.handleOut(2) - node;
    const double inLine = inAfter.x() * outAfter.y() - inAfter.y() * outAfter.x();
    // The handle goes with its point, and bends along when a neighbour moves.
    QuickShape carried = handled;
    carried.nodes[2].pos += QPointF(50, 30);
    const QPointF carriedOut = carried.handleOut(2) - carried.nodes[2].pos;
    const bool handleOk = std::hypot(outAfter.x() - 90, outAfter.y() - 60) < 1e-6 && std::abs(inLine) < 1e-6
                          && std::hypot(carriedOut.x() - 90, carriedOut.y() - 60) < 60.0 && std::hypot(carriedOut.x(), carriedOut.y()) > 40.0
                          && inAfter.x() * outAfter.x() + inAfter.y() * outAfter.y() < 0.0
                          && std::abs(std::hypot(inAfter.x(), inAfter.y()) - std::hypot(inBefore.x(), inBefore.y())) < 1e-6;
    pathCheck("smooth points and sharp corners", corners(rounded) == 4 && rounded.name() == QLatin1String("Shape") && nearBeyond < 6.0
                                                     && gap(outline(pentagon), outline(sharpAgain)) < 1e-6 && corners(kinked) == 1
                                                     && kinked.handleOut(2) == kinked.nodes[2].pos && handleOk,
              QStringLiteral("a corner made smooth bends %1 px from where it was sharp; a moved handle keeps the other in line").arg(nearBeyond, 0, 'f', 1));

    // Resizing a held shape turns and scales it; handles go with their points.
    QuickShape turned = ovalPlus;
    turned.transformSimilar(QPointF(100, 100), 1.7, 40.0);
    QuickShape ovalTurned = oval;
    ovalTurned.transformSimilar(QPointF(100, 100), 1.7, 40.0);
    const double turnedGap = gap(outline(ovalTurned), outline(turned));
    pathCheck("a path scaled and turned", turnedGap < 0.3, QStringLiteral("still the ellipse to within %1 px").arg(turnedGap, 0, 'f', 3));
    return failures;
}


// ---- selections, fill, clipboard and alpha-locked smudge, checked on pixels

bool nearby(const QColor& c, int r, int g, int b, int tol = 3)
{
    return std::abs(c.red() - r) <= tol && std::abs(c.green() - g) <= tol && std::abs(c.blue() - b) <= tol;
}

bool runEditingTests(CanvasEngine& engine)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset& round = *lib.find(QStringLiteral("round-brush"));
    bool all = true;
    auto check = [&](const char* what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", QString::fromUtf8(what));
    };
    auto at = [&](int x, int y) { return engine.renderImage().pixelColor(x, y); };

    const int undoBase = engine.history().undoCount();
    const QImage before = engine.renderImage();
    const QColor paper = before.pixelColor(1500, 1250);   // an untouched corner
    const int layer = engine.addLayer();

    // Selection limits a fill and a brush stroke.
    QPainterPath box;
    box.addRect(1400, 1100, 200, 120);
    engine.setSelection(box, QStringLiteral("Select"));
    engine.fillLayer(layer, QColor(200, 30, 30));
    check("fill stays inside the selection", nearby(at(1500, 1160), 200, 30, 30) && at(1380, 1160) == before.pixelColor(1380, 1160));
    wave(engine, StrokeTool::Brush, round, 120, 1.0f, QColor(20, 60, 220), QPointF(1300, 1160), QPointF(1700, 1160), 0);
    check("brush stroke stays inside the selection",
          nearby(at(1500, 1160), 20, 60, 220, 6) && at(1650, 1160) == before.pixelColor(1650, 1160));

    // Inverting flips what is protected; clearing only removes what is selected.
    engine.invertSelection();
    engine.fillLayer(layer, QColor(30, 160, 60));
    check("inverted selection fills the outside", nearby(at(1700, 1250), 30, 160, 60) && nearby(at(1500, 1160), 20, 60, 220, 6));
    engine.clearLayer(layer);
    check("clear removes only the selected part", at(1700, 1250) == paper && nearby(at(1500, 1160), 20, 60, 220, 6));

    // Moving a selection takes its pixels and its outline along.
    engine.invertSelection();
    const bool began = engine.beginTransform();
    QTransform shift;
    shift.translate(-180, 0);
    engine.setTransform(shift);
    engine.commitEdit();
    check("transform moves the selected pixels and the outline",
          began && nearby(at(1320, 1160), 20, 60, 220, 6) && at(1590, 1160) == before.pixelColor(1590, 1160)
              && std::abs(engine.selectionBounds().left() - 1220) <= 1);

    // Copy gives back exactly the selected rectangle; pasting it lands in place.
    QPoint origin;
    const QImage copied = engine.copyPixels(&origin);
    engine.clearSelection();
    const int pasted = engine.addImageLayer(copied, QStringLiteral("Pasted"), origin + QPoint(0, -110));
    check("copy and paste", copied.size() == QSize(200, 120) && origin == QPoint(1220, 1100) && pasted != 0
                                && nearby(at(1320, 1050), 20, 60, 220, 6));
    engine.removeLayer(pasted);
    engine.setActiveLayer(layer);

    // Flood fill stops at a painted outline.
    QPainterPath frame;
    frame.addRect(1450, 1020, 200, 200);
    QPainterPath hole;
    hole.addRect(1470, 1040, 160, 160);
    engine.setSelection(frame.subtracted(hole), QStringLiteral("Select"));
    engine.fillLayer(layer, QColor(10, 10, 10));
    engine.clearSelection();
    const bool filled = engine.floodFill(QPoint(1550, 1120), QColor(250, 200, 20), 0.12f);
    check("flood fill is contained by an outline",
          filled && nearby(at(1550, 1120), 250, 200, 20) && nearby(at(1460, 1120), 10, 10, 10) && at(1700, 1250) == paper);

    // Smudging on an alpha-locked layer must leave every alpha value alone.
    engine.setLayerAlphaLock(layer, true);
    const QByteArray alphaBefore = engine.layerPixels(layer);
    wave(engine, StrokeTool::Smudge, round, 90, 1.0f, Qt::black, QPointF(1300, 1160), QPointF(1700, 1120), 30);
    const QByteArray alphaAfter = engine.layerPixels(layer);
    bool alphaSame = alphaBefore.size() == alphaAfter.size(), colourMoved = false;
    const auto* a = reinterpret_cast<const quint16*>(alphaBefore.constData());
    const auto* b = reinterpret_cast<const quint16*>(alphaAfter.constData());
    for (qsizetype i = 0; alphaSame && i < alphaBefore.size() / 8; ++i) {
        alphaSame = a[i * 4 + 3] == b[i * 4 + 3];
        colourMoved = colourMoved || a[i * 4] != b[i * 4] || a[i * 4 + 1] != b[i * 4 + 1];
    }
    check("smudge under alpha lock changes colour but no alpha", alphaSame && colourMoved);

    while (engine.history().undoCount() > undoBase)
        engine.undo(true);
    check("everything undoes back to the start", engine.renderImage() == before && !engine.hasSelection());
    return all;
}


// ---- layer masks and groups

int maxDiff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size())
        return 255;
    int worst = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar* ra = a.constScanLine(y);
        const uchar* rb = b.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x)
            worst = std::max(worst, std::abs(int(ra[x]) - int(rb[x])));
    }
    return worst;
}

bool runMaskAndGroupTests(CanvasEngine& engine, const QString& docPath)
{
    const BrushPreset& round = *BrushLibrary::instance().find(QStringLiteral("round-brush"));
    bool all = true;
    auto check = [&](const char* what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", QString::fromUtf8(what));
    };
    auto at = [&](int x, int y) { return engine.renderImage().pixelColor(x, y); };
    auto info = [&](int id) { return engine.layerInfo(id); };
    const QImage before = engine.renderImage();

    // ---- masks
    const int red = engine.addLayer();
    engine.fillLayer(red, QColor(200, 30, 30));
    const QImage filled = engine.renderImage();
    engine.addLayerMask(red);
    check("a new mask shows the whole layer", engine.renderImage() == filled && engine.maskEditing() && info(red).hasMask);

    const QByteArray pixels = engine.layerPixels(red);
    wave(engine, StrokeTool::Brush, round, 80, 1.0f, Qt::black, QPointF(1300, 1150), QPointF(1700, 1150), 0);
    check("black on the mask hides the layer without touching its pixels",
          at(1500, 1150) == before.pixelColor(1500, 1150) && nearby(at(1500, 1050), 200, 30, 30) && engine.layerPixels(red) == pixels);
    wave(engine, StrokeTool::Eraser, round, 80, 1.0f, Qt::black, QPointF(1300, 1150), QPointF(1500, 1150), 0);
    check("the eraser reveals again", nearby(at(1400, 1150), 200, 30, 30) && at(1620, 1150) == before.pixelColor(1620, 1150));
    engine.setLayerMaskEnabled(red, false);
    const bool disabledShows = nearby(at(1620, 1150), 200, 30, 30);
    engine.setLayerMaskEnabled(red, true);
    check("a disabled mask is ignored", disabledShows && at(1620, 1150) == before.pixelColor(1620, 1150));
    engine.invertLayerMask(red);
    const bool inverted = nearby(at(1620, 1150), 200, 30, 30) && at(1500, 1050) == before.pixelColor(1500, 1050);
    engine.invertLayerMask(red);
    check("inverting swaps hidden and shown", inverted && nearby(at(1500, 1050), 200, 30, 30));

    // Moving the layer takes its mask along; what comes in from outside is shown.
    engine.setMaskEditing(false);
    QTransform up;
    up.translate(0, -100);
    engine.beginTransform();
    engine.setTransform(up);
    engine.commitEdit();
    check("a transform moves the mask with the layer",
          at(1620, 1050) == before.pixelColor(1620, 1050) && nearby(at(1620, 1150), 200, 30, 30));

    const QImage masked = engine.renderImage();
    engine.applyLayerMask(red);
    check("applying a mask bakes it in", maxDiff(engine.renderImage(), masked) <= 1 && !info(red).hasMask);
    engine.undo(true);

    const int blue = engine.addLayer();
    engine.fillLayer(blue, QColor(20, 60, 220));
    QPainterPath oval;
    oval.addEllipse(1400, 1080, 200, 140);
    engine.setSelection(oval, QStringLiteral("Select"));
    engine.addLayerMask(blue);
    engine.clearSelection();
    engine.setMaskEditing(false);
    check("a mask made from a selection shows only what was selected",
          nearby(at(1500, 1150), 20, 60, 220) && !nearby(at(1350, 1100), 20, 60, 220));

    // ---- groups
    const QImage loose = engine.renderImage();
    const int group = engine.groupLayer(blue);
    engine.moveLayer(red, group, blue);   // into the group, underneath blue
    check("putting layers in a group keeps the picture",
          maxDiff(engine.renderImage(), loose) <= 1 && info(red).parent == group && info(blue).parent == group && info(red).depth == 1);

    // A group blends as one thing: at half opacity the layer on top fades, but
    // the one beneath it inside the group must not start showing through.
    engine.setLayerOpacity(group, 0.5f);
    const QColor under = before.pixelColor(1500, 1150);
    const QColor faded = at(1500, 1150);
    check("a group's opacity applies to the group as a whole",
          nearby(faded, (under.red() + 20) / 2, (under.green() + 60) / 2, (under.blue() + 220) / 2, 3));
    engine.setLayerVisible(group, false);
    const bool hidden = engine.renderImage() == before;
    engine.setLayerVisible(group, true);
    engine.setLayerOpacity(group, 1.0f);
    check("hiding a group hides everything in it", hidden && maxDiff(engine.renderImage(), loose) <= 1);

    const int outer = engine.groupLayer(group);
    const bool nested = info(blue).depth == 2 && info(group).parent == outer;
    engine.moveLayer(blue, 0, 0);
    const bool movedOut = info(blue).parent == 0 && engine.layers().last().id == blue;
    engine.undo(true);
    check("groups nest, and a layer can be dragged out to the top", nested && movedOut && info(blue).parent == group);

    const QImage grouped = engine.renderImage();
    const int count = engine.layerCount();
    engine.duplicateLayer(outer);
    const bool duplicated = engine.layerCount() == count + 4;   // two groups and two layers
    engine.undo(true);
    engine.flattenGroup(group);
    const bool flattened = engine.layerCount() == count - 2 && maxDiff(engine.renderImage(), grouped) <= 2;
    engine.undo(true);
    engine.ungroupLayer(group);
    const bool ungrouped = engine.layerCount() == count - 1 && info(red).parent == outer && maxDiff(engine.renderImage(), grouped) <= 1;
    engine.undo(true);
    engine.removeLayer(outer);
    const bool removed = engine.layerCount() == count - 4 && engine.renderImage() == before;
    engine.undo(true);
    check("duplicate, flatten, ungroup and delete a group, each undone",
          duplicated && flattened && ungrouped && removed && engine.layerCount() == count && maxDiff(engine.renderImage(), grouped) <= 1);

    // ---- everything survives a save and reload
    engine.setLayerCollapsed(outer, true);
    struct Row {
        QString name;
        bool isGroup, hasMask, collapsed;
        int depth;
    };
    auto rows = [&] {
        QVector<Row> out;
        for (const LayerInfo& l : engine.layers())
            out.push_back({l.name, l.isGroup, l.hasMask, l.collapsed, l.depth});
        return out;
    };
    auto same = [](const QVector<Row>& a, const QVector<Row>& b) {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); ++i) {
            if (a[i].name != b[i].name || a[i].isGroup != b[i].isGroup || a[i].hasMask != b[i].hasMask
                || a[i].collapsed != b[i].collapsed || a[i].depth != b[i].depth)
                return false;
        }
        return true;
    };
    const QVector<Row> structure = rows();
    QString error;
    const bool saved = docio::save(engine, docPath, &error) && docio::load(engine, docPath, &error);
    // Masks are stored at 8 bits, so a soft mask edge may shift by a level or two.
    check("masks and groups survive a save and reload", saved && same(structure, rows()) && maxDiff(engine.renderImage(), grouped) <= 3);
    return all;
}

// ---- soft selections, automatic selection, and fills that follow a reference layer

bool runFloodAndSelectionTests(CanvasEngine& engine, const QString& docPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 1200, H = 900;
    engine.newDocument(QSize(W, H), QColor(250, 247, 240));
    auto at = [&](int x, int y) { return engine.renderImage().pixelColor(x, y); };
    auto alpha = [&](int id, int x, int y) {
        const QByteArray px = engine.layerPixels(id);
        return float(reinterpret_cast<const qfloat16*>(px.constData())[(size_t(y) * W + size_t(x)) * 4 + 3]);
    };
    auto rect = [](int x, int y, int w, int h) {
        QPainterPath p;
        p.addRect(x, y, w, h);
        return p;
    };
    auto undoCount = [&] { return engine.history().undoCount(); };

    // Line art: an antialiased ring 8 px wide, radius 192..200 around (600, 450).
    const int ink = engine.activeLayerId();
    QPainterPath ring;
    ring.addEllipse(QPointF(600, 450), 200, 200);
    QPainterPath inner;
    inner.addEllipse(QPointF(600, 450), 192, 192);
    engine.setSelection(ring.subtracted(inner), QStringLiteral("Select"));
    engine.fillLayer(ink, QColor(0, 0, 0));
    engine.clearSelection();
    const int colour = engine.addLayer();
    engine.moveLayer(colour, 0, ink);   // colour goes underneath the lines
    engine.setActiveLayer(colour);

    // ---- reference layer
    engine.floodFill(QPoint(600, 450), QColor(200, 30, 30), 0.12f);
    check(QStringLiteral("without a reference, a fill on an empty layer floods all of it"),
          alpha(colour, 600, 450) > 0.99f && alpha(colour, 100, 100) > 0.99f);
    engine.undo(true);

    engine.setReferenceLayer(ink);
    const int beforeFill = undoCount();
    const bool filled = engine.floodFill(QPoint(600, 450), QColor(200, 30, 30), 0.12f);
    check(QStringLiteral("with one, the fill stops at lines drawn on the other layer"),
          filled && engine.layerInfo(ink).reference && alpha(colour, 600, 450) > 0.99f && alpha(colour, 100, 100) == 0.0f
              && alpha(colour, 830, 450) == 0.0f && undoCount() == beforeFill + 1);
    // Along a ray outwards the line starts 192 px from the centre: colour must
    // reach a little way under it and no further, and from well inside to the
    // middle of the line nothing but red and black may show, in any direction.
    bool noFringe = true;
    const QImage flat = engine.renderImage();
    for (double degrees : {0.0, 45.0, 90.0, 200.0, 291.0}) {
        const double a = degrees * 3.14159265358979 / 180.0;
        for (double r = 170.0; r <= 196.0; r += 0.5)
            noFringe = noFringe && flat.pixelColor(int(600 + r * std::cos(a)), int(450 - r * std::sin(a))).green() <= 34;
    }
    check(QStringLiteral("colour runs %1 px under the line: no fringe, and none comes out the far side")
              .arg(alpha(colour, 794, 450) > 0.99f ? 3 : alpha(colour, 793, 450) > 0.99f ? 2 : alpha(colour, 792, 450) > 0.99f ? 1 : 0),
          noFringe && alpha(colour, 793, 450) > 0.99f && alpha(colour, 797, 450) == 0.0f && alpha(colour, 803, 450) == 0.0f);
    engine.undo(true);

    engine.setLayerVisible(ink, false);
    engine.floodFill(QPoint(600, 450), QColor(200, 30, 30), 0.12f);
    const bool hiddenIgnored = alpha(colour, 100, 100) > 0.99f;
    engine.undo(true);
    engine.undo(true);
    engine.setActiveLayer(ink);
    engine.floodFill(QPoint(600, 450), QColor(200, 30, 30), 0.12f);
    const bool ownLayer = alpha(ink, 600, 450) > 0.99f && alpha(ink, 100, 100) == 0.0f;
    engine.undo(true);
    engine.setActiveLayer(colour);
    check(QStringLiteral("a hidden reference is ignored; the reference layer itself fills as before"), hiddenIgnored && ownLayer);

    // ---- automatic selection
    engine.floodSelect(QPoint(600, 450), 0.12f, SelectOp::Add);
    const QRect disc = engine.selectionBounds();
    check(QStringLiteral("automatic selection follows the reference: %1 x %2 px inside the ring").arg(disc.width()).arg(disc.height()),
          std::abs(disc.width() - 390) <= 3 && std::abs(disc.height() - 390) <= 3 && engine.selectionAt(QPoint(600, 450)) == 1.0f
              && engine.selectionAt(QPoint(100, 100)) == 0.0f);
    const int beforeLive = undoCount();
    const bool began = engine.beginFlood(FloodKind::Select, QPoint(100, 100), 0.12f, QColor(), SelectOp::Add);
    const bool grown = engine.selectionAt(QPoint(100, 100)) == 1.0f && engine.selectionAt(QPoint(600, 450)) == 1.0f;
    engine.cancelFlood();
    check(QStringLiteral("a second area is added while the pen is down, and dropped again on cancel"),
          began && grown && engine.selectionBounds() == disc && undoCount() == beforeLive);
    engine.floodSelect(QPoint(600, 450), 0.12f, SelectOp::Subtract);
    const bool emptied = !engine.hasSelection();
    engine.undo(true);
    check(QStringLiteral("Remove takes an area out again; undo brings it back"), emptied && engine.selectionBounds() == disc);
    engine.selectLayerContents(ink);
    check(QStringLiteral("select a layer's contents"),
          engine.selectionAt(QPoint(796, 450)) == 1.0f && engine.selectionAt(QPoint(600, 450)) == 0.0f
              && std::abs(engine.selectionBounds().width() - 400) <= 2);
    engine.clearSelection();

    // ---- live threshold: two greys 30 levels apart, side by side
    const int greys = engine.addLayer();
    engine.setSelection(rect(100, 100, 200, 200), QStringLiteral("Select"));
    engine.fillLayer(greys, QColor(100, 100, 100));
    engine.setSelection(rect(300, 100, 200, 200), QStringLiteral("Select"));
    engine.fillLayer(greys, QColor(130, 130, 130));
    engine.clearSelection();
    const int beforeSession = undoCount();
    const QColor green(30, 160, 60);
    engine.beginFlood(FloodKind::Fill, QPoint(200, 200), 0.05f, green);
    const bool narrow = nearby(at(200, 200), 30, 160, 60) && nearby(at(400, 200), 130, 130, 130);
    engine.setFloodTolerance(0.2f);
    const bool wide = nearby(at(200, 200), 30, 160, 60) && nearby(at(400, 200), 30, 160, 60);
    engine.setFloodTolerance(0.05f);
    const bool narrowAgain = nearby(at(400, 200), 130, 130, 130);
    engine.cancelFlood();
    const bool cancelled = nearby(at(200, 200), 100, 100, 100) && undoCount() == beforeSession;
    engine.beginFlood(FloodKind::Fill, QPoint(200, 200), 0.05f, green);
    engine.setFloodTolerance(0.2f);
    engine.endFlood();
    const bool kept = nearby(at(200, 200), 30, 160, 60) && nearby(at(400, 200), 30, 160, 60) && undoCount() == beforeSession + 1;
    engine.undo(true);
    check(QStringLiteral("a fill spreads and shrinks with its threshold before it is let go; cancel leaves nothing"),
          narrow && wide && narrowAgain && cancelled && kept && nearby(at(400, 200), 130, 130, 130));

    // ---- soft edges
    engine.setSelection(rect(400, 300, 400, 300), QStringLiteral("Select"));
    const int beforeFeather = undoCount();
    engine.featherSelection(12.0f);
    engine.featherSelection(20.0f);   // the same feather, turned up
    const float edge20 = engine.selectionAt(QPoint(400, 450)), in20 = engine.selectionAt(QPoint(410, 450));
    engine.featherSelection(40.0f);
    const float out40 = engine.selectionAt(QPoint(380, 450));
    check(QStringLiteral("feather: %1 at the edge, %2 ten pixels in; at 40 px, %3 twenty pixels out")
              .arg(edge20, 0, 'f', 2).arg(in20, 0, 'f', 2).arg(out40, 0, 'f', 2),
          std::abs(edge20 - 0.5f) < 0.06f && std::abs(in20 - 0.84f) < 0.06f && std::abs(out40 - 0.16f) < 0.06f
              && engine.selectionAt(QPoint(600, 450)) == 1.0f && engine.selectionAt(QPoint(300, 450)) == 0.0f
              && undoCount() == beforeFeather + 1);
    const int soft = engine.addLayer();
    engine.fillLayer(soft, QColor(20, 60, 220));
    const float painted = alpha(soft, 380, 450);
    check(QStringLiteral("paint through a soft selection fades with it (%1 where the selection is %2)")
              .arg(painted, 0, 'f', 2).arg(out40, 0, 'f', 2),
          std::abs(painted - out40) < 0.02f && alpha(soft, 600, 450) > 0.99f && alpha(soft, 300, 450) == 0.0f);
    engine.undo(true);   // the fill
    engine.undo(true);   // the layer
    engine.undo(true);   // the feather, in one step
    const bool hardAgain = engine.selectionAt(QPoint(398, 450)) == 0.0f && engine.selectionAt(QPoint(402, 450)) == 1.0f;
    engine.redo(true);
    check(QStringLiteral("undo gives the hard edge back in one step; redo softens it again"),
          hardAgain && std::abs(engine.selectionAt(QPoint(380, 450)) - out40) < 0.01f);

    // ---- the selection as an image: add, remove, mirror
    engine.setSelection(rect(100, 500, 200, 100), QStringLiteral("Select"));
    engine.selectPath(rect(250, 550, 200, 100), SelectOp::Add);
    engine.selectPath(rect(150, 520, 60, 60), SelectOp::Subtract);
    const bool combined = engine.selectionAt(QPoint(400, 620)) == 1.0f && engine.selectionAt(QPoint(120, 520)) == 1.0f
                          && engine.selectionAt(QPoint(180, 550)) == 0.0f && engine.selectionBounds() == QRect(100, 500, 350, 150);
    engine.flipCanvas(true);
    const bool mirrored = engine.selectionBounds() == QRect(W - 450, 500, 350, 150);
    engine.undo(true);
    check(QStringLiteral("areas add up and cut away; flipping the canvas takes the selection along"),
          combined && mirrored && engine.selectionBounds() == QRect(100, 500, 350, 150));
    engine.clearSelection();

    // ---- the reference mark is part of the document
    QString error;
    const bool saved = docio::save(engine, docPath, &error) && docio::load(engine, docPath, &error);
    int marked = 0;
    for (const LayerInfo& l : engine.layers())
        marked += l.reference;
    const int reloaded = engine.referenceLayerId();
    engine.setReferenceLayer(0);
    const bool cleared = engine.referenceLayerId() == 0;
    engine.undo(true);
    check(QStringLiteral("the reference layer survives a save and reload, and switching it off undoes"),
          saved && marked == 1 && reloaded != 0 && cleared && engine.referenceLayerId() == reloaded);
    return all;
}

// ---- the artist's own brushes: made, changed, stored, shared

bool runBrushLibraryTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    BrushLibrary& lib = BrushLibrary::instance();
    QTemporaryDir scratch;
    constexpr int W = 900, H = 600;
    engine.newDocument(QSize(W, H), QColor(250, 247, 240));
    const int layer = engine.activeLayerId();
    auto alpha = [&](int x, int y) {
        const QByteArray px = engine.layerPixels(layer);
        return float(reinterpret_cast<const qfloat16*>(px.constData())[(size_t(y) * W + size_t(x)) * 4 + 3]);
    };
    auto stamp = [&](const BrushPreset& preset, float size, const QPointF& at) {
        StrokeSample s;
        s.pos = at;
        s.pressure = 1.0f;
        engine.beginStroke(StrokeTool::Brush, preset, size, 1.0f, Qt::black, s);
        engine.endStroke();
    };

    // ---- what ships
    {
        const QStringList tips = texgen::tipNames(), grains = texgen::grainNames();
        QSet<QString> ids;
        QStringList wrong;
        for (const BrushPreset& p : lib.presets()) {
            if (ids.contains(p.id) || (!p.tip.isEmpty() && !tips.contains(p.tip)) || (!p.grain.isEmpty() && !grains.contains(p.grain))
                || p.minSize > p.size || p.size > p.maxSize || p.name.isEmpty())
                wrong << p.id;
            ids.insert(p.id);
        }
        // Every picture a brush can be made of is really a picture: some of it light, some of it dark.
        auto drawn = [](const QImage& img) {
            int lightest = 0, darkest = 255;
            for (int y = 0; y < img.height(); y += 4) {
                const uchar* row = img.constScanLine(y);
                for (int x = 0; x < img.width(); x += 4) {
                    lightest = std::max(lightest, int(row[x]));
                    darkest = std::min(darkest, int(row[x]));
                }
            }
            return img.format() == QImage::Format_Grayscale8 && img.size() == QSize(texgen::kSize, texgen::kSize) && lightest > 200 && darkest < 60;
        };
        QStringList blank;
        for (const QString& tip : tips) {
            if (!drawn(texgen::tip(tip)) || texgen::tipTitle(tip) == tip)
                blank << tip;
        }
        for (const QString& grain : grains) {
            if (!drawn(texgen::grain(grain)) || texgen::grainTitle(grain) == grain)
                blank << grain;
        }
        const QStringList sets = lib.categories();
        check(QStringLiteral("%1 brushes ship in %2 sets (%3 … %4), made of %5 tip shapes and %6 grains; every one refers to pictures that exist %7")
                  .arg(lib.presets().size()).arg(sets.size()).arg(sets.value(0), sets.value(sets.size() - 1)).arg(tips.size()).arg(grains.size())
                  .arg(wrong.join(QLatin1Char(' ')) + blank.join(QLatin1Char(' '))),
              lib.presets().size() >= 110 && sets.size() == 14 && sets.value(0) == QLatin1String("Sketching") && sets.value(1) == QLatin1String("Inking")
                  && sets.last() == QLatin1String("Abstract") && tips.size() == 30 && grains.size() == 21 && wrong.isEmpty() && blank.isEmpty()
                  && lib.inCategory(QStringLiteral("Calligraphy")).size() == 7 && lib.find(QStringLiteral("monoline"))->category == QLatin1String("Calligraphy"));
    }
    // ---- a set put in an order of one's own
    {
        auto idsOf = [&](const QString& set) {
            QStringList ids;
            for (const BrushPreset* p : lib.inCategory(set))
                ids << p->id;
            return ids;
        };
        const QString set = QStringLiteral("Inking");
        const QStringList shippedOrder = idsOf(set);
        const QString first = shippedOrder.value(0);
        const bool moved = lib.moveInCategory(first, 2);
        const QStringList rearranged = idsOf(set);
        lib.save();
        lib.load();
        const bool kept = idsOf(set) == rearranged;
        // A brush made after that still goes on top, as new brushes do.
        const QString fresh = lib.duplicate(first);
        const bool onTop = idsOf(set).value(0) == fresh;
        lib.remove(fresh);
        const bool refused = !lib.moveInCategory(first, 2) && !lib.moveInCategory(QStringLiteral("no-such-brush"), 0);
        const bool back = lib.moveInCategory(first, 0) && idsOf(set) == shippedOrder;
        lib.save();
        check(QStringLiteral("a brush moved to third place in its set stays there after the library is saved and read again; a new brush still goes on top"),
              shippedOrder.size() > 3 && moved && rearranged.value(2) == first && rearranged.value(0) == shippedOrder.value(1)
                  && rearranged.value(3) == shippedOrder.value(3) && kept && onTop && refused && back);
    }
    // ---- a brush's own pressure curve, and paint that runs out at speed
    {
        BrushPreset pen = *lib.find(QStringLiteral("studio-pen"));
        pen.pressureSize = 1.0f;
        pen.pressureGamma = 1.0f;
        pen.taper = pen.taperEnd = 0.0f;
        pen.streamline = 0.0f;
        // The width of a steady stroke at one pressure, and the paint in its stamps, at a given pace.
        auto steady = [](const BrushPreset& preset, float pressure, double seconds, float* flow) {
            StrokeEngine stroke;
            for (int i = 0; i <= 60; ++i) {
                StrokeSample s;
                s.pos = QPointF(100 + i * 10, 300);
                s.pressure = pressure;
                s.time = seconds * i / 60.0;
                if (i == 0)
                    stroke.begin(preset, 40.0f, Qt::black, s);
                else
                    stroke.add(s);
            }
            stroke.end();
            const std::vector<Dab>& dabs = stroke.dabs();
            if (flow)
                *flow = dabs.empty() ? 0.0f : dabs[dabs.size() / 2].flow;
            return dabs.empty() ? 0.0f : dabs[dabs.size() / 2].size;
        };
        const float plain = steady(pen, 0.3f, 1.0, nullptr);
        pen.pressureCurve = {QPointF(0, 0), QPointF(0.3, 0.6), QPointF(1, 1)};
        const float curved = steady(pen, 0.3f, 1.0, nullptr), full = steady(pen, 1.0f, 1.0, nullptr);
        pen.speedFlow = 0.8f;
        float slowFlow = 0, fastFlow = 0;
        steady(pen, 1.0f, 3.0, &slowFlow);
        steady(pen, 1.0f, 0.12, &fastFlow);
        // The curve and the new number travel in a brush file like everything else.
        const QString own = lib.duplicate(QStringLiteral("studio-pen"));
        BrushPreset mine = *lib.find(own);
        mine.pressureCurve = pen.pressureCurve;
        mine.speedFlow = 0.8f;
        lib.update(mine);
        QString fileError;
        const QString file = scratch.filePath(QStringLiteral("curved.impbrush"));
        const bool written = lib.exportBrush(own, file, &fileError);
        const QString back = lib.importBrush(file, &fileError);
        const BrushPreset* read = lib.find(back);
        // A brush file from when the app was Sketchbook imports the same.
        QString oldBack;
        {
            QFile in(file);
            in.open(QIODevice::ReadOnly);
            QByteArray text = in.readAll();
            in.close();
            text.replace("\"impasto-brush\"", "\"sketchbook-brush\"");
            QFile out(scratch.filePath(QStringLiteral("curved.sbkbrush")));
            out.open(QIODevice::WriteOnly);
            out.write(text);
            out.close();
            oldBack = lib.importBrush(out.fileName(), &fileError);
        }
        const bool oldReads = !oldBack.isEmpty() && lib.find(oldBack) && lib.find(oldBack)->speedFlow == 0.8f;
        if (!oldBack.isEmpty())
            lib.remove(oldBack);
        check(QStringLiteral("a brush's own pressure curve: at 30% pressure the line is %1 px wide without it and %2 px with a curve that lifts a light touch; "
                             "with flow tied to speed a fast stroke carries %3% of the paint of a slow one; both survive a brush file")
                  .arg(plain, 0, 'f', 1).arg(curved, 0, 'f', 1).arg(qRound(100 * fastFlow / std::max(slowFlow, 0.001f))),
              std::abs(plain - 12.0f) < 1.5f && std::abs(curved - 24.0f) < 1.5f && std::abs(full - 40.0f) < 0.5f && fastFlow < slowFlow * 0.7f
                  && slowFlow > 0.8f && written && read && read->pressureCurve == pen.pressureCurve && read->speedFlow == 0.8f && oldReads);
        lib.remove(own);
        lib.remove(back);
    }

    // ---- duplicate, edit, rename, move
    const BrushPreset* shipped = lib.find(QStringLiteral("studio-pen"));
    const float shippedSpacing = shipped->spacing;
    const int shippedCount = int(lib.presets().size());
    const QString id = lib.duplicate(QStringLiteral("studio-pen"));
    const BrushPreset* copy = lib.find(id);
    check(QStringLiteral("a duplicate is a brush of its own: \"%1\", first in its set").arg(copy ? copy->name : QString()),
          copy && copy->custom && copy->name == QLatin1String("Studio Pen copy") && copy->category == shipped->category
              && lib.inCategory(shipped->category).first() == copy && int(lib.presets().size()) == shippedCount + 1
              && lib.find(QStringLiteral("studio-pen")) == shipped);
    BrushPreset edited = *copy;
    edited.spacing = 0.5f;
    edited.tip = QStringLiteral("chalk");
    edited.count = 3;
    edited.angleSource = BrushPreset::AngleSource::Direction;
    lib.update(edited);
    check(QStringLiteral("editing it leaves the shipped brush alone, and it never counts as a modified one"),
          lib.find(id)->spacing == 0.5f && shipped->spacing == shippedSpacing && !lib.isModified(id)
              && !lib.isModified(QStringLiteral("studio-pen")));
    const bool renamed = lib.rename(id, QStringLiteral("  Liner "));
    const QString second = lib.duplicate(id);
    const bool clash = lib.rename(second, QStringLiteral("liner"));
    check(QStringLiteral("rename: \"%1\"; a second brush asking for the same name becomes \"%2\"; shipped brushes keep theirs")
              .arg(lib.find(id)->name, lib.find(second)->name),
          renamed && clash && lib.find(id)->name == QLatin1String("Liner") && lib.find(second)->name == QLatin1String("liner 2")
              && !lib.rename(QStringLiteral("studio-pen"), QStringLiteral("Mine")) && !lib.remove(QStringLiteral("studio-pen")));
    const bool moved = lib.moveToCategory(id, QStringLiteral("Comics"));
    check(QStringLiteral("moving it to a new set adds that set after the shipped ones"),
          moved && lib.categories().last() == QLatin1String("Comics") && lib.inCategory(QStringLiteral("Comics")).size() == 1
              && !lib.moveToCategory(QStringLiteral("studio-pen"), QStringLiteral("Comics")));
    const bool removedSecond = lib.remove(second);

    // ---- an imported tip shape: a dark bar on white paper, and the same with transparency
    QImage paper(600, 400, QImage::Format_RGB32);
    paper.fill(Qt::white);
    {
        QPainter p(&paper);
        p.fillRect(100, 170, 400, 60, Qt::black);
    }
    const QString barFile = scratch.filePath(QStringLiteral("bar.png"));
    paper.save(barFile);
    QString error;
    const QString bar = brushtex::importTip(barFile, &error);
    const QImage barMap = brushtex::load(bar);
    QImage cutout(300, 300, QImage::Format_ARGB32);
    cutout.fill(Qt::transparent);
    {
        QPainter p(&cutout);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(200, 40, 40));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(150, 150), 100, 100);
    }
    const QString dot = brushtex::tipFromImage(cutout, QStringLiteral("dot"), &error);
    QImage blank(64, 64, QImage::Format_RGB32);
    blank.fill(Qt::white);
    const QImage dotMap = brushtex::load(dot);
    check(QStringLiteral("importing a tip: the dark shape on paper becomes the paint, cropped and centred; transparency works too"),
          brushtex::isUser(bar) && !barMap.isNull() && barMap.size() == QSize(512, 512) && qGray(barMap.pixel(256, 256)) > 240
              && qGray(barMap.pixel(40, 256)) > 240 && qGray(barMap.pixel(256, 150)) == 0 && qGray(barMap.pixel(2, 2)) == 0
              && brushtex::importTip(barFile, &error) == bar && !dotMap.isNull() && qGray(dotMap.pixel(256, 256)) > 240
              && qGray(dotMap.pixel(20, 20)) == 0 && brushtex::tipFromImage(blank, {}, &error).isEmpty());

    BrushPreset barBrush = *lib.find(id);
    barBrush.tip = bar;
    barBrush.count = 1;
    barBrush.pressureSize = 0.0f;
    barBrush.angleSource = BrushPreset::AngleSource::Fixed;
    barBrush.angle = 0.0f;
    barBrush.taper = barBrush.taperEnd = 0.0f;
    barBrush.flow = 1.0f;
    barBrush.pressureFlow = 0.0f;
    lib.update(barBrush);
    stamp(*lib.find(id), 200, QPointF(300, 300));
    const float onBar = alpha(300, 300), alongBar = alpha(370, 300), offBar = alpha(300, 350);
    engine.undo(true);
    check(QStringLiteral("a stamp of that brush is the bar, not a disc: %1 on it, %2 along it, %3 beside it")
              .arg(onBar, 0, 'f', 2).arg(alongBar, 0, 'f', 2).arg(offBar, 0, 'f', 2),
          onBar > 0.9f && alongBar > 0.9f && offBar == 0.0f);
    const QString negative = brushtex::inverted(bar);
    check(QStringLiteral("inverting an imported shape swaps paint and paper, and inverting again gives the first one back"),
          brushtex::isUser(negative) && negative != bar && qGray(brushtex::load(negative).pixel(256, 256)) < 15
              && qGray(brushtex::load(negative).pixel(256, 150)) == 255 && brushtex::inverted(negative) == bar);

    // ---- an imported grain: stripes show up in the paint
    QImage stripes(256, 256, QImage::Format_RGB32);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x)
            stripes.setPixel(x, y, (x / 32) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
    }
    const QString grain = brushtex::grainFromImage(stripes, QStringLiteral("stripes"), &error);
    BrushPreset grainy = *lib.find(id);
    grainy.tip.clear();
    grainy.hardness = 1.0f;
    grainy.spacing = 0.05f;
    grainy.grain = grain;
    grainy.grainMode = BrushPreset::GrainMode::Multiply;
    grainy.grainDepth = 1.0f;
    grainy.grainScale = 1.0f;
    grainy.grainMoving = false;
    lib.update(grainy);
    wave(engine, StrokeTool::Brush, *lib.find(id), 80, 1.0f, Qt::black, QPointF(100, 150), QPointF(800, 150), 0);
    float low = 1.0f, high = 0.0f;
    {
        const QByteArray px = engine.layerPixels(layer);
        const auto* half = reinterpret_cast<const qfloat16*>(px.constData());
        for (int x = 300; x < 600; ++x) {
            const float a = float(half[(size_t(150) * W + size_t(x)) * 4 + 3]);
            low = std::min(low, a);
            high = std::max(high, a);
        }
    }
    engine.undo(true);
    bool listed = false;
    for (const brushtex::Entry& e : brushtex::list())
        listed = listed || (e.name == grain && e.grain && e.label == QLatin1String("stripes"));
    check(QStringLiteral("an imported grain breaks up the paint (cover runs from %1 to %2 along one stroke)").arg(low, 0, 'f', 2).arg(high, 0, 'f', 2),
          brushtex::isUser(grain) && listed && low < 0.1f && high > 0.9f);

    // ---- kept between sessions
    lib.update(barBrush);
    lib.save();
    lib.load();   // as a fresh start would
    const BrushPreset* kept = lib.find(id);
    check(QStringLiteral("after a restart the brush is still there, as it was left"),
          removedSecond && kept && kept->custom && kept->name == QLatin1String("Liner") && kept->category == QLatin1String("Comics")
              && kept->spacing == 0.5f && kept->tip == bar && kept->angleSource == BrushPreset::AngleSource::Fixed
              && !lib.find(second) && lib.categories().contains(QStringLiteral("Comics"))
              && lib.find(QStringLiteral("studio-pen"))->spacing == shippedSpacing);

    // ---- shared as a file, shape included
    const QString file = scratch.filePath(QStringLiteral("liner.") + QLatin1String(BrushLibrary::fileExtension()));
    const bool exported = lib.exportBrush(id, file, &error);
    const bool removed = lib.remove(id);
    const bool setGone = !lib.categories().contains(QStringLiteral("Comics"));
    // The receiving end has never seen the shape: take it away before importing.
    QFile::remove(brushtex::directory() + QLatin1Char('/') + bar.mid(5) + QStringLiteral(".png"));
    const bool shapeGone = brushtex::load(bar).isNull();
    const QString back = lib.importBrush(file, &error);
    const BrushPreset* arrived = lib.find(back);
    BrushPreset round = arrived ? *arrived : BrushPreset();
    stamp(round, 200, QPointF(300, 300));
    const float besideAgain = alpha(300, 350), onAgain = alpha(300, 300);
    engine.undo(true);
    check(QStringLiteral("export and import: the brush arrives with its settings and its shape"),
          exported && removed && setGone && shapeGone && arrived && arrived->custom && back != id && arrived->name == QLatin1String("Liner")
              && arrived->category == QLatin1String("Comics") && arrived->spacing == 0.5f && arrived->tip == bar
              && !brushtex::load(bar).isNull() && onAgain > 0.9f && besideAgain == 0.0f
              && lib.importBrush(barFile, &error).isEmpty());

    // Leave the library as it shipped for whatever runs next.
    lib.remove(back);
    lib.save();
    lib.load();
    check(QStringLiteral("deleting the last of them leaves the library as it shipped"),
          int(lib.presets().size()) == shippedCount && !lib.categories().contains(QStringLiteral("Comics")));
    return all;
}

// ---- symmetry: strokes repeated about the centre of the canvas

bool runGuideTests(CanvasEngine& engine, const QString& docPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 1000, H = 800;
    const BrushPreset& pen = *BrushLibrary::instance().find(QStringLiteral("studio-pen"));
    const BrushPreset& hard = *BrushLibrary::instance().find(QStringLiteral("airbrush-hard"));
    engine.newDocument(QSize(W, H), QColor(250, 247, 240));
    const int layer = engine.activeLayerId();
    auto alphas = [&] {
        const QByteArray px = engine.layerPixels(layer);
        const auto* half = reinterpret_cast<const qfloat16*>(px.constData());
        std::vector<float> a(size_t(W) * H);
        for (size_t i = 0; i < a.size(); ++i)
            a[i] = float(half[i * 4 + 3]);
        return a;
    };
    // Largest difference between the layer and its own mirror image, and how much is painted on each half.
    auto compare = [&](const std::vector<float>& a, bool flipX, bool flipY, int* painted, int* paintedOther) {
        float worst = 0.0f;
        *painted = *paintedOther = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float here = a[size_t(y) * W + x];
                const float there = a[size_t(flipY ? H - 1 - y : y) * W + (flipX ? W - 1 - x : x)];
                worst = std::max(worst, std::abs(here - there));
                const bool firstHalf = flipX ? x < W / 2 : y < H / 2;
                if (here > 0.5f)
                    ++(firstHalf ? *painted : *paintedOther);
            }
        }
        return worst;
    };
    Guides guides;

    wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(120, 200), QPointF(420, 330), 40);
    int nearby = 0, farthest = 0;
    compare(alphas(), true, false, &nearby, &farthest);
    const bool plain = nearby > 2000 && farthest == 0;
    engine.undo(true);

    guides.symmetry = Guides::Symmetry::Vertical;
    engine.setGuides(guides);
    const int before = engine.history().undoCount();
    wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(120, 200), QPointF(420, 330), 40);
    const float offVertical = compare(alphas(), true, false, &nearby, &farthest);
    const bool oneStep = engine.history().undoCount() == before + 1;
    check(QStringLiteral("vertical symmetry: a stroke on the left appears mirrored on the right (%1 px each side, worst mismatch %2)")
              .arg(nearby).arg(offVertical, 0, 'f', 3),
          plain && nearby > 2000 && farthest == nearby && offVertical < 0.02f && oneStep);
    engine.undo(true);
    compare(alphas(), true, false, &nearby, &farthest);
    check(QStringLiteral("it is one stroke: one undo takes both halves away"), nearby == 0 && farthest == 0);

    guides.symmetry = Guides::Symmetry::Quadrant;
    engine.setGuides(guides);
    wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(120, 120), QPointF(420, 300), 30);
    const std::vector<float> four = alphas();
    int a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    const float offX = compare(four, true, false, &a1, &a2), offY = compare(four, false, true, &b1, &b2);
    check(QStringLiteral("quadrant: mirrored left to right and top to bottom"),
          offX < 0.02f && offY < 0.02f && a1 > 4000 && a1 == a2 && b1 == b2);

    // The eraser follows the same rule.
    guides.symmetry = Guides::Symmetry::Horizontal;
    engine.setGuides(guides);
    engine.fillLayer(layer, QColor(40, 90, 200));
    wave(engine, StrokeTool::Eraser, hard, 60, 1.0f, Qt::black, QPointF(200, 150), QPointF(800, 250), 20);
    const std::vector<float> holed = alphas();
    const float offErase = compare(holed, false, true, &nearby, &farthest);
    check(QStringLiteral("horizontal symmetry applies to the eraser too"),
          offErase < 0.02f && holed[size_t(200) * W + 500] < 0.05f && holed[size_t(H - 1 - 200) * W + 500] < 0.05f
              && holed[size_t(400) * W + 500] > 0.99f);
    engine.clearLayer(layer);

    // Radial: one stamp becomes a ring of them.
    guides.symmetry = Guides::Symmetry::Radial;
    guides.segments = 6;
    engine.setGuides(guides);
    StrokeSample tap;
    tap.pos = QPointF(W / 2.0 + 200, H / 2.0);
    tap.pressure = 1.0f;
    BrushPreset solid = hard;   // one stamp at full strength, whatever the airbrush would do
    solid.flow = 1.0f;
    solid.pressureFlow = solid.pressureSize = 0.0f;
    solid.hardness = 1.0f;
    engine.beginStroke(StrokeTool::Brush, solid, 60, 1.0f, Qt::black, tap);
    engine.endStroke();
    const std::vector<float> ring = alphas();
    bool six = true, between = true;
    for (int k = 0; k < 6; ++k) {
        const double on = 6.283185307179586 * k / 6.0, off = on + 6.283185307179586 / 12.0;
        six = six && ring[size_t(std::lround(H / 2.0 + 200 * std::sin(on))) * W + size_t(std::lround(W / 2.0 + 200 * std::cos(on)))] > 0.9f;
        between = between && ring[size_t(std::lround(H / 2.0 + 200 * std::sin(off))) * W + size_t(std::lround(W / 2.0 + 200 * std::cos(off)))] == 0.0f;
    }
    check(QStringLiteral("radial, six-fold: one stamp lands in six places round the centre"), six && between);

    // ---- symmetry that has been moved and turned
    engine.clearLayer(layer);
    {
        // The axis 150 px left of the middle: what is drawn left of x = 350 comes back right of it.
        guides = Guides();
        guides.symmetry = Guides::Symmetry::Vertical;
        guides.symOffset = QPointF(-150, 0);
        engine.setGuides(guides);
        wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(120, 200), QPointF(320, 330), 40);
        const std::vector<float> a = alphas();
        float worst = 0.0f;
        int onLeft = 0, onRight = 0, beyond = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const float here = a[size_t(y) * W + x];
                if (x >= 700) {
                    beyond += here > 0.0f;
                    continue;
                }
                worst = std::max(worst, std::abs(here - a[size_t(y) * W + (699 - x)]));
                if (here > 0.5f)
                    ++(x < 350 ? onLeft : onRight);
            }
        }
        check(QStringLiteral("the axis moved 150 px left: a stroke is mirrored about x = 350 (%1 px each side, worst mismatch %2), and nothing lands further out")
                  .arg(onLeft).arg(worst, 0, 'f', 3),
              engine.symmetryCentre() == QPointF(350, 400) && onLeft > 2000 && onLeft == onRight && worst < 0.02f && beyond == 0);
        engine.undo(true);
    }
    {
        // Turned 45 degrees, the upright axis runs from lower left to upper right:
        // the pixel at (x, y) and the one at (899 - y, 899 - x) mirror each other.
        guides = Guides();
        guides.symmetry = Guides::Symmetry::Vertical;
        guides.symAngle = 45.0f;
        engine.setGuides(guides);
        wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(300, 250), QPointF(420, 330), 20);
        const std::vector<float> a = alphas();
        float worst = 0.0f;
        int upperLeft = 0, lowerRight = 0;
        for (int y = 100; y < 800; ++y) {
            for (int x = 100; x < 800; ++x) {
                const float here = a[size_t(y) * W + x];
                worst = std::max(worst, std::abs(here - a[size_t(899 - x) * W + (899 - y)]));
                if (here > 0.5f)
                    ++((x - 500) + (y - 400) < 0 ? upperLeft : lowerRight);
            }
        }
        engine.undo(true);
        // A quarter turn makes it the level axis: the same as Horizontal.
        guides.symAngle = 90.0f;
        engine.setGuides(guides);
        wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(200, 150), QPointF(800, 250), 20);
        const std::vector<float> quarter = alphas();
        engine.undo(true);
        guides.symAngle = 0.0f;
        guides.symmetry = Guides::Symmetry::Horizontal;
        engine.setGuides(guides);
        wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(200, 150), QPointF(800, 250), 20);
        const std::vector<float> level = alphas();
        engine.undo(true);
        float apart = 0.0f;
        for (size_t i = 0; i < level.size(); ++i)
            apart = std::max(apart, std::abs(level[i] - quarter[i]));
        check(QStringLiteral("the axis turned 45°: a stroke is mirrored across the diagonal (%1 px each side, worst mismatch %2); turned 90° it is the horizontal axis")
                  .arg(upperLeft).arg(worst, 0, 'f', 3),
              upperLeft > 1500 && std::abs(upperLeft - lowerRight) < upperLeft / 50 && worst < 0.05f && apart < 0.001f);
    }
    {
        // Rotational: the repeat is the stroke turned half way round, not its mirror image.
        guides = Guides();
        guides.symmetry = Guides::Symmetry::Vertical;
        guides.symRotational = true;
        engine.setGuides(guides);
        wave(engine, StrokeTool::Brush, pen, 30, 1.0f, Qt::black, QPointF(120, 200), QPointF(420, 330), 40);
        const std::vector<float> a = alphas();
        const float halfTurn = compare(a, true, true, &nearby, &farthest);
        const float mirror = compare(a, true, false, &nearby, &farthest);
        engine.undo(true);
        guides.symmetry = Guides::Symmetry::Quadrant;
        engine.setGuides(guides);
        engine.beginStroke(StrokeTool::Brush, solid, 60, 1.0f, Qt::black, [&] {
            StrokeSample s;
            s.pos = QPointF(W / 2.0 + 200, H / 2.0 - 60);
            s.pressure = 1.0f;
            return s;
        }());
        engine.endStroke();
        const std::vector<float> four = alphas();
        auto at = [&](const std::vector<float>& v, int dx, int dy) { return v[size_t(H / 2 + dy) * W + size_t(W / 2 + dx)]; };
        const bool turnedFour = at(four, 200, -60) > 0.9f && at(four, 60, 200) > 0.9f && at(four, -200, 60) > 0.9f && at(four, -60, -200) > 0.9f
                                && at(four, -200, -60) == 0.0f && at(four, 200, 60) == 0.0f;
        engine.undo(true);
        check(QStringLiteral("rotational: the repeat is the stroke turned half way round (mismatch %1) and not its mirror image (%2); Quadrant gives four quarter turns")
                  .arg(halfTurn, 0, 'f', 3).arg(mirror, 0, 'f', 2),
              halfTurn < 0.02f && mirror > 0.9f && turnedFour);

        // Radial, four-fold, mirrored: a kaleidoscope of eight.
        guides = Guides();
        guides.symmetry = Guides::Symmetry::Radial;
        guides.segments = 4;
        engine.setGuides(guides);
        auto stampAt = [&] {
            StrokeSample s;
            s.pos = QPointF(W / 2.0 + 200, H / 2.0 - 60);
            s.pressure = 1.0f;
            engine.beginStroke(StrokeTool::Brush, solid, 60, 1.0f, Qt::black, s);
            engine.endStroke();
            const std::vector<float> v = alphas();
            engine.undo(true);
            return v;
        };
        const std::vector<float> plainFour = stampAt();
        guides.symMirrored = true;
        engine.setGuides(guides);
        const std::vector<float> eight = stampAt();
        bool turns = true, mirrors = true, onlyTurns = true;
        const int spots[4][2] = {{200, -60}, {60, 200}, {-200, 60}, {-60, -200}};
        for (const auto& spot : spots) {
            turns = turns && at(eight, spot[0], spot[1]) > 0.9f && at(plainFour, spot[0], spot[1]) > 0.9f;
            mirrors = mirrors && at(eight, -spot[0], spot[1]) > 0.9f;
            onlyTurns = onlyTurns && at(plainFour, -spot[0], spot[1]) == 0.0f;
        }
        check(QStringLiteral("radial, four-fold: four stamps; mirrored as well, eight"), turns && mirrors && onlyTurns && at(eight, 140, 140) == 0.0f);
    }
    {
        // Smudging is repeated too: a smear outwards on the left pulls the right-hand blot out the same way.
        guides = Guides();
        guides.symmetry = Guides::Symmetry::Vertical;
        engine.setGuides(guides);
        StrokeSample blot;
        blot.pos = QPointF(300, 400);
        blot.pressure = 1.0f;
        engine.beginStroke(StrokeTool::Brush, solid, 160, 1.0f, Qt::black, blot);
        engine.endStroke();
        const std::vector<float> blots = alphas();
        const int stepsBefore = engine.history().undoCount();
        wave(engine, StrokeTool::Smudge, hard, 70, 1.0f, Qt::black, QPointF(300, 400), QPointF(150, 400), 0);
        const std::vector<float> smeared = alphas();
        const float offSmear = compare(smeared, true, false, &nearby, &farthest);
        const size_t leftOut = size_t(400) * W + 205, rightOut = size_t(400) * W + (W - 1 - 205);
        const bool oneStep = engine.history().undoCount() == stepsBefore + 1;
        engine.undo(true);
        guides.symmetry = Guides::Symmetry::Off;
        engine.setGuides(guides);
        wave(engine, StrokeTool::Smudge, hard, 70, 1.0f, Qt::black, QPointF(300, 400), QPointF(150, 400), 0);
        const std::vector<float> oneSided = alphas();
        engine.undo(true);
        engine.undo(true);
        check(QStringLiteral("smudge under symmetry: both blots are drawn out alike (worst mismatch %1, %2% where there was nothing); without symmetry only the one under the pen")
                  .arg(offSmear, 0, 'f', 3).arg(qRound(smeared[leftOut] * 100)),
              blots[leftOut] == 0.0f && blots[rightOut] == 0.0f && smeared[leftOut] > 0.2f && smeared[rightOut] > 0.2f && offSmear < 0.03f && oneStep
                  && oneSided[leftOut] > 0.2f && oneSided[rightOut] == 0.0f);
    }
    {
        // Cropping and flipping the canvas take the axis along with the picture.
        guides = Guides();
        guides.symmetry = Guides::Symmetry::Vertical;
        guides.symOffset = QPointF(-150, 40);
        guides.symAngle = 20.0f;
        engine.setGuides(guides);
        const bool cropped = engine.resizeCanvas(QSize(600, 500), QTransform::fromTranslate(-100, -50), QStringLiteral("Crop Canvas"));
        const QPointF afterCrop = engine.symmetryCentre();   // was (350, 440) on the larger canvas
        engine.undo(true);
        const bool restored = engine.guides() == guides && engine.canvasSize() == QSize(W, H);
        engine.flipCanvas(true);
        const Guides flipped = engine.guides();
        engine.undo(true);
        check(QStringLiteral("cropping keeps the axis on the same spot of the picture (%1, %2); flipping the canvas turns it over with it; undo puts both back")
                  .arg(afterCrop.x()).arg(afterCrop.y()),
              cropped && afterCrop == QPointF(250, 390) && restored && flipped.symOffset == QPointF(150, 40) && flipped.symAngle == -20.0f
                  && engine.guides() == guides);
    }

    // The guides travel with the document, wherever symmetry was put.
    guides = Guides();
    guides.symmetry = Guides::Symmetry::Radial;
    guides.segments = 6;
    guides.symOffset = QPointF(-120.5, 64);
    guides.symAngle = 30.0f;
    guides.symMirrored = true;
    guides.symRotational = true;
    guides.grid = true;
    guides.gridSpacing = 64.0f;
    engine.setGuides(guides);
    QString error;
    const bool saved = docio::save(engine, docPath, &error);
    engine.newDocument(QSize(W, H), Qt::white);
    const bool resetOnNew = engine.guides() == Guides();
    const bool loaded = docio::load(engine, docPath, &error);
    check(QStringLiteral("a new canvas starts without guides; a saved one brings its symmetry back with its centre, angle and kind, and its grid"),
          saved && resetOnNew && loaded && engine.guides() == guides);
    engine.setGuides(Guides());
    return all;
}

// ---- perspective guides: which line a stroke is held to

bool runPerspectiveTests()
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    using perspective::Constraint;
    auto cross = [](const QPointF& a, const QPointF& b) { return a.x() * b.y() - a.y() * b.x(); };
    auto onLine = [&](const QPointF& p, const QPointF& a, const QPointF& b) {
        return std::abs(cross(p - a, b - a)) / std::hypot(b.x() - a.x(), b.y() - a.y()) < 1e-6;
    };
    Guides g;

    // One point: towards it, level, or upright, whichever the stroke sets off along.
    g.perspective = Guides::Perspective::OnePoint;
    g.vp[0] = QPointF(500, 300);
    const QPointF start(200, 500);
    const Constraint toPoint = perspective::choose(g, start, start + QPointF(30, -17));
    const Constraint level = perspective::choose(g, start, start + QPointF(50, 6));
    const Constraint upright = perspective::choose(g, start, start + QPointF(-4, 40));
    const QPointF wobbly(350, 431);
    check(QStringLiteral("one-point: a stroke heads for the vanishing point, or runs level, or upright"),
          toPoint.valid && onLine(toPoint.apply(wobbly), start, g.vp[0]) && level.apply(wobbly).y() == 500.0
              && upright.apply(wobbly).x() == 200.0 && perspective::candidates(g, start).size() == 3);
    // The line to the point leaves here at about 34 degrees: a pen setting off at 20 could mean that or level.
    double closeCall = 0.0, clearCall = 0.0;
    perspective::choose(g, start, start + QPointF(std::cos(0.35), -std::sin(0.35)) * 12.0, &closeCall);
    perspective::choose(g, start, start + QPointF(40, -1), &clearCall);
    check(QStringLiteral("it knows a close call (%1° between the two nearest lines) from a clear one (%2°)").arg(closeCall, 0, 'f', 0).arg(clearCall, 0, 'f', 0),
          closeCall < 14.0 && clearCall > 25.0);

    // Two points on a tilted horizon: uprights stand square on it.
    g.perspective = Guides::Perspective::TwoPoint;
    g.vp[0] = QPointF(100, 400);
    g.vp[1] = QPointF(900, 300);
    const QPointF horizon = g.vp[1] - g.vp[0];
    const Constraint square = perspective::choose(g, QPointF(500, 600), QPointF(500, 600) + QPointF(8, 70));
    const Constraint toRight = perspective::choose(g, QPointF(500, 600), QPointF(560, 555));
    check(QStringLiteral("two-point: towards either point, or square to a tilted horizon"),
          square.valid && std::abs(QPointF::dotProduct(square.dir, horizon)) < 1e-6
              && onLine(toRight.apply(QPointF(700, 480)), QPointF(500, 600), g.vp[1]));

    g.perspective = Guides::Perspective::ThreePoint;
    g.vp[2] = QPointF(500, 1400);
    const Constraint down = perspective::choose(g, QPointF(300, 500), QPointF(310, 560));
    check(QStringLiteral("three-point: the uprights converge on the third point"),
          down.valid && onLine(down.apply(QPointF(350, 800)), QPointF(300, 500), g.vp[2]));

    // Isometric with unequal angles.
    g.perspective = Guides::Perspective::Isometric;
    g.isoRight = 30.0f;
    g.isoLeft = 45.0f;
    const QPointF from(400, 400);
    const Constraint right = perspective::choose(g, from, from + QPointF(60, -30));
    const Constraint left = perspective::choose(g, from, from + QPointF(-40, -46));
    const Constraint up = perspective::choose(g, from, from + QPointF(3, 50));
    check(QStringLiteral("isometric: 30° to the right, 45° to the left, and upright"),
          std::abs(std::abs(right.dir.x()) - std::cos(30 * 3.14159265358979 / 180)) < 1e-6 && right.dir.x() * right.dir.y() < 0
              && std::abs(std::abs(left.dir.x()) - std::cos(45 * 3.14159265358979 / 180)) < 1e-6 && left.dir.x() * left.dir.y() > 0
              && std::abs(up.dir.x()) < 1e-9);

    // Fisheye: level lines bend through the left and right of the rim.
    g.perspective = Guides::Perspective::FivePoint;
    g.vp[0] = QPointF(500, 400);
    g.vp[1] = QPointF(800, 400);
    const QPointF high(500, 250);
    const Constraint bent = perspective::choose(g, high, high + QPointF(40, 2));
    const Constraint inward = perspective::choose(g, QPointF(650, 300), QPointF(630, 313));
    const QPointF held = bent.apply(QPointF(640, 300));
    const Constraint flat = perspective::choose(g, QPointF(400, 400), QPointF(440, 401));
    check(QStringLiteral("fisheye: a level stroke follows an arc through both sides of the rim (radius %1); through the middle it is straight")
              .arg(bent.radius, 0, 'f', 1),
          bent.circle && std::abs(std::hypot(200 - bent.centre.x(), 400 - bent.centre.y()) - bent.radius) < 1e-6
              && std::abs(std::hypot(800 - bent.centre.x(), 400 - bent.centre.y()) - bent.radius) < 1e-6
              && std::abs(std::hypot(held.x() - bent.centre.x(), held.y() - bent.centre.y()) - bent.radius) < 1e-6
              && !inward.circle && onLine(inward.apply(QPointF(600, 350)), QPointF(650, 300), g.vp[0]) && !flat.circle);
    return all;
}

// ---- adjustments and filters, checked on known colours

bool runAdjustmentTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 600, H = 400;
    engine.newDocument(QSize(W, H), Qt::white);
    const int layer = engine.activeLayerId();
    auto rect = [](int x, int y, int w, int h) {
        QPainterPath p;
        p.addRect(x, y, w, h);
        return p;
    };
    // Three patches: dark grey, light grey, red; the right of the layer stays empty.
    auto patch = [&](int x, const QColor& c) {
        engine.setSelection(rect(x, 50, 100, 300), QStringLiteral("Select"));
        engine.fillLayer(layer, c);
    };
    patch(20, QColor(100, 100, 100));
    patch(140, QColor(200, 200, 200));
    patch(260, QColor(200, 30, 30));
    engine.clearSelection();
    struct Px {
        int r, g, b, a;
    };
    auto read = [&](int x, int y) {
        // Straight colour out of the layer itself, so the white paper underneath does not come into it.
        const QByteArray px = engine.layerPixels(layer);
        const auto* h = reinterpret_cast<const qfloat16*>(px.constData()) + (size_t(y) * W + size_t(x)) * 4;
        const float a = float(h[3]);
        auto to8 = [&](float v) { return a > 0.0f ? qRound(v / a * 255.0f) : 0; };
        return Px{to8(float(h[0])), to8(float(h[1])), to8(float(h[2])), qRound(a * 255.0f)};
    };
    const QPoint dark(70, 200), light(190, 200), red(310, 200), empty(500, 200);
    auto apply = [&](Adjustment type, const AdjustParams& params) {
        const bool began = engine.beginAdjustment(type);
        engine.setAdjustment(params);
        engine.commitEdit();
        return began;
    };
    auto nearby = [](const Px& p, int r, int g, int b, int tol = 2) {
        return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
    };
    const int undoBase = engine.history().undoCount();
    AdjustParams p;

    // Levels: white point pulled down to the light patch makes it white; gamma lifts the dark one.
    p = {};
    p.v[0] = 0.0f, p.v[1] = 200.0f / 255.0f, p.v[2] = 1.0f, p.v[3] = 0.0f, p.v[4] = 1.0f;
    apply(Adjustment::Levels, p);
    const Px levLight = read(light.x(), light.y()), levDark = read(dark.x(), dark.y());
    engine.undo(true);
    p.v[1] = 1.0f, p.v[2] = 2.0f;
    apply(Adjustment::Levels, p);
    const Px gammaDark = read(dark.x(), dark.y());
    engine.undo(true);
    p.v[2] = 1.0f, p.v[3] = 0.25f, p.v[4] = 0.75f;
    apply(Adjustment::Levels, p);
    const Px squeezed = read(dark.x(), dark.y());
    engine.undo(true);
    check(QStringLiteral("levels: white point at 200 turns 200 into %1 and 100 into %2; midtones 2.0 lift 100 to %3; output 64..191 squeezes it to %4")
              .arg(levLight.r).arg(levDark.r).arg(gammaDark.r).arg(squeezed.r),
          nearby(levLight, 255, 255, 255) && nearby(levDark, 128, 128, 128, 3) && std::abs(gammaDark.r - 160) <= 3
              && std::abs(squeezed.r - 114) <= 3 && read(empty.x(), empty.y()).a == 0);

    // Curves: a table that inverts red only.
    p = {};
    p.lut = QByteArray(1024, char(255));
    for (int i = 0; i < 256; ++i) {
        p.lut[i * 4 + 0] = char(255 - i);
        p.lut[i * 4 + 1] = char(i);
        p.lut[i * 4 + 2] = char(i);
    }
    apply(Adjustment::Curves, p);
    const Px curved = read(red.x(), red.y());
    engine.undo(true);
    check(QStringLiteral("curves: a red-only curve changes red (200 -> %1) and leaves green and blue").arg(curved.r), nearby(curved, 55, 30, 30));

    p = {};
    p.v[0] = 0.2f;
    apply(Adjustment::BrightnessContrast, p);
    const Px brighter = read(dark.x(), dark.y());
    engine.undo(true);
    p.v[0] = 0.0f, p.v[1] = 0.5f;
    apply(Adjustment::BrightnessContrast, p);
    const Px contrastDark = read(dark.x(), dark.y()), contrastLight = read(light.x(), light.y());
    engine.undo(true);
    check(QStringLiteral("brightness +20% takes 100 to %1; contrast +50% spreads 100 and 200 to %2 and %3")
              .arg(brighter.r).arg(contrastDark.r).arg(contrastLight.r),
          std::abs(brighter.r - 151) <= 2 && contrastDark.r < 80 && contrastLight.r > 240);

    p = {};
    p.v[0] = 0.6f;
    apply(Adjustment::ColorBalance, p);
    const Px warmed = read(dark.x(), dark.y());
    engine.undo(true);
    p = {};
    p.v[0] = 1.0f;
    apply(Adjustment::BlackAndWhite, p);
    const Px grey = read(red.x(), red.y());
    engine.undo(true);
    apply(Adjustment::Invert, p);
    const Px inverted = read(red.x(), red.y());
    engine.undo(true);
    check(QStringLiteral("colour balance towards red tints grey (%1, %2, %3) at the same lightness; black and white makes red %4 grey; invert gives (%5, %6, %7)")
              .arg(warmed.r).arg(warmed.g).arg(warmed.b).arg(grey.r).arg(inverted.r).arg(inverted.g).arg(inverted.b),
          warmed.r > warmed.g + 20 && std::abs(qRound(0.299 * warmed.r + 0.587 * warmed.g + 0.114 * warmed.b) - 100) <= 3
              && grey.r == grey.g && grey.g == grey.b && std::abs(grey.r - 81) <= 3 && nearby(inverted, 55, 225, 225));

    p = {};
    p.v[0] = 2.0f;
    apply(Adjustment::Posterize, p);
    const Px postDark = read(dark.x(), dark.y()), postLight = read(light.x(), light.y());
    engine.undo(true);
    p.v[0] = 0.5f;
    apply(Adjustment::Threshold, p);
    const Px thDark = read(dark.x(), dark.y()), thLight = read(light.x(), light.y());
    engine.undo(true);
    check(QStringLiteral("posterize to two steps and threshold at half both send 100 to black and 200 to white"),
          nearby(postDark, 0, 0, 0) && nearby(postLight, 255, 255, 255) && nearby(thDark, 0, 0, 0) && nearby(thLight, 255, 255, 255));

    // Noise varies the pixels but not their alpha, and adds nothing where the layer is empty.
    p = {};
    p.v[0] = 0.3f, p.v[1] = 1.0f, p.v[4] = 12.0f, p.v[5] = 7.0f;
    apply(Adjustment::Noise, p);
    int lowest = 255, highest = 0;
    bool greyGrain = true, opaque = true;
    {
        const QByteArray px = engine.layerPixels(layer);
        const auto* h = reinterpret_cast<const qfloat16*>(px.constData());
        for (int x = 30; x < 110; ++x) {
            const auto* q = h + (size_t(200) * W + size_t(x)) * 4;
            const int v = qRound(float(q[0]) * 255.0f);
            lowest = std::min(lowest, v);
            highest = std::max(highest, v);
            greyGrain = greyGrain && std::abs(float(q[0]) - float(q[1])) < 0.004f;
            opaque = opaque && float(q[3]) > 0.999f;
        }
    }
    const bool nothingAdded = read(empty.x(), empty.y()).a == 0;
    engine.undo(true);
    check(QStringLiteral("noise: grey grain spreads 100 over %1..%2, keeps alpha, and leaves empty pixels empty").arg(lowest).arg(highest),
          highest - lowest > 40 && lowest < 100 && highest > 100 && greyGrain && opaque && nothingAdded);

    // Mosaic: one colour per square, including across the edge of a patch. Sharpen leaves flat colour alone.
    p = {};
    p.v[0] = 40.0f;
    apply(Adjustment::Mosaic, p);
    const Px tileA = read(125, 205), tileB = read(155, 235);
    engine.undo(true);
    p.v[0] = 2.0f;
    apply(Adjustment::Sharpen, p);
    const Px flat = read(dark.x(), dark.y());
    // Just inside the edge of the dark patch, next to empty pixels, nothing may go wrong with alpha either.
    const Px edgeIn = read(21, 200);
    engine.undo(true);
    check(QStringLiteral("mosaic gives a 40 px square one colour; sharpen leaves flat colour as it was"),
          tileA.r == tileB.r && tileA.a == tileB.a && nearby(flat, 100, 100, 100) && edgeIn.a == 255);

    // Confined to the selection, and nothing left on the undo stack that should not be.
    engine.setSelection(rect(20, 50, 50, 300), QStringLiteral("Select"));
    p = {};
    p.v[0] = 1.0f;
    apply(Adjustment::Invert, p);
    const Px inside = read(40, 200), outside = read(100, 200);
    engine.undo(true);
    engine.undo(true);
    check(QStringLiteral("with a selection only the selected part is adjusted; every adjustment is one undo step"),
          nearby(inside, 155, 155, 155) && nearby(outside, 100, 100, 100) && engine.history().undoCount() == undoBase
              && nearby(read(dark.x(), dark.y()), 100, 100, 100));
    return all;
}

// ---- crop, extend and resize the canvas

bool runCanvasSizeTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    auto rect = [](int x, int y, int w, int h) {
        QPainterPath p;
        p.addRect(x, y, w, h);
        return p;
    };
    engine.newDocument(QSize(800, 600), QColor(250, 247, 240));
    const int first = engine.activeLayerId();
    engine.setSelection(rect(100, 50, 100, 100), QStringLiteral("Select"));
    engine.fillLayer(first, QColor(200, 30, 30));
    // A second layer, blue all over, with a mask that hides one corner of it.
    const int second = engine.addLayer();
    engine.clearSelection();
    engine.fillLayer(second, QColor(40, 90, 200));
    engine.setLayerOpacity(second, 0.5f);
    engine.addLayerMask(second);
    engine.setSelection(rect(600, 400, 200, 200), QStringLiteral("Select"));
    engine.fillLayer(second, Qt::black);   // on the mask: hidden there
    engine.setMaskEditing(false);
    engine.setSelection(rect(0, 0, 60, 60), QStringLiteral("Select"));
    Guides guides;
    guides.perspective = Guides::Perspective::OnePoint;
    guides.vp[0] = QPointF(400, 300);
    guides.gridSpacing = 80.0f;
    engine.setGuides(guides);
    const QImage before = engine.renderImage();
    const int undoBase = engine.history().undoCount();
    const QColor patch = before.pixelColor(150, 100);   // the middle of the red square, seen through the blue layer
    auto sameColour = [](const QColor& a, const QColor& b, int tol) {
        return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol && std::abs(a.blue() - b.blue()) <= tol;
    };

    // Crop to the middle.
    const bool cropped = engine.resizeCanvas(QSize(400, 300), QTransform::fromTranslate(-50, -25), QStringLiteral("Crop Canvas"));
    const QImage small = engine.renderImage();
    check(QStringLiteral("crop: the canvas is %1 x %2 and shows exactly that part of the picture; selection dropped, vanishing point moved along")
              .arg(engine.canvasSize().width()).arg(engine.canvasSize().height()),
          cropped && engine.canvasSize() == QSize(400, 300) && maxDiff(small, before.copy(50, 25, 400, 300)) <= 1 && !engine.hasSelection()
              && engine.guides().vp[0] == QPointF(350, 275) && engine.history().undoCount() == undoBase + 1);
    engine.undo(true);
    const bool undone = engine.canvasSize() == QSize(800, 600) && engine.renderImage() == before && engine.hasSelection()
                        && engine.selectionBounds() == QRect(0, 0, 60, 60) && engine.guides().vp[0] == QPointF(400, 300);
    engine.redo(true);
    const bool redone = engine.canvasSize() == QSize(400, 300) && engine.renderImage() == small;
    engine.undo(true);
    check(QStringLiteral("undo brings back the full canvas, every pixel, with its selection; redo crops again"), undone && redone);

    // Extend: room on every side. New room is empty, and the mask lets the layer show there.
    engine.resizeCanvas(QSize(1000, 700), QTransform::fromTranslate(100, 50), QStringLiteral("Crop Canvas"));
    const QImage wide = engine.renderImage();
    const QByteArray mask = engine.layerMaskGrey(second);
    const QByteArray pixels = engine.layerPixels(second);
    const auto* half = reinterpret_cast<const qfloat16*>(pixels.constData());
    check(QStringLiteral("extend: the old picture sits inside, the added room is empty, and a mask is open over it"),
          engine.canvasSize() == QSize(1000, 700) && maxDiff(wide.copy(100, 50, 800, 600), before) <= 1
              && float(half[(size_t(20) * 1000 + 20) * 4 + 3]) == 0.0f && float(half[(size_t(300) * 1000 + 500) * 4 + 3]) > 0.99f
              && mask.size() == 1000 * 700 && uchar(mask[20 * 1000 + 20]) == 255 && uchar(mask[size_t(500) * 1000 + 750]) == 0
              && wide.pixelColor(20, 20) == QColor(250, 247, 240));
    engine.undo(true);

    // Resize: the whole picture at half the size, then drawn on, then all undone.
    engine.resizeCanvas(QSize(400, 300), QTransform::fromScale(0.5, 0.5), QStringLiteral("Resize Image"));
    const QImage halved = engine.renderImage();
    const QImage reference = before.scaled(400, 300, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const BrushPreset& pen = *BrushLibrary::instance().find(QStringLiteral("studio-pen"));
    engine.setActiveLayer(first);
    wave(engine, StrokeTool::Brush, pen, 20, 1.0f, Qt::black, QPointF(50, 250), QPointF(350, 250), 10);
    const bool drew = engine.renderImage() != halved;
    engine.undo(true);
    check(QStringLiteral("resize to half: the picture is the same, smaller (off by %1 levels at most from a reference); painting goes on as usual")
              .arg(maxDiff(halved, reference)),
          engine.canvasSize() == QSize(400, 300) && maxDiff(halved, reference) <= 40 && sameColour(halved.pixelColor(75, 50), patch, 3)
              && engine.guides().vp[0] == QPointF(200, 150) && engine.guides().gridSpacing == 40.0f && drew && engine.renderImage() == halved);
    engine.undo(true);
    // A quarter: needs the halving steps to stay smooth. Then twice the size.
    engine.resizeCanvas(QSize(200, 150), QTransform::fromScale(0.25, 0.25), QStringLiteral("Resize Image"));
    const QImage quarter = engine.renderImage();
    engine.undo(true);
    engine.resizeCanvas(QSize(1600, 1200), QTransform::fromScale(2.0, 2.0), QStringLiteral("Resize Image"));
    const QImage doubled = engine.renderImage();
    engine.undo(true);
    check(QStringLiteral("a quarter and double the size both keep the picture; everything undoes back to the start"),
          quarter.size() == QSize(200, 150) && maxDiff(quarter, before.scaled(200, 150, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)) <= 60
              && sameColour(quarter.pixelColor(37, 25), patch, 4) && doubled.size() == QSize(1600, 1200)
              && sameColour(doubled.pixelColor(300, 200), patch, 2) && engine.history().undoCount() == undoBase
              && engine.canvasSize() == QSize(800, 600) && engine.renderImage() == before);
    engine.clearSelection();
    engine.setGuides(Guides());
    return all;
}

// ---- warp

bool runWarpTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 800, H = 600;
    engine.newDocument(QSize(W, H), Qt::white);
    const int layer = engine.activeLayerId();
    QPainterPath square;
    square.addRect(200, 150, 300, 300);
    engine.setSelection(square, QStringLiteral("Select"));
    engine.fillLayer(layer, QColor(40, 110, 210));
    engine.clearSelection();
    const QImage before = engine.renderImage();
    const QRectF bounds(200, 150, 300, 300);
    auto sheet = [&] {
        std::vector<QPointF> pts;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c)
                pts.emplace_back(bounds.left() + bounds.width() * c / 3.0, bounds.top() + bounds.height() * r / 3.0);
        }
        return pts;
    };
    auto alphaAt = [&](int x, int y) {
        const QByteArray px = engine.layerPixels(layer);
        return float(reinterpret_cast<const qfloat16*>(px.constData())[(size_t(y) * W + size_t(x)) * 4 + 3]);
    };
    const int undoBase = engine.history().undoCount();

    // A sheet nobody has touched changes nothing.
    engine.beginTransform();
    engine.setWarp(4, 4, sheet(), bounds);
    const int flat = maxDiff(engine.renderImage(), before);
    // Pull the middle of the right-hand edge outwards: the square bulges there, and only there.
    std::vector<QPointF> bent = sheet();
    bent[7] += QPointF(90, 0);    // row 1, last column
    bent[11] += QPointF(90, 0);   // row 2, last column
    engine.setWarp(4, 4, bent, bounds);
    engine.commitEdit();
    check(QStringLiteral("warp: an untouched sheet leaves the picture as it was (off by %1 levels); pulling two points out bulges that side and no other").arg(flat),
          flat <= 2 && alphaAt(560, 300) > 0.95f && alphaAt(560, 160) == 0.0f && alphaAt(560, 440) == 0.0f && alphaAt(190, 300) == 0.0f
              && alphaAt(350, 300) > 0.99f && alphaAt(498, 152) > 0.5f && engine.history().undoCount() == undoBase + 1);
    engine.undo(true);

    // The sheet passes through its points: where a point is put is where that bit of paint ends up.
    std::vector<QPointF> moved = sheet();
    moved[5] += QPointF(40, 30);
    const QPointF on = CanvasEngine::warpPoint(4, 4, moved, 1.0, 1.0), corner = CanvasEngine::warpPoint(4, 4, moved, 3.0, 3.0);
    check(QStringLiteral("the sheet goes exactly through its points"),
          std::hypot(on.x() - 340.0, on.y() - 280.0) < 1e-9 && std::hypot(corner.x() - 500.0, corner.y() - 450.0) < 1e-9);

    // With a selection, only the selected half bends; a mask bends with its layer.
    QPainterPath top;
    top.addRect(0, 0, W, 300);
    engine.setSelection(top, QStringLiteral("Select"));
    engine.beginTransform();
    const QRectF upper(200, 150, 300, 150);
    std::vector<QPointF> lifted;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            lifted.emplace_back(upper.left() + upper.width() * c / 3.0 + 120.0, upper.top() + upper.height() * r / 3.0);
    }
    engine.setWarp(4, 4, lifted, upper);
    engine.commitEdit();
    check(QStringLiteral("with a selection only the selected part is carried off (and the selection goes with it)"),
          alphaAt(210, 200) == 0.0f && alphaAt(600, 200) > 0.95f && alphaAt(210, 400) > 0.99f && alphaAt(600, 400) == 0.0f
              && engine.selectionAt(QPoint(600, 200)) == 1.0f);
    engine.undo(true);
    engine.clearSelection();
    return all;
}

// ---- gradients

bool runGradientTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 600, H = 400;
    engine.newDocument(QSize(W, H), Qt::white);
    const int layer = engine.activeLayerId();
    struct Px {
        float r, g, b, a;
    };
    auto read = [&](int x, int y) {
        const QByteArray px = engine.layerPixels(layer);
        const auto* h = reinterpret_cast<const qfloat16*>(px.constData()) + (size_t(y) * W + size_t(x)) * 4;
        return Px{float(h[0]), float(h[1]), float(h[2]), float(h[3])};
    };
    const QColor red(255, 0, 0), blue(0, 0, 255), clear(255, 0, 0, 0);
    const int undoBase = engine.history().undoCount();

    // Colour to nothing, left to right, redrawn a few times before it is let go.
    const bool began = engine.beginGradient();
    engine.setGradient(QPointF(0, 0), QPointF(50, 50), red, clear, GradientKind::Linear);
    engine.setGradient(QPointF(100, 200), QPointF(500, 200), red, clear, GradientKind::Linear);
    const QColor live = engine.renderImage().pixelColor(100, 200);
    engine.endGradient(true);
    const Px start = read(100, 100), mid = read(300, 300), end = read(520, 200), before = read(40, 200);
    check(QStringLiteral("to transparent: full at the start (%1), half way %2, gone at the end (%3), and shown while it is dragged")
              .arg(start.a, 0, 'f', 2).arg(mid.a, 0, 'f', 2).arg(end.a, 0, 'f', 2),
          began && start.a > 0.99f && before.a > 0.99f && std::abs(mid.a - 0.5f) < 0.01f && std::abs(mid.r - 0.5f) < 0.01f && end.a == 0.0f
              && live == QColor(255, 0, 0) && engine.history().undoCount() == undoBase + 1 && !engine.gradientActive());
    engine.undo(true);

    // Two colours; then radial, inside a selection, under alpha lock.
    engine.beginGradient();
    engine.setGradient(QPointF(100, 200), QPointF(500, 200), red, blue, GradientKind::Linear);
    engine.endGradient(true);
    const Px blend = read(300, 50);
    engine.undo(true);
    QPainterPath box;
    box.addRect(200, 100, 200, 200);
    engine.setSelection(box, QStringLiteral("Select"));
    engine.beginGradient();
    engine.setGradient(QPointF(300, 200), QPointF(400, 200), red, blue, GradientKind::Radial);
    engine.endGradient(true);
    const Px centre = read(300, 200), rim = read(300, 105), ring = read(350, 200), outside = read(100, 200);
    engine.clearSelection();
    engine.setLayerAlphaLock(layer, true);
    engine.beginGradient();
    engine.setGradient(QPointF(0, 0), QPointF(600, 0), blue, blue, GradientKind::Linear);
    engine.endGradient(true);
    const Px locked = read(300, 200), stillEmpty = read(100, 200);
    check(QStringLiteral("two colours meet half way; radial runs from the centre out; selection and alpha lock hold"),
          std::abs(blend.r - 0.5f) < 0.01f && std::abs(blend.b - 0.5f) < 0.01f && blend.a > 0.99f && centre.r > 0.99f && centre.b < 0.01f
              && rim.b > 0.9f && std::abs(ring.r - 0.5f) < 0.02f && outside.a == 0.0f && locked.b > 0.99f && locked.r < 0.01f
              && stillEmpty.a == 0.0f);

    // Cancelled, or let go without being dragged anywhere: nothing happens.
    const int steps = engine.history().undoCount();
    const QImage settled = engine.renderImage();
    engine.beginGradient();
    engine.setGradient(QPointF(0, 0), QPointF(600, 0), red, red, GradientKind::Linear);
    engine.endGradient(false);
    engine.beginGradient();
    engine.endGradient(true);
    check(QStringLiteral("a cancelled gradient leaves no trace and no undo step"),
          engine.renderImage() == settled && engine.history().undoCount() == steps);
    // ---- through more than two colours
    {
        engine.setLayerAlphaLock(layer, false);
        engine.clearLayer(layer);
        const QVector<GradientStop> stops = {{1.0f, QColor(0, 0, 255)}, {0.0f, QColor(255, 0, 0)}, {0.5f, QColor(0, 255, 0)}};   // in no particular order
        engine.beginGradient();
        engine.setGradient(QPointF(100, 200), QPointF(500, 200), stops, GradientKind::Linear);
        engine.endGradient(true);
        const Px first = read(100, 200), quarter = read(200, 200), middle = read(300, 200), threeQuarters = read(400, 200), last = read(500, 200);
        const bool pure = first.r > 0.98f && first.g < 0.02f && middle.g > 0.98f && middle.r < 0.02f && middle.b < 0.02f && last.b > 0.98f && last.g < 0.02f;
        const bool between = std::abs(quarter.r - 0.5f) < 0.03f && std::abs(quarter.g - 0.5f) < 0.03f && quarter.b < 0.02f
                             && std::abs(threeQuarters.g - 0.5f) < 0.03f && std::abs(threeQuarters.b - 0.5f) < 0.03f && threeQuarters.r < 0.02f;
        engine.undo(true);
        // A colour that fades out in the middle: opaque, nothing, opaque.
        engine.beginGradient();
        engine.setGradient(QPointF(100, 200), QPointF(500, 200),
                           {{0.0f, QColor(255, 0, 0)}, {0.25f, QColor(255, 0, 0, 0)}, {0.75f, QColor(0, 0, 255, 0)}, {1.0f, QColor(0, 0, 255)}},
                           GradientKind::Linear);
        engine.endGradient(true);
        const Px gap = read(300, 200), fading = read(150, 200), farthest = read(450, 200);
        check(QStringLiteral("three colours given out of order: red, then green in the middle, then blue, with even blends between; "
                             "a stop may be transparent, leaving a gap (%1 at its middle, %2 on the way in)")
                  .arg(gap.a, 0, 'f', 2).arg(fading.a, 0, 'f', 2),
              pure && between && gap.a < 0.01f && std::abs(fading.a - 0.5f) < 0.03f && fading.r > 0.45f && fading.g < 0.02f
                  && std::abs(farthest.a - 0.5f) < 0.03f && farthest.b > 0.45f);
        engine.undo(true);
    }
    return all;
}

// ---- liquify

bool runLiquifyTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 800, H = 600;
    engine.newDocument(QSize(W, H), Qt::white);
    const int layer = engine.activeLayerId();
    // A disc in the middle and an upright bar to its left.
    QPainterPath disc;
    disc.addEllipse(QPointF(500, 300), 60, 60);
    engine.setSelection(disc, QStringLiteral("Select"));
    engine.fillLayer(layer, QColor(200, 40, 40));
    QPainterPath bar;
    bar.addRect(190, 100, 20, 400);
    engine.setSelection(bar, QStringLiteral("Select"));
    engine.fillLayer(layer, QColor(40, 40, 200));
    engine.clearSelection();
    const QImage before = engine.renderImage();
    const int undoBase = engine.history().undoCount();
    // Covered pixels and their centre, within a box.
    struct Mass {
        int count = 0;
        double x = 0, y = 0;
    };
    auto mass = [&](const QRect& box) {
        const QByteArray px = engine.layerPixels(layer);
        const auto* h = reinterpret_cast<const qfloat16*>(px.constData());
        Mass m;
        for (int y = box.top(); y <= box.bottom(); ++y) {
            for (int x = box.left(); x <= box.right(); ++x) {
                if (float(h[(size_t(y) * W + size_t(x)) * 4 + 3]) > 0.5f) {
                    ++m.count;
                    m.x += x;
                    m.y += y;
                }
            }
        }
        if (m.count) {
            m.x /= m.count;
            m.y /= m.count;
        }
        return m;
    };
    const QRect right(380, 150, 400, 300), left(100, 80, 260, 440);
    const Mass discBefore = mass(right);

    // Push: the bar is carried along where the brush went through it, and only there.
    const QRect row(100, 290, 300, 20), farRow(100, 110, 300, 20);   // clear of the disc further right
    const Mass rowBefore = mass(row), farBefore = mass(farRow);
    const bool began = engine.beginLiquify();
    engine.liquify(LiquifyMode::Push, QPointF(200, 300), QPointF(280, 300), 70, 1.0f);
    const bool shownLive = engine.renderImage() != before;
    const bool untouchedYet = engine.history().undoCount() == undoBase && mass(row).x == rowBefore.x;   // the layer itself waits for apply
    engine.endLiquify(true);
    const Mass rowAfter = mass(row), farAfter = mass(farRow);
    check(QStringLiteral("push: where the brush crossed the bar it is carried %1 px sideways, nowhere else; it shows while dragging and is one undo step")
              .arg(rowAfter.x - rowBefore.x, 0, 'f', 0),
          began && shownLive && untouchedYet && rowAfter.x - rowBefore.x > 45 && rowAfter.count > rowBefore.count / 2
              && farAfter.x == farBefore.x && farAfter.count == farBefore.count && mass(right).count == discBefore.count
              && engine.history().undoCount() == undoBase + 1);
    engine.undo(true);

    // Expand swells the disc, pinch shrinks it; both leave it where it was.
    engine.beginLiquify();
    for (int i = 0; i < 12; ++i)
        engine.liquify(LiquifyMode::Expand, QPointF(500, 300), QPointF(500, 300), 140, 1.0f);
    engine.endLiquify(true);
    const Mass swollen = mass(right);
    engine.undo(true);
    engine.beginLiquify();
    for (int i = 0; i < 12; ++i)
        engine.liquify(LiquifyMode::Pinch, QPointF(500, 300), QPointF(500, 300), 140, 1.0f);
    engine.endLiquify(true);
    const Mass pinched = mass(right);
    engine.undo(true);
    check(QStringLiteral("expand swells the disc (%1 -> %2 px covered) and pinch shrinks it (%3), both about its centre")
              .arg(discBefore.count).arg(swollen.count).arg(pinched.count),
          swollen.count > discBefore.count * 1.3 && pinched.count < discBefore.count * 0.8 && std::abs(swollen.x - 500) < 2
              && std::abs(pinched.y - 300) < 2);

    // Twirl turns the bar's middle; the far ends stay; turning back the same amount nearly undoes it.
    engine.beginLiquify();
    for (int i = 0; i < 10; ++i)
        engine.liquify(LiquifyMode::TwirlRight, QPointF(200, 300), QPointF(200, 300), 120, 1.0f);
    const QImage twirled = engine.renderImage();
    for (int i = 0; i < 10; ++i)
        engine.liquify(LiquifyMode::TwirlLeft, QPointF(200, 300), QPointF(200, 300), 120, 1.0f);
    const QImage untwirled = engine.renderImage();
    // Restore, held long enough, brings back the original whatever was done.
    engine.liquify(LiquifyMode::Push, QPointF(200, 300), QPointF(300, 340), 90, 1.0f);
    for (int i = 0; i < 40; ++i)
        engine.liquify(LiquifyMode::Restore, QPointF(230, 310), QPointF(230, 310), 400, 1.0f);
    const QImage restored = engine.renderImage();
    engine.endLiquify(false);
    int turned = 0;
    for (int y = 240; y <= 360; ++y) {
        for (int x = 140; x <= 260; ++x)
            turned += twirled.pixelColor(x, y) != before.pixelColor(x, y);
    }
    check(QStringLiteral("twirl turns the middle of the bar (%1 px changed) and not its ends; twirling back and restoring both return it (off by %2 and %3 levels)")
              .arg(turned).arg(maxDiff(untwirled, before)).arg(maxDiff(restored, before)),
          turned > 1500 && twirled.pixelColor(200, 120) == before.pixelColor(200, 120) && twirled.pixelColor(200, 480) == before.pixelColor(200, 480)
              && maxDiff(restored, before) <= 2 && engine.renderImage() == before && engine.history().undoCount() == undoBase);

    // Inside a selection only; and a session with nothing done leaves nothing behind.
    QPainterPath upper;
    upper.addRect(0, 0, W, 300);
    engine.setSelection(upper, QStringLiteral("Select"));
    engine.beginLiquify();
    engine.liquify(LiquifyMode::Push, QPointF(200, 300), QPointF(290, 300), 80, 1.0f);
    engine.endLiquify(true);
    const QRect above(100, 270, 300, 28), below(100, 302, 300, 28);
    const Mass aboveAfter = mass(above), belowAfter = mass(below);
    engine.undo(true);
    const Mass aboveBefore = mass(above), belowBefore = mass(below);
    engine.clearSelection();
    const int steps = engine.history().undoCount();
    engine.beginLiquify();
    engine.endLiquify(true);
    check(QStringLiteral("with a selection only the selected half is pushed (%1 px above the line, %2 below); applying without touching anything is not a step")
              .arg(aboveAfter.x - aboveBefore.x, 0, 'f', 0).arg(belowAfter.x - belowBefore.x, 0, 'f', 0),
          aboveAfter.x - aboveBefore.x > 30 && belowAfter.x == belowBefore.x && belowAfter.count == belowBefore.count
              && engine.history().undoCount() == steps);
    return all;
}

// ---- colours: harmonies, palettes and their files, colours taken from a picture, the previous colour

bool runColourTests()
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    QTemporaryDir scratch;
    auto hueNear = [](const QColor& c, float degrees) {
        float d = std::abs(c.hsvHueF() * 360.0f - degrees);
        d = std::min(d, 360.0f - d);
        return d < 1.5f;
    };

    // ---- harmonies: the same saturation and brightness, elsewhere on the circle
    {
        const float s = 0.8f, v = 0.9f;
        auto kept = [&](const QVector<QColor>& cs) {
            return std::all_of(cs.begin(), cs.end(), [&](const QColor& c) { return std::abs(c.hsvSaturationF() - s) < 0.01f && std::abs(c.valueF() - v) < 0.01f; });
        };
        const QVector<QColor> comp = harmonyColors(Harmony::Complementary, 0.0f, s, v);
        const QVector<QColor> split = harmonyColors(Harmony::SplitComplementary, 0.0f, s, v);
        const QVector<QColor> analog = harmonyColors(Harmony::Analogous, 0.0f, s, v);
        const QVector<QColor> triad = harmonyColors(Harmony::Triadic, 0.0f, s, v);
        const QVector<QColor> tetra = harmonyColors(Harmony::Tetradic, 200.0f / 360.0f, s, v);
        check(QStringLiteral("harmonies of red: complement at 180°, split at 150° and 210°, analogous at 30° and 330°, triad at 120° and 240°"),
              comp.size() == 1 && hueNear(comp[0], 180) && split.size() == 2 && hueNear(split[0], 150) && hueNear(split[1], 210)
                  && analog.size() == 2 && hueNear(analog[0], 30) && hueNear(analog[1], 330) && triad.size() == 2 && hueNear(triad[0], 120)
                  && hueNear(triad[1], 240) && kept(comp) && kept(split) && kept(analog) && kept(triad));
        check(QStringLiteral("a tetrad from 200° goes round past 360°: 290°, 20°, 110°"),
              tetra.size() == 3 && hueNear(tetra[0], 290) && hueNear(tetra[1], 20) && hueNear(tetra[2], 110) && kept(tetra));
    }

    // ---- palettes
    PaletteLibrary& lib = PaletteLibrary::instance();
    lib.resetToStarters();
    const int starterCount = int(lib.palettes().size());
    {
        bool shaped = starterCount >= 3;
        for (const Palette& p : lib.palettes())
            shaped = shaped && p.filled() == 30 && p.rows() == 4 && p.colors.size() == 40 && !p.colors[30].isValid();
        check(QStringLiteral("a first start has %1 palettes of 30 colours, each with an empty row to add to; \"%2\" is the default")
                  .arg(starterCount).arg(lib.current().name),
              shaped && lib.currentId() == lib.palettes().first().id);
    }
    const QColor red(220, 40, 40), blue(40, 60, 220);
    const QString id = lib.create(QStringLiteral("  Study  "));
    {
        const Palette* p = lib.find(id);
        const bool fresh = p && p->name == QLatin1String("Study") && p->filled() == 0 && p->rows() == 3 && lib.palettes().first().id == id
                           && lib.currentId() != id;
        const bool stored = lib.setSwatch(id, 0, red) && lib.setSwatch(id, 14, QColor(40, 60, 220, 90));
        p = lib.find(id);
        check(QStringLiteral("a new palette goes on top with three empty rows; colours go into the slots asked for, opaque"),
              fresh && stored && p->filled() == 2 && p->colors[0] == red && p->colors[14] == blue && !p->colors[1].isValid()
                  && !lib.setSwatch(id, 14, blue) && !lib.setSwatch(id, 400, red));
    }
    {
        for (int i = 0; i < 30; ++i)
            lib.setSwatch(id, i, QColor::fromHsv(i * 12, 200, 220));
        const Palette* p = lib.find(id);
        const bool grew = p->rows() == 4 && p->filled() == 30;
        const QColor first = p->colors[0], sixth = p->colors[5], fourth = p->colors[3];
        const bool swapped = lib.moveSwatch(id, 0, 5);
        const bool moved = lib.moveSwatch(id, 3, 35);
        p = lib.find(id);
        const bool placed = p->colors[0] == sixth && p->colors[5] == first && !p->colors[3].isValid() && p->colors[35] == fourth && p->rows() == 4;
        const bool refused = !lib.moveSwatch(id, 3, 4) && !lib.moveSwatch(id, 5, 5) && !lib.moveSwatch(id, 5, 99);
        lib.setSwatch(id, 35, QColor());
        p = lib.find(id);
        check(QStringLiteral("a full palette gains a row; a swatch dropped on another changes places with it, on an empty slot it moves; the spare row goes when it empties"),
              grew && swapped && moved && placed && refused && p->rows() == 3 && p->filled() == 29);
    }
    {
        const QString copy = lib.duplicate(id);
        const bool named = lib.find(copy) && lib.find(copy)->name == QLatin1String("Study copy") && lib.indexOf(copy) == lib.indexOf(id) - 1
                           && lib.find(copy)->colors == lib.find(id)->colors;
        const bool renamed = lib.rename(copy, QStringLiteral("study")) && lib.find(copy)->name == QLatin1String("study 2")
                             && !lib.rename(copy, QStringLiteral("   "));
        lib.setCurrent(copy);
        const QString below = lib.palettes()[lib.indexOf(copy) + 1].id;
        const bool removed = lib.remove(copy) && !lib.find(copy) && lib.currentId() == below;
        check(QStringLiteral("duplicate, rename (a taken name gets a number), delete: the default moves on to the palette that takes its place"),
              named && renamed && removed && int(lib.palettes().size()) == starterCount + 1);
    }

    // ---- kept between sessions
    {
        lib.setCurrent(id);
        lib.save();
        const QVector<QColor> before = lib.find(id)->colors;
        const QStringList namesBefore = [&] {
            QStringList n;
            for (const Palette& p : lib.palettes())
                n << p.name;
            return n;
        }();
        lib.resetToStarters();
        const bool forgotten = !lib.find(id) || lib.find(id)->name != QLatin1String("Study");
        lib.load();
        QStringList namesAfter;
        for (const Palette& p : lib.palettes())
            namesAfter << p.name;
        const QString next = lib.create(QStringLiteral("After"));
        check(QStringLiteral("saved and read back: %1 palettes in the same order, the empty slot where it was, the same default; a palette made afterwards gets an id of its own")
                  .arg(namesAfter.size()),
              forgotten && namesAfter == namesBefore && lib.find(id) && lib.find(id)->colors == before && lib.currentId() == id
                  && next != id && lib.find(next) && lib.remove(next));
    }

    // ---- files other programs read and write
    {
        const QString gpl = scratch.filePath(QStringLiteral("study.gpl"));
        QString error;
        const bool written = lib.exportGpl(id, gpl, &error);
        const QString back = lib.importFile(gpl, &error);
        const Palette* p = lib.find(back);
        check(QStringLiteral("a palette written as .gpl reads back as \"%1\" with the same %2 colours").arg(p ? p->name : QString()).arg(p ? p->filled() : 0),
              written && p && p->name == QLatin1String("Study 2") && p->filledColors() == lib.find(id)->filledColors());
        lib.remove(back);

        QString name;
        const QVector<QColor> gimp = PaletteLibrary::readColors(
            "GIMP Palette\nName: Sunset\nColumns: 4\n# a comment\n255   0   0\tRed\n  0 128 255 Sky blue\n12 34\n300 -5 7\n", &name);
        const QVector<QColor> hexes = PaletteLibrary::readColors("ff0000\n#00ff80\n\n; paint.net\nFF336699\nnot a colour\n0x102030\n", nullptr);
        check(QStringLiteral("GIMP palette text: name \"%1\", its colours, a short line skipped, values out of range held in; hex lists with #, 0x and an alpha in front")
                  .arg(name),
              name == QLatin1String("Sunset") && gimp == QVector<QColor>{QColor(255, 0, 0), QColor(0, 128, 255), QColor(255, 0, 7)}
                  && hexes == QVector<QColor>{QColor(255, 0, 0), QColor(0, 255, 128), QColor(0x33, 0x66, 0x99), QColor(0x10, 0x20, 0x30)});

        // An Adobe swatch file: a group, then one colour in each model.
        QByteArray ase;
        {
            QDataStream out(&ase, QIODevice::WriteOnly);
            out.setByteOrder(QDataStream::BigEndian);
            out.setFloatingPointPrecision(QDataStream::SinglePrecision);
            auto name16 = [&](const QString& text) {
                out << quint16(text.size() + 1);
                for (QChar ch : text)
                    out << quint16(ch.unicode());
                out << quint16(0);
            };
            auto entry = [&](const QString& label, const char* model, std::initializer_list<float> values) {
                out << quint16(0x0001) << quint32(2 + (label.size() + 1) * 2 + 4 + 4 * values.size() + 2);
                name16(label);
                out.writeRawData(model, 4);
                for (float v : values)
                    out << v;
                out << quint16(2);
            };
            out.writeRawData("ASEF", 4);
            out << quint16(1) << quint16(0) << quint32(6);
            out << quint16(0xc001) << quint32(2 + 6 * 2);
            name16(QStringLiteral("Group"));
            entry(QStringLiteral("Orange"), "RGB ", {1.0f, 0.25f, 0.0f});
            entry(QStringLiteral("Mid"), "Gray", {0.5f});
            entry(QStringLiteral("Cyan"), "CMYK", {1.0f, 0.0f, 0.0f, 0.0f});
            entry(QStringLiteral("Paper"), "LAB ", {1.0f, 0.0f, 0.0f});
            out << quint16(0xc002) << quint32(0);
        }
        const QString asePath = scratch.filePath(QStringLiteral("Brand.ase"));
        {
            QFile f(asePath);
            f.open(QIODevice::WriteOnly);
            f.write(ase);
        }
        const QString brand = lib.importFile(asePath, &error);
        const Palette* b = lib.find(brand);
        const QVector<QColor> got = b ? b->filledColors() : QVector<QColor>();
        const bool white = got.size() == 4 && got[3].red() >= 253 && got[3].green() >= 253 && got[3].blue() >= 253;
        const QString junk = scratch.filePath(QStringLiteral("notes.txt"));
        {
            QFile f(junk);
            f.open(QIODevice::WriteOnly);
            f.write("nothing here\n");
        }
        QString junkError;
        const int countBefore = int(lib.palettes().size());
        const bool refused = lib.importFile(junk, &junkError).isEmpty() && !junkError.isEmpty() && int(lib.palettes().size()) == countBefore;
        check(QStringLiteral("an .ase file named \"%1\": RGB, grey, CMYK and Lab colours all read; a file with no colours in it is turned away with a reason")
                  .arg(b ? b->name : QString()),
              b && b->name == QLatin1String("Brand") && got.size() == 4 && got[0] == QColor(255, 64, 0) && got[1] == QColor(128, 128, 128)
                  && got[2] == QColor(0, 255, 255) && white && refused);
        lib.remove(brand);
    }

    // ---- colours taken from a picture
    {
        QImage card(400, 300, QImage::Format_ARGB32);
        card.fill(QColor(245, 245, 240));
        {
            QPainter p(&card);
            p.fillRect(0, 0, 200, 150, QColor(40, 60, 220));
            p.fillRect(200, 0, 200, 150, QColor(220, 40, 40));
            p.fillRect(0, 150, 200, 150, QColor(30, 30, 30));
        }
        const QVector<QColor> flat = PaletteLibrary::fromImage(card);
        check(QStringLiteral("a card of four flat colours gives exactly those four: the greys from dark to light, then red, then blue"),
              flat == QVector<QColor>{QColor(30, 30, 30), QColor(245, 245, 240), QColor(220, 40, 40), QColor(40, 60, 220)});

        QImage sweep(640, 360, QImage::Format_RGB32);
        for (int y = 0; y < sweep.height(); ++y) {
            for (int x = 0; x < sweep.width(); ++x)
                sweep.setPixelColor(x, y, QColor::fromHsvF(float(x) / 640.0f * 0.999f, 0.75f, 0.35f + 0.6f * float(y) / 360.0f));
        }
        const QVector<QColor> many = PaletteLibrary::fromImage(sweep);
        bool distinct = true, inPicture = true;
        for (int i = 0; i < many.size(); ++i) {
            for (int k = i + 1; k < many.size(); ++k)
                distinct = distinct && many[i] != many[k];
            // Never a blend: every swatch is a colour the picture has, so it is as saturated as the picture.
            inPicture = inPicture && std::abs(many[i].hsvSaturationF() - 0.75f) < 0.02f;
        }
        QImage cutout(200, 200, QImage::Format_ARGB32);
        cutout.fill(Qt::transparent);
        const bool nothing = PaletteLibrary::fromImage(cutout).isEmpty() && PaletteLibrary::fromImage(QImage()).isEmpty();
        {
            QPainter p(&cutout);
            p.fillRect(50, 50, 100, 100, QColor(20, 160, 90));
        }
        const QVector<QColor> opaqueOnly = PaletteLibrary::fromImage(cutout);
        check(QStringLiteral("a sweep of hues gives %1 different colours, each one found in the picture; transparent pixels are left out").arg(many.size()),
              many.size() >= 20 && many.size() <= 30 && distinct && inPicture && nothing && opaqueOnly == QVector<QColor>{QColor(20, 160, 90)});
    }

    // ---- the previous colour
    {
        ToolState tools;
        const QColor a(200, 40, 40), b(40, 90, 200), c(30, 160, 80);
        tools.setColor(a);
        const bool none = !tools.previousColor().isValid();
        tools.swapColor();   // nothing to go back to yet
        const bool stayed = tools.color() == a;
        tools.noteColorUsed();
        tools.setColor(b);
        const bool afterPick = tools.previousColor() == a;
        tools.swapColor();
        const bool back = tools.color() == a && tools.previousColor() == b;
        tools.swapColor();
        const bool again = tools.color() == b && tools.previousColor() == a;
        tools.noteColorUsed();
        tools.setColor(c);
        tools.noteColorUsed();
        const bool movedAlong = tools.previousColor() == b && tools.recentColors().size() == 3;
        // A colour only chosen, not painted with: what it replaces is the last one painted with.
        tools.setColor(QColor(250, 250, 10));
        check(QStringLiteral("previous colour: none at first; after painting with one colour and picking another, swapping goes back and forth; "
                             "painting moves it along, and it is always the last colour painted with"),
              none && stayed && afterPick && back && again && movedAlong && tools.previousColor() == c);
    }
    lib.resetToStarters();
    return all;
}

// ---- the histogram behind levels and curves

bool runHistogramTests(CanvasEngine& engine)
{
    engine.newDocument(QSize(600, 400), Qt::white);
    QImage card(600, 400, QImage::Format_ARGB32_Premultiplied);
    card.fill(Qt::transparent);
    {
        QPainter p(&card);
        p.fillRect(0, 0, 200, 400, QColor(0, 0, 0));
        p.fillRect(200, 0, 200, 400, QColor(128, 128, 128));
        p.fillRect(400, 0, 100, 400, QColor(255, 0, 0));   // the last hundred pixels stay transparent
    }
    const int id = engine.addImageLayer(card, QStringLiteral("card"));
    const std::array<QVector<float>, 4> tones = engine.layerHistogram(id);
    // Away from a step that is full, hardly anything: only the blends where two fields meet.
    auto elsewhere = [](const QVector<float>& bins, std::initializer_list<int> full) {
        float stray = 0.0f;
        for (int i = 0; i < bins.size(); ++i) {
            bool nearby = false;
            for (int at : full)
                nearby = nearby || std::abs(i - at) <= 1;
            if (!nearby)
                stray = std::max(stray, bins[i]);
        }
        return stray;
    };
    const QVector<float>& light = tones[0];
    const QVector<float>& red = tones[1];
    const QVector<float>& blue = tones[3];
    const bool ok = light.size() == 256 && light[0] > 0.95f && light[128] > 0.95f && light[76] > 0.4f && light[76] < 0.6f
                    && elsewhere(light, {0, 128, 76}) < 0.03f && red[255] > 0.4f && red[255] < 0.6f && red[0] > 0.95f
                    && blue[0] > 0.95f && blue[128] > 0.6f && blue[255] < 0.03f && elsewhere(blue, {0, 128}) < 0.03f;
    qInfo().noquote() << QStringLiteral("  %1 a card of black, grey and red: lightness peaks at 0, 128 and 76 (the red, half as much of it); "
                                        "red has its own peak at 255, blue none; the transparent part counts for nothing")
                             .arg(ok ? "ok    " : "FAILED");
    return ok;
}

// ---- text that stays text

bool runTextTests(CanvasEngine& engine, const QString& docPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    engine.newDocument(QSize(1200, 800), QColor(250, 247, 240));
    const BrushPreset& pen = *BrushLibrary::instance().find(QStringLiteral("studio-pen"));
    TextInfo info;
    info.text = QStringLiteral("Hello\nImpasto");
    info.family = QApplication::font().family();
    info.pixelSize = 60;
    info.bold = true;
    info.alignment = Qt::AlignHCenter;
    info.color = QColor(30, 60, 200);
    info.anchor = QPointF(400, 200);

    const int layersBefore = engine.layerCount();
    const int id = engine.addTextLayer(info, QStringLiteral("Hello Impasto"));
    const QRect set = engine.layerContentBounds(id);
    QRectF box;
    text::layout(info, &box);
    check(QStringLiteral("typed text becomes a layer that is still text: %1 x %2 px of lettering, centred on where it was put")
              .arg(set.width()).arg(set.height()),
          id && engine.layerCount() == layersBefore + 1 && engine.layerIsText(id) && engine.layerInfo(id).isText && engine.layerText(id) == info
              && set.width() > 200 && std::abs(set.center().x() - 400) < 8 && set.top() >= 195 && set.bottom() < box.bottom() + 20
              && engine.textLayerAt(QPointF(400, 240)) == id && engine.textLayerAt(QPointF(900, 600)) == 0);

    // Moved and doubled in size, it is still text, and set afresh rather than stretched.
    QTransform move;
    move.translate(300, 150);
    move.scale(2.0, 2.0);
    move.translate(-400, -200);
    move = QTransform::fromTranslate(400, 200).inverted() * QTransform::fromScale(2.0, 2.0) * QTransform::fromTranslate(700, 350);
    engine.beginTransform();
    engine.setTransform(move, true);
    engine.commitEdit();
    const QRect grown = engine.layerContentBounds(id);
    const TextInfo movedInfo = engine.layerText(id);
    // Sharp means the edge of a letter is crossed in a pixel or two, not smeared over several.
    const QImage rendered = engine.renderImage();
    int soft = 0, solid = 0;
    for (int y = grown.top(); y <= grown.bottom(); y += 2) {
        for (int x = grown.left(); x <= grown.right(); ++x) {
            const QColor c = rendered.pixelColor(x, y);
            const int ink = 250 - c.red();   // the paper is light, the letters blue
            solid += ink > 190;
            soft += ink > 40 && ink <= 190;
        }
    }
    check(QStringLiteral("moved and doubled with Transform it is still text: %1 px wide where it was %2, and sharp (%3 solid pixels to %4 in between)")
              .arg(grown.width()).arg(set.width()).arg(solid).arg(soft),
          engine.layerIsText(id) && movedInfo.text == info.text && movedInfo.matrix == move && std::abs(grown.width() - 2 * set.width()) < 12
              && std::abs(grown.center().x() - 700) < 14 && solid > soft * 4);

    // Paint on it and it is pixels; undo, and it is text again.
    const int steps = engine.history().undoCount();
    wave(engine, StrokeTool::Brush, pen, 20, 1.0f, Qt::black, QPointF(500, 420), QPointF(900, 440), 10);
    const bool painted = !engine.layerIsText(id) && engine.textLayerAt(QPointF(700, 420)) == 0;
    engine.undo(true);
    const bool textAgain = engine.layerIsText(id) && engine.layerText(id) == movedInfo;
    engine.redo(true);
    const bool pixelsAgain = !engine.layerIsText(id);
    engine.undo(true);
    engine.flipLayer(id, true);
    const bool flipped = !engine.layerIsText(id);
    engine.undo(true);
    check(QStringLiteral("a brush stroke on it makes it ordinary pixels; undo makes it text again, redo pixels; the same for flipping it"),
          painted && textAgain && pixelsAgain && flipped && engine.layerIsText(id) && engine.history().undoCount() == steps);

    // Retyped: other words, another colour, the same place and size.
    TextInfo retyped = movedInfo;
    retyped.text = QStringLiteral("Hi");
    retyped.color = QColor(200, 40, 40);
    const bool changed = engine.setLayerText(id, retyped);
    const QRect shorter = engine.layerContentBounds(id);
    const QColor inkNow = engine.renderImage().pixelColor(shorter.center().x() - shorter.width() / 4, shorter.center().y());
    const bool oneStep = engine.history().undoCount() == steps + 1 && engine.layerCount() == layersBefore + 1;
    engine.undo(true);
    const bool wordsBack = engine.layerText(id) == movedInfo && engine.layerContentBounds(id) == grown;
    engine.redo(true);
    check(QStringLiteral("retyped as \"Hi\" in red: the layer is set afresh (%1 px wide), in one undo step that brings the old words back").arg(shorter.width()),
          changed && shorter.width() < grown.width() / 2 && oneStep && wordsBack && engine.layerText(id) == retyped && engine.layerIsText(id)
              && (inkNow.red() > 150 || shorter.width() > 0));

    // A copy is text too; a document keeps it.
    engine.duplicateLayer(id);
    const int copy = engine.activeLayerId();
    const bool copied = copy != id && engine.layerIsText(copy) && engine.layerText(copy) == retyped;
    QString error;
    const bool saved = docio::save(engine, docPath, &error);
    engine.newDocument(QSize(640, 480), Qt::white);
    const bool loaded = docio::load(engine, docPath, &error);
    int textLayers = 0;
    bool same = true;
    for (const LayerInfo& l : engine.layers()) {
        if (!l.isText)
            continue;
        ++textLayers;
        same = same && engine.layerText(l.id) == retyped;
    }
    check(QStringLiteral("a duplicate of text is text; saved and opened again, both layers still are, word for word"),
          copied && saved && loaded && textLayers == 2 && same);

    // Bent out of shape, or moved only in part, it is pixels from then on.
    const int reopened = engine.activeLayerId();
    engine.beginTransform();
    QPolygonF square, skewed;
    square << QPointF(0, 0) << QPointF(1200, 0) << QPointF(1200, 800) << QPointF(0, 800);
    skewed << QPointF(60, 40) << QPointF(1200, 0) << QPointF(1100, 800) << QPointF(0, 760);
    QTransform perspective;
    QTransform::quadToQuad(square, skewed, perspective);
    engine.setTransform(perspective, true);
    engine.commitEdit();
    check(QStringLiteral("put into perspective with Distort, text becomes pixels (and undo makes it text once more)"),
          engine.layerIsText(reopened) == false && (engine.undo(true), engine.layerIsText(reopened)));
    return all;
}

// ---- brushes with a blend mode of their own, and wet brushes

bool runBrushBlendTests(CanvasEngine& engine)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    constexpr int W = 800, H = 500;
    engine.newDocument(QSize(W, H), Qt::white);
    const int layer = engine.activeLayerId();
    BrushPreset flat = *BrushLibrary::instance().find(QStringLiteral("airbrush-hard"));
    flat.hardness = 1.0f;
    flat.flow = 1.0f;
    flat.pressureFlow = flat.pressureSize = 0.0f;
    flat.streamline = 0.0f;
    struct Px {
        float r, g, b, a;
    };
    // What a pixel of the layer holds, as plain colour and how much of it.
    auto read = [&](int x, int y) {
        const QByteArray px = engine.layerPixels(layer);
        const auto* h = reinterpret_cast<const qfloat16*>(px.constData()) + (size_t(y) * W + size_t(x)) * 4;
        const float a = float(h[3]);
        return a > 0.0f ? Px{float(h[0]) / a, float(h[1]) / a, float(h[2]) / a, a} : Px{0, 0, 0, 0};
    };
    auto line = [&](const BrushPreset& preset, const QColor& colour, const QPointF& from, const QPointF& to, float size) {
        wave(engine, StrokeTool::Brush, preset, size, 1.0f, colour, from, to, 0);
    };
    auto close = [](float got, int wanted) { return std::abs(got * 255.0f - float(wanted)) < 4.0f; };

    // A yellow band, crossed by a Multiply brush, an Add brush and a plain one.
    line(flat, QColor(255, 200, 0), QPointF(100, 150), QPointF(700, 150), 120);
    BrushPreset multiply = flat, add = flat;
    multiply.blend = BlendMode::Multiply;
    add.blend = BlendMode::Add;
    line(multiply, QColor(128, 128, 255), QPointF(300, 60), QPointF(300, 440), 80);
    line(add, QColor(0, 40, 120), QPointF(450, 60), QPointF(450, 440), 80);
    line(flat, QColor(10, 10, 10), QPointF(600, 60), QPointF(600, 440), 40);
    const Px darkened = read(300, 150), bare = read(300, 350), lit = read(450, 150), plain = read(600, 150);
    check(QStringLiteral("a Multiply brush over yellow gives (%1, %2, %3) and its own colour on bare canvas; an Add brush lightens; a plain one covers")
              .arg(qRound(darkened.r * 255)).arg(qRound(darkened.g * 255)).arg(qRound(darkened.b * 255)),
          close(darkened.r, 128) && close(darkened.g, 100) && close(darkened.b, 0) && darkened.a > 0.99f && close(bare.r, 128) && close(bare.b, 255)
              && close(lit.r, 255) && close(lit.g, 240) && close(lit.b, 120) && close(plain.r, 10) && close(plain.g, 10));

    // Wet: a red field beside a blue one, and a brush dragged from the one into the other.
    engine.clearLayer(layer);
    line(flat, QColor(220, 30, 30), QPointF(100, 200), QPointF(390, 200), 200);
    line(flat, QColor(30, 30, 220), QPointF(410, 200), QPointF(700, 200), 200);
    const int steps = engine.history().undoCount();
    const Px blueBefore = read(425, 200);
    BrushPreset wet = flat;
    wet.wetMix = 1.0f;
    wet.wetCharge = 0.02f;   // next to no paint of its own: it only moves what is there
    line(wet, QColor(20, 200, 60), QPointF(250, 200), QPointF(600, 200), 80);
    const Px dragged = read(425, 200);
    const bool oneStep = engine.history().undoCount() == steps + 1;
    engine.undo(true);
    wet.wetCharge = 1.0f;    // all its own
    line(wet, QColor(20, 200, 60), QPointF(250, 200), QPointF(600, 200), 80);
    const Px loaded = read(480, 200);
    engine.undo(true);
    // Less pull and a part charge: the blue it passes over comes into the mix and stays there.
    wet.wetMix = 0.5f;
    wet.wetCharge = 0.3f;
    line(wet, QColor(20, 200, 60), QPointF(250, 200), QPointF(600, 200), 80);
    const Px mixed = read(520, 200), untouched = read(520, 120);
    engine.undo(true);
    wet.wetMix = 1.0f;
    wet.wetCharge = 1.0f;
    // One touch of a loaded brush on bare canvas leaves its mark.
    StrokeSample tap;
    tap.pos = QPointF(400, 420);
    tap.pressure = 1.0f;
    engine.beginStroke(StrokeTool::Brush, wet, 60, 1.0f, QColor(20, 200, 60), tap);
    engine.endStroke();
    const Px dot = read(400, 420);
    engine.undo(true);
    check(QStringLiteral("a wet brush with next to no paint drags red into the blue (red %1 -> %2 there); fully charged it lays its own green; "
                         "part charged, green and blue mix (%3 and %4); a single touch leaves a mark; each stroke is one undo step")
              .arg(qRound(blueBefore.r * 255)).arg(qRound(dragged.r * 255)).arg(qRound(mixed.g * 255)).arg(qRound(mixed.b * 255)),
          blueBefore.r < 0.15f && dragged.r > blueBefore.r + 0.2f && dragged.g < 0.3f && oneStep && loaded.g > 0.7f && loaded.r < 0.2f && loaded.b < 0.35f
              && mixed.g > 0.25f && mixed.g < 0.72f && mixed.b > 0.3f && close(untouched.b, 220) && dot.a > 0.9f && dot.g > 0.7f
              && engine.history().undoCount() == steps);
    return all;
}

// ---- smoothing and line weight, checked on the dabs the stroke engine emits

bool runSmoothingTests()
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED", what);
    };
    BrushPreset base = *BrushLibrary::instance().find(QStringLiteral("technical-pen"));
    base.streamline = 0.0f;
    base.pressureSize = 0.0f;
    base.taper = 0.0f;
    base.spacing = 0.1f;
    const float size = 10.0f;

    auto run = [&](const BrushPreset& preset, int samples, const std::function<StrokeSample(int)>& at, bool* regenerated = nullptr) {
        StrokeEngine engine;
        engine.setSeed(7);
        engine.begin(preset, size, Qt::black, at(0));
        for (int i = 1; i < samples; ++i)
            engine.add(at(i));
        const bool again = engine.end();
        if (regenerated)
            *regenerated = again;
        return engine.dabs();
    };
    // A line drawn left to right by a shaky hand: every sample jumps 4 px to
    // the other side of it.
    auto shaky = [](int i) {
        StrokeSample smp;
        smp.pos = QPointF(100 + i * 5, 300 + ((i % 2) ? 4 : -4));
        smp.pressure = 0.6f;
        smp.time = i * 0.005;
        return smp;
    };
    auto roughness = [](const std::vector<Dab>& dabs) {
        double sum = 0.0;
        int n = 0;
        for (const Dab& d : dabs) {
            if (d.x > 300 && d.x < 750) {
                sum += double(d.y - 300.0f) * double(d.y - 300.0f);
                ++n;
            }
        }
        return n ? std::sqrt(sum / n) : 1e9;
    };
    const double raw = roughness(run(base, 160, shaky));

    BrushPreset p = base;
    p.streamline = 0.8f;
    const double stream = roughness(run(p, 160, shaky));
    p = base;
    p.stabilization = 0.8f;
    const double steady = roughness(run(p, 160, shaky));
    p = base;
    p.stringLength = 0.3f;
    const std::vector<Dab> strung = run(p, 160, shaky);
    const double string = roughness(strung);
    check(QStringLiteral("shake of %1 px drops to %2 (StreamLine), %3 (stabilisation), %4 (pull string)")
              .arg(raw, 0, 'f', 2).arg(stream, 0, 'f', 2).arg(steady, 0, 'f', 2).arg(string, 0, 'f', 2),
          raw > 1.0 && stream < raw * 0.35 && steady < raw * 0.35 && string < raw * 0.35);
    // The string is 48 px here; the line stops that far short of the pen and does not catch up.
    const float penEnd = 100 + 159 * 5;
    check(QStringLiteral("pull string leaves the line %1 px behind the pen").arg(penEnd - strung.back().x, 0, 'f', 0),
          !strung.empty() && penEnd - strung.back().x > 38.0f && penEnd - strung.back().x < 56.0f);

    // End taper: the last stretch thins to the tip size; everything before it is untouched.
    auto straight = [](int i) {
        StrokeSample smp;
        smp.pos = QPointF(100 + i * 5, 300);
        smp.pressure = 0.6f;
        smp.time = i * 0.005;
        return smp;
    };
    const std::vector<Dab> plain = run(base, 120, straight);
    p = base;
    p.taperEnd = 3.0f;
    p.taperTip = 0.1f;
    bool regenerated = false;
    const std::vector<Dab> tapered = run(p, 120, straight, &regenerated);
    bool sameBefore = plain.size() > 50 && tapered.size() > 50;
    for (size_t i = 0; sameBefore && i < 50; ++i)
        sameBefore = plain[i].x == tapered[i].x && plain[i].y == tapered[i].y && plain[i].size == tapered[i].size;
    check(QStringLiteral("end taper: width %1 px mid-stroke, %2 px at the tip").arg(tapered[tapered.size() / 2].size, 0, 'f', 1).arg(tapered.back().size, 0, 'f', 1),
          regenerated && sameBefore && tapered[tapered.size() / 2].size == size && tapered.back().size < size * 0.3f);

    // Weight smoothing: pressure jumps from light to full halfway; shortly after, the smoothed line is still thin.
    auto step = [](int i) {
        StrokeSample smp;
        smp.pos = QPointF(100 + i * 5, 300);
        smp.pressure = i < 60 ? 0.2f : 1.0f;
        smp.time = i * 0.005;
        return smp;
    };
    auto widthAt = [](const std::vector<Dab>& dabs, float x) {
        for (const Dab& d : dabs) {
            if (d.x >= x)
                return d.size;
        }
        return 0.0f;
    };
    p = base;
    p.pressureSize = 1.0f;
    const float quick = widthAt(run(p, 120, step), 450);
    p.pressureSmoothing = 0.9f;
    const float slow = widthAt(run(p, 120, step), 450);
    check(QStringLiteral("weight smoothing: 50 px after a sudden press the line is %1 px wide instead of %2").arg(slow, 0, 'f', 1).arg(quick, 0, 'f', 1),
          quick > size * 0.8f && slow < quick * 0.65f);

    // Speed weighting, both ways.
    auto fast = [](int i) {
        StrokeSample smp;
        smp.pos = QPointF(100 + i * 20, 300);
        smp.pressure = 0.6f;
        smp.time = i * 0.005;
        return smp;
    };
    p = base;
    p.speedSize = 0.6f;
    const float thinner = widthAt(run(p, 60, fast), 700);
    p.speedSize = -0.6f;
    const float thicker = widthAt(run(p, 60, fast), 700);
    check(QStringLiteral("speed weighting: a fast stroke is %1 px (thinning) or %2 px (thickening) against %3").arg(thinner, 0, 'f', 1).arg(thicker, 0, 'f', 1).arg(size, 0, 'f', 0),
          thinner < size * 0.75f && thicker > size * 1.25f);

    // The pressure curve passes through its points and never doubles back.
    const QVector<QPointF> lifted = {{0.0, 0.0}, {0.3, 0.6}, {0.7, 0.9}, {1.0, 1.0}};
    bool monotone = true;
    float before = -1.0f;
    for (int i = 0; i <= 100; ++i) {
        const float y = CurveEditor::evaluate(lifted, float(i) / 100.0f);
        monotone = monotone && y >= before;
        before = y;
    }
    check(QStringLiteral("pressure curve: a lifted curve turns 30% pressure into %1%").arg(qRound(CurveEditor::evaluate(lifted, 0.3f) * 100)),
          monotone && std::abs(CurveEditor::evaluate(lifted, 0.3f) - 0.6f) < 0.01f
              && std::abs(CurveEditor::evaluate(CurveEditor::identity(), 0.42f) - 0.42f) < 0.01f);
    return all;
}

} // namespace

// ---- Layers that take memory only where there is paint. Two things have to
// hold: the picture is the same to the last bit whether layers are stored
// that way or whole, through everything that can be done to a canvas; and
// the memory really does follow the paint.
static bool runSparseTests(CanvasEngine& engine, const QString& docPath)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED").arg(what);
    };
    const bool wasAllowed = gfx::sparseAllowed();
    auto stroke = [&](StrokeTool tool, const char* brush, float size, const QColor& c, QPointF from, QPointF to, quint32 seed, float amplitude = 60.0f) {
        const BrushPreset& preset = *lib.find(QString::fromLatin1(brush));
        constexpr int n = 70;
        for (int i = 0; i <= n; ++i) {
            const float t = float(i) / float(n);
            StrokeSample s;
            s.pos = from + (to - from) * t + QPointF(0, std::sin(t * 9.0f) * amplitude);
            s.pressure = 0.15f + 0.85f * std::pow(std::sin(t * 3.14159f), 0.7f);
            s.time = i * 0.008;
            if (i == 0)
                engine.beginStroke(tool, preset, size, 1.0f, c, s, seed);
            else
                engine.strokeTo(s);
        }
        engine.endStroke();
    };

    struct Mark {
        QString what;
        QByteArray picture, layers;
    };
    // Everything that can be done to a canvas, with nothing left to chance,
    // on one that is neither a whole number of blocks nor of bands.
    // `lean`: the buffers a stroke passes through are stored that way too, as
    // they are on a canvas much larger than this one.
    auto script = [&](bool sparse, bool lean = false) {
        std::vector<Mark> marks;
        gfx::setSparseAllowed(sparse);
        CanvasEngine::setLeanFrom(lean ? 0 : std::numeric_limits<qint64>::max());
        engine.newDocument(QSize(3000, 2300), QColor(250, 247, 240));
        auto mark = [&](const char* what, bool layersToo = false) {
            Mark m;
            m.what = QString::fromUtf8(what);
            const QImage img = engine.renderImage();
            m.picture = QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(img.constBits()), img.sizeInBytes()), QCryptographicHash::Md5);
            if (layersToo) {
                QCryptographicHash h(QCryptographicHash::Md5);
                for (const LayerInfo& l : engine.layers()) {
                    if (l.isGroup)
                        continue;
                    h.addData(engine.layerPixels(l.id));
                    if (l.hasMask)
                        h.addData(engine.layerMaskGrey(l.id));
                }
                m.layers = h.result();
            }
            marks.push_back(m);
        };
        auto select = [&](const QRectF& r, bool ellipse = false) {
            QPainterPath path;
            if (ellipse)
                path.addEllipse(r);
            else
                path.addRect(r);
            engine.setSelection(path, QStringLiteral("Select"));
        };
        const int base = engine.activeLayerId();
        mark("empty canvas", true);

        // Strokes of every kind, across the seams between bands and blocks.
        stroke(StrokeTool::Brush, "round-brush", 140, QColor(200, 60, 40), QPointF(300, 500), QPointF(2700, 1900), 11);
        stroke(StrokeTool::Brush, "pencil-6b", 40, QColor(30, 30, 40), QPointF(2800, 300), QPointF(200, 2100), 12);
        stroke(StrokeTool::Brush, "airbrush-soft", 500, QColor(60, 120, 220), QPointF(1500, 200), QPointF(1700, 2200), 13, 200.0f);
        mark("brush strokes", true);
        stroke(StrokeTool::Eraser, "round-brush", 180, Qt::black, QPointF(100, 1150), QPointF(2900, 1150), 14);
        mark("eraser");
        stroke(StrokeTool::Smudge, "round-brush", 160, Qt::black, QPointF(600, 700), QPointF(2300, 1700), 15);
        mark("smudge");
        stroke(StrokeTool::Brush, "wet-oil", 150, QColor(240, 200, 40), QPointF(400, 1800), QPointF(2600, 600), 16);
        mark("wet brush", true);

        // A second layer: fills, selections, alpha lock.
        const int flats = engine.addLayer();
        select(QRectF(1800, 1500, 900, 600));
        engine.fillLayer(flats, QColor(40, 170, 90));
        mark("fill inside a selection");
        engine.invertSelection();
        stroke(StrokeTool::Brush, "round-brush", 200, QColor(160, 40, 160), QPointF(1500, 1700), QPointF(2900, 1800), 17);
        engine.clearSelection();
        engine.setLayerAlphaLock(flats, true);
        engine.fillLayer(flats, QColor(250, 120, 20));
        engine.setLayerAlphaLock(flats, false);
        mark("fill with alpha lock", true);
        select(QRectF(1900, 1400, 500, 500), true);
        engine.clearLayer(flats);
        engine.clearSelection();
        mark("clear inside a selection");
        engine.floodFill(QPoint(2500, 2000), QColor(20, 200, 200), 0.2f);
        mark("flood fill", true);

        // Gradients.
        const int wash = engine.addLayer();
        engine.beginGradient();
        engine.setGradient(QPointF(200, 200), QPointF(2800, 2000), QColor(255, 80, 80), QColor(80, 80, 255, 0), GradientKind::Linear);
        engine.endGradient(true);
        engine.setLayerOpacity(wash, 0.5f);
        engine.setLayerBlend(wash, BlendMode::Multiply);
        mark("gradient over the whole canvas");
        select(QRectF(300, 300, 800, 700), true);
        engine.beginGradient();
        engine.setGradient(QPointF(700, 650), QPointF(1100, 650), QColor(255, 255, 255), QColor(0, 0, 0), GradientKind::Radial);
        engine.endGradient(true);
        engine.clearSelection();
        mark("radial gradient inside a selection", true);

        // Transforms: the whole layer, a selected part, a warp.
        engine.setActiveLayer(flats);
        engine.beginTransform();
        QTransform turn;
        turn.translate(1400, 1100);
        turn.rotate(17);
        turn.scale(0.8, 1.1);
        turn.translate(-1700, -1300);
        engine.setTransform(turn);
        mark("transform in progress");
        engine.commitEdit();
        mark("layer turned, sized and moved", true);
        select(QRectF(900, 900, 700, 500));
        engine.beginTransform();
        engine.setTransform(QTransform::fromTranslate(-650, -500));
        engine.commitEdit();
        engine.clearSelection();
        mark("selected part moved");
        engine.setActiveLayer(base);
        engine.beginTransform();
        {
            const QRectF b(200, 200, 2600, 1900);
            std::vector<QPointF> pts;
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c)
                    pts.emplace_back(b.x() + b.width() * c / 3.0 + (r == 1 && c == 2 ? 180 : 0), b.y() + b.height() * r / 3.0 + (r == 2 && c == 1 ? -140 : 0));
            }
            engine.setWarp(4, 4, pts, b);
        }
        engine.commitEdit();
        mark("warp", true);

        // Adjustments and filters, on a whole layer and inside a selection.
        AdjustParams hsb;
        hsb.hue = 0.2f;
        hsb.saturation = 0.3f;
        hsb.brightness = -0.1f;
        engine.beginAdjustment(Adjustment::HueSaturation);
        engine.setAdjustment(hsb);
        engine.commitEdit();
        mark("hue and saturation");
        AdjustParams blur;
        blur.radius = 14.0f;
        engine.setActiveLayer(flats);
        engine.beginAdjustment(Adjustment::GaussianBlur);
        engine.setAdjustment(blur);
        engine.commitEdit();
        mark("blur", true);
        select(QRectF(1200, 300, 1300, 900));
        AdjustParams mosaic;
        mosaic.v[0] = 24.0f;
        engine.setActiveLayer(base);
        engine.beginAdjustment(Adjustment::Mosaic);
        engine.setAdjustment(mosaic);
        engine.commitEdit();
        engine.featherSelection(30.0f);
        AdjustParams noise;
        noise.v[0] = 0.3f;
        noise.v[1] = 1.0f;
        engine.beginAdjustment(Adjustment::Noise);
        engine.setAdjustment(noise);
        engine.commitEdit();
        engine.clearSelection();
        mark("mosaic and noise inside a selection", true);

        // Liquify.
        engine.beginLiquify();
        engine.liquify(LiquifyMode::Push, QPointF(1000, 1000), QPointF(1300, 1250), 260.0f, 0.9f);
        engine.liquify(LiquifyMode::TwirlRight, QPointF(2100, 1500), QPointF(2100, 1500), 300.0f, 0.8f);
        engine.endLiquify(true);
        mark("liquify");

        // Masks, groups and clipping.
        select(QRectF(400, 400, 1800, 1300), true);
        engine.addLayerMask(base);
        engine.clearSelection();
        stroke(StrokeTool::Brush, "round-brush", 220, Qt::black, QPointF(500, 1100), QPointF(2400, 1000), 18);
        engine.setMaskEditing(false);
        mark("mask from a selection, painted on", true);
        engine.invertLayerMask(base);
        mark("mask inverted");
        engine.invertLayerMask(base);
        const int group = engine.addGroup();
        engine.moveLayer(flats, group, 0);
        engine.moveLayer(wash, group, 0);
        engine.setLayerOpacity(group, 0.7f);
        engine.setLayerBlend(group, BlendMode::Screen);
        const int clipped = engine.addLayer();
        engine.setActiveLayer(clipped);
        stroke(StrokeTool::Brush, "round-brush", 300, QColor(255, 255, 255), QPointF(200, 1300), QPointF(2800, 1300), 19, 300.0f);
        engine.setLayerClipping(clipped, true);
        mark("a group with opacity and a blend mode, a clipped layer", true);
        engine.flattenGroup(group);
        mark("group flattened", true);
        engine.applyLayerMask(base);
        mark("mask applied");

        // Text, duplicates, flips, merging.
        TextInfo words;
        words.text = QStringLiteral("Impasto\nlarge canvas");
        words.pixelSize = 220;
        words.color = QColor(20, 20, 30);
        words.anchor = QPointF(1500, 1700);
        words.alignment = Qt::AlignHCenter;
        const int text = engine.addTextLayer(words, QStringLiteral("Words"));
        mark("text");
        engine.duplicateLayer(text);
        const int copy = engine.activeLayerId();
        engine.flipLayer(copy, true);
        engine.setLayerOpacity(copy, 0.4f);
        mark("duplicate, flipped", true);
        engine.flipCanvas(false);
        mark("canvas flipped");
        engine.mergeDown(copy);
        mark("merged down", true);
        engine.removeLayer(clipped);
        mark("layer removed");

        // Another canvas size: cropped and shifted, then scaled.
        engine.resizeCanvas(QSize(2600, 2100), QTransform::fromTranslate(-150, -80), QStringLiteral("Crop"));
        mark("canvas cropped", true);
        engine.resizeCanvas(QSize(3300, 2665), QTransform::fromScale(3300.0 / 2600.0, 2665.0 / 2100.0), QStringLiteral("Resize"));
        mark("canvas scaled up", true);

        // Back through all of it and forward again.
        int steps = 0;
        while (engine.history().canUndo()) {
            engine.undo(true);
            if (++steps % 6 == 0)
                mark("undone part of the way");
        }
        mark("everything undone", true);
        while (engine.history().canRedo())
            engine.redo(true);
        mark("everything redone", true);

        // And through a file.
        QString error;
        const bool written = docio::save(engine, docPath, &error) && docio::load(engine, docPath, &error);
        mark(written ? "saved and opened again" : "FAILED TO SAVE", true);
        return marks;
    };

    QElapsedTimer timer;
    timer.start();
    const std::vector<Mark> whole = script(false);
    const qint64 wholeMs = timer.restart();
    const std::vector<Mark> again = script(false);
    timer.restart();
    const std::vector<Mark> sparse = script(true);
    const qint64 sparseMs = timer.restart();
    const std::vector<Mark> lean = script(true, true);
    const qint64 leanMs = timer.elapsed();
    CanvasEngine::setLeanFrom(-1);
    auto firstDifference = [](const std::vector<Mark>& a, const std::vector<Mark>& b) {
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            if (a[i].picture != b[i].picture)
                return a[i].what + QStringLiteral(" (the picture)");
            if (a[i].layers != b[i].layers)
                return a[i].what + QStringLiteral(" (a layer)");
        }
        return a.size() == b.size() ? QString() : QStringLiteral("the number of steps");
    };
    const QString unsteady = firstDifference(whole, again);
    check(QStringLiteral("the test canvas comes out the same every time it is made (%1 steps)%2").arg(whole.size())
              .arg(unsteady.isEmpty() ? QString() : QStringLiteral(": differs at ") + unsteady),
          unsteady.isEmpty() && whole.size() > 30);
    if (!CanvasEngine::sparseLayers()) {
        qInfo().noquote() << "  (this graphics card stores every layer whole; nothing more to compare)";
        gfx::setSparseAllowed(wasAllowed);
        return all;
    }
    const QString differs = firstDifference(whole, sparse);
    check(QStringLiteral("layers stored only where there is paint: every step gives the same picture and the same layers, to the bit (%1 ms against %2 ms)%3")
              .arg(sparseMs).arg(wholeMs).arg(differs.isEmpty() ? QString() : QStringLiteral(": differs first at ") + differs),
          differs.isEmpty());
    const QString leanDiffers = firstDifference(whole, lean);
    check(QStringLiteral("the stroke buffers stored that way as well, as on a very large canvas: the same again (%1 ms)%2")
              .arg(leanMs).arg(leanDiffers.isEmpty() ? QString() : QStringLiteral(": differs first at ") + leanDiffers),
          leanDiffers.isEmpty());

    // Memory follows the paint. A canvas of 67 megapixels, which whole would
    // be 512 MB a layer.
    gfx::setSparseAllowed(true);
    const QSize big(8192, 8192);
    const qint64 layerBytes = qint64(big.width()) * big.height() * 8;
    const int blocks = (big.width() / gfx::kPage) * (big.height() / gfx::kPage);
    auto megabytes = [](qint64 bytes) { return double(bytes) / double(1 << 20); };
    engine.newDocument(big, Qt::white);
    engine.prepareFrame(true);
    const qint64 empty = engine.memoryInUse();
    const int first = engine.activeLayerId();
    std::vector<int> ids{first};
    for (int i = 0; i < 9; ++i)
        ids.push_back(engine.addLayer());
    engine.prepareFrame(true);
    const qint64 tenLayers = engine.memoryInUse();
    check(QStringLiteral("an empty canvas of %1 megapixels takes %2 MB, and nine more empty layers add %3 MB (whole, each layer is %4 MB)")
              .arg(qint64(big.width()) * big.height() / 1'000'000).arg(megabytes(empty), 0, 'f', 0).arg(megabytes(tenLayers - empty), 0, 'f', 1).arg(megabytes(layerBytes), 0, 'f', 0),
          engine.layerHeldBlocks(first) == 0 && tenLayers - empty < (qint64(4) << 20) && empty < layerBytes * 2);

    engine.setActiveLayer(first);
    stroke(StrokeTool::Brush, "round-brush", 60, QColor(30, 30, 30), QPointF(3000, 3000), QPointF(3700, 3000), 21, 20.0f);
    engine.prepareFrame(true);
    const int strokeBlocks = engine.layerHeldBlocks(first);
    const qint64 afterStroke = engine.memoryInUse();
    const QColor inked = engine.pickColor(QPoint(3350, 3000));
    engine.undo(true);
    const int afterUndo = engine.layerHeldBlocks(first);
    engine.redo(true);
    const int afterRedo = engine.layerHeldBlocks(first);
    check(QStringLiteral("a stroke 700 px long gives the layer memory for %1 blocks of %2 (%3 MB in all, undo included); undo gives them back, redo takes them again")
              .arg(strokeBlocks).arg(blocks).arg(megabytes(afterStroke - tenLayers), 0, 'f', 1),
          strokeBlocks >= 3 && strokeBlocks <= 12 && afterUndo == 0 && afterRedo == strokeBlocks && inked.red() < 80 && afterStroke - tenLayers < (qint64(40) << 20));

    stroke(StrokeTool::Eraser, "round-brush", 400, Qt::black, QPointF(2800, 3000), QPointF(3900, 3000), 22, 0.0f);
    const int afterErase = engine.layerHeldBlocks(first);
    check(QStringLiteral("rubbing the stroke out leaves the layer holding %1 blocks").arg(afterErase), afterErase == 0 && engine.pickColor(QPoint(3350, 3000)).red() > 240);

    engine.fillLayer(first, QColor(200, 40, 40));
    const int filled = engine.layerHeldBlocks(first);
    engine.clearLayer(first);
    const int cleared = engine.layerHeldBlocks(first);
    check(QStringLiteral("a layer filled edge to edge holds all %1 blocks, cleared it holds %2").arg(filled).arg(cleared), filled == blocks && cleared == 0);

    // Content moved across the canvas takes its memory with it.
    engine.undo(true);
    engine.undo(true);
    engine.undo(true);   // back to the stroke
    const int before = engine.layerHeldBlocks(first);
    const qint64 beforeMoving = engine.memoryInUse();
    engine.beginTransform();
    engine.setTransform(QTransform::fromTranslate(3111, 2777));
    const qint64 whileMoving = engine.memoryInUse();
    engine.commitEdit();
    const int moved = engine.layerHeldBlocks(first);
    const QRect where = engine.layerHeldBounds(first);
    check(QStringLiteral("moved to another part of the canvas the stroke holds %1 blocks (before: %2), around %3,%4; while it was being moved the canvas held %5 MB %6")
              .arg(moved).arg(before).arg(where.center().x()).arg(where.center().y()).arg(std::abs(megabytes(whileMoving - beforeMoving)), 0, 'f', 0)
              .arg(whileMoving > beforeMoving ? QStringLiteral("more") : QStringLiteral("less")),
          before == strokeBlocks && moved >= 3 && moved <= 14 && where.center().x() > 6000 && where.center().y() > 5400
              && engine.pickColor(QPoint(3350 + 3111, 3000 + 2777)).red() < 80 && whileMoving - beforeMoving < (qint64(40) << 20));

    // A filter over a layer with a little paint on it leaves the rest of the layer without memory.
    AdjustParams soften;
    soften.radius = 20.0f;
    engine.beginAdjustment(Adjustment::GaussianBlur);
    engine.setAdjustment(soften);
    engine.commitEdit();
    const int blurred = engine.layerHeldBlocks(first);
    engine.flipLayer(first, true);
    const int flipped = engine.layerHeldBlocks(first);
    check(QStringLiteral("blurred it holds %1 blocks, flipped %2").arg(blurred).arg(flipped), blurred >= moved && blurred <= moved + 12 && flipped >= 3 && flipped <= blurred + 2);

    // Through a file and back: the same paint, the same memory.
    QString error;
    const QImage pictureBefore = engine.renderImage();
    const bool roundTrip = docio::save(engine, docPath, &error) && docio::load(engine, docPath, &error);
    const int loaded = roundTrip ? engine.layerHeldBlocks(engine.layers().value(0).id) : -1;
    engine.prepareFrame(true);
    const qint64 afterLoad = engine.memoryInUse();
    check(QStringLiteral("saved and opened again: the same picture, %1 blocks, %2 MB for the ten layers").arg(loaded).arg(megabytes(afterLoad - empty), 0, 'f', 1),
          roundTrip && engine.renderImage() == pictureBefore && loaded == flipped && afterLoad - empty < (qint64(40) << 20));

    // The same on a canvas of ordinary size, where the buffers a stroke
    // passes through are kept whole: what is done to a layer with a little
    // paint on it must not leave the layer holding memory for all of itself.
    {
        engine.newDocument(QSize(4096, 4096), Qt::white);
        const int all4k = (4096 / gfx::kPage) * (4096 / gfx::kPage);
        const int a = engine.activeLayerId();
        stroke(StrokeTool::Brush, "round-brush", 60, QColor(30, 30, 30), QPointF(1000, 1000), QPointF(1600, 1000), 51, 20.0f);
        QStringList counts;
        int most = 0;
        auto note = [&](const char* what) {
            const int n = engine.layerHeldBlocks(a);
            most = std::max(most, n);
            counts << QStringLiteral("%1 %2").arg(QString::fromUtf8(what)).arg(n);
        };
        note("a stroke");
        engine.flipLayer(a, true);
        note("flipped");
        AdjustParams soft;
        soft.radius = 12.0f;
        engine.beginAdjustment(Adjustment::GaussianBlur);
        engine.setAdjustment(soft);
        engine.commitEdit();
        note("blurred");
        engine.beginTransform();
        engine.setTransform(QTransform::fromTranslate(-1500, 1800));
        engine.commitEdit();
        note("moved");
        const int b = engine.addLayer();
        stroke(StrokeTool::Brush, "round-brush", 60, QColor(200, 30, 30), QPointF(3000, 3000), QPointF(3400, 3000), 52, 20.0f);
        engine.mergeDown(b);
        note("merged with another");
        engine.addLayerMask(a);
        engine.setMaskEditing(false);
        engine.applyLayerMask(a);
        note("mask applied");
        QPainterPath patch;
        patch.addRect(200, 200, 300, 300);
        engine.setSelection(patch, QStringLiteral("Select"));
        engine.beginGradient();
        engine.setGradient(QPointF(200, 200), QPointF(500, 500), QColor(255, 0, 0), QColor(0, 0, 255), GradientKind::Linear);
        engine.endGradient(true);
        engine.clearSelection();
        note("gradient in a small selection");
        engine.beginLiquify();
        engine.liquify(LiquifyMode::Push, QPointF(300, 300), QPointF(420, 380), 120.0f, 0.9f);
        engine.endLiquify(true);
        note("liquified");
        const int filledBefore = engine.layerHeldBlocks(a);
        engine.resizeCanvas(QSize(5000, 4500), QTransform::fromTranslate(400, 200), QStringLiteral("Canvas Size"));
        const int afterResize = engine.layerHeldBlocks(a);
        check(QStringLiteral("on an ordinary canvas (%1 blocks a layer) a little paint stays a little memory through it all: %2; on a larger canvas %3")
                  .arg(all4k).arg(counts.join(QStringLiteral(", "))).arg(afterResize),
              most <= 30 && most >= 3 && afterResize <= filledBefore + 12 && afterResize >= 3);
    }

    // When memory runs short. The card is not really filled: the engine is
    // told it may use only a little more than it already does.
    {
        engine.newDocument(big, Qt::white);
        for (int i = 0; i < 6; ++i)
            stroke(StrokeTool::Brush, "round-brush", 200, QColor(40, 40, 160), QPointF(500, 800 + i * 900), QPointF(7600, 900 + i * 900), quint32(30 + i), 60.0f);
        engine.prepareFrame(true);
        QStringList said;
        const QMetaObject::Connection listening = QObject::connect(&engine, &CanvasEngine::message, [&](const QString& text) { said << text; });
        // What is only kept for speed goes first, and quietly: the working copies of the strokes just drawn.
        const int stepsBefore = engine.history().undoCount();
        CanvasEngine::setBudgetForTests(engine.memoryInUse() + (qint64(40) << 20));
        stroke(StrokeTool::Brush, "round-brush", 60, QColor(200, 30, 30), QPointF(1000, 4000), QPointF(1500, 4000), 40, 0.0f);
        const bool quietly = said.isEmpty() && engine.history().undoCount() == stepsBefore + 1 && engine.pickColor(QPoint(1250, 4000)).red() > 150;
        // After that, undo steps, oldest first: here nearly all of them have to go.
        const qint64 historyBefore = engine.history().bytes();
        CanvasEngine::setBudgetForTests(engine.memoryInUse() - engine.memoryForSpeed() - historyBefore / 2 + (qint64(40) << 20));
        stroke(StrokeTool::Brush, "round-brush", 60, QColor(200, 30, 30), QPointF(1000, 5000), QPointF(1500, 5000), 41, 0.0f);
        const bool drawn = engine.pickColor(QPoint(1250, 5000)).red() > 150;
        const int stepsAfter = engine.history().undoCount();
        const bool toldForgot = !said.isEmpty() && said.first().contains(QLatin1String("undo"));
        check(QStringLiteral("short of memory, a stroke still goes down: first the working copies are given up, then %1 of %2 undo steps (%3 MB), and the artist is told")
                  .arg(stepsBefore + 2 - stepsAfter).arg(stepsBefore + 1).arg(megabytes(historyBefore - engine.history().bytes()), 0, 'f', 0),
              quietly && drawn && stepsAfter < stepsBefore + 2 && stepsAfter >= 1 && toldForgot);
        // No room at all for a layer's worth of paint: refused, with the reason, and nothing changes.
        said.clear();
        CanvasEngine::setBudgetForTests(engine.memoryInUse() + (qint64(100) << 20));
        const int other = engine.addLayer();
        const QImage pictureBefore = engine.renderImage();
        const int undoBefore = engine.history().undoCount();
        engine.fillLayer(other, QColor(10, 200, 10));
        const bool refused = engine.layerHeldBlocks(other) == 0 && engine.renderImage() == pictureBefore && engine.history().undoCount() <= undoBefore
                             && !said.isEmpty() && said.last().contains(QLatin1String("full"));
        // An empty layer costs nothing, so adding one was no trouble; and with room again the fill goes through.
        CanvasEngine::setBudgetForTests(-1);
        engine.fillLayer(other, QColor(10, 200, 10));
        check(QStringLiteral("with no room for it, filling a layer edge to edge is refused and says why; with room again it goes through"),
              refused && engine.layerHeldBlocks(other) == blocks && engine.pickColor(QPoint(4000, 4000)).green() > 150);
        QObject::disconnect(listening);
    }

    CanvasEngine::setBudgetForTests(-1);
    engine.newDocument(QSize(64, 64), Qt::white);
    gfx::setSparseAllowed(wasAllowed);
    return all;
}

static void isolateSettings();

// ---- Layered files for other programs (PSD, OpenRaster).

// A document with a bit of everything those files carry: blend modes,
// opacity, a hidden layer, a group with its own opacity and mode, a mask,
// a clipped layer, alpha lock, text and paper. `allModes` adds one layer for
// every blend mode, named after it, for checking that each is understood.
static void buildInterchangeSample(CanvasEngine& engine, bool allModes)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset& round = *lib.find(QStringLiteral("round-brush"));
    auto stroke = [&](const QColor& c, float size, QPointF from, QPointF to, quint32 seed) {
        constexpr int n = 50;
        for (int i = 0; i <= n; ++i) {
            const float t = float(i) / float(n);
            StrokeSample s;
            s.pos = from + (to - from) * t + QPointF(0, std::sin(t * 6.0f) * 30.0f);
            s.pressure = 0.4f + 0.6f * std::sin(t * 3.14159f);
            s.time = i * 0.008;
            if (i == 0)
                engine.beginStroke(StrokeTool::Brush, round, size, 1.0f, c, s, seed);
            else
                engine.strokeTo(s);
        }
        engine.endStroke();
    };
    auto box = [&](const QRectF& r) {
        QPainterPath p;
        p.addRect(r);
        engine.setSelection(p, QStringLiteral("Select"));
    };
    engine.newDocument(QSize(900, 700), QColor(250, 246, 236));
    engine.setResolution(300);

    const int ink = engine.activeLayerId();
    engine.setLayerName(ink, QStringLiteral("Ink"));
    stroke(QColor(30, 30, 50), 40, QPointF(80, 120), QPointF(820, 160), 1);
    stroke(QColor(200, 60, 40), 90, QPointF(100, 360), QPointF(800, 420), 2);

    const int wash = engine.addLayer();
    engine.setLayerName(wash, QStringLiteral("Wash · multiply"));
    box(QRectF(60, 200, 500, 300));
    engine.fillLayer(wash, QColor(90, 160, 230));
    engine.clearSelection();
    engine.setLayerBlend(wash, BlendMode::Multiply);
    engine.setLayerOpacity(wash, 0.6f);

    const int light = engine.addLayer();
    engine.setLayerName(light, QStringLiteral("Light · screen"));
    stroke(QColor(255, 220, 120), 120, QPointF(500, 100), QPointF(860, 600), 3);
    engine.setLayerBlend(light, BlendMode::Screen);

    const int hidden = engine.addLayer();
    engine.setLayerName(hidden, QStringLiteral("Hidden"));
    stroke(QColor(0, 200, 0), 60, QPointF(80, 600), QPointF(800, 640), 4);
    engine.setLayerVisible(hidden, false);

    // A group: a masked layer and a layer clipped to it.
    const int group = engine.addGroup();
    engine.setLayerName(group, QStringLiteral("Group 50%"));
    engine.setLayerOpacity(group, 0.5f);
    const int shape = engine.addLayer();
    engine.setLayerName(shape, QStringLiteral("Masked"));
    box(QRectF(300, 250, 420, 300));
    engine.fillLayer(shape, QColor(120, 40, 160));
    engine.clearSelection();
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(340, 280, 340, 240));
    engine.setSelection(ellipse, QStringLiteral("Select"));
    engine.addLayerMask(shape);
    engine.clearSelection();
    engine.setMaskEditing(false);
    const int clipped = engine.addLayer();
    engine.setLayerName(clipped, QStringLiteral("Clipped stripes"));
    for (int i = 0; i < 5; ++i)
        stroke(QColor(255, 255, 255), 30, QPointF(280, 270 + i * 60), QPointF(740, 250 + i * 60), quint32(10 + i));
    engine.setLayerClipping(clipped, true);
    engine.setLayerAlphaLock(clipped, true);
    engine.moveLayer(shape, group, 0);
    engine.moveLayer(clipped, group, 0);

    TextInfo words;
    words.text = QStringLiteral("Impasto");
    words.pixelSize = 64;
    words.color = QColor(40, 40, 40);
    words.anchor = QPointF(450, 560);
    words.alignment = Qt::AlignHCenter;
    engine.addTextLayer(words, QStringLiteral("Title"));

    if (allModes) {
        for (const BlendModeInfo& m : blendModes()) {
            if (m.mode == BlendMode::Normal)
                continue;
            const int id = engine.addLayer();
            engine.setLayerName(id, QString::fromLatin1(m.name));
            engine.setLayerBlend(id, m.mode);
            box(QRectF(20 + (int(m.mode) % 13) * 66, 20 + (int(m.mode) / 13) * 60, 50, 40));
            engine.fillLayer(id, QColor(70, 130, 200));
            engine.clearSelection();
        }
    }
}

// A person with no skeleton, for rigging: the figure with its arms out in an
// A-pose, flattened into a plain OBJ. Its true joints are returned too, in
// the order Model3D::rigJointNames() gives, to measure a fit by.
static std::vector<QVector3D> writeFigureObj(const QString& path, bool facingAway)
{
    Model3D figure = Model3D::makeFigure();
    auto node = [&](const char* name) {
        for (size_t i = 0; i < figure.nodes.size(); ++i)
            if (figure.nodes[i].name == QLatin1String(name))
                return int(i);
        return -1;
    };
    figure.nodes[size_t(node("upperarm.L"))].pose = QQuaternion::fromAxisAndAngle(0, 0, 1, 36);
    figure.nodes[size_t(node("upperarm.R"))].pose = QQuaternion::fromAxisAndAngle(0, 0, 1, -36);
    const QQuaternion turn = facingAway ? QQuaternion::fromAxisAndAngle(0, 1, 0, 180) : QQuaternion();
    QByteArray obj("# the figure, unrigged\n");
    quint32 base = 1;
    for (const Model3D::Part& part : figure.posed()) {
        for (const QVector3D& v : part.positions) {
            const QVector3D p = turn.rotatedVector(v);
            obj += QByteArray("v ") + QByteArray::number(p.x(), 'f', 5) + ' ' + QByteArray::number(p.y(), 'f', 5) + ' ' + QByteArray::number(p.z(), 'f', 5) + '\n';
        }
        for (size_t i = 0; i + 2 < part.indices->size(); i += 3)
            obj += "f " + QByteArray::number(base + (*part.indices)[i]) + ' ' + QByteArray::number(base + (*part.indices)[i + 1]) + ' '
                   + QByteArray::number(base + (*part.indices)[i + 2]) + '\n';
        base += quint32(part.positions.size());
    }
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(obj);
    const auto world = figure.worldTransforms();
    auto at = [&](const char* name, const QVector3D& tip = QVector3D()) { return turn.rotatedVector(world[size_t(node(name))].map(tip)); };
    // The figure's left is +X facing +Z; turned round it is -X, and the joints go with it.
    return {at("hips"), at("spine"), at("chest"), at("neck"), at("head"), at("head", {0, 0.24f, 0}),
            at("upperarm.L"), at("forearm.L"), at("hand.L"), at("hand.L", {0, -0.17f, 0}),
            at("upperarm.R"), at("forearm.R"), at("hand.R"), at("hand.R", {0, -0.17f, 0}),
            at("thigh.L"), at("shin.L"), at("foot.L"), at("foot.L", {0, -0.05f, 0.17f}),
            at("thigh.R"), at("shin.R"), at("foot.R"), at("foot.R", {0, -0.05f, 0.17f})};
}

// A model made into a figure to build in: written as a compact GLB, with its
// name and joints beside it. The joints come from a settings file where the
// Rig button kept them (so that any put right by hand stay so), or are fitted.
int runMakeFigure(const QString& modelPath, const QString& outGlb, const QString& name, const QString& settingsFile)
{
    QString error;
    Model3D model;
    if (!Model3D::load(modelPath, &model, &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    std::vector<QVector3D> joints;
    if (!settingsFile.isEmpty()) {
        const QSettings kept(settingsFile, QSettings::IniFormat);
        const QVariantList saved = kept.value(QStringLiteral("model/rigs")).toMap().value(QFileInfo(modelPath).absoluteFilePath()).toList();
        for (int i = 0; i + 2 < saved.size(); i += 3)
            joints.emplace_back(saved[i].toFloat(), saved[i + 1].toFloat(), saved[i + 2].toFloat());
        qInfo().noquote() << (joints.size() == 22 ? "joints from the settings file" : "no joints for it in the settings file: fitting");
    }
    if (joints.size() != 22 && !model.fitRig(&joints, &error)) {
        qCritical().noquote() << "no fit:" << error;
        return 1;
    }
    if (!model.saveGlb(outGlb, &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    QJsonArray rig;
    for (const QVector3D& j : joints)
        rig << j.x() << j.y() << j.z();
    const QString json = QFileInfo(outGlb).path() + QLatin1Char('/') + QFileInfo(outGlb).completeBaseName() + QStringLiteral(".json");
    QFile f(json);
    f.open(QIODevice::WriteOnly);
    f.write(QJsonDocument(QJsonObject{{"name", name}, {"rig", rig}}).toJson());
    f.close();
    // Read back as the app would.
    Model3D back;
    const bool loaded = Model3D::load(outGlb, &back, &error);
    size_t points = 0, triangles = 0;
    for (const ModelMesh& m : back.meshes) {
        points += m.positions.size();
        triangles += m.indices.size() / 3;
    }
    const bool bound = loaded && back.bindRig(joints);
    qInfo().noquote() << QStringLiteral("%1: %2 KB, %3 points, %4 triangles; binds: %5").arg(outGlb).arg(QFileInfo(outGlb).size() / 1024).arg(points).arg(triangles).arg(bound);
    return bound ? 0 : 1;
}

// A model rigged as the Rig button would, drawn as fitted (with its
// skeleton) and in a few of the figure's poses: for looking at by eye.
int runRigCheck(const QString& path, const QString& outPrefix)
{
    QString error;
    auto model = std::make_unique<Model3D>();
    if (!Model3D::load(path, model.get(), &error)) {
        qCritical().noquote() << error;
        return 1;
    }
    std::vector<QVector3D> joints;
    if (!model->fitRig(&joints, &error)) {
        qCritical().noquote() << "no fit:" << error;
        return 1;
    }
    const QStringList names = Model3D::rigJointNames();
    for (size_t i = 0; i < joints.size(); ++i)
        qInfo().noquote() << QStringLiteral("%1: %2, %3, %4").arg(names[int(i)], -18).arg(joints[i].x(), 0, 'f', 3).arg(joints[i].y(), 0, 'f', 3).arg(joints[i].z(), 0, 'f', 3);
    const bool away = model->facesAway(joints);
    qInfo().noquote() << "faces" << (away ? "-Z" : "+Z");
    ModelView view;
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(600, 800);
    view.show();
    QApplication::processEvents();
    view.setModel(std::move(model));
    view.beginRig(joints, away);
    view.setFloor(false);
    auto shot = [&](const QString& name, bool skeleton) {
        QImage img = view.renderImage(QSize(600, 800), false);
        if (skeleton) {
            QPainter p(&img);
            p.setRenderHint(QPainter::Antialiasing);
            const QVector<QPointF> at = view.rigPositions();
            p.setPen(QPen(Qt::white, 2));
            for (const auto& [a, b] : Model3D::rigBones())
                p.drawLine(at[a], at[b]);
            p.setBrush(QColor(255, 159, 10));
            for (const QPointF& pt : at)
                p.drawEllipse(pt, 5, 5);
        }
        img.save(outPrefix + QStringLiteral("-") + name + QStringLiteral(".png"));
    };
    shot(QStringLiteral("fit"), true);
    view.endRig();
    QElapsedTimer t;
    t.start();
    const bool bound = view.model()->bindRig(joints);
    qInfo().noquote() << "bound" << bound << "in" << t.elapsed() << "ms";
    if (!bound)
        return 1;
    view.modelEdited();
    shot(QStringLiteral("bound"), false);
    for (const QString& pose : {QStringLiteral("Stand"), QStringLiteral("Walk"), QStringLiteral("Sit"), QStringLiteral("Reach")}) {
        view.model()->applyFigurePose(pose);
        view.modelEdited();
        view.fit();
        shot(pose.toLower(), false);
    }
    QVariantMap side = view.viewState();
    side[QStringLiteral("yaw")] = side.value(QStringLiteral("yaw")).toDouble() + 90.0;
    view.setViewState(side);
    view.model()->applyFigurePose(QStringLiteral("Walk"));
    view.modelEdited();
    shot(QStringLiteral("walk-side"), false);
    return 0;
}

// 3D reference: the files a model comes in, the figure, and posing it.
static bool runModelTests(const QString& outPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED").arg(what);
    };
    auto close = [](const QVector3D& a, const QVector3D& b, float tol = 1e-3f) { return (a - b).length() <= tol; };
    QString error;

    // OBJ: a cube of quads, no normals; STL: a binary triangle standing on Z.
    const QString obj = outPath + QStringLiteral(".cube.obj");
    {
        QFile f(obj);
        f.open(QIODevice::WriteOnly);
        f.write("# a cube\nv -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
                "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\nf 2 3 7 6\n");
    }
    Model3D cube;
    const bool objOk = Model3D::load(obj, &cube, &error) && cube.meshes.size() == 1 && cube.meshes[0].indices.size() == 36
                       && cube.meshes[0].normals.size() == cube.meshes[0].positions.size();
    QVector3D lo, hi;
    const bool cubeBounds = objOk && cube.bounds(&lo, &hi) && close(lo, QVector3D(-1, -1, -1)) && close(hi, QVector3D(1, 1, 1));
    const QString stl = outPath + QStringLiteral(".tri.stl");
    {
        QByteArray b(80, ' ');
        auto u32 = [&](quint32 v) { b.append(reinterpret_cast<const char*>(&v), 4); };
        auto f32 = [&](float v) { b.append(reinterpret_cast<const char*>(&v), 4); };
        u32(1);
        for (float v : {0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f})
            f32(v);
        b.append(2, '\0');
        QFile f(stl);
        f.open(QIODevice::WriteOnly);
        f.write(b);
    }
    Model3D tri;
    const bool stlOk = Model3D::load(stl, &tri, &error) && tri.meshes.size() == 1 && tri.meshes[0].positions.size() == 3
                       && close(tri.meshes[0].positions[2], QVector3D(0, 1, 0));
    check(QStringLiteral("3D: an OBJ cube of quads opens as 12 triangles with normals made for it; a binary STL opens and is stood upright"),
          objOk && cubeBounds && stlOk);

    // glTF: a strip of two bones, its buffer inside the file, bent at its middle joint.
    QByteArray bin;
    auto f32 = [&](float v) { bin.append(reinterpret_cast<const char*>(&v), 4); };
    for (int row = 0; row < 3; ++row)
        for (float x : {-0.1f, 0.1f}) {
            f32(x);
            f32(float(row));
            f32(0.0f);
        }
    const int jointsAt = int(bin.size());
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 2; ++k)
            bin.append(QByteArray::fromRawData(row == 2 ? "\x01\x00\x00\x00" : row == 1 ? "\x00\x01\x00\x00" : "\x00\x00\x00\x00", 4));
    const int weightsAt = int(bin.size());
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 2; ++k) {
            f32(row == 1 ? 0.5f : 1.0f);
            f32(row == 1 ? 0.5f : 0.0f);
            f32(0.0f);
            f32(0.0f);
        }
    const int indicesAt = int(bin.size());
    for (quint16 i : {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4})
        bin.append(reinterpret_cast<const char*>(&i), 2);
    const int ibmAt = int(bin.size());
    for (float v : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
                    1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 1.f})
        f32(v);
    auto view = [](int offset, int length) { return QJsonObject{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}}; };
    auto accessor = [](int bufferView, int type, int count, const char* kind) {
        return QJsonObject{{"bufferView", bufferView}, {"componentType", type}, {"count", count}, {"type", kind}};
    };
    QJsonObject gltf{
        {"asset", QJsonObject{{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", QJsonArray{QJsonObject{{"nodes", QJsonArray{0, 2}}}}},
        {"nodes", QJsonArray{QJsonObject{{"name", "root"}, {"children", QJsonArray{1}}},
                             QJsonObject{{"name", "bend"}, {"translation", QJsonArray{0, 1, 0}}},
                             QJsonObject{{"name", "strip"}, {"mesh", 0}, {"skin", 0}}}},
        {"skins", QJsonArray{QJsonObject{{"joints", QJsonArray{0, 1}}, {"inverseBindMatrices", 4}}}},
        {"meshes", QJsonArray{QJsonObject{{"primitives", QJsonArray{QJsonObject{
                       {"attributes", QJsonObject{{"POSITION", 0}, {"JOINTS_0", 1}, {"WEIGHTS_0", 2}}}, {"indices", 3}}}}}}},
        {"bufferViews", QJsonArray{view(0, jointsAt), view(jointsAt, weightsAt - jointsAt), view(weightsAt, indicesAt - weightsAt),
                                   view(indicesAt, ibmAt - indicesAt), view(ibmAt, int(bin.size()) - ibmAt)}},
        {"accessors", QJsonArray{accessor(0, 5126, 6, "VEC3"), accessor(1, 5121, 6, "VEC4"), accessor(2, 5126, 6, "VEC4"),
                                 accessor(3, 5123, 12, "SCALAR"), accessor(4, 5126, 2, "MAT4")}},
    };
    QJsonObject embedded = gltf;
    embedded["buffers"] = QJsonArray{QJsonObject{{"byteLength", int(bin.size())}, {"uri", QStringLiteral("data:application/octet-stream;base64,") + QString::fromLatin1(bin.toBase64())}}};
    const QString gltfPath = outPath + QStringLiteral(".strip.gltf"), glbPath = outPath + QStringLiteral(".strip.glb");
    {
        QFile f(gltfPath);
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(embedded).toJson());
    }
    {
        QJsonObject binary = gltf;
        binary["buffers"] = QJsonArray{QJsonObject{{"byteLength", int(bin.size())}}};
        QByteArray json = QJsonDocument(binary).toJson(QJsonDocument::Compact);
        while (json.size() % 4)
            json.append(' ');
        QByteArray padded = bin;
        while (padded.size() % 4)
            padded.append('\0');
        QByteArray glb("glTF");
        auto u32 = [&](quint32 v) { glb.append(reinterpret_cast<const char*>(&v), 4); };
        u32(2);
        u32(quint32(12 + 8 + json.size() + 8 + padded.size()));
        u32(quint32(json.size()));
        u32(0x4E4F534A);
        glb.append(json);
        u32(quint32(padded.size()));
        u32(0x004E4942);
        glb.append(padded);
        QFile f(glbPath);
        f.open(QIODevice::WriteOnly);
        f.write(glb);
    }
    Model3D strip, stripGlb;
    const bool gltfOk = Model3D::load(gltfPath, &strip, &error) && strip.instances.size() == 1 && strip.instances[0].skin == 0 && strip.handles.size() == 1
                        && strip.handles[0].at == 1 && strip.handles[0].turns == 0;
    const bool glbOk = Model3D::load(glbPath, &stripGlb, &error) && stripGlb.instances.size() == 1;
    bool bent = false;
    if (gltfOk && glbOk) {
        // The upper bone turned a quarter about the middle joint: the top swings over to the left.
        strip.turnNode(1, QQuaternion::fromAxisAndAngle(0, 0, 1, 90));
        stripGlb.turnNode(1, QQuaternion::fromAxisAndAngle(0, 0, 1, 90));
        const auto a = strip.posed(), b = stripGlb.posed();
        const auto& p = a[0].positions;
        bent = close(p[0], QVector3D(-0.1f, 0, 0)) && close(p[2], QVector3D(-0.05f, 0.95f, 0)) && close(p[4], QVector3D(-1.0f, 0.9f, 0))   // the middle is half each bone's
               && close(p[5], QVector3D(-1.0f, 1.1f, 0)) && close(b[0].positions[5], p[5]);
    }
    check(QStringLiteral("3D: a rigged glTF (buffer inside) and the same as GLB open with one joint to take hold of; turned, the upper bone swings and the skin follows"),
          gltfOk && glbOk && bent);

    // The figure: a person's height, standing on the floor, with its handles.
    Model3D figure = Model3D::makeFigure();
    const bool standing = figure.bounds(&lo, &hi) && hi.y() > 1.68f && hi.y() < 1.82f && std::abs(lo.y()) < 0.04f;
    const auto world = figure.worldTransforms();
    auto nodeId = [&](const char* name) {
        for (size_t i = 0; i < figure.nodes.size(); ++i)
            if (figure.nodes[i].name == QLatin1String(name))
                return int(i);
        return -1;
    };
    const int shoulder = nodeId("upperarm.L"), elbow = nodeId("forearm.L"), hand = nodeId("hand.L");
    // Turning a joint carries everything below it round the joint, the way it was asked.
    const QQuaternion q = QQuaternion::fromAxisAndAngle(1, 0, 0, -70);
    const QVector3D pivot = figure.jointPosition(shoulder, world), elbowBefore = figure.jointPosition(elbow, world);
    figure.turnNode(shoulder, q);
    const auto turned = figure.worldTransforms();
    const bool carried = close(figure.jointPosition(elbow, turned), pivot + q.rotatedVector(elbowBefore - pivot), 2e-3f);
    const QVariantList state = figure.poseState();
    figure.resetPose();
    const bool reset = !figure.posed_() && close(figure.jointPosition(elbow, figure.worldTransforms()), elbowBefore);
    figure.setPoseState(state);
    const bool restored = close(figure.jointPosition(elbow, figure.worldTransforms()), figure.jointPosition(elbow, turned));
    figure.applyFigurePose(QStringLiteral("T-pose"));
    const QVector3D handT = figure.jointPosition(hand, figure.worldTransforms());
    figure.applyFigurePose(QStringLiteral("Sit"));
    const bool sat = figure.nodes[0].offset.y() < -0.3f;
    check(QStringLiteral("3D: the figure stands %1 m tall on the floor with %2 handles; a joint turned carries the arm round it; the pose comes back as kept; T-pose holds the hand out at %3 m")
              .arg(hi.y(), 0, 'f', 2).arg(figure.handles.size()).arg(handT.x(), 0, 'f', 2),
          standing && figure.handles.size() == 17 && carried && reset && restored && handT.x() > 0.7f && std::abs(handT.y() - 1.42f) < 0.12f && sat);

    // Rigging a person that came without joints: the figure in an A-pose, flattened.
    for (const bool away : {false, true}) {
        const QString path = outPath + (away ? QStringLiteral(".person-away.obj") : QStringLiteral(".person.obj"));
        const std::vector<QVector3D> truth = writeFigureObj(path, away);
        Model3D person;
        const bool loaded = Model3D::load(path, &person, &error) && person.canRig() && person.handles.empty();
        std::vector<QVector3D> fit;
        const bool fitted = loaded && person.fitRig(&fit, &error);
        // How far each joint the drawing hangs on is from where it truly is.
        float worst = 0.0f;
        QString worstName;
        const QStringList names = Model3D::rigJointNames();
        for (int i : {0, 2, 4, 6, 7, 8, 10, 11, 12, 14, 15, 16, 18, 19, 20}) {
            // A shoulder is guessed from the width of the chest, which is less sure than the rest.
            const float allow = (i == 6 || i == 10) ? 0.10f / 0.07f : 1.0f;
            const float d = fitted ? (fit[size_t(i)] - truth[size_t(i)]).length() / allow : 9.0f;
            if (d > worst) {
                worst = d;
                worstName = names[i];
            }
        }
        // Bound, it looks as it did; walking, its left foot goes forward, whichever way it faces.
        bool unchanged = false, walks = false;
        if (fitted) {
            const auto before = person.posed();
            const bool bound = person.bindRig(fit);
            const auto after = person.posed();
            float moved = 0.0f;
            for (size_t k = 0; k < before.size() && k < after.size(); ++k)
                for (size_t v = 0; v < before[k].positions.size(); ++v)
                    moved = std::max(moved, (before[k].positions[v] - after[k].positions[v]).length());
            unchanged = bound && person.humanoid && person.handles.size() == 17 && moved < 1e-3f && person.facesAway(fit) == away;
            auto ankle = [&](const char* name) {
                for (size_t i = 0; i < person.nodes.size(); ++i)
                    if (person.nodes[i].name == QLatin1String(name))
                        return person.jointPosition(int(i), person.worldTransforms());
                return QVector3D();
            };
            person.applyFigurePose(QStringLiteral("Stand"));
            const QVector3D standing = ankle("foot.L");
            person.applyFigurePose(QStringLiteral("Walk"));
            const float forward = (ankle("foot.L").z() - standing.z()) * (away ? -1.0f : 1.0f);
            walks = forward > 0.08f;
        }
        if (!away && fitted) {
            // Written out as a figure would be, it comes back the same shape, its points welded.
            Model3D plain;
            Model3D::load(path, &plain, &error);
            const QString glb = outPath + QStringLiteral(".person.glb");
            Model3D back;
            const bool written = plain.saveGlb(glb, &error) && Model3D::load(glb, &back, &error);
            size_t before = 0, after = 0, triBefore = 0, triAfter = 0;
            for (const ModelMesh& m : plain.meshes) {
                before += m.positions.size();
                triBefore += m.indices.size();
            }
            for (const ModelMesh& m : back.meshes) {
                after += m.positions.size();
                triAfter += m.indices.size();
            }
            QVector3D l0, h0, l1, h1;
            plain.bounds(&l0, &h0);
            back.bounds(&l1, &h1);
            check(QStringLiteral("a model written as GLB comes back with its %1 triangles where they were (%2 points welded to %3)").arg(triAfter / 3).arg(before).arg(after),
                  written && triAfter == triBefore && after <= before && (l0 - l1).length() < 1e-4f && (h0 - h1).length() < 1e-4f && back.canRig());
        }
        check(QStringLiteral("rigging an OBJ person (A-pose, facing %1): the joints land within %2 m of the true ones (%3 the furthest; shoulders within 0.10); bound, it looks as it did; walking, its left foot goes forward")
                  .arg(away ? QStringLiteral("away") : QStringLiteral("forward")).arg(worst, 0, 'f', 3).arg(worstName),
              fitted && worst < 0.07f && unchanged && walks);
    }
    // The woman built in: listed among the figures, rigged from what comes with her, and posable.
    {
        const QVector<figures::Figure> all = figures::list();
        QString path;
        for (const figures::Figure& f : all) {
            if (f.builtIn && f.name == QLatin1String("Woman"))
                path = f.path;
        }
        Model3D woman;
        std::vector<QVector3D> joints;
        QString name;
        const bool listed = !path.isEmpty() && figures::rigFor(path, &joints, &name) && name == QLatin1String("Woman");
        const bool rigged = listed && Model3D::load(path, &woman, &error) && woman.bindRig(joints) && woman.humanoid && woman.handles.size() == 17;
        bool walks = false;
        if (rigged) {
            int foot = -1;
            for (size_t i = 0; i < woman.nodes.size(); ++i)
                if (woman.nodes[i].name == QLatin1String("foot.L"))
                    foot = int(i);
            woman.applyFigurePose(QStringLiteral("Stand"));
            const QVector3D standing = woman.jointPosition(foot, woman.worldTransforms());
            woman.applyFigurePose(QStringLiteral("Walk"));
            walks = (woman.jointPosition(foot, woman.worldTransforms()) - standing).length() > 0.05f;
        }
        check(QStringLiteral("the woman built in is among the figures, comes with her joints, binds, and walks"), listed && rigged && walks);
    }
    return all;
}

// Animation: frames, onion skins, holds, the background frame, and the files it goes out as.
static bool runAnimationTests(CanvasEngine& engine, const QString& outPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED").arg(what);
    };
    auto nearby = [](const QColor& c, int r, int g, int b, int tol) {
        return c.isValid() && std::abs(c.red() - r) <= tol && std::abs(c.green() - g) <= tol && std::abs(c.blue() - b) <= tol;
    };
    auto block = [](const QColor& c, int x) {
        QImage img(60, 100, QImage::Format_ARGB32_Premultiplied);
        img.fill(c);
        return std::make_pair(img, QPoint(x, 100));
    };
    engine.newDocument(QSize(400, 300), Qt::white);
    const int empty = engine.activeLayerId();
    const auto [redImg, redAt] = block(QColor(220, 30, 30), 20);
    const auto [blueImg, blueAt] = block(QColor(30, 30, 220), 160);
    const auto [greenImg, greenAt] = block(QColor(30, 160, 60), 300);
    const int red = engine.addImageLayer(redImg, QStringLiteral("Red"), redAt);
    const int blue = engine.addImageLayer(blueImg, QStringLiteral("Blue"), blueAt);
    const int green = engine.addImageLayer(greenImg, QStringLiteral("Green"), greenAt);
    engine.removeLayer(empty);
    engine.setActiveLayer(green);
    const QImage still = engine.renderImage();
    const bool allThree = nearby(still.pixelColor(50, 150), 220, 30, 30, 2) && nearby(still.pixelColor(190, 150), 30, 30, 220, 2) && nearby(still.pixelColor(330, 150), 30, 160, 60, 2);

    // One frame at a time, the one before it faded behind it.
    Animation anim;
    anim.enabled = true;
    anim.onionFrames = 1;
    anim.onionOpacity = 0.5f;
    anim.tintOnion = false;
    engine.setAnimation(anim);
    const QImage frame = engine.renderImage();
    const bool alone = engine.frames() == QVector<int>{red, blue, green} && engine.currentFrame() == 2 && nearby(frame.pixelColor(330, 150), 30, 160, 60, 2)
                       && nearby(frame.pixelColor(190, 150), 255, 255, 255, 1) && nearby(frame.pixelColor(50, 150), 255, 255, 255, 1);
    const QColor onion = engine.pickColor(QPoint(190, 150));
    const bool faded = nearby(onion, 142, 142, 237, 4) && nearby(engine.pickColor(QPoint(50, 150)), 255, 255, 255, 1);
    anim.tintOnion = true;
    engine.setAnimation(anim);
    const QColor tinted = engine.pickColor(QPoint(190, 150));
    const bool reddened = tinted.red() > tinted.blue() + 40 && tinted.red() > tinted.green() + 40;
    check(QStringLiteral("animation: each top-level layer is a frame and shows alone; the one before is faded behind it (%1), tinted red when asked (%2)")
              .arg(onion.name(), tinted.name()),
          allThree && alone && faded && reddened);

    // Choosing a frame chooses its layer; the first frame can stay as a background.
    engine.showFrame(0);
    const QImage first = engine.renderImage();
    const bool chosen = engine.activeLayerId() == red && nearby(first.pixelColor(50, 150), 220, 30, 30, 2) && nearby(first.pixelColor(330, 150), 255, 255, 255, 1);
    anim.background = true;
    engine.setAnimation(anim);
    const QImage withBackground = engine.renderFrame(2);
    const bool background = nearby(withBackground.pixelColor(50, 150), 220, 30, 30, 2) && nearby(withBackground.pixelColor(330, 150), 30, 160, 60, 2)
                            && nearby(withBackground.pixelColor(190, 150), 255, 255, 255, 1);
    anim.background = false;
    engine.setAnimation(anim);
    check(QStringLiteral("animation: showing the first frame makes its layer the active one; as a background it stays under the others"), chosen && background);

    // Holds are an undo step; a new frame goes straight after the current one.
    engine.setFrameHold(blue, 3);
    const bool held = engine.layerInfo(blue).hold == 3;
    engine.undo();
    const bool undone = engine.layerInfo(blue).hold == 1;
    engine.redo();
    engine.showFrame(0);
    const int added = engine.addFrame();
    const bool placed = engine.frames().size() == 4 && engine.frames()[1] == added && engine.currentFrame() == 1;
    engine.undo();
    const bool oneStep = engine.frames() == QVector<int>{red, blue, green};
    check(QStringLiteral("animation: a frame's hold is an undo step; a new frame goes in after the current one, as one step"),
          held && undone && engine.layerInfo(blue).hold == 3 && placed && oneStep);

    // A group is one frame, put together.
    engine.showFrame(1);
    const int grouped = engine.groupLayer(blue);
    const QImage thumb = engine.frameThumbnail(1, 64);
    const bool groupFrame = engine.frames().size() == 3 && engine.frames()[1] == grouped && !thumb.isNull() && thumb.width() == 64;
    engine.undo();
    check(QStringLiteral("animation: a group is one frame, and its thumbnail shows what is in it"), groupFrame && engine.frames() == QVector<int>{red, blue, green});

    // Kept with the document.
    anim.fps = 10;
    anim.playback = Animation::Playback::PingPong;
    anim.onionFrames = 3;
    engine.setAnimation(anim);
    engine.showFrame(1);
    QString error;
    const QString doc = outPath + QStringLiteral(".animation.imp");
    const bool saved = docio::save(engine, doc, &error);
    engine.newDocument(QSize(64, 64), Qt::white);
    const bool loaded = saved && docio::load(engine, doc, &error);
    const QVector<int> back = engine.frames();
    const bool kept = loaded && engine.animation() == anim && back.size() == 3 && engine.layerInfo(back[1]).hold == 3 && engine.currentFrame() == 1 && !engine.isModified();
    check(QStringLiteral("animation: the frame rate, ping-pong, onion skins, holds and the current frame are kept in the file"), kept);

    // Out as an animated GIF and an animated PNG.
    QVector<animio::Frame> frames;
    for (int i = 0; i < back.size(); ++i)
        frames.push_back({engine.renderFrame(i), engine.layerInfo(back[i]).hold});
    const QString gif = outPath + QStringLiteral(".animation.gif"), apng = outPath + QStringLiteral(".animation.png");
    const bool gifWritten = animio::writeGif(gif, frames, 10, true, &error);
    QImageReader reader(gif);
    QVector<QImage> read;
    QList<int> delays;
    while (reader.canRead()) {
        const QImage img = reader.read();
        if (img.isNull())
            break;
        read.push_back(img);
        delays.push_back(reader.nextImageDelay());
    }
    std::sort(delays.begin(), delays.end());
    const bool gifOk = gifWritten && read.size() == 3 && reader.loopCount() == -1 && delays == QList<int>{100, 100, 300}
                       && nearby(read[0].pixelColor(50, 150), 220, 30, 30, 24) && nearby(read[1].pixelColor(190, 150), 30, 30, 220, 24)
                       && nearby(read[2].pixelColor(330, 150), 30, 160, 60, 24) && nearby(read[0].pixelColor(330, 150), 255, 255, 255, 16);
    const bool apngWritten = animio::writeApng(apng, frames, 10, true, &error);
    QFile file(apng);
    file.open(QIODevice::ReadOnly);
    const QByteArray bytes = file.readAll();
    file.close();
    int fctl = 0, fdat = 0, numFrames = -1, secondDelay = -1;
    for (qsizetype at = 8; at + 12 <= bytes.size();) {
        auto u32 = [&](qsizetype i) { return int(quint32(uchar(bytes[i])) << 24 | quint32(uchar(bytes[i + 1])) << 16 | quint32(uchar(bytes[i + 2])) << 8 | quint32(uchar(bytes[i + 3]))); };
        const int length = u32(at);
        const QByteArray type = bytes.mid(at + 4, 4);
        if (type == "acTL")
            numFrames = u32(at + 8);
        if (type == "fcTL" && ++fctl == 2)
            secondDelay = (uchar(bytes[at + 8 + 20]) << 8 | uchar(bytes[at + 8 + 21])) * 1000 / std::max(1, (uchar(bytes[at + 8 + 22]) << 8 | uchar(bytes[at + 8 + 23])));
        fdat += type == "fdAT";
        at += 12 + length;
    }
    const QImage firstFrame(apng);
    const bool apngOk = apngWritten && numFrames == 3 && fctl == 3 && fdat == 2 && secondDelay == 300 && nearby(firstFrame.pixelColor(50, 150), 220, 30, 30, 1);
    check(QStringLiteral("animation: out as a GIF (%1 frames, %2 ms the held one, looping) and as an animated PNG (%3 frames, the default image the first)")
              .arg(read.size()).arg(delays.value(2)).arg(numFrames),
          gifOk && apngOk);
    // A video, where ffmpeg is installed: each held frame repeated, so 1 + 3 + 1 frames.
    if (!animio::ffmpegPath().isEmpty()) {
        const QString video = outPath + QStringLiteral(".animation.mp4");
        const bool written = animio::writeVideo(video, frames, 10, &error);
        QProcess probe;
        probe.start(QStandardPaths::findExecutable(QStringLiteral("ffprobe")),
                    {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-count_frames"), QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                     QStringLiteral("-show_entries"), QStringLiteral("stream=nb_read_frames,width,height"), QStringLiteral("-of"), QStringLiteral("csv=p=0"), video});
        probe.waitForFinished(30000);
        const QString seen = QString::fromLatin1(probe.readAllStandardOutput()).trimmed();
        check(QStringLiteral("animation: out as an MP4 video through ffmpeg (width, height, frames: %1)").arg(seen),
              written && seen == QLatin1String("400,300,5"));
    } else {
        qInfo().noquote() << "  (no ffmpeg here: video export not tried)";
    }
    engine.newDocument(QSize(64, 64), Qt::white);
    return all;
}

// The selection kept with the document, the newer gradients, masks on groups,
// a group as the reference, spacing in text, and Procreate's palettes.
static bool runSmallGapTests(CanvasEngine& engine, const QString& outPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED").arg(what);
    };
    auto nearby = [](const QColor& c, int r, int g, int b, int tol) {
        return std::abs(c.red() - r) <= tol && std::abs(c.green() - g) <= tol && std::abs(c.blue() - b) <= tol;
    };
    auto box = [&](const QRectF& r, bool ellipse = false) {
        QPainterPath p;
        if (ellipse)
            p.addEllipse(r);
        else
            p.addRect(r);
        engine.setSelection(p, QStringLiteral("Select"));
    };

    // 1. The selection goes into the file and comes back, soft edge and all.
    {
        engine.newDocument(QSize(800, 600), Qt::white);
        box(QRectF(120, 90, 400, 300), true);
        engine.featherSelection(24.0f);
        const QRect bounds = engine.selectionBounds();
        const QImage before = engine.selectionPixels();
        const QString path = outPath + QStringLiteral(".selection.imp");
        QString error;
        const bool saved = docio::save(engine, path, &error);
        engine.newDocument(QSize(64, 64), Qt::white);
        const bool loaded = saved && docio::load(engine, path, &error);
        const bool same = loaded && engine.selectionBounds() == bounds && engine.selectionPixels() == before;
        // Without a selection a document is written as before, so that older versions still open it.
        engine.clearSelection();
        const QString plain = outPath + QStringLiteral(".noselection.imp");
        docio::save(engine, plain, &error);
        QFile file(plain);
        file.open(QIODevice::ReadOnly);
        QDataStream in(&file);
        quint32 magic = 0, version = 0;
        in >> magic >> version;
        file.close();
        const bool reopened = docio::load(engine, plain, &error) && !engine.hasSelection();
        check(QStringLiteral("a feathered selection is saved with the document and comes back the same (%1 x %2); without one the file is version %3 as before")
                  .arg(bounds.width()).arg(bounds.height()).arg(version),
              same && version == 2 && reopened);
    }

    // 2. Gradients of every kind, red to blue, from (300,300) towards (500,300).
    {
        engine.newDocument(QSize(800, 600), Qt::white);
        const QColor red(255, 0, 0), blue(0, 0, 255);
        struct Probe {
            GradientKind kind;
            const char* name;
            QPoint at;
            int r, b;   // what is there: red and blue (green stays 0)
        };
        const Probe probes[] = {
            {GradientKind::Linear, "linear", QPoint(300, 300), 255, 0},   {GradientKind::Linear, "linear", QPoint(500, 380), 0, 255},
            {GradientKind::Radial, "radial", QPoint(300, 300), 255, 0},   {GradientKind::Radial, "radial", QPoint(300, 500), 0, 255},
            {GradientKind::Angle, "angle", QPoint(450, 290), 255, 0},     {GradientKind::Angle, "angle", QPoint(450, 310), 0, 255},
            {GradientKind::Angle, "angle", QPoint(150, 300), 128, 128},   {GradientKind::Reflected, "reflected", QPoint(100, 300), 0, 255},
            {GradientKind::Reflected, "reflected", QPoint(300, 300), 255, 0}, {GradientKind::Reflected, "reflected", QPoint(400, 300), 128, 128},
            {GradientKind::Diamond, "diamond", QPoint(400, 400), 0, 255}, {GradientKind::Diamond, "diamond", QPoint(350, 300), 191, 64},
        };
        QStringList wrong;
        for (const Probe& p : probes) {
            engine.clearLayer(engine.activeLayerId());
            engine.beginGradient();
            engine.setGradient(QPointF(300, 300), QPointF(500, 300), red, blue, p.kind);
            engine.endGradient(true);
            const QColor c = engine.pickColor(p.at);
            if (!nearby(c, p.r, 0, p.b, 12))
                wrong << QStringLiteral("%1 at %2,%3 is %4").arg(QLatin1String(p.name)).arg(p.at.x()).arg(p.at.y()).arg(c.name());
        }
        check(QStringLiteral("linear, radial, angle, reflected and diamond gradients each put the colours where they belong%1")
                  .arg(wrong.isEmpty() ? QString() : QStringLiteral(": ") + wrong.join(QStringLiteral("; "))),
              wrong.isEmpty());
    }

    // 3. A mask on a group hides part of everything in it, can be painted on, and lasts.
    {
        engine.newDocument(QSize(800, 600), Qt::white);
        const int inside = engine.activeLayerId();
        engine.fillLayer(inside, QColor(220, 30, 30));
        const int group = engine.groupLayer(inside);
        box(QRectF(0, 0, 400, 600));
        engine.addLayerMask(group);
        engine.clearSelection();
        const bool hides = nearby(engine.pickColor(QPoint(200, 300)), 220, 30, 30, 2) && nearby(engine.pickColor(QPoint(600, 300)), 255, 255, 255, 2);
        // Painting white on the group's mask brings the right side back where the brush goes.
        engine.setActiveLayer(group);
        engine.setMaskEditing(true);
        const BrushPreset& round = *BrushLibrary::instance().find(QStringLiteral("round-brush"));
        StrokeSample a, b;
        a.pos = QPointF(450, 300);
        b.pos = QPointF(750, 300);
        b.time = 0.2;
        const bool began = engine.beginStroke(StrokeTool::Brush, round, 80, 1.0f, Qt::white, a, 7);
        engine.strokeTo(b);
        engine.endStroke();
        const bool painted = began && nearby(engine.pickColor(QPoint(600, 300)), 220, 30, 30, 4) && nearby(engine.pickColor(QPoint(600, 500)), 255, 255, 255, 2);
        engine.setMaskEditing(false);
        const QImage picture = engine.renderImage();
        // Through a file, through PSD and OpenRaster, and flattened.
        QString error;
        const QString path = outPath + QStringLiteral(".groupmask.imp");
        const bool saved = docio::save(engine, path, &error) && docio::load(engine, path, &error);
        int masked = 0;
        for (const LayerInfo& l : engine.layers())
            masked += l.isGroup && l.hasMask;
        auto apart = [](const QImage& x, const QImage& y) {
            int worst = 0;
            const QImage p = x.convertToFormat(QImage::Format_ARGB32), q = y.convertToFormat(QImage::Format_ARGB32);
            for (int yy = 0; yy < p.height(); yy += 3) {
                for (int xx = 0; xx < p.width(); xx += 3) {
                    const QRgb u = p.pixel(xx, yy), v = q.pixel(xx, yy);
                    worst = std::max({worst, std::abs(qRed(u) - qRed(v)), std::abs(qGreen(u) - qGreen(v)), std::abs(qBlue(u) - qBlue(v))});
                }
            }
            return worst;
        };
        const int fileApart = saved ? apart(engine.renderImage(), picture) : 255;
        const bool kept = saved && masked == 1 && fileApart <= 1;
        const QString psd = outPath + QStringLiteral(".groupmask.psd"), ora = outPath + QStringLiteral(".groupmask.ora");
        const bool wrote = interchange::exportPsd(engine, psd, 8, &error, nullptr) && interchange::exportOra(engine, ora, &error, nullptr);
        engine.newDocument(QSize(64, 64), Qt::white);
        bool psdOk = wrote && interchange::open(engine, psd, &error, nullptr) && apart(picture, engine.renderImage()) <= 2;
        int psdMasked = 0;
        for (const LayerInfo& l : engine.layers())
            psdMasked += l.isGroup && l.hasMask;
        psdOk = psdOk && psdMasked == 1;
        engine.newDocument(QSize(64, 64), Qt::white);
        const bool oraOk = wrote && interchange::open(engine, ora, &error, nullptr) && apart(picture, engine.renderImage()) <= 2;
        docio::load(engine, path, &error);
        int groupId = 0;
        for (const LayerInfo& l : engine.layers()) {
            if (l.isGroup)
                groupId = l.id;
        }
        engine.flattenGroup(groupId);
        const LayerInfo flat = engine.layerInfo(engine.activeLayerId());
        const int flatApart = apart(engine.renderImage(), picture);
        const bool flattened = !flat.isGroup && flat.hasMask && flatApart <= 1;
        QStringList missing;
        const std::pair<bool, const char*> parts[] = {{hides, "hide"}, {painted, "paint"}, {kept, "file"}, {psdOk, "PSD"}, {oraOk, "OpenRaster"}, {flattened, "flatten"}};
        for (const auto& [ok, name] : parts) {
            if (!ok)
                missing << QLatin1String(name);
        }
        if (!kept || !flattened)
            missing << QStringLiteral("masked groups %1, file %2 apart, flattened %3 apart, flat group %4 mask %5").arg(masked).arg(fileApart).arg(flatApart).arg(flat.isGroup).arg(flat.hasMask);
        check(QStringLiteral("a mask on a group hides that part of it, takes paint, and survives the file, PSD (as a mask) and OpenRaster (as one layer), and flattening%1")
                  .arg(missing.isEmpty() ? QString() : QStringLiteral(" (not: %1)").arg(missing.join(QStringLiteral(", ")))),
              missing.isEmpty());
    }

    // 4. A group as the reference: a fill on another layer stops at the lines of everything in it.
    {
        engine.newDocument(QSize(800, 600), Qt::white);
        const int lines = engine.activeLayerId();
        const BrushPreset& pen = *BrushLibrary::instance().find(QStringLiteral("studio-pen"));
        const QPointF corners[] = {QPointF(200, 150), QPointF(600, 150), QPointF(600, 450), QPointF(200, 450), QPointF(200, 150)};
        for (int k = 0; k < 4; ++k) {
            StrokeSample a, b;
            a.pos = corners[k];
            b.pos = corners[k + 1];
            b.time = 0.2;
            engine.beginStroke(StrokeTool::Brush, pen, 8, 1.0f, Qt::black, a, quint32(k + 1));
            engine.strokeTo(b);
            engine.endStroke();
        }
        const int group = engine.groupLayer(lines);
        engine.setReferenceLayer(group);
        const bool marked = engine.referenceLayerId() == group && engine.layerInfo(group).reference;
        // Colour on a layer of its own, outside the group.
        engine.setActiveLayer(group);
        const int colour = engine.addLayer();
        engine.moveLayer(colour, 0, 0);
        engine.setActiveLayer(colour);
        const bool filled = engine.floodFill(QPoint(400, 300), QColor(40, 160, 60), 0.2f);
        const bool contained = filled && nearby(engine.pickColor(QPoint(400, 300)), 40, 160, 60, 3) && nearby(engine.pickColor(QPoint(100, 300)), 255, 255, 255, 2);
        // And on a layer of the group itself, the lines on the other layers of it still hold the colour in.
        engine.clearLayer(colour);
        engine.setActiveLayer(lines);
        const int member = engine.addLayer();
        const bool inside = engine.layerInfo(member).parent == group;
        const bool filledInside = engine.floodFill(QPoint(400, 300), QColor(200, 120, 30), 0.2f);
        const bool heldInside = inside && filledInside && nearby(engine.pickColor(QPoint(400, 300)), 200, 120, 30, 3)
                                && nearby(engine.pickColor(QPoint(100, 300)), 255, 255, 255, 2);
        check(QStringLiteral("a group as the reference: a fill on a layer outside it, or on another layer inside it, stops at its lines"),
              marked && contained && heldInside);
    }

    // 5. Spacing in text.
    {
        TextInfo plain;
        plain.text = QStringLiteral("Spacing\nlines");
        plain.pixelSize = 60;
        QRectF tight, wide, tall;
        const QVector<TextLine> a = text::layout(plain, &tight);
        TextInfo spaced = plain;
        spaced.letterSpacing = 0.25;
        text::layout(spaced, &wide);
        TextInfo airy = plain;
        airy.lineSpacing = 2.0;
        const QVector<TextLine> c = text::layout(airy, &tall);
        // Seven letters in the first line: six gaps grow by a quarter of the size each.
        const double grew = wide.width() - tight.width();
        const bool letters = std::abs(grew - 6 * 0.25 * 60) < 3.0;
        const bool rows = a.size() == 2 && c.size() == 2 && std::abs((c[1].top - c[0].top) - 2.0 * (a[1].top - a[0].top)) < 0.5;
        engine.newDocument(QSize(800, 600), Qt::white);
        spaced.anchor = QPointF(100, 100);
        spaced.lineSpacing = 1.5;
        const int id = engine.addTextLayer(spaced, QStringLiteral("Spaced"));
        QString error;
        const QString path = outPath + QStringLiteral(".text-spacing.imp");
        const bool kept = id && docio::save(engine, path, &error) && docio::load(engine, path, &error);
        TextInfo back;
        for (const LayerInfo& l : engine.layers()) {
            if (l.isText)
                back = engine.layerText(l.id);
        }
        // Lines set close never cut the letters: one line is the same at any line spacing, and the
        // letters never touch the edge of what is rendered, in a heavy font with deep descenders too.
        TextInfo heavy;
        heavy.text = QStringLiteral("ghjghj");
        heavy.pixelSize = 389;
        heavy.bold = true;
        heavy.alignment = Qt::AlignHCenter;
        heavy.anchor = QPointF(1200, 100);   // on a canvas wide enough that its edges do not cut the letters either
        const QStringList families = QFontDatabase::families();
        for (const QString& f : {QStringLiteral("Noto Sans CJK HK Black"), QStringLiteral("Noto Sans CJK HK"), QStringLiteral("Noto Sans CJK SC")}) {
            if (families.contains(f)) {
                heavy.family = f;
                break;
            }
        }
        auto placed = [](const TextInfo& t) {
            QPoint at;
            const QImage letters = text::render(t, QSize(2400, 1000), &at);
            QImage whole(2400, 1000, QImage::Format_ARGB32_Premultiplied);
            whole.fill(Qt::transparent);
            QPainter(&whole).drawImage(at, letters);
            bool edgeClear = true;
            for (int x = 0; x < letters.width(); ++x)
                edgeClear = edgeClear && qAlpha(letters.pixel(x, 0)) == 0 && qAlpha(letters.pixel(x, letters.height() - 1)) == 0;
            for (int y = 0; y < letters.height(); ++y)
                edgeClear = edgeClear && qAlpha(letters.pixel(0, y)) == 0 && qAlpha(letters.pixel(letters.width() - 1, y)) == 0;
            return std::make_pair(whole, edgeClear && !letters.isNull());
        };
        TextInfo close = heavy;
        close.lineSpacing = 0.5;
        const auto [atOne, clearOne] = placed(heavy);
        const auto [atHalf, clearHalf] = placed(close);
        // And through a layer: set at half spacing, then at the usual, the canvas does not change.
        engine.newDocument(QSize(2400, 1000), Qt::white);
        const int heavyId = engine.addTextLayer(close, QStringLiteral("Heavy"));
        const QImage layerHalf = engine.renderImage();
        engine.setLayerText(heavyId, heavy);
        const bool sameLayer = heavyId && layerHalf == engine.renderImage();
        QStringList heavyMissing;
        if (atOne != atHalf)
            heavyMissing << QStringLiteral("not the same as text");
        if (!clearOne || !clearHalf)
            heavyMissing << QStringLiteral("letters reach the edge (%1, %2)").arg(clearOne).arg(clearHalf);
        if (!sameLayer)
            heavyMissing << QStringLiteral("not the same as a layer");
        check(QStringLiteral("one line in %1 at 389 px comes out the same at half line spacing as at the usual, uncut, as text and as a layer%2")
                  .arg(heavy.family.isEmpty() ? QStringLiteral("the default font") : heavy.family,
                       heavyMissing.isEmpty() ? QString() : QStringLiteral(" (") + heavyMissing.join(QStringLiteral("; ")) + QLatin1Char(')')),
              heavyMissing.isEmpty());
        check(QStringLiteral("letter spacing widens a line by %1 px (6 gaps of 15), line spacing doubles the step between lines, and both are saved")
                  .arg(grew, 0, 'f', 1),
              letters && rows && kept && std::abs(back.letterSpacing - 0.25) < 1e-9 && std::abs(back.lineSpacing - 1.5) < 1e-9);
    }

    // 6. Procreate's palettes, out and in; and a copy dropped onto a taken slot finds the next free one.
    {
        PaletteLibrary& lib = PaletteLibrary::instance();
        const QVector<QColor> colours{QColor(230, 57, 70), QColor(), QColor(29, 53, 87), QColor(241, 250, 238), QColor(168, 218, 220)};
        const QString id = lib.create(QStringLiteral("Procreate test"), colours);
        const QString path = outPath + QStringLiteral(".palette.swatches");
        QString error;
        const bool exported = lib.exportSwatches(id, path, &error);
        zip::Reader zip;
        const bool isZip = zip.open(path) && zip.contains(QStringLiteral("Swatches.json"));
        const QJsonDocument json = QJsonDocument::fromJson(zip.read(QStringLiteral("Swatches.json")));
        const QJsonObject first = json.array().first().toObject();
        const bool shaped = json.isArray() && first.value(QStringLiteral("name")).toString() == QLatin1String("Procreate test")
                            && first.value(QStringLiteral("swatches")).toArray().size() == 5 && first.value(QStringLiteral("swatches")).toArray().at(1).isNull();
        const QString back = lib.importFile(path, &error);
        const Palette* imported = lib.find(back);
        bool same = imported && imported->colors.size() >= 5;
        for (int k = 0; same && k < colours.size(); ++k) {
            const QColor c = imported->colors[k];
            same = colours[k].isValid() ? c.isValid() && nearby(c, colours[k].red(), colours[k].green(), colours[k].blue(), 1) : !c.isValid();
        }
        const int placed = lib.placeSwatch(id, 0, QColor(10, 20, 30));
        const bool nextFree = placed == 1 && lib.find(id)->colors[1] == QColor(10, 20, 30);
        lib.remove(id);
        lib.remove(back);
        check(QStringLiteral("a palette goes out as Procreate's .swatches (a zip with Swatches.json, the gap kept as null) and comes back the same; a copy dropped on a taken slot goes to the next free one"),
              exported && isZip && shaped && same && nextFree);
    }
    // 7. Shapes closed and opened.
    {
        // How far the outline of `b` strays from the shape `a` at most.
        auto strayFrom = [](const QuickShape& a, const QuickShape& b) {
            const QuickShape path = a.toPath();
            double worst = 0.0;
            for (const PathPoint& pt : b.path(1.0))
                worst = std::max(worst, path.nearest(pt.pos).distance);
            return worst;
        };
        auto apart = [](const QPointF& u, const QPointF& v) { return std::hypot(u.x() - v.x(), u.y() - v.y()); };
        QuickShape zig;
        zig.kind = QuickShape::Kind::Polyline;
        zig.pts = {QPointF(100, 100), QPointF(300, 120), QPointF(320, 300), QPointF(140, 280)};
        QuickShape closedZig = zig;
        const bool zigClosed = closedZig.closePath() && closedZig.closed() && closedZig.nodes.size() == 4 && closedZig.segmentCount() == 4;
        // Ends drawn to meet, a couple of pixels apart: one point.
        QuickShape almost = zig;
        almost.pts.push_back(QPointF(102, 101));
        const bool merged = almost.closePath() && almost.nodes.size() == 4;
        // A smooth curve closed: what was drawn stays where it was, and the join is new.
        QuickShape wave;
        wave.kind = QuickShape::Kind::Curve;
        wave.pts = {QPointF(100, 300), QPointF(220, 180), QPointF(360, 320), QPointF(480, 200)};
        QuickShape loop = wave;
        const bool curveClosed = loop.closePath() && loop.closed() && loop.segmentCount() == loop.nodes.size();
        const double curveShift = curveClosed ? strayFrom(loop, wave) : 99.0;   // every point of the curve is still on the shape
        QuickShape line;
        line.kind = QuickShape::Kind::Line;
        line.pts = {QPointF(10, 10), QPointF(200, 40)};
        const bool lineStays = !line.closePath() && line.kind == QuickShape::Kind::Line;
        // A circle and a rectangle cut open: the same outline, two ends at the cut.
        QuickShape circle;
        circle.kind = QuickShape::Kind::Ellipse;
        circle.center = QPointF(400, 300);
        circle.rx = circle.ry = 150;
        QuickShape cutCircle = circle;
        const double circleShift = cutCircle.openPath(2) ? strayFrom(circle, cutCircle) : 99.0;
        const bool circleOpened = !cutCircle.closed() && cutCircle.nodes.size() == circle.toPath().nodes.size() + 1
                                  && apart(cutCircle.nodes.front().pos, cutCircle.nodes.back().pos) < 1e-9 && circleShift < 0.05;
        QuickShape box;
        box.kind = QuickShape::Kind::Rectangle;
        box.center = QPointF(400, 300);
        box.rx = 200;
        box.ry = 120;
        box.rot = 20;
        QuickShape cutBox = box;
        const double boxShift = cutBox.openPath(0) ? strayFrom(box, cutBox) : 99.0;
        const bool boxOpened = !cutBox.closed() && cutBox.nodes.size() == 5 && boxShift < 0.05;
        // And closed again, it is the shape it was.
        QuickShape again = cutCircle;
        const bool reclosed = again.closePath() && again.closed() && again.nodes.size() == circle.toPath().nodes.size() && strayFrom(circle, again) < 0.05;
        check(QStringLiteral("shapes: an open one closes with one more run (ends that nearly meet become one point, a line will not); "
                             "a circle and a rectangle cut open keep their outline (%1 and %2 px off, as finely as it is measured), and close again%3")
                  .arg(circleShift, 0, 'f', 4).arg(boxShift, 0, 'f', 4)
                  .arg(zigClosed && merged && lineStays && circleOpened && boxOpened && reclosed && curveClosed && curveShift < 0.05
                           ? QString()
                           : QStringLiteral(" (closed %1, merged %2, line %3, circle %4, box %5, again %6, curve %7 moved %8 px)")
                                 .arg(zigClosed).arg(merged).arg(lineStays).arg(circleOpened).arg(boxOpened).arg(reclosed).arg(curveClosed).arg(curveShift, 0, 'f', 3)),
              zigClosed && merged && lineStays && circleOpened && boxOpened && reclosed && curveClosed && curveShift < 0.05);
    }
    engine.newDocument(QSize(64, 64), Qt::white);
    return all;
}

// Out to PSD and OpenRaster and back in again: the same picture, and as
// much of the layers as each format can hold.
static bool runInterchangeTests(CanvasEngine& engine, const QString& outPath)
{
    bool all = true;
    auto check = [&](const QString& what, bool ok) {
        all = all && ok;
        qInfo().noquote() << QStringLiteral("  %1 %2").arg(ok ? "ok    " : "FAILED").arg(what);
    };
    // The largest difference in any colour channel between two pictures.
    auto apart = [](const QImage& a, const QImage& b) {
        const QImage x = a.convertToFormat(QImage::Format_ARGB32), y = b.convertToFormat(QImage::Format_ARGB32);
        if (x.size() != y.size())
            return 255;
        int worst = 0;
        for (int row = 0; row < x.height(); ++row) {
            const QRgb* p = reinterpret_cast<const QRgb*>(x.constScanLine(row));
            const QRgb* q = reinterpret_cast<const QRgb*>(y.constScanLine(row));
            for (int col = 0; col < x.width(); ++col) {
                worst = std::max({worst, std::abs(qRed(p[col]) - qRed(q[col])), std::abs(qGreen(p[col]) - qGreen(q[col])),
                                  std::abs(qBlue(p[col]) - qBlue(q[col]))});
            }
        }
        return worst;
    };
    auto byName = [&](const QString& name) {
        for (const LayerInfo& l : engine.layers()) {
            if (l.name == name)
                return l;
        }
        return LayerInfo{};
    };

    struct Way {
        const char* what;
        QString suffix;
        int bits;   // 0: OpenRaster
    };
    const Way ways[] = {{"OpenRaster", QStringLiteral(".ora"), 0}, {"PSD, 8 bits", QStringLiteral("8.psd"), 8}, {"PSD, 16 bits", QStringLiteral("16.psd"), 16}};
    for (const Way& way : ways) {
        buildInterchangeSample(engine, false);
        const QImage original = engine.renderImage();
        const int layersBefore = engine.layerCount();
        const QString path = outPath + QStringLiteral(".sample") + way.suffix;
        QString error;
        QStringList notes;
        const bool written = way.bits ? interchange::exportPsd(engine, path, way.bits, &error, &notes) : interchange::exportOra(engine, path, &error, &notes);
        engine.newDocument(QSize(64, 64), Qt::white);   // nothing left over from before
        QStringList openNotes;
        const bool opened = written && interchange::open(engine, path, &error, &openNotes);
        const int off = opened ? apart(original, engine.renderImage()) : 255;
        const LayerInfo group = byName(QStringLiteral("Group 50%")), masked = byName(QStringLiteral("Masked"));
        const LayerInfo clipped = byName(QStringLiteral("Clipped stripes")), wash = byName(QStringLiteral("Wash · multiply"));
        const LayerInfo hidden = byName(QStringLiteral("Hidden")), light = byName(QStringLiteral("Light · screen"));
        const bool common = opened && engine.layerCount() == layersBefore && group.isGroup && std::abs(group.opacity - 0.5f) < 0.01f
                            && masked.parent == group.id && clipped.parent == group.id && wash.blend == BlendMode::Multiply
                            && std::abs(wash.opacity - 0.6f) < 0.01f && light.blend == BlendMode::Screen && !hidden.visible && hidden.id
                            && engine.backgroundVisible() && engine.backgroundColor().name() == QColor(250, 246, 236).name() && engine.resolution() == 300;
        // PSD keeps masks, clipping and alpha lock; OpenRaster bakes the first two into the pixels.
        const bool kept = way.bits ? masked.hasMask && masked.maskEnabled && clipped.clipping && clipped.alphaLock
                                   : !masked.hasMask && !clipped.clipping && notes.join(QLatin1Char(' ')).contains(QLatin1String("mask"));
        if (!(common && kept))
            qInfo().noquote() << QStringLiteral("       layers %1 of %2, group %3 at %4, paper %5, %6 dpi, mask %7, clipped %8, alpha lock %9")
                                     .arg(engine.layerCount()).arg(layersBefore).arg(group.isGroup).arg(group.opacity).arg(engine.backgroundColor().name())
                                     .arg(engine.resolution()).arg(masked.hasMask).arg(clipped.clipping).arg(clipped.alphaLock);
        check(QStringLiteral("%1: written and opened again, the same picture to within %2/255, layers, group, modes, opacity, paper and resolution kept%3%4")
                  .arg(QLatin1String(way.what)).arg(off)
                  .arg(way.bits ? QStringLiteral(", and the mask, clipping and alpha lock") : QStringLiteral(", masks and clipping baked in (with a note saying so)"))
                  .arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error),
              written && opened && off <= 2 && common && kept);
    }

    // Every blend mode by its name in each format.
    buildInterchangeSample(engine, true);
    for (const Way& way : ways) {
        const QString path = outPath + QStringLiteral(".modes") + way.suffix;
        QString error;
        const bool written = way.bits ? interchange::exportPsd(engine, path, way.bits, &error, nullptr) : interchange::exportOra(engine, path, &error, nullptr);
        layered::Document doc;
        const bool read = written && (way.bits ? interchange::readPsd(path, &doc, &error) : interchange::readOra(path, &doc, &error));
        int matched = 0, expected = 0;
        QStringList wrong;
        for (const BlendModeInfo& m : blendModes()) {
            if (m.mode == BlendMode::Normal)
                continue;
            ++expected;
            for (const LayerData& d : doc.layers) {
                if (d.info.name == QLatin1String(m.name)) {
                    if (d.info.blend == m.mode)
                        ++matched;
                    else
                        wrong << QString::fromLatin1(m.name);
                }
            }
        }
        check(QStringLiteral("%1: %2 of %3 blend modes come back as themselves%4").arg(QLatin1String(way.what)).arg(matched).arg(expected)
                  .arg(wrong.isEmpty() ? QString() : QStringLiteral(" (not: ") + wrong.join(QStringLiteral(", ")) + QLatin1Char(')')),
              read && matched == expected);
    }

    // A side longer than 30,000 pixels: Photoshop's large document format.
    if (CanvasEngine::maxCanvasDimension() > 30000) {
        engine.newDocument(QSize(30500, 96), QColor(240, 240, 240));
        QPainterPath strip;
        strip.addRect(29000, 20, 1400, 50);
        engine.setSelection(strip, QStringLiteral("Select"));
        engine.fillLayer(engine.activeLayerId(), QColor(200, 30, 30));
        engine.clearSelection();
        const QImage wide = engine.renderImage();
        const QString path = outPath + QStringLiteral(".wide.psb");
        QString error;
        QStringList notes;
        const bool written = interchange::exportPsd(engine, path, 8, &error, &notes);
        QFile file(path);
        const bool large = file.open(QIODevice::ReadOnly) && file.read(6) == QByteArray("8BPS\x00\x02", 6);
        file.close();
        engine.newDocument(QSize(64, 64), Qt::white);
        const bool opened = written && interchange::open(engine, path, &error, nullptr);
        check(QStringLiteral("a canvas 30,500 px wide is written in the large document format, says so, and opens again the same (within %1/255)")
                  .arg(opened ? apart(wide, engine.renderImage()) : 255),
              written && large && opened && apart(wide, engine.renderImage()) <= 2 && !notes.isEmpty());
    } else {
        qInfo().noquote() << "  (this card cannot make a canvas wider than 30,000 px; the large document format is not tried)";
    }

    // A damaged file is refused with a reason, and the open document stays as it was.
    {
        buildInterchangeSample(engine, false);
        const QImage before = engine.renderImage();
        bool refused = true;
        for (const QString& suffix : {QStringLiteral("8.psd"), QStringLiteral("16.psd"), QStringLiteral(".ora")}) {
            QFile whole(outPath + QStringLiteral(".sample") + suffix);
            whole.open(QIODevice::ReadOnly);
            const QByteArray data = whole.readAll();
            whole.close();
            for (const double keep : {0.03, 0.25, 0.97}) {
                const QString cut = outPath + QStringLiteral(".cut") + suffix;
                QFile part(cut);
                part.open(QIODevice::WriteOnly | QIODevice::Truncate);
                part.write(data.left(qsizetype(data.size() * keep)));
                part.close();
                QString error;
                const bool opened = interchange::open(engine, cut, &error, nullptr);
                // A PSD cut off after its layers (in the flattened copy that
                // follows them, which in a 16-bit file is most of it) still
                // holds every layer whole: it may open, as the same picture.
                if (opened && keep > 0.1 && suffix.endsWith(QLatin1String("psd")) && apart(before, engine.renderImage()) <= 2)
                    continue;
                refused = refused && !opened && !error.isEmpty();
            }
        }
        check(QStringLiteral("files cut short are refused with a reason and the canvas is left as it was (a PSD whose layers are all there opens as itself)"),
              refused && apart(before, engine.renderImage()) <= 2);
    }
    engine.newDocument(QSize(64, 64), Qt::white);
    return all;
}

// Opens a layered file as Impasto would and writes what it shows, with
// the layers it found.
int runInterchangeOpen(const QString& path, const QString& outPng)
{
    isolateSettings();
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    QStringList notes;
    if (!interchange::open(engine, path, &error, &notes)) {
        qCritical().noquote() << "could not open:" << error;
        return 1;
    }
    engine.renderImage().save(outPng);
    qInfo().noquote() << QStringLiteral("%1 x %2, %3 dpi, paper %4").arg(engine.canvasSize().width()).arg(engine.canvasSize().height())
                             .arg(engine.resolution()).arg(engine.backgroundVisible() ? engine.backgroundColor().name() : QStringLiteral("none"));
    for (const LayerInfo& l : engine.layers())
        qInfo().noquote() << QStringLiteral("  %1%2 %3 opacity %4 %5%6%7%8").arg(QString(l.depth * 2, QLatin1Char(' ')), l.isGroup ? QStringLiteral("[group]") : QStringLiteral("layer"))
                                 .arg(l.name).arg(l.opacity, 0, 'f', 2).arg(QString::fromLatin1(blendModeInfo(l.blend).name))
                                 .arg(l.visible ? QString() : QStringLiteral(" hidden")).arg(l.clipping ? QStringLiteral(" clipped") : QString())
                                 .arg(l.hasMask ? QStringLiteral(" masked") : QString());
    for (const QString& n : notes)
        qInfo().noquote() << "  note:" << n;
    return 0;
}

// Writes the sample out in every layered format, and the picture as Impasto shows it.
int runInterchangeSample(const QString& dir)
{
    isolateSettings();
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    QDir().mkpath(dir);
    bool ok = true;
    for (bool modes : {false, true}) {
        buildInterchangeSample(engine, modes);
        const QString base = dir + (modes ? QStringLiteral("/modes") : QStringLiteral("/look"));
        engine.renderImage().save(base + QStringLiteral(".png"));
        QStringList notes;
        ok = interchange::exportOra(engine, base + QStringLiteral(".ora"), &error, &notes) && ok;
        ok = interchange::exportPsd(engine, base + QStringLiteral("8.psd"), 8, &error, &notes) && ok;
        ok = interchange::exportPsd(engine, base + QStringLiteral("16.psd"), 16, &error, &notes) && ok;
        qInfo().noquote() << base << (ok ? "written" : error) << notes;
    }
    return ok ? 0 : 1;
}

static void isolateSettings()
{
    static QTemporaryDir dir, data;
#if defined(Q_OS_WIN)
    // XDG paths do not redirect the Windows registry or known folders.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.path());
    QStandardPaths::setTestModeEnabled(true);
#endif
    qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.path().toUtf8());   // figures kept, and anything else kept as data
}

int runSelfTest(const QString& outPath)
{
    isolateSettings();   // the brush tests write settings: never the artist's own
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    qInfo().noquote() << "renderer:" << engine.rendererName();
    qInfo().noquote() << QStringLiteral("canvas limits on this card: %1 px a side, %2 megapixels (%3 GB of graphics memory); a 4K canvas has room for about %4 layers")
                             .arg(CanvasEngine::maxCanvasDimension()).arg(CanvasEngine::maxCanvasPixels() / 1'000'000)
                             .arg(double(CanvasEngine::graphicsMemoryBytes()) / double(1 << 30), 0, 'f', 1)
                             .arg(CanvasEngine::layerEstimate(QSize(3840, 2160)));

    // IMPASTO_ONLY=gaps (or animation) runs just those checks, while working on them.
    if (qEnvironmentVariable("IMPASTO_ONLY") == QLatin1String("gaps"))
        return runSmallGapTests(engine, outPath) ? 0 : 2;
    if (qEnvironmentVariable("IMPASTO_ONLY") == QLatin1String("animation"))
        return runAnimationTests(engine, outPath) ? 0 : 2;
    if (qEnvironmentVariable("IMPASTO_ONLY") == QLatin1String("model"))
        return runModelTests(outPath) ? 0 : 2;

    const BrushLibrary& lib = BrushLibrary::instance();
    engine.newDocument(QSize(1800, 1300), QColor(250, 247, 240));

    QElapsedTimer timer;
    timer.start();
    const QStringList ids = {QStringLiteral("pencil-6b"), QStringLiteral("charcoal-stick"),
                             QStringLiteral("studio-pen"), QStringLiteral("brush-pen"),
                             QStringLiteral("marker"), QStringLiteral("calligraphy"),
                             QStringLiteral("round-brush"), QStringLiteral("oil-paint"),
                             QStringLiteral("dry-brush"), QStringLiteral("airbrush-soft"),
                             QStringLiteral("spray-paint"), QStringLiteral("confetti")};
    const QList<QColor> colors = {QColor(40, 40, 46), QColor(30, 30, 30), QColor(20, 20, 24),
                                  QColor(16, 40, 96), QColor(240, 150, 30), QColor(120, 20, 60),
                                  QColor(200, 60, 50), QColor(30, 120, 110), QColor(90, 60, 30),
                                  QColor(60, 110, 220), QColor(40, 150, 70), QColor(230, 70, 140)};
    for (int i = 0; i < ids.size(); ++i) {
        const BrushPreset* p = lib.find(ids[i]);
        if (!p) {
            qCritical() << "missing preset" << ids[i];
            return 1;
        }
        const float y = 70.0f + float(i) * 95.0f;
        wave(engine, StrokeTool::Brush, *p, p->size * 1.6f, p->opacity, colors[i], QPointF(80, y), QPointF(1150, y),
             22.0f);
    }
    const qint64 strokeMs = timer.elapsed();

    // Layer 2: blend modes, clipping and alpha lock.
    const int base = engine.addLayer();
    const BrushPreset& round = *lib.find(QStringLiteral("round-brush"));
    wave(engine, StrokeTool::Brush, round, 260, 1.0f, QColor(250, 200, 40), QPointF(1300, 200), QPointF(1700, 200), 0);
    engine.setLayerBlend(base, BlendMode::Multiply);
    const int clipped = engine.addLayer();
    engine.setLayerClipping(clipped, true);
    wave(engine, StrokeTool::Brush, *lib.find(QStringLiteral("airbrush-medium")), 220, 1.0f, QColor(220, 30, 90),
         QPointF(1250, 260), QPointF(1750, 140), 0);

    // Layer 4: eraser, smudge, undo/redo.
    const int top = engine.addLayer();
    wave(engine, StrokeTool::Brush, round, 150, 1.0f, QColor(40, 90, 200), QPointF(1300, 560), QPointF(1700, 560), 0);
    wave(engine, StrokeTool::Brush, round, 150, 1.0f, QColor(240, 80, 40), QPointF(1300, 700), QPointF(1700, 700), 0);
    wave(engine, StrokeTool::Eraser, *lib.find(QStringLiteral("studio-pen")), 30, 1.0f, Qt::black,
         QPointF(1280, 560), QPointF(1720, 560), 30);
    wave(engine, StrokeTool::Smudge, round, 110, 0.9f, Qt::black, QPointF(1400, 520), QPointF(1500, 760), 0);
    wave(engine, StrokeTool::Smudge, round, 110, 0.9f, Qt::black, QPointF(1640, 760), QPointF(1560, 520), 0);

    wave(engine, StrokeTool::Brush, round, 90, 1.0f, QColor(0, 160, 60), QPointF(1300, 950), QPointF(1700, 950), 40);
    const QImage withStroke = engine.renderImage();
    engine.undo();
    const QImage undone = engine.renderImage();
    engine.redo();
    const QImage redone = engine.renderImage();
    const bool undoOk = withStroke != undone && withStroke == redone;

    // Alpha lock: repaint the green stroke without spilling outside it.
    engine.setLayerAlphaLock(top, true);
    wave(engine, StrokeTool::Brush, round, 200, 1.0f, QColor(150, 40, 200), QPointF(1500, 900), QPointF(1700, 1000), 0);
    engine.setLayerAlphaLock(top, false);

    // Transform and adjustment round-trips on a throwaway layer.
    const int fx = engine.addLayer();
    wave(engine, StrokeTool::Brush, *lib.find(QStringLiteral("technical-pen")), 14, 1.0f, QColor(20, 20, 20),
         QPointF(1300, 1120), QPointF(1500, 1120), 12);
    bool editOk = engine.beginTransform();
    QTransform t;
    t.translate(1400, 1120);
    t.rotate(-18);
    t.scale(1.5, 1.5);
    t.translate(-1400, -1120);
    engine.setTransform(t);
    engine.commitEdit();
    editOk = engine.beginAdjustment(Adjustment::GaussianBlur) && editOk;
    AdjustParams blur;
    blur.radius = 3.0f;
    engine.setAdjustment(blur);
    engine.commitEdit();
    Q_UNUSED(fx);

    // Fill, merge down, flip and layer removal all have to undo cleanly.
    const QImage beforeOps = engine.renderImage();
    const int undoBase = engine.history().undoCount();
    const int layersBase = engine.layerCount();
    const int fillId = engine.addLayer();
    engine.fillLayer(fillId, QColor(10, 200, 30));
    const QColor filled = engine.pickColor(QPoint(900, 650));
    const bool fillOk = std::abs(filled.red() - 10) <= 1 && std::abs(filled.green() - 200) <= 1 && std::abs(filled.blue() - 30) <= 1;
    engine.setLayerOpacity(fillId, 0.5f);
    engine.mergeDown(fillId);
    engine.flipCanvas(true);
    engine.flipLayer(engine.activeLayerId(), false);
    engine.duplicateLayer(engine.activeLayerId());
    engine.removeLayer(engine.activeLayerId());
    const bool changed = engine.renderImage() != beforeOps;
    while (engine.history().undoCount() > undoBase)
        engine.undo();
    const bool restored = engine.renderImage() == beforeOps && engine.layerCount() == layersBase;
    const bool opsOk = fillOk && changed && restored;
    qInfo().noquote() << QStringLiteral("layer operations: %1 (fill %2, changed %3, undo restored %4)")
                             .arg(opsOk ? "ok" : "FAILED")
                             .arg(fillOk ? "ok" : filled.name())
                             .arg(changed ? "yes" : "no")
                             .arg(restored ? "yes" : "no");

    const QImage img = engine.renderImage();
    if (!img.save(outPath)) {
        qCritical().noquote() << "could not write" << outPath;
        return 1;
    }

    // Native format round trip: what comes back must composite identically.
    const QString docPath = outPath + QStringLiteral(".imp");
    QString ioError;
    QElapsedTimer ioTimer;
    ioTimer.start();
    engine.setResolution(254);   // the size it is meant to be on paper is part of the document
    bool ioOk = docio::save(engine, docPath, &ioError);
    const qint64 saveMs = ioTimer.restart();
    const int layersBefore = engine.layerCount();
    engine.setResolution(0);
    ioOk = ioOk && docio::load(engine, docPath, &ioError);
    const qint64 loadMs = ioTimer.elapsed();
    ioOk = ioOk && engine.layerCount() == layersBefore && engine.renderImage() == img && engine.resolution() == 254;
    ioOk = ioOk && docio::exportImage(engine, outPath + QStringLiteral(".jpg"), &ioError);
    // ...and of what is exported: 254 dots per inch is 10,000 per metre.
    ioOk = ioOk && docio::exportImage(engine, outPath + QStringLiteral(".export.png"), &ioError)
           && QImage(outPath + QStringLiteral(".export.png")).dotsPerMeterX() == 10000;
    qInfo().noquote() << QStringLiteral("document i/o: %1 (save %2 ms, load %3 ms, %4 KB) %5")
                             .arg(ioOk ? "ok" : "FAILED")
                             .arg(saveMs)
                             .arg(loadMs)
                             .arg(QFileInfo(docPath).size() / 1024)
                             .arg(ioError);
    qInfo().noquote() << QStringLiteral("selftest: %1 layers, %2 undo steps, history %3 MB, 12 strokes in %4 ms, undo/redo %5, edits %6")
                             .arg(engine.layerCount())
                             .arg(engine.history().undoCount())
                             .arg(engine.history().bytes() / (1024 * 1024))
                             .arg(strokeMs)
                             .arg(undoOk ? "ok" : "FAILED")
                             .arg(editOk ? "ok" : "FAILED");
    qInfo().noquote() << "selection, fill, clipboard:";
    const bool editingOk = runEditingTests(engine);
    qInfo().noquote() << "layer masks and groups:";
    const bool structureOk = runMaskAndGroupTests(engine, outPath + QStringLiteral(".groups.imp"));
    qInfo().noquote() << "reference fills, automatic and soft selections:";
    const bool floodOk = runFloodAndSelectionTests(engine, outPath + QStringLiteral(".reference.imp"));
    qInfo().noquote() << "smoothing and line weight:";
    const bool smoothingOk = runSmoothingTests();
    qInfo().noquote() << "shape recognition:";
    const int shapeFailures = runShapeTests();
    qInfo().noquote() << "symmetry:";
    const bool guidesOk = runGuideTests(engine, outPath + QStringLiteral(".guides.imp"));
    qInfo().noquote() << "warp:";
    const bool warpOk = runWarpTests(engine);
    qInfo().noquote() << "liquify:";
    const bool liquifyOk = runLiquifyTests(engine);
    qInfo().noquote() << "gradients:";
    const bool gradientOk = runGradientTests(engine);
    qInfo().noquote() << "canvas size:";
    const bool sizeOk = runCanvasSizeTests(engine);
    qInfo().noquote() << "adjustments and filters:";
    const bool adjustOk = runAdjustmentTests(engine);
    qInfo().noquote() << "perspective guides:";
    const bool perspectiveOk = runPerspectiveTests();
    qInfo().noquote() << "your own brushes:";
    const bool brushesOk = runBrushLibraryTests(engine);
    qInfo().noquote() << "colours and palettes:";
    const bool coloursOk = runColourTests();
    qInfo().noquote() << "histogram:";
    const bool histogramOk = runHistogramTests(engine);
    qInfo().noquote() << "brush blend modes and wet brushes:";
    const bool blendOk = runBrushBlendTests(engine);
    qInfo().noquote() << "text that stays text:";
    const bool textOk = runTextTests(engine, outPath + QStringLiteral(".text.imp"));
    qInfo().noquote() << "selection in the file, gradients, group masks and references, text spacing, palettes:";
    const bool gapsOk = runSmallGapTests(engine, outPath);
    qInfo().noquote() << "animation:";
    const bool animationOk = runAnimationTests(engine, outPath);
    qInfo().noquote() << "3D reference:";
    const bool modelOk = runModelTests(outPath);
    qInfo().noquote() << "layered files for other programs:";
    const bool interchangeOk = runInterchangeTests(engine, outPath);
    qInfo().noquote() << "layers stored only where there is paint:";
    const bool sparseOk = runSparseTests(engine, outPath + QStringLiteral(".sparse.imp"));
    return undoOk && editOk && ioOk && opsOk && editingOk && structureOk && floodOk && smoothingOk && guidesOk && warpOk && liquifyOk && gradientOk && sizeOk && adjustOk && perspectiveOk && brushesOk && coloursOk && histogramOk && blendOk && textOk && sparseOk && interchangeOk && gapsOk && animationOk && modelOk && shapeFailures == 0 ? 0 : 2;
}

int runBrushSheet(const QString& outPath)
{
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    const auto& presets = BrushLibrary::instance().presets();
    const QSize cell(520, 110);
    const int columns = 3;
    const int rows = (int(presets.size()) + columns - 1) / columns;
    QImage sheet(cell.width() * columns, cell.height() * rows, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor(44, 44, 46));
    QPainter p(&sheet);
    p.setPen(QColor(200, 200, 205));
    int i = 0;
    for (const BrushPreset& preset : presets) {
        const QPoint at((i % columns) * cell.width(), (i / columns) * cell.height());
        const QImage preview = engine.brushPreview(preset, cell, Qt::white, 1.0f);
        p.drawImage(at, preview);
        p.drawText(at + QPoint(10, 18), preset.name);
        ++i;
    }
    p.end();
    return sheet.save(outPath) ? 0 : 1;
}

int runBrushShowcase(const QString& outPath, const QStringList& brushIds)
{
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    const BrushLibrary& lib = BrushLibrary::instance();
    QVector<const BrushPreset*> brushes;
    for (const QString& id : brushIds) {
        if (const BrushPreset* p = lib.find(id))
            brushes.push_back(p);
        else
            qWarning().noquote() << "unknown brush:" << id;
    }
    if (brushes.isEmpty()) {
        for (const BrushPreset& p : lib.presets())
            brushes.push_back(&p);
    }
    const int rowH = 300, width = 1900;
    engine.newDocument(QSize(width, rowH * int(brushes.size())), QColor(250, 247, 240));
    const QList<QColor> inks = {QColor(28, 28, 34), QColor(24, 70, 150), QColor(150, 40, 50), QColor(30, 110, 80)};

    // One pen stroke through the given points, with pressure as a function of progress.
    auto stroke = [&](const BrushPreset& b, float size, const QColor& c, const std::vector<QPointF>& pts,
                      const std::function<float(float)>& pressure, float tilt = 0.0f) {
        for (size_t i = 0; i < pts.size(); ++i) {
            const float t = float(i) / float(pts.size() - 1);
            StrokeSample s;
            s.pos = pts[i];
            s.pressure = pressure(t);
            s.tiltX = tilt;
            s.time = double(i) * 0.006;
            if (i == 0)
                engine.beginStroke(StrokeTool::Brush, b, size, b.opacity, c, s);
            else
                engine.strokeTo(s);
        }
        engine.endStroke();
    };
    for (int r = 0; r < brushes.size(); ++r) {
        const BrushPreset& b = *brushes[r];
        const float y = float(r * rowH);
        const QColor ink = b.category == QLatin1String("Painting") ? inks[1]
                           : b.id == QLatin1String("foliage")      ? inks[3]
                                                                   : inks[0];
        const float big = std::clamp(b.size * 1.5f, b.minSize, std::min(b.maxSize, 150.0f));

        // 1. A flowing line that swells and thins.
        std::vector<QPointF> wavy;
        for (int i = 0; i <= 140; ++i) {
            const float t = float(i) / 140.0f;
            wavy.emplace_back(60 + t * 640, y + 150 + std::sin(t * 7.5f) * 62);
        }
        stroke(b, big, ink, wavy, [](float t) { return 0.06f + 0.94f * std::pow(std::sin(t * 3.14159f), 0.8f); });

        // 2. Quick hatching at default size, light then firm.
        for (int k = 0; k < 9; ++k) {
            std::vector<QPointF> hatch;
            for (int i = 0; i <= 24; ++i) {
                const float t = float(i) / 24.0f;
                hatch.emplace_back(770 + k * 26 + t * 110, y + 235 - t * 170);
            }
            const float firm = 0.25f + 0.08f * float(k);
            stroke(b, b.size, ink, hatch, [firm](float t) { return firm * (0.35f + 0.65f * std::sin(t * 3.14159f)); });
        }

        // 3. Two fat strokes crossing, then a third over both: shows build-up and glazing.
        auto bar = [&](QPointF a, QPointF c, const QColor& col) {
            std::vector<QPointF> pts;
            for (int i = 0; i <= 60; ++i)
                pts.push_back(a + (c - a) * (float(i) / 60.0f));
            stroke(b, std::min(big * 1.6f, b.maxSize), col, pts, [](float t) { return 0.55f + 0.45f * std::sin(t * 3.14159f); });
        };
        bar(QPointF(1180, y + 90), QPointF(1500, y + 215), ink);
        bar(QPointF(1180, y + 215), QPointF(1500, y + 90), ink);
        bar(QPointF(1200, y + 150), QPointF(1480, y + 150), b.category == QLatin1String("Painting") ? inks[2] : ink);

        // 4. A slow, tilted shading pass for brushes that respond to tilt.
        std::vector<QPointF> shade;
        for (int i = 0; i <= 90; ++i) {
            const float t = float(i) / 90.0f;
            shade.emplace_back(1580 + t * 260, y + 110 + std::sin(t * 20.0f) * 70 + t * 40);
        }
        stroke(b, b.size, ink, shade, [](float) { return 0.5f; }, 45.0f);
    }

    QImage out = engine.renderImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter p(&out);
    p.setPen(QColor(120, 110, 100));
    QFont f = p.font();
    f.setPixelSize(20);
    f.setBold(true);
    p.setFont(f);
    for (int r = 0; r < brushes.size(); ++r) {
        p.drawText(QPoint(24, r * rowH + 34), QStringLiteral("%1  ·  %2").arg(brushes[r]->name, brushes[r]->category));
        p.drawLine(0, (r + 1) * rowH - 1, width, (r + 1) * rowH - 1);
    }
    p.end();
    return out.save(outPath) ? 0 : 1;
}

// Times what one pen sample costs end to end: dabs, stroke merge, recomposite of
// a ten-layer stack and the mipmap refresh used while zoomed out.
// How long a snapped shape takes to be drawn again as it is resized: the
// outline worked out, the brush run along it, and the frame composited.
static void benchShapeResize(CanvasEngine& engine, const QSize& size)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    struct Case {
        const char* brush;
        float size;
    };
    const Case cases[] = {{"technical-pen", 3}, {"pencil-hb", 4.5f}, {"studio-pen", 8}, {"brush-pen", 30}, {"oil-paint", 80}};
    for (const Case& c : cases) {
        const BrushPreset& preset = *lib.find(QString::fromLatin1(c.brush));
        StrokeSample first;
        first.pos = QPointF(size.width() / 2.0, size.height() / 2.0);
        first.pressure = 0.8f;
        engine.beginStroke(StrokeTool::Brush, preset, c.size, 1.0f, QColor(30, 60, 160), first);
        QuickShape circle;
        circle.kind = QuickShape::Kind::Ellipse;
        circle.center = first.pos;
        double outlineMs = 0.0, strokeMs = 0.0, frameMs = 0.0, worst = 0.0;
        size_t points = 0;
        constexpr int steps = 40;
        engine.finish();
        for (int i = 0; i < steps; ++i) {
            // Growing from a quarter of the canvas height to most of it.
            circle.rx = circle.ry = size.height() * (0.12 + 0.3 * i / double(steps - 1));
            QElapsedTimer t;
            t.start();
            std::vector<PathPoint> path = circle.path(std::clamp(double(c.size) * 0.2, 1.0, 5.0));
            for (PathPoint& p : path)
                p.pressure = 0.8f;
            const double a = t.nsecsElapsed() / 1e6;
            engine.setStrokePath(path, true);
            engine.finish();
            const double b = t.nsecsElapsed() / 1e6;
            engine.prepareFrame(true);
            engine.finish();
            const double done = t.nsecsElapsed() / 1e6;
            outlineMs += a;
            strokeMs += b - a;
            frameMs += done - b;
            worst = std::max(worst, done);
            points = path.size();
        }
        engine.endStroke();
        engine.undo(true);
        qInfo().noquote() << QStringLiteral("  resizing a circle, %1 @ %2 px: %3 ms a step (worst %4): outline %5, brush along it %6, frame %7; %8 outline points at its largest")
                                 .arg(QLatin1String(c.brush), -11).arg(c.size, 3).arg((outlineMs + strokeMs + frameMs) / steps, 0, 'f', 2)
                                 .arg(worst, 0, 'f', 2).arg(outlineMs / steps, 0, 'f', 2).arg(strokeMs / steps, 0, 'f', 2)
                                 .arg(frameMs / steps, 0, 'f', 2).arg(points);
    }
}

int runBenchmark(const QSize& only)
{
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    qInfo().noquote() << "renderer:" << engine.rendererName();
    const BrushLibrary& lib = BrushLibrary::instance();
    struct Case {
        const char* brush;
        float size;
    };
    const Case cases[] = {{"studio-pen", 12}, {"pencil-6b", 20}, {"round-brush", 120}, {"airbrush-soft", 600}, {"oil-paint", 200}};
    // One size given on the command line is tried on its own, with the two
    // layers a canvas near the limit has room for.
    const std::vector<QSize> sizes = only.isEmpty() ? std::vector<QSize>{QSize(2560, 1440), QSize(3840, 2160), QSize(7680, 4320)} : std::vector<QSize>{only};
    const int extraLayers = only.isEmpty() ? 9 : 1;
    for (const QSize& size : sizes) {
        if (qint64(size.width()) * size.height() > CanvasEngine::maxCanvasPixels() || std::max(size.width(), size.height()) > CanvasEngine::maxCanvasDimension()) {
            qCritical().noquote() << QStringLiteral("%1 x %2 is more than this card holds").arg(size.width()).arg(size.height());
            return 1;
        }
        engine.newDocument(size, Qt::white);
        for (int i = 0; i < extraLayers; ++i) {
            const int id = engine.addLayer();
            if (i % 3 == 1)
                engine.setLayerBlend(id, BlendMode::Multiply);
        }
        qInfo().noquote() << QStringLiteral("canvas %1 x %2, %3 megapixels, %4 layers").arg(size.width()).arg(size.height())
                                 .arg(qint64(size.width()) * size.height() / 1'000'000).arg(extraLayers + 1);
        for (const Case& c : cases) {
            const BrushPreset& preset = *lib.find(QString::fromLatin1(c.brush));
            constexpr int n = 240;
            QElapsedTimer t;
            qint64 worstNs = 0;
            engine.finish();
            t.start();
            for (int i = 0; i <= n; ++i) {
                const float u = float(i) / n;
                StrokeSample s;
                s.pos = QPointF(size.width() * (0.1 + 0.8 * u), size.height() * (0.5 + 0.3 * std::sin(u * 12.0f)));
                s.pressure = 0.3f + 0.7f * std::abs(std::sin(u * 9.0f));
                s.time = i * 0.005;
                const qint64 before = t.nsecsElapsed();
                if (i == 0)
                    engine.beginStroke(StrokeTool::Brush, preset, c.size, 1.0f, QColor(30, 60, 160), s);
                else
                    engine.strokeTo(s);
                engine.prepareFrame(true);
                engine.finish();
                worstNs = std::max(worstNs, t.nsecsElapsed() - before);
            }
            const double avgMs = t.nsecsElapsed() / 1e6 / (n + 1);
            const qint64 before = t.nsecsElapsed();
            engine.endStroke();
            engine.finish();
            const double commitMs = (t.nsecsElapsed() - before) / 1e6;
            qInfo().noquote() << QStringLiteral("  %1 @ %2 px: %3 ms per sample (worst %4 ms), commit %5 ms")
                                     .arg(QLatin1String(c.brush), -14)
                                     .arg(c.size, 4)
                                     .arg(avgMs, 0, 'f', 2)
                                     .arg(worstNs / 1e6, 0, 'f', 2)
                                     .arg(commitMs, 0, 'f', 2);
        }
        benchShapeResize(engine, size);
        qInfo().noquote() << QStringLiteral("  undo history: %1 MB over %2 steps; graphics memory in use: %3 MB")
                                 .arg(engine.history().bytes() / (1024 * 1024))
                                 .arg(engine.history().undoCount())
                                 .arg(engine.memoryInUse() / (1024 * 1024));
        // A stroke on a layer that already has paint all over half of it: what starting one costs then.
        if (!only.isEmpty()) {
            const int half = engine.addLayer();
            QPainterPath left;
            left.addRect(0, 0, size.width() / 2, size.height());
            engine.setSelection(left, QStringLiteral("Select"));
            QElapsedTimer t;
            t.start();
            engine.fillLayer(half, QColor(200, 180, 150));
            engine.finish();
            const double fillMs = t.nsecsElapsed() / 1e6;
            engine.clearSelection();
            const BrushPreset& pen = *lib.find(QStringLiteral("studio-pen"));
            double firstMs = 0.0, laterMs = 0.0;
            for (int k = 0; k < 4; ++k) {
                engine.setActiveLayer(k % 2 ? half : engine.layers().value(0).id);
                engine.setActiveLayer(half);
                StrokeSample a, b;
                a.pos = QPointF(size.width() * 0.2, size.height() * (0.3 + 0.1 * k));
                b.pos = a.pos + QPointF(400, 60);
                b.time = 0.1;
                t.restart();
                engine.beginStroke(StrokeTool::Brush, pen, 12, 1.0f, QColor(20, 20, 20), a);
                engine.prepareFrame(true);
                engine.finish();
                (k == 0 ? firstMs : laterMs) = t.nsecsElapsed() / 1e6;
                engine.strokeTo(b);
                engine.endStroke();
            }
            qInfo().noquote() << QStringLiteral("  half the canvas filled in %1 ms; a stroke on that layer starts in %2 ms the first time, %3 ms after that; in use: %4 MB")
                                     .arg(fillMs, 0, 'f', 0).arg(firstMs, 0, 'f', 1).arg(laterMs, 0, 'f', 1).arg(engine.memoryInUse() / (1024 * 1024));
            engine.removeLayer(half);
            engine.history().clear();
        }
        if (!only.isEmpty() && qint64(size.width()) * size.height() > 100'000'000) {
            qInfo().noquote() << "  (whole-canvas fills, selections and liquify are not timed at this size)";
            continue;
        }

        // Fills and selections work on the whole canvas at once: time the
        // worst case, a region that is nearly all of it.
        const int ink = engine.addLayer();
        const int flats = engine.addLayer();
        engine.setActiveLayer(ink);
        QPainterPath ring, inner;
        ring.addEllipse(QRectF(size.width() * 0.05, size.height() * 0.05, size.width() * 0.9, size.height() * 0.9));
        inner.addEllipse(QRectF(size.width() * 0.05 + 8, size.height() * 0.05 + 8, size.width() * 0.9 - 16, size.height() * 0.9 - 16));
        engine.setSelection(ring.subtracted(inner), QStringLiteral("Select"));
        engine.fillLayer(ink, Qt::black);
        engine.clearSelection();
        engine.setActiveLayer(flats);
        engine.setReferenceLayer(ink);
        auto timed = [&](const std::function<void()>& run) {
            engine.finish();
            QElapsedTimer t;
            t.start();
            run();
            engine.prepareFrame(true);
            engine.finish();
            return t.nsecsElapsed() / 1e6;
        };
        const QPoint middle(size.width() / 2, size.height() / 2);
        const double beginMs = timed([&] { engine.beginFlood(FloodKind::Fill, middle, 0.12f, QColor(200, 40, 40)); });
        const double slideMs = timed([&] { engine.setFloodTolerance(0.3f); });
        const double endMs = timed([&] { engine.endFlood(); });
        const double wandMs = timed([&] { engine.floodSelect(middle, 0.12f, SelectOp::Add); });
        const double featherMs = timed([&] { engine.featherSelection(40.0f); });
        const double lassoMs = timed([&] { engine.selectPath(ring, SelectOp::Subtract); });
        engine.clearSelection();
        engine.setActiveLayer(ink);
        const double liquifyStart = timed([&] { engine.beginLiquify(); });
        double liquifyTouch = 0.0, liquifyWorst = 0.0;
        for (int i = 0; i < 60; ++i) {
            const QPointF a(size.width() * (0.2 + 0.01 * i), size.height() * 0.5), b(size.width() * (0.21 + 0.01 * i), size.height() * 0.52);
            const double ms = timed([&] { engine.liquify(LiquifyMode::Push, a, b, 300.0f, 0.8f); });
            liquifyTouch += ms / 60.0;
            liquifyWorst = std::max(liquifyWorst, ms);
        }
        const double liquifyEnd = timed([&] { engine.endLiquify(true); });
        engine.undo(true);
        engine.setActiveLayer(flats);
        qInfo().noquote() << QStringLiteral("  liquify with a 600 px brush: %1 ms to start, %2 ms per touch (worst %3), %4 ms to apply")
                                 .arg(liquifyStart, 0, 'f', 1).arg(liquifyTouch, 0, 'f', 2).arg(liquifyWorst, 0, 'f', 2).arg(liquifyEnd, 0, 'f', 1);
        // And at the largest brush there is: 5000 px across.
        engine.setActiveLayer(ink);
        engine.beginLiquify();
        double hugeTouch = 0.0, hugeWorst = 0.0;
        for (int i = 0; i < 30; ++i) {
            const QPointF a(size.width() * (0.3 + 0.01 * i), size.height() * 0.5), b(size.width() * (0.31 + 0.01 * i), size.height() * 0.51);
            const double ms = timed([&] { engine.liquify(LiquifyMode::Push, a, b, 2500.0f, 0.8f); });
            hugeTouch += ms / 30.0;
            hugeWorst = std::max(hugeWorst, ms);
        }
        engine.endLiquify(false);
        engine.setActiveLayer(flats);
        qInfo().noquote() << QStringLiteral("  liquify with a 5000 px brush: %1 ms per touch (worst %2)").arg(hugeTouch, 0, 'f', 2).arg(hugeWorst, 0, 'f', 2);
        qInfo().noquote() << QStringLiteral("  reference fill of most of the canvas: %1 ms to start, %2 ms per threshold step, %3 ms to commit")
                                 .arg(beginMs, 0, 'f', 0).arg(slideMs, 0, 'f', 0).arg(endMs, 0, 'f', 0);
        qInfo().noquote() << QStringLiteral("  automatic selection %1 ms, feather 40 px %2 ms, cutting a shape out %3 ms")
                                 .arg(wandMs, 0, 'f', 0).arg(featherMs, 0, 'f', 0).arg(lassoMs, 0, 'f', 0);
    }
    return 0;
}

int runDeviceReport()
{
    // Device discovery happens when the platform plugin starts; a window is not needed.
    QApplication::processEvents();
    int pens = 0;
    for (const QInputDevice* dev : QInputDevice::devices()) {
        const auto* pd = qobject_cast<const QPointingDevice*>(dev);
        if (!pd)
            continue;
        const auto caps = dev->capabilities();
        const bool pressure = caps.testFlag(QInputDevice::Capability::Pressure);
        const bool tilt = caps.testFlag(QInputDevice::Capability::XTilt);
        const bool stylus = dev->type() == QInputDevice::DeviceType::Stylus;
        pens += stylus && pressure;
        qInfo().noquote() << QStringLiteral("%1  type=%2  pressure=%3  tilt=%4  eraser=%5")
                                 .arg(dev->name(), -44)
                                 .arg(stylus ? "stylus" : dev->type() == QInputDevice::DeviceType::Mouse ? "mouse" : "other", -6)
                                 .arg(pressure ? "yes" : "no", -3)
                                 .arg(tilt ? "yes" : "no", -3)
                                 .arg(pd->pointerType() == QPointingDevice::PointerType::Eraser ? "yes" : "no");
    }
    qInfo().noquote() << (pens ? QStringLiteral("pen input: %1 pressure-sensitive stylus device(s) found").arg(pens)
                               : QStringLiteral("pen input: no pressure-sensitive stylus found; a tablet would act as a plain mouse"));
    return 0;
}

// Test runs must neither read nor write the artist's own settings and brush
// tweaks, so they get a throwaway configuration directory.
// If the run is killed by a watchdog (`timeout`), say where it was stuck.
static void dumpStackAndExit(int)
{
#if defined(Q_OS_LINUX)
    void* frames[64];
    const int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    _exit(124);
#else
    std::_Exit(124);
#endif
}

// The 3D reference through the window: the figure on show, turned, posed by a joint, and placed.
static bool runModelUi(MainWindow& window, CanvasEngine& engine, const std::function<void(QWidget*, const QPointF&)>& penTap,
                       const std::function<void(QEvent::Type, const QPointF&, qreal, Qt::MouseButton, Qt::MouseButtons)>& penWindow,
                       const std::function<void(const QString&)>& capture, const std::function<void()>& settle)
{
    QStringList log;
    bool ok = true;
    auto check = [&](const QString& what, bool passed) {
        ok = ok && passed;
        log << QStringLiteral("%1 %2").arg(passed ? "ok    " : "FAILED", what);
    };
    auto drag = [&](QWidget* w, const QPointF& from, const QPointF& to) {
        const QPointF a = w->mapTo(&window, from.toPoint()), b = w->mapTo(&window, to.toPoint());
        penWindow(QEvent::TabletMove, a, 0.0, Qt::NoButton, Qt::NoButton);
        penWindow(QEvent::TabletPress, a, 0.5, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 12; ++i)
            penWindow(QEvent::TabletMove, a + (b - a) * (i / 12.0), 0.6, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, b, 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
    };
    engine.newDocument(QSize(1600, 1200), Qt::white);
    window.showForScreenshot(QStringLiteral("main"));
    settle();
    auto* button = window.findChild<QWidget*>(QStringLiteral("model-button"));
    penTap(button, QPointF(button->width() / 2.0, button->height() / 2.0));
    settle();
    auto* pop = window.findChild<Popover*>(QStringLiteral("model-popover"));
    ModelPanel* panel = window.modelPanel();
    ModelView* view = panel ? panel->view() : nullptr;
    if (!pop || !pop->isVisible() || !view || !view->model()) {
        check(QStringLiteral("the cube button opens the 3D panel with the figure in it"), false);
        for (const QString& line : log)
            qInfo().noquote() << "3D reference:" << line;
        return false;
    }
    panel->showFigure();
    view->fit();
    settle();
    const QImage first = view->grabFramebuffer();
    const QColor middle = first.pixelColor(first.width() / 2, int(first.height() * 0.45));
    const bool drawn = view->model()->figure && std::abs(middle.red() - 54) + std::abs(middle.green() - 54) > 30;
    check(QStringLiteral("the cube button opens the 3D panel, the figure on show in it (%1 in the middle of the view)").arg(middle.name()), drawn);
    capture(QStringLiteral("model-figure"));

    // Turned with a drag.
    const double yawBefore = view->yaw();
    drag(view, QPointF(view->width() * 0.5, view->height() * 0.8), QPointF(view->width() * 0.5 + 120, view->height() * 0.8));
    check(QStringLiteral("a drag across the view turns the figure round (%1° to %2°)").arg(yawBefore, 0, 'f', 0).arg(view->yaw(), 0, 'f', 0),
          std::abs(view->yaw() - yawBefore) > 30.0);

    // Posed by its left elbow, dragged out.
    auto* modes = window.findChild<SegmentBar*>(QStringLiteral("model-modes"));
    penTap(modes, modes->segmentRect(1).center());
    const int elbowHandle = 5;   // after hips, chest, neck, head and the top of the head
    Model3D* figure = view->model();
    auto nodeId = [&](const char* name) {
        for (size_t i = 0; i < figure->nodes.size(); ++i)
            if (figure->nodes[i].name == QLatin1String(name))
                return int(i);
        return -1;
    };
    const QVector3D handBefore = figure->jointPosition(nodeId("hand.L"), figure->worldTransforms());
    const QPointF elbowAt = view->handlePositions().value(elbowHandle);
    const QPointF shoulderAt = view->project(figure->jointPosition(nodeId("upperarm.L"), figure->worldTransforms()));
    // Round the shoulder, away from the body.
    const QPointF away = shoulderAt + (elbowAt - shoulderAt) * 0.2 + QPointF((elbowAt.x() > shoulderAt.x() ? 1 : -1) * 70.0, -20.0);
    drag(view, elbowAt, away);
    const QVector3D handAfter = figure->jointPosition(nodeId("hand.L"), figure->worldTransforms());
    check(QStringLiteral("in Pose, the elbow dragged out lifts the arm: the hand moves %1 m").arg((handAfter - handBefore).length(), 0, 'f', 2),
          view->mode() == ModelView::Mode::Pose && (handAfter - handBefore).length() > 0.15f);
    capture(QStringLiteral("model-pose"));

    // The light moved with a drag in Light, and made hard.
    penTap(modes, modes->segmentRect(2).center());
    const QVector3D lightBefore = view->lightDirection();
    drag(view, QPointF(view->width() * 0.3, view->height() * 0.5), QPointF(view->width() * 0.6, view->height() * 0.4));
    auto* hard = window.findChild<QWidget*>(QStringLiteral("model-hard"));
    penTap(hard, QPointF(hard->width() / 2.0, hard->height() / 2.0));
    check(QStringLiteral("in Light, a drag moves the light (it turns %1°); Hard light makes it two tones")
              .arg(std::acos(std::clamp(double(QVector3D::dotProduct(lightBefore, view->lightDirection())), -1.0, 1.0)) * 180.0 / std::numbers::pi, 0, 'f', 0),
          QVector3D::dotProduct(lightBefore, view->lightDirection()) < 0.95f && view->hardLight());
    capture(QStringLiteral("model-light"));
    penTap(hard, QPointF(hard->width() / 2.0, hard->height() / 2.0));
    penTap(modes, modes->segmentRect(1).center());

    // A named pose, then placed on the canvas as a layer.
    IconButton* sit = nullptr;
    for (IconButton* b : window.findChildren<IconButton*>(QStringLiteral("model-pose"))) {
        if (b->property("pose").toString() == QLatin1String("Sit"))
            sit = b;
    }
    if (sit)
        penTap(sit, QPointF(sit->width() / 2.0, sit->height() / 2.0));
    const bool seated = figure->nodes[0].offset.y() < -0.3f;
    const int layersBefore = engine.layerCount();
    auto* place = window.findChild<QWidget*>(QStringLiteral("model-place"));
    penTap(place, QPointF(place->width() / 2.0, place->height() / 2.0));
    settle();
    const LayerInfo placed = engine.layerInfo(engine.activeLayerId());
    const QRect inked = engine.layerContentBounds(engine.activeLayerId());
    check(QStringLiteral("Sit seats the figure; Place puts the view on the canvas as a layer of its own (%1 x %2 px of figure)").arg(inked.width()).arg(inked.height()),
          seated && engine.layerCount() == layersBefore + 1 && placed.name == QLatin1String("3D reference") && inked.height() > 300
              && inked.width() < engine.canvasSize().width());
    capture(QStringLiteral("model-placed"));
    engine.undo();

    // A model from a file in place of the figure: no figure poses for it.
    QTemporaryDir dir;
    const QString cube = dir.filePath(QStringLiteral("Box.obj"));
    {
        QFile f(cube);
        f.open(QIODevice::WriteOnly);
        f.write("v -1 0 -1\nv 1 0 -1\nv 1 2 -1\nv -1 2 -1\nv -1 0 1\nv 1 0 1\nv 1 2 1\nv -1 2 1\n"
                "f 1 4 3 2\nf 5 6 7 8\nf 1 2 6 5\nf 4 8 7 3\nf 1 5 8 4\nf 2 3 7 6\n");
    }
    const bool opened = panel->openModel(cube);
    settle();
    bool posesHidden = true;
    for (IconButton* b : window.findChildren<IconButton*>(QStringLiteral("model-pose")))
        posesHidden = posesHidden && !b->isVisible();
    check(QStringLiteral("a model from a file takes the figure's place, without the figure's poses"),
          opened && view->model() && !view->model()->figure && view->model()->name == QLatin1String("Box") && posesHidden);
    // A person without joints: Rig fits a skeleton, a joint moved moves its twin, Bind makes it posable.
    {
        const QString personPath = dir.filePath(QStringLiteral("Person.obj"));
        writeFigureObj(personPath, false);
        const bool openedPerson = panel->openModel(personPath);
        settle();
        auto* rigButton = window.findChild<IconButton*>(QStringLiteral("model-rig"));
        const bool offered = openedPerson && rigButton && rigButton->isVisible() && view->model()->handles.empty();
        if (rigButton)
            penTap(rigButton, QPointF(rigButton->width() / 2.0, rigButton->height() / 2.0));
        const bool rigging = view->rigging() && view->rigPositions().size() == Model3D::rigJointNames().size();
        bool mirrored = false, bound = false, posable = false, remembered = false;
        if (rigging) {
            capture(QStringLiteral("model-rig"));
            const QPointF elbow = view->rigPositions()[7];
            const QVector3D before = view->rigJoints()[7];
            drag(view, elbow, elbow + QPointF(0, -24));
            const auto& j = view->rigJoints();
            mirrored = (j[7] - before).length() > 0.01f && std::abs(j[11].x() - (2.0f * j[0].x() - j[7].x())) < 1e-4f && std::abs(j[11].y() - j[7].y()) < 1e-4f;
            drag(view, view->rigPositions()[7], view->rigPositions()[7] + QPointF(0, 24));   // and back
            auto* bind = window.findChild<QWidget*>(QStringLiteral("model-rig-bind"));
            penTap(bind, QPointF(bind->width() / 2.0, bind->height() / 2.0));
            bound = !view->rigging() && view->model()->humanoid && view->model()->handles.size() == 17 && view->mode() == ModelView::Mode::Pose;
            if (bound) {
                Model3D* m = view->model();
                int handL = -1;
                for (size_t i = 0; i < m->nodes.size(); ++i)
                    if (m->nodes[i].name == QLatin1String("hand.L"))
                        handL = int(i);
                const QVector3D handBefore = m->jointPosition(handL, m->worldTransforms());
                const QPointF at = view->handlePositions().value(5);
                drag(view, at, at + QPointF(60, -40));
                posable = (m->jointPosition(handL, m->worldTransforms()) - handBefore).length() > 0.1f;
                capture(QStringLiteral("model-rigged"));
            }
            remembered = panel->openModel(personPath) && view->model()->humanoid && view->model()->handles.size() == 17;
        }
        // The poses work on it too: Walk moves its left foot.
        bool presets = false;
        if (remembered) {
            Model3D* m = view->model();
            int foot = -1;
            for (size_t i = 0; i < m->nodes.size(); ++i)
                if (m->nodes[i].name == QLatin1String("foot.L"))
                    foot = int(i);
            const QVector3D before = m->jointPosition(foot, m->worldTransforms());
            for (IconButton* b : window.findChildren<IconButton*>(QStringLiteral("model-pose"))) {
                if (b->property("pose").toString() == QLatin1String("Walk") && b->isVisible())
                    penTap(b, QPointF(b->width() / 2.0, b->height() / 2.0));
            }
            presets = (m->jointPosition(foot, m->worldTransforms()) - before).length() > 0.05f;
        }
        check(QStringLiteral("the figure's poses work on a model rigged here: Walk moves its foot"), presets);
        // Kept among the figures, it is listed and opens rigged.
        auto* keepButton = window.findChild<QWidget*>(QStringLiteral("model-keep"));
        bool kept = false;
        if (keepButton && keepButton->isVisible()) {
            penTap(keepButton, QPointF(keepButton->width() / 2.0, keepButton->height() / 2.0));
            bool listed = false;
            for (const figures::Figure& f : figures::list())
                listed = listed || (!f.builtIn && f.name == QLatin1String("Person") && f.path == view->model()->path);
            kept = listed && view->model()->humanoid && QFileInfo(view->model()->path).absolutePath() == QDir(figures::ownFolder()).absolutePath()
                   && !figures::ownFolder().startsWith(QDir::homePath() + QStringLiteral("/.local"));   // never the artist's own folder in a test
        }
        check(QStringLiteral("Keep puts a rigged model among the figures, in a folder of the test's own"), kept);
        check(QStringLiteral("a person without joints offers Rig; its skeleton shows to be checked, a joint moved moves its twin, Bind makes it posable, and it opens rigged again"),
              offered && rigging && mirrored && bound && posable && remembered);
    }
    // Figures: the mannequin, the woman built in, and the one just kept.
    {
        auto* figuresButton = window.findChild<QWidget*>(QStringLiteral("model-figures"));
        penTap(figuresButton, QPointF(figuresButton->width() / 2.0, figuresButton->height() / 2.0));
        QStringList names;
        IconButton* woman = nullptr;
        for (IconButton* b : window.findChildren<IconButton*>(QStringLiteral("model-figure"))) {
            if (!b->isVisible())
                continue;
            names << b->property("figure").toString();
            if (b->property("figure").toString() == QLatin1String("Woman"))
                woman = b;
        }
        bool chosen = false;
        if (woman) {
            penTap(woman, QPointF(woman->width() / 2.0, woman->height() / 2.0));
            settle();
            chosen = view->model() && view->model()->humanoid && view->model()->name == QLatin1String("Woman");
            capture(QStringLiteral("model-woman"));
        }
        check(QStringLiteral("Figures lists %1; the woman opens posable").arg(names.join(QStringLiteral(", "))),
              names.contains(QStringLiteral("Mannequin")) && names.contains(QStringLiteral("Woman")) && names.contains(QStringLiteral("Person")) && chosen);
    }
    panel->showFigure();
    pop->hidePanel();
    for (const QString& line : log)
        qInfo().noquote() << "3D reference:" << line;
    qInfo().noquote() << "3D reference:" << (ok ? "ok" : "FAILED");
    engine.setModified(false);
    return ok;
}

// Animation Assist through the window: the timeline, its frames, playback, settings and export.
static bool runAnimationUi(MainWindow& window, CanvasEngine& engine, const std::function<void(QWidget*, const QPointF&)>& penTap,
                           const std::function<void(const QString&)>& capture, const std::function<void()>& settle, const QString& outPrefix)
{
    QStringList log;
    bool ok = true;
    auto check = [&](const QString& what, bool passed) {
        ok = ok && passed;
        log << QStringLiteral("%1 %2").arg(passed ? "ok    " : "FAILED", what);
    };
    auto pause = [&](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QApplication::processEvents(QEventLoop::AllEvents, 5);
    };
    // A ball crossing the canvas, a frame for each place it is in.
    engine.newDocument(QSize(1200, 800), Qt::white);
    const int empty = engine.activeLayerId();
    QVector<int> balls;
    for (int k = 0; k < 4; ++k) {
        QImage ball(140, 140, QImage::Format_ARGB32_Premultiplied);
        ball.fill(Qt::transparent);
        QPainter p(&ball);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(QColor(40, 40, 48));
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(10, 10, 120, 120));
        p.end();
        balls.push_back(engine.addImageLayer(ball, QStringLiteral("Ball %1").arg(k + 1), QPoint(130 + k * 250, 330)));
    }
    engine.removeLayer(empty);
    engine.setActiveLayer(balls.last());
    window.showForScreenshot(QStringLiteral("main"));
    settle();
    window.setAnimationAssist(true);
    settle();
    TimelineBar* timeline = window.timeline();
    FrameStrip* strip = timeline ? timeline->strip() : nullptr;
    const bool shown = timeline && timeline->isVisible() && strip && engine.frames().size() == 4 && engine.currentFrame() == 3;
    // The ball before shows as an onion skin, tinted red; the one two back fainter still.
    const QColor before = engine.pickColor(QPoint(130 + 2 * 250 + 70, 400)), twoBack = engine.pickColor(QPoint(130 + 1 * 250 + 70, 400));
    const bool onion = before.isValid() && before.red() > before.green() + 40 && twoBack.isValid() && twoBack.lightness() > before.lightness();
    check(QStringLiteral("Animation Assist: the timeline shows 4 frames; the ball before is an onion skin in red (%1), the one before that fainter (%2)")
              .arg(before.name(), twoBack.name()),
          shown && onion);
    if (!shown)
        return false;
    capture(QStringLiteral("animation"));

    // A tap on a frame makes it the current one; + adds one after it, as one undo step.
    penTap(strip, strip->cellRect(0).center());
    const bool tapped = engine.currentFrame() == 0 && engine.activeLayerId() == balls[0];
    auto* add = window.findChild<QWidget*>(QStringLiteral("timeline-add"));
    penTap(add, QPointF(add->width() / 2.0, add->height() / 2.0));
    const bool added = engine.frames().size() == 5 && engine.currentFrame() == 1;
    engine.undo();
    settle();
    check(QStringLiteral("a tap on the first frame works on it; + puts a new frame after it, and undo takes it out again"),
          tapped && added && engine.frames().size() == 4);

    // Play: the frames go by on their own; a second tap stops on the frame being worked on.
    auto* play = window.findChild<QWidget*>(QStringLiteral("timeline-play"));
    penTap(play, QPointF(play->width() / 2.0, play->height() / 2.0));
    QSet<int> seen;
    for (int i = 0; i < 30; ++i) {
        pause(20);
        seen.insert(engine.playbackFrame());
    }
    const bool playing = timeline->playing() && seen.size() >= 3 && !seen.contains(-1);
    penTap(play, QPointF(play->width() / 2.0, play->height() / 2.0));
    check(QStringLiteral("play shows the frames in turn (%1 different in 0.6 s at 12 a second); a second tap stops").arg(seen.size()),
          playing && !timeline->playing() && engine.playbackFrame() == -1);

    // Settings open above the bar; the frame rate set there is the document's.
    auto* settingsBtn = window.findChild<QWidget*>(QStringLiteral("timeline-settings"));
    penTap(settingsBtn, QPointF(settingsBtn->width() / 2.0, settingsBtn->height() / 2.0));
    auto* settingsPop = window.findChild<Popover*>(QStringLiteral("timeline-settings-popover"));
    auto* fps = window.findChild<Slider*>(QStringLiteral("animation-fps"));
    bool settingsOk = settingsPop && settingsPop->isVisible() && settingsPop->geometry().bottom() <= timeline->geometry().top() + 20 && fps;   // its shadow may overlap
    if (settingsOk) {
        const QPointF knob(10 + (fps->width() - 20) * fps->value(), fps->height() / 2.0);
        const QPointF to(10 + (fps->width() - 20) * (23.0 / 59.0), fps->height() / 2.0);
        penTap(fps, to);
        settingsOk = engine.animation().fps == 24;
        Q_UNUSED(knob);
        capture(QStringLiteral("animation-settings"));
        settingsPop->dismiss();
    }
    if (!settingsOk)
        log << QStringLiteral("       panel %1, shown %2, bottom %3 vs timeline top %4, slider %5")
                   .arg(settingsPop != nullptr).arg(settingsPop && settingsPop->isVisible()).arg(settingsPop ? settingsPop->geometry().bottom() : -1)
                   .arg(timeline->geometry().top()).arg(fps != nullptr);
    check(QStringLiteral("Settings opens above the timeline; a tap along the frame rate slider sets %1 frames a second").arg(engine.animation().fps), settingsOk);

    // A tap on the frame being worked on opens its options: hold it longer.
    penTap(strip, strip->cellRect(0).center());
    auto* framePop = window.findChild<Popover*>(QStringLiteral("frame-options-popover"));
    bool holdOk = framePop && framePop->isVisible();
    if (holdOk) {
        auto* more = framePop->findChild<QWidget*>(QStringLiteral("frame-hold-more"));
        penTap(more, QPointF(more->width() / 2.0, more->height() / 2.0));
        holdOk = engine.layerInfo(balls[0]).hold == 2;
        framePop->dismiss();
    }
    check(QStringLiteral("a tap on the current frame opens its options; + holds it for 2 frames"), holdOk);

    // Out as a GIF at half size (the file dialog itself is not driven).
    const QString gif = outPrefix + QStringLiteral("-animation.gif");
    QString error;
    const bool exported = timeline->exportAnimation(TimelineBar::Format::Gif, gif, 0.5, &error);
    QImageReader reader(gif);
    check(QStringLiteral("export as an animated GIF at half size: %1 frames of %2 x %3").arg(reader.imageCount()).arg(reader.size().width()).arg(reader.size().height()),
          exported && reader.imageCount() == 4 && reader.size() == QSize(600, 400));

    window.setAnimationAssist(false);
    settle();
    const QImage all = engine.renderImage();
    check(QStringLiteral("Animation Assist off: the timeline goes and every ball shows again"),
          !timeline->isVisible() && all.pixelColor(130 + 70, 400).lightness() < 80 && all.pixelColor(130 + 3 * 250 + 70, 400).lightness() < 80);
    for (const QString& line : log)
        qInfo().noquote() << "animation:" << line;
    qInfo().noquote() << "animation:" << (ok ? "ok" : "FAILED");
    engine.setModified(false);
    return ok;
}

int runUiShot(const QString& outPrefix)
{
    std::signal(SIGTERM, dumpStackAndExit);
    std::signal(SIGSEGV, dumpStackAndExit);
    isolateSettings();
    QTemporaryDir wallDir;   // never the artist's own gallery
    Gallery::setFolder(wallDir.path());
    theme::apply(*qApp);
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << "engine init failed:" << error;
        return 1;
    }
    // IMPASTO_LIVE=1 maps the window for real and captures it from the screen,
    // which also exercises compositing of the translucent panels over GL.
    const bool live = qEnvironmentVariableIsSet("IMPASTO_LIVE");
    MainWindow window(&engine);
    if (!live)
        window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1600, 1000);
    engine.newDocument(QSize(2400, 1600), QColor(250, 247, 240));

    const BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset& round = *lib.find(QStringLiteral("round-brush"));
    wave(engine, StrokeTool::Brush, *lib.find(QStringLiteral("airbrush-soft")), 700, 1.0f, QColor(255, 170, 60),
         QPointF(500, 700), QPointF(1900, 700), 120);
    const int ink = engine.addLayer();
    engine.setLayerName(ink, QStringLiteral("Ink"));
    wave(engine, StrokeTool::Brush, *lib.find(QStringLiteral("brush-pen")), 70, 1.0f, QColor(24, 24, 30),
         QPointF(400, 500), QPointF(2000, 520), 160);
    wave(engine, StrokeTool::Brush, *lib.find(QStringLiteral("pencil-6b")), 26, 1.0f, QColor(40, 40, 60),
         QPointF(400, 1000), QPointF(2000, 1000), 90, 30.0f);
    const int colour = engine.addLayer();
    engine.setLayerName(colour, QStringLiteral("Colour"));
    engine.setLayerBlend(colour, BlendMode::Multiply);
    engine.setLayerOpacity(colour, 0.8f);
    wave(engine, StrokeTool::Brush, round, 220, 1.0f, QColor(60, 140, 230), QPointF(600, 1200), QPointF(1800, 1150), 60);
    wave(engine, StrokeTool::Brush, *lib.find(QStringLiteral("oil-paint")), 160, 1.0f, QColor(220, 60, 90),
         QPointF(700, 320), QPointF(1700, 300), 40);

    qInfo() << "showing window";
    window.show();
    auto settle = [] {
        for (int i = 0; i < 8; ++i)
            QApplication::processEvents(QEventLoop::AllEvents, 30);
    };
    // capture() only takes the picture; shot() first puts the window into the named state.
    auto capture = [&](const QString& what) {
        settle();
        const QString path = QStringLiteral("%1-%2.png").arg(outPrefix, what);
        // QWidget::grab() would lose what the viewport paints with QPainter on
        // top of GL (cursor, handles), so start from the real framebuffer and
        // render the floating panels over it.
        QWidget* host = window.centralWidget();
        auto* canvas = host->findChild<QOpenGLWidget*>();
        QImage frame = canvas->grabFramebuffer().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&frame);
        for (QObject* child : host->children()) {
            auto* w = qobject_cast<QWidget*>(child);
            if (w && w != canvas && w->isVisible())
                w->render(&painter, w->pos(), QRegion(), QWidget::DrawChildren);
        }
        // A 3D view in a panel: its own picture, with what it paints over the scene.
        for (QOpenGLWidget* gl : host->findChildren<QOpenGLWidget*>()) {
            if (gl != canvas && gl->isVisible())
                painter.drawImage(QRectF(gl->mapTo(host, QPoint(0, 0)), QSizeF(gl->size())), gl->grabFramebuffer());
        }
        painter.end();
        if (!frame.save(path))
            qCritical().noquote() << "could not write" << path;
    };
    auto shot = [&](const QString& what) {
        qInfo().noquote() << "shot:" << what;
        window.showForScreenshot(what);
        capture(what);
    };
    settle();
    for (const char* what : {"main", "brushes", "layers", "colors", "transform", "adjust"})
        shot(QString::fromLatin1(what));
    window.showForScreenshot(QStringLiteral("main"));

    // Drive the viewport through its real event handlers.
    auto* view = window.centralWidget()->findChild<CanvasWidget*>();
    bool penPanelsOk = false;
    auto* tools = window.findChild<ToolState*>();
    const int undoBefore = engine.history().undoCount();
    auto mouse = [&](QEvent::Type t, const QPointF& pos, Qt::MouseButton b, Qt::MouseButtons held) {
        QMouseEvent ev(t, pos, view->mapToGlobal(pos), b, held, Qt::NoModifier);
        QApplication::sendEvent(view, &ev);
    };
    qInfo() << "input: mouse stroke";
    QPointingDevice stylus(QStringLiteral("test stylus"), 4242, QInputDevice::DeviceType::Stylus,
                           QPointingDevice::PointerType::Pen,
                           QInputDevice::Capability::Position | QInputDevice::Capability::Pressure
                               | QInputDevice::Capability::XTilt | QInputDevice::Capability::YTilt,
                           1, 3);

    // ---- The pen in panels and menus. Events enter at the window, as they do
    // from the platform: Qt picks the widget under the pen, lets an unhandled
    // press travel up through its parents, and replays whatever nobody accepted
    // as a mouse event. Sending straight to a widget would skip all of that.
    QWindow* handle = window.windowHandle();
    auto penWindow = [&](QEvent::Type t, const QPointF& pos, qreal pressure, Qt::MouseButton b, Qt::MouseButtons held) {
        const QPointF global = handle->mapToGlobal(pos);
        QTabletEvent ev(t, &stylus, pos, global, pressure, 15.0f, 5.0f, 0.0f, 0.0, 0.0f, Qt::NoModifier, b, held);
        ev.setAccepted(false);
        QCoreApplication::sendEvent(handle, &ev);
        if (ev.isAccepted())
            return;
        const QEvent::Type mouseType = t == QEvent::TabletPress     ? QEvent::MouseButtonPress
                                       : t == QEvent::TabletRelease ? QEvent::MouseButtonRelease
                                                                    : QEvent::MouseMove;
        QMouseEvent me(mouseType, pos, global, b, held, Qt::NoModifier);
        QCoreApplication::sendEvent(handle, &me);
    };
    auto penTap = [&](QWidget* w, const QPointF& local) {
        const QPointF p = w->mapTo(&window, local.toPoint());
        penWindow(QEvent::TabletMove, p, 0.0, Qt::NoButton, Qt::NoButton);   // hover in
        penWindow(QEvent::TabletPress, p, 0.4, Qt::LeftButton, Qt::LeftButton);
        penWindow(QEvent::TabletMove, p + QPointF(0.5, 0.5), 0.5, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, p, 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
    };
    auto centre = [](QWidget* w) { return QPointF(w->width() / 2.0, w->height() / 2.0); };
    if (qEnvironmentVariable("IMPASTO_ONLY") == QLatin1String("animation"))
        return runAnimationUi(window, engine, penTap, capture, settle, outPrefix) ? 0 : 2;
    if (qEnvironmentVariable("IMPASTO_ONLY") == QLatin1String("model"))
        return runModelUi(window, engine, penTap, penWindow, capture, settle) ? 0 : 2;
    {
        QStringList log;
        bool penOk = true;
        auto check = [&](const QString& what, bool ok) {
            penOk = penOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        const int undoBase = engine.history().undoCount();
        auto* brushBtn = window.findChild<QWidget*>(QStringLiteral("tool-brush"));
        auto* layersBtn = window.findChild<QWidget*>(QStringLiteral("layers-button"));
        auto* brushList = window.findChild<QWidget*>(QStringLiteral("brush-list"));
        auto* categories = window.findChild<QWidget*>(QStringLiteral("brush-categories"));
        auto* layerList = window.findChild<QWidget*>(QStringLiteral("layer-list"));
        auto* layerAdd = window.findChild<QWidget*>(QStringLiteral("layer-add"));
        auto* layerOpacity = window.findChild<QWidget*>(QStringLiteral("layer-opacity"));
        tools->setTool(StrokeTool::Brush);
        tools->setPreset(QStringLiteral("round-brush"));

        penTap(brushBtn, centre(brushBtn));
        check(QStringLiteral("tap the active tool opens the brush library"), brushList->isVisible());
        penTap(categories, QPointF(40, 40 * 1 + 18));   // second set: Inking
        penTap(brushList, QPointF(brushList->width() / 2.0, 84 * 2 + 40));   // third brush in it
        const auto inking = BrushLibrary::instance().inCategory(QStringLiteral("Inking"));
        check(QStringLiteral("tap a set, then a brush -> %1").arg(tools->preset().name),
              inking.size() > 2 && tools->preset().id == inking[2]->id && brushList->isVisible());
        penTap(brushBtn, centre(brushBtn));
        check(QStringLiteral("tap the tool button again closes the library"), !brushList->isVisible());

        penTap(layersBtn, centre(layersBtn));
        check(QStringLiteral("tap Layers opens the layer panel"), layerList->isVisible());
        const int layersBefore = engine.layerCount();
        penTap(layerAdd, centre(layerAdd));
        check(QStringLiteral("tap + adds a layer"), engine.layerCount() == layersBefore + 1 && layerList->isVisible());
        const int topId = engine.layers().last().id;
        penTap(layerList, QPointF(layerList->width() - 28, 30));   // eye of the top row
        check(QStringLiteral("tap the eye hides the layer"), !engine.layerInfo(topId).visible);
        penTap(layerList, QPointF(layerList->width() - 28, 30));
        penTap(layerList, QPointF(140, 66 + 30));   // second row
        check(QStringLiteral("tap a row selects that layer"), engine.activeLayerId() != topId && engine.layerInfo(topId).visible);

        // Drag the opacity slider with the pen.
        const QPointF a = layerOpacity->mapTo(&window, QPoint(layerOpacity->width() - 12, layerOpacity->height() / 2));
        penWindow(QEvent::TabletPress, a, 0.5, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 10; ++i)
            penWindow(QEvent::TabletMove, a - QPointF(i * 6.0, 0), 0.5, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, a - QPointF(60, 0), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        const float dragged = engine.layerInfo(engine.activeLayerId()).opacity;
        check(QStringLiteral("drag the opacity slider -> %1%").arg(qRound(dragged * 100)), dragged < 0.75f && dragged > 0.2f);
        capture(QStringLiteral("pen-layers"));

        // A press on the canvas closes the panel.
        penWindow(QEvent::TabletPress, QPointF(700, 600), 0.4, Qt::LeftButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, QPointF(700, 600), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        check(QStringLiteral("press on the canvas closes the panel"), !layerList->isVisible());

        while (engine.history().undoCount() > undoBase)
            engine.undo(true);
        for (const QString& line : log)
            qInfo().noquote() << "pen in panels:" << line;
        qInfo().noquote() << "pen in panels:" << (penOk ? "ok" : "FAILED");
        penPanelsOk = penOk;
    }

    // ---- Selection, fill and brush settings, again with the pen at window level.
    bool toolsOk = true;
    {
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            toolsOk = toolsOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        auto textButton = [&](const char* label) -> QWidget* {
            for (IconButton* b : window.findChildren<IconButton*>()) {
                if (b->isVisible() && b->text() == QLatin1String(label))
                    return b;
            }
            return nullptr;
        };
        auto penDrag = [&](const QPointF& from, const QPointF& to, int steps = 24) {
            penWindow(QEvent::TabletMove, from, 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, from, 0.5, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= steps; ++i)
                penWindow(QEvent::TabletMove, from + (to - from) * (double(i) / steps), 0.6, Qt::NoButton, Qt::LeftButton);
            penWindow(QEvent::TabletRelease, to, 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
        };
        auto canvasPixel = [&](const QPointF& widgetPos) {
            const QPointF c = view->widgetToCanvas(widgetPos);
            return engine.renderImage().pixelColor(int(c.x()), int(c.y()));
        };
        const int undoBase = engine.history().undoCount();
        const int startLayer = engine.activeLayerId();
        engine.addLayer();

        // Outline a box, then cut an oval out of its middle.
        auto* selectBtn = window.findChild<QWidget*>(QStringLiteral("select-button"));
        penTap(selectBtn, centre(selectBtn));
        check(QStringLiteral("tap Select enters selection mode"), view->selectMode() && textButton("Rectangle"));
        if (QWidget* b = textButton("Rectangle"))
            penTap(b, centre(b));
        penDrag(QPointF(300, 300), QPointF(700, 560));
        const QRectF boxOnCanvas = QRectF(view->widgetToCanvas(QPointF(300, 300)), view->widgetToCanvas(QPointF(700, 560)));
        const QRectF got = engine.selectionBounds();
        check(QStringLiteral("drag a rectangle -> %1 x %2 px selected").arg(qRound(got.width())).arg(qRound(got.height())),
              engine.hasSelection() && std::abs(got.width() - boxOnCanvas.width()) < 3 && std::abs(got.height() - boxOnCanvas.height()) < 3);
        if (QWidget* b = textButton("Ellipse"))
            penTap(b, centre(b));
        if (QWidget* b = textButton("Remove"))
            penTap(b, centre(b));
        penDrag(QPointF(420, 360), QPointF(580, 500));
        const QPointF hole = view->widgetToCanvas(QPointF(500, 430));
        const QPointF rim = view->widgetToCanvas(QPointF(330, 430));
        check(QStringLiteral("Remove + ellipse cuts a hole"), engine.selectionAt(hole.toPoint()) < 0.01f && engine.selectionAt(rim.toPoint()) > 0.99f);
        capture(QStringLiteral("select"));
        if (QWidget* b = window.findChild<QWidget*>(QStringLiteral("select-button")))
            penTap(b, centre(b));
        check(QStringLiteral("leaving the mode keeps the selection"), !view->selectMode() && engine.hasSelection());

        // A stroke straight across only lands inside the selection.
        tools->setTool(StrokeTool::Brush);
        tools->setPreset(QStringLiteral("round-brush"));
        tools->setSize(60);
        tools->setColor(QColor(30, 110, 220));
        const QColor outsideBefore = canvasPixel(QPointF(240, 430)), holeBefore = canvasPixel(QPointF(500, 430));
        penDrag(QPointF(200, 430), QPointF(800, 430), 60);
        const QColor inRim = canvasPixel(QPointF(340, 430));
        check(QStringLiteral("painting is confined to the selection"),
              canvasPixel(QPointF(240, 430)) == outsideBefore && canvasPixel(QPointF(500, 430)) == holeBefore
                  && inRim.blue() > inRim.red() + 40);
        capture(QStringLiteral("select-painted"));

        // ColorDrop: drag the colour chip onto the canvas with the pen.
        engine.clearSelection();
        QPainterPath pool;
        pool.addRect(QRectF(view->widgetToCanvas(QPointF(900, 300)), view->widgetToCanvas(QPointF(1200, 520))));
        engine.setSelection(pool, QStringLiteral("Select"));
        tools->setColor(QColor(240, 170, 20));
        auto* chip = window.findChild<QWidget*>(QStringLiteral("color-chip"));
        const QColor besideBefore = canvasPixel(QPointF(860, 400));
        penDrag(chip->mapTo(&window, centre(chip).toPoint()), QPointF(1050, 410), 30);
        const QColor poured = canvasPixel(QPointF(1050, 410));
        check(QStringLiteral("dragging the colour chip onto the canvas fills there"),
              std::abs(poured.red() - 240) < 6 && std::abs(poured.green() - 170) < 6 && canvasPixel(QPointF(860, 400)) == besideBefore);
        engine.clearSelection();
        capture(QStringLiteral("fill"));

        // ---- Reference fills, automatic selection, feather and thresholds, with the pen.
        {
            auto pause = [](int ms) {
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < ms)
                    QApplication::processEvents(QEventLoop::AllEvents, 10);
            };
            auto named = [&](const char* name) { return window.findChild<QWidget*>(QLatin1String(name)); };
            auto toCanvas = [&](const QPointF& widgetPos) { return view->widgetToCanvas(widgetPos).toPoint(); };

            // Line art on its own layer (a box outline) with the colour layer beneath it.
            const int lines = engine.addLayer();
            engine.setLayerName(lines, QStringLiteral("Line art"));
            const QRectF outer(view->widgetToCanvas(QPointF(880, 580)), view->widgetToCanvas(QPointF(1240, 840)));
            QPainterPath frame;
            frame.addRoundedRect(outer, 40, 40);
            QPainterPath hollow;
            hollow.addRoundedRect(outer.adjusted(9, 9, -9, -9), 32, 32);
            engine.setSelection(frame.subtracted(hollow), QStringLiteral("Select"));
            engine.fillLayer(lines, QColor(24, 24, 30));
            engine.clearSelection();
            const int flats = engine.addLayer();
            engine.setLayerName(flats, QStringLiteral("Flats"));
            engine.moveLayer(flats, 0, lines);
            engine.setActiveLayer(flats);
            engine.setReferenceLayer(lines);
            // ...and a blob with a soft rim on the colour layer, to give a threshold something to do.
            const QPointF blobCentre = view->widgetToCanvas(QPointF(1000, 300));
            QPainterPath blobShape;
            blobShape.addEllipse(blobCentre, 110, 110);
            engine.setSelection(blobShape, QStringLiteral("Select"));
            engine.featherSelection(60.0f);
            engine.fillLayer(flats, QColor(90, 60, 200));
            engine.clearSelection();
            // 100 px out the blob is about 60% there: beyond a 12% threshold, within a 50% one.
            const QPointF rimPoint = view->canvasToWidget(blobCentre + QPointF(100, 0));
            settle();

            // Automatic: one tap inside the outline selects its inside.
            penTap(selectBtn, centre(selectBtn));
            if (QWidget* b = named("select-automatic"))
                penTap(b, centre(b));
            penTap(view, QPointF(1060, 710));
            const QRect inside = engine.selectionBounds();
            const QRectF hole = outer.adjusted(9, 9, -9, -9);
            check(QStringLiteral("Automatic: a tap inside line art on another layer selects %1 x %2 px").arg(inside.width()).arg(inside.height()),
                  view->selectShape() == CanvasWidget::SelectShape::Automatic && std::abs(inside.width() - hole.width()) < 10
                      && std::abs(inside.height() - hole.height()) < 10 && engine.selectionAt(toCanvas(QPointF(1060, 710))) == 1.0f
                      && engine.selectionAt(toCanvas(QPointF(700, 710))) == 0.0f);

            // Feather it with the slider: one undo step however long the drag.
            const int beforeFeather = engine.history().undoCount();
            if (QWidget* b = named("select-feather"))
                penTap(b, centre(b));
            auto* featherSlider = window.findChild<Slider*>(QStringLiteral("select-feather-slider"));
            const bool sliderShown = featherSlider && featherSlider->isVisible();
            const QPoint onEdge(int(hole.left()) - 2, int(hole.center().y()));
            const float hard = engine.selectionAt(onEdge + QPoint(12, 0));
            if (sliderShown) {
                const QPointF from = featherSlider->mapTo(&window, QPoint(8, featherSlider->height() / 2));
                penDrag(from, from + QPointF(featherSlider->width() * 0.5, 0));
            }
            const float softened = engine.selectionAt(onEdge + QPoint(12, 0));
            check(QStringLiteral("Feather slider: 12 px inside the edge the selection goes from %1 to %2").arg(hard, 0, 'f', 2).arg(softened, 0, 'f', 2),
                  sliderShown && featherSlider->value() > 0.4f && hard == 1.0f && softened > 0.5f && softened < 0.95f
                      && engine.selectionAt(onEdge - QPoint(12, 0)) > 0.05f
                      && engine.history().undoCount() == beforeFeather + 1);
            capture(QStringLiteral("select-feather"));
            engine.undo(true);
            check(QStringLiteral("undoing the feather resets its slider"), featherSlider && featherSlider->value() == 0.0f
                                                                             && engine.selectionAt(onEdge + QPoint(12, 0)) == 1.0f);
            engine.clearSelection();

            // Automatic on a soft blob: sliding sideways before lifting widens the threshold.
            const QPointF blob(1000, 300);
            penWindow(QEvent::TabletMove, view->mapTo(&window, blob.toPoint()), 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, view->mapTo(&window, blob.toPoint()), 0.5, Qt::LeftButton, Qt::LeftButton);
            settle();
            const int tight = engine.selectionBounds().width();
            const float startTolerance = engine.floodTolerance();
            for (int i = 1; i <= 20; ++i)
                penWindow(QEvent::TabletMove, view->mapTo(&window, blob.toPoint()) + QPointF(i * 8, 0), 0.5, Qt::NoButton, Qt::LeftButton);
            settle();
            const bool live = engine.floodActive();
            const int wide = engine.selectionBounds().width();
            const float endTolerance = engine.floodTolerance();
            capture(QStringLiteral("select-threshold"));
            penWindow(QEvent::TabletRelease, view->mapTo(&window, blob.toPoint()) + QPointF(160, 0), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            check(QStringLiteral("slide while selecting: threshold %1% -> %2%, selection %3 -> %4 px wide")
                      .arg(qRound(startTolerance * 100)).arg(qRound(endTolerance * 100)).arg(tight).arg(wide),
                  live && !engine.floodActive() && endTolerance > startTolerance + 0.2f && wide > tight + 20
                      && engine.selectionBounds().width() == wide
                      && std::abs(QSettings().value(QStringLiteral("prefs/selectTolerance")).toFloat() - endTolerance) < 0.001f);
            engine.clearSelection();
            if (QWidget* b = named("select-done"))
                penTap(b, centre(b));

            // Fill with a tap: the colour stays inside the lines of the reference layer.
            tools->setColor(QColor(250, 200, 40));
            const QColor outsideBefore = canvasPixel(QPointF(800, 710));
            view->armFill(true);
            penTap(view, QPointF(1060, 710));
            const QColor dropped = canvasPixel(QPointF(1060, 710));
            check(QStringLiteral("armed fill: a tap inside the outline colours only its inside"),
                  nearby(dropped, 250, 200, 40, 6) && canvasPixel(QPointF(800, 710)) == outsideBefore && !view->fillArmed());
            capture(QStringLiteral("fill-reference"));
            engine.undo(true);

            // The same with a slide: the threshold follows the pen and is remembered.
            view->armFill(true);
            const QPointF pour = view->mapTo(&window, QPoint(1000, 300));
            penWindow(QEvent::TabletMove, pour, 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, pour, 0.5, Qt::LeftButton, Qt::LeftButton);
            settle();
            const QColor rimEarly = canvasPixel(rimPoint);
            for (int i = 1; i <= 24; ++i)
                penWindow(QEvent::TabletMove, pour + QPointF(i * 8, 0), 0.5, Qt::NoButton, Qt::LeftButton);
            settle();
            const float fillTolerance = engine.floodTolerance();
            const QColor rimLate = canvasPixel(rimPoint);
            capture(QStringLiteral("fill-threshold"));
            penWindow(QEvent::TabletRelease, pour + QPointF(192, 0), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            check(QStringLiteral("slide while filling: threshold reaches %1%, the colour spreads out over the blob's soft rim").arg(qRound(fillTolerance * 100)),
                  fillTolerance > 0.4f && !nearby(rimEarly, 250, 200, 40, 12) && nearby(rimLate, 250, 200, 40, 12) && !engine.floodActive()
                      && nearby(canvasPixel(rimPoint), 250, 200, 40, 12)
                      && std::abs(QSettings().value(QStringLiteral("prefs/fillTolerance")).toFloat() - fillTolerance) < 0.001f);
            engine.undo(true);
            view->setFillTolerance(0.12f);

            // The colour chip rested on the canvas does the same without arming anything.
            tools->setColor(QColor(40, 170, 120));
            const QPointF chipAt = chip->mapTo(&window, centre(chip).toPoint());
            const QPointF rest = view->mapTo(&window, QPoint(1060, 710));
            penWindow(QEvent::TabletMove, chipAt, 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, chipAt, 0.5, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 20; ++i)
                penWindow(QEvent::TabletMove, chipAt + (rest - chipAt) * (i / 20.0), 0.5, Qt::NoButton, Qt::LeftButton);
            const bool notYet = !engine.floodActive();
            pause(750);
            const bool pouring = engine.floodActive() && nearby(canvasPixel(QPointF(1060, 710)), 40, 170, 120, 6);
            const float restTolerance = engine.floodTolerance();
            for (int i = 1; i <= 10; ++i)
                penWindow(QEvent::TabletMove, rest + QPointF(i * 8, 0), 0.5, Qt::NoButton, Qt::LeftButton);
            const float slidTolerance = engine.floodTolerance();
            penWindow(QEvent::TabletRelease, rest + QPointF(80, 0), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            check(QStringLiteral("rest the colour chip on the canvas: it pours, and sliding takes the threshold from %1% to %2%")
                      .arg(qRound(restTolerance * 100)).arg(qRound(slidTolerance * 100)),
                  notYet && pouring && slidTolerance > restTolerance + 0.1f && !engine.floodActive()
                      && nearby(canvasPixel(QPointF(1060, 710)), 40, 170, 120, 6) && canvasPixel(QPointF(800, 710)) == outsideBefore);

            // The layer panel says which layer is the reference.
            auto* layersBtn = named("layers-button");
            penTap(layersBtn, centre(layersBtn));
            capture(QStringLiteral("layers-reference"));
            window.showForScreenshot(QStringLiteral("main"));
            engine.setActiveLayer(flats);
            view->setFillTolerance(0.12f);
        }

        // ---- The brush lab: opened from the library, tried on its pad, changed, cancelled, kept.
        {
            BrushLibrary& library = BrushLibrary::instance();
            auto named = [&](const char* name) -> QWidget* {
                QWidget* found = nullptr;
                for (QWidget* w : window.findChildren<QWidget*>(QLatin1String(name))) {
                    if (w->isVisible())
                        found = w;
                }
                return found;
            };
            auto tapNamed = [&](const char* name) {
                QWidget* w = named(name);
                if (w)
                    penTap(w, centre(w));
                return w != nullptr;
            };
            auto at = [&](QWidget* w, const QPointF& local) { return QPointF(w->mapTo(&window, local.toPoint())); };
            const int shippedCount = int(library.presets().size());
            const int strokesOnCanvas = engine.history().undoCount();
            tools->setTool(StrokeTool::Brush);
            tools->setPreset(QStringLiteral("studio-pen"));
            tools->setSize(26);
            const float spacingBefore = tools->preset().spacing;

            tapNamed("tool-brush");
            const bool viaLibrary = tapNamed("brush-edit");
            settle();
            BrushLab* lab = window.brushLab();
            const bool opened = viaLibrary && lab && lab->isVisible() && lab->brushId() == QLatin1String("studio-pen") && lab->pad()
                                && !named("brush-list");
            check(QStringLiteral("the sliders button in the brush library opens the brush lab on Studio Pen, over the whole window"),
                  opened && lab->size() == window.centralWidget()->size());
            if (opened) {
                CanvasWidget* pad = lab->pad();
                CanvasEngine* padEngine = lab->padEngine();
                // Light paint on the dark pad, counted.
                auto ink = [&] {
                    const QImage img = padEngine->renderImage().convertToFormat(QImage::Format_Grayscale8);
                    int n = 0;
                    for (int y = 0; y < img.height(); y += 2) {
                        const uchar* row = img.constScanLine(y);
                        for (int x = 0; x < img.width(); x += 2)
                            n += row[x] > 110;
                    }
                    return n;
                };
                const QSize padPixels = padEngine->canvasSize();
                penDrag(at(pad, QPointF(pad->width() * 0.15, pad->height() * 0.3)), at(pad, QPointF(pad->width() * 0.85, pad->height() * 0.42)), 40);
                const int inkDrawn = ink();
                check(QStringLiteral("the pad is %1 x %2 px, the size it has on screen; a stroke on it is paint on the pad and nothing on the canvas")
                          .arg(padPixels.width()).arg(padPixels.height()),
                      padPixels == pad->size() && padEngine->journalSize() == 1 && inkDrawn > 1500 && engine.history().undoCount() == strokesOnCanvas);

                // Spacing pulled wide: the stroke that is already there is drawn again, now as separate stamps.
                auto* spacing = lab->findChild<Slider*>(QStringLiteral("lab-spacing"));
                const QPointF knob(10 + (spacing->width() - 20) * spacing->value(), spacing->height() / 2.0);
                penDrag(at(spacing, knob), at(spacing, knob + QPointF(spacing->width() * 0.62, 0)));
                lab->redrawPadNow();
                settle();
                const float spacingAfter = tools->preset().spacing;
                const int inkSpaced = ink();
                check(QStringLiteral("drag Spacing: %1% -> %2%; the stroke on the pad is drawn again with its stamps apart (%3 -> %4 px of paint)")
                          .arg(qRound(spacingBefore * 100)).arg(qRound(spacingAfter * 100)).arg(inkDrawn).arg(inkSpaced),
                      spacingAfter > spacingBefore * 4 && library.isModified(QStringLiteral("studio-pen")) && inkSpaced < inkDrawn * 0.97
                          && inkSpaced > 100 && padEngine->journalSize() == 1);
                capture(QStringLiteral("brush-lab"));

                // Pen: the pressure curve, with room to work on it.
                penTap(lab->sectionList(), lab->sectionRect(BrushLab::Pen).center());
                auto* curve = lab->findChild<CurveEditor*>(QStringLiteral("lab-pressure-curve"));
                const bool penPage = lab->section() == BrushLab::Pen && curve && curve->isVisible() && curve->width() >= 360 && curve->height() >= 280
                                     && curve->points().size() == 5;
                const QPointF middle = curve->points().value(2);
                const QPointF onCurve(10 + middle.x() * (curve->width() - 20), curve->height() - 10 - middle.y() * (curve->height() - 20));
                penDrag(at(curve, onCurve), at(curve, onCurve + QPointF(0, -75)));
                const BrushPreset& curved = tools->preset();
                check(QStringLiteral("Pen section: the pressure curve is %1 x %2 px; its middle point dragged up gives the brush a curve of its own (%3% at half pressure)")
                          .arg(curve->width()).arg(curve->height()).arg(qRound(curved.pressureCurve.value(2).y() * 100)),
                      penPage && curved.pressureCurve.size() == 5 && curved.pressureCurve[2].y() > middle.y() + 0.2 && curved.pressureGamma == 1.0f);
                capture(QStringLiteral("brush-lab-pen"));

                // Shape: the tips as pictures; a tap takes one.
                penTap(lab->sectionList(), lab->sectionRect(BrushLab::Shape).center());
                const QImage roundPreview = engine.brushPreview(tools->preset(), QSize(400, 80), Qt::white, 1.0f);
                auto showTile = [&](const QString& tip) {
                    if (auto* shapePage = qobject_cast<QScrollArea*>(named("lab-page-3")))
                        shapePage->ensureVisible(100, lab->tipPicker()->y() + lab->tipTileRect(tip).center().y(), 0, 90);
                    settle();
                };
                showTile(QStringLiteral("chalk"));
                penTap(lab->tipPicker(), lab->tipTileRect(QStringLiteral("chalk")).center());
                settle();
                check(QStringLiteral("Shape section: a tap on the chalk picture makes it the tip"),
                      lab->section() == BrushLab::Shape && tools->preset().tip == QLatin1String("chalk")
                          && engine.brushPreview(tools->preset(), QSize(400, 80), Qt::white, 1.0f) != roundPreview);
                lab->redrawPadNow();
                capture(QStringLiteral("brush-lab-shape"));
                lab->setSection(BrushLab::Grain);
                settle();
                capture(QStringLiteral("brush-lab-grain"));

                // Cancel puts the brush back as it was when the lab opened.
                tapNamed("lab-cancel");
                const BrushPreset& back = tools->preset();
                check(QStringLiteral("Cancel: the lab closes and Studio Pen is as it shipped again"),
                      !lab->isVisible() && qFuzzyCompare(back.spacing, spacingBefore) && back.tip.isEmpty() && back.pressureCurve.isEmpty()
                          && !library.isModified(QStringLiteral("studio-pen")));

                // Done keeps a change; Reset, in About, undoes it later.
                window.showBrushLab();
                settle();
                lab->setSection(BrushLab::StrokePath);
                const QPointF knobAgain(10 + (spacing->width() - 20) * spacing->value(), spacing->height() / 2.0);
                penDrag(at(spacing, knobAgain), at(spacing, knobAgain + QPointF(spacing->width() * 0.3, 0)));
                tapNamed("lab-done");
                const bool kept = !lab->isVisible() && tools->preset().spacing > spacingBefore * 2 && library.isModified(QStringLiteral("studio-pen"));
                window.showBrushLab();
                settle();
                lab->setSection(BrushLab::About);
                settle();
                auto* nameEdit = lab->findChild<QLineEdit*>(QStringLiteral("lab-name"));
                const bool lockedName = nameEdit && nameEdit->isReadOnly();
                tapNamed("lab-reset");
                check(QStringLiteral("Done keeps the change; Reset, under About this brush, puts the shipped brush back"),
                      kept && qFuzzyCompare(tools->preset().spacing, spacingBefore) && !library.isModified(QStringLiteral("studio-pen")));

                // Your own brushes, made and unmade with the pen.
                const bool tappedDuplicate = tapNamed("lab-duplicate");
                settle();
                const QString copyId = tools->preset().id;
                check(QStringLiteral("Duplicate -> \"%1\": the lab moves on to the copy, whose name can be edited").arg(tools->preset().name),
                      tappedDuplicate && lockedName && tools->preset().custom && tools->preset().name == QLatin1String("Studio Pen copy")
                          && lab->isVisible() && lab->brushId() == copyId && !nameEdit->isReadOnly() && nameEdit->text() == QLatin1String("Studio Pen copy"));
                nameEdit->setFocus();
                nameEdit->setText(QStringLiteral("Liner"));
                emit nameEdit->editingFinished();
                lab->setSection(BrushLab::Shape);
                settle();
                showTile(QStringLiteral("charcoal"));
                penTap(lab->tipPicker(), lab->tipTileRect(QStringLiteral("charcoal")).center());
                const BrushPreset& mine = tools->preset();
                check(QStringLiteral("rename it and give it the charcoal tip: all on the copy only"),
                      mine.id == copyId && mine.name == QLatin1String("Liner") && mine.tip == QLatin1String("charcoal") && !library.isModified(copyId)
                          && !library.isModified(QStringLiteral("studio-pen")) && library.find(QStringLiteral("studio-pen"))->tip.isEmpty());
                capture(QStringLiteral("brush-custom"));
                tapNamed("lab-done");

                // Back in the library it heads its set; + starts another one from scratch, in the lab.
                tapNamed("tool-brush");
                const bool heads = library.inCategory(QStringLiteral("Inking")).first()->id == copyId && named("brush-list");
                capture(QStringLiteral("brush-library-custom"));
                const bool tappedNew = tapNamed("brush-new");
                settle();
                const QString freshId = tools->preset().id;
                check(QStringLiteral("tap + in the library -> \"%1\" in the same set, opened in the lab").arg(tools->preset().name),
                      heads && tappedNew && freshId != copyId && tools->preset().custom && tools->preset().name == QLatin1String("New Brush")
                          && tools->preset().category == QLatin1String("Inking") && lab->isVisible() && lab->brushId() == freshId
                          && int(library.presets().size()) == shippedCount + 2);

                // Delete takes two taps; one alone does nothing.
                lab->setSection(BrushLab::About);
                settle();
                auto* del = lab->findChild<QPushButton*>(QStringLiteral("lab-delete"));
                tapNamed("lab-delete");
                const bool armed = library.find(freshId) && del && del->text() == QLatin1String("Really delete?");
                tapNamed("lab-delete");
                settle();
                const bool goneOne = !library.find(freshId) && tools->preset().id == copyId && !lab->isVisible();
                window.showBrushLab();
                settle();
                lab->setSection(BrushLab::About);
                settle();
                tapNamed("lab-delete");
                tapNamed("lab-delete");
                settle();
                check(QStringLiteral("Delete asks twice, then the brush is gone, the lab closes, and the next brush in the set takes over"),
                      armed && goneOne && !library.find(copyId) && !lab->isVisible() && !tools->preset().custom
                          && tools->preset().category == QLatin1String("Inking") && int(library.presets().size()) == shippedCount
                          && engine.history().undoCount() == strokesOnCanvas);
            }
        }

        window.showForScreenshot(QStringLiteral("main"));

        // Groups and masks in the layer panel, operated with the pen.
        {
            auto* panel = window.findChild<LayerPanel*>();
            auto* layersBtn = window.findChild<QWidget*>(QStringLiteral("layers-button"));
            auto* list = window.findChild<QWidget*>(QStringLiteral("layer-list"));
            auto* addGroup = window.findChild<QWidget*>(QStringLiteral("layer-add-group"));
            auto inWindow = [&](const QPointF& listPos) { return QPointF(list->mapTo(&window, listPos.toPoint())); };
            auto part = [&](int id, const char* what) { return panel->rowPart(id, QLatin1String(what)); };
            // The list is longer than the panel here, so a plain pen drag scrolls it;
            // resting on a layer picks it up.
            auto rest = [](int ms) {
                QElapsedTimer t;
                t.start();
                while (t.elapsed() < ms)
                    QApplication::processEvents(QEventLoop::AllEvents, 10);
            };
            auto penCarry = [&](const QPointF& from, const QPointF& to, int steps = 24) {
                penWindow(QEvent::TabletMove, from, 0.0, Qt::NoButton, Qt::NoButton);
                penWindow(QEvent::TabletPress, from, 0.5, Qt::LeftButton, Qt::LeftButton);
                rest(420);
                for (int i = 1; i <= steps; ++i)
                    penWindow(QEvent::TabletMove, from + (to - from) * (double(i) / steps), 0.6, Qt::NoButton, Qt::LeftButton);
                penWindow(QEvent::TabletRelease, to, 0.0, Qt::LeftButton, Qt::NoButton);
                settle();
            };

            const int paint = engine.addLayer();
            engine.setLayerName(paint, QStringLiteral("Paint"));
            engine.fillLayer(paint, QColor(235, 120, 40));
            const int sketch = engine.addLayer();
            engine.setLayerName(sketch, QStringLiteral("Sketch"));
            penTap(layersBtn, centre(layersBtn));
            penTap(addGroup, centre(addGroup));
            const int folder = engine.activeLayerId();
            check(QStringLiteral("tap the folder button adds a group"), engine.layerInfo(folder).isGroup);

            // Drop Sketch onto the group, then Paint underneath it inside.
            // First the plain drag: the list moves, the layers stay in their order.
            auto* layerScroll = panel->findChild<QScrollArea*>();
            const QVector<LayerInfo> orderBefore = engine.layers();
            const int scrollBefore = layerScroll->verticalScrollBar()->value();
            penDrag(inWindow(part(sketch, "row").center() + QPointF(0, 150)), inWindow(part(sketch, "row").center()));
            const int scrolledTo = layerScroll->verticalScrollBar()->value();
            bool sameOrder = engine.layers().size() == orderBefore.size() && engine.activeLayerId() == folder;
            for (int i = 0; sameOrder && i < orderBefore.size(); ++i)
                sameOrder = engine.layers()[i].id == orderBefore[i].id && engine.layers()[i].parent == orderBefore[i].parent;
            layerScroll->verticalScrollBar()->setValue(scrollBefore);
            settle();
            check(QStringLiteral("a pen drag on a long layer list scrolls it (%1 px) and moves nothing").arg(scrolledTo - scrollBefore),
                  scrolledTo > scrollBefore + 100 && sameOrder);

            penCarry(inWindow(part(sketch, "row").center()), inWindow(part(folder, "row").center()));
            check(QStringLiteral("rest on a layer, then drag it onto a group: it goes inside"), engine.layerInfo(sketch).parent == folder);
            const QRectF sketchRow = part(sketch, "row");
            penCarry(inWindow(part(paint, "row").center()), inWindow(QPointF(60, sketchRow.bottom() + 2)));
            check(QStringLiteral("drag a layer below a member, indented, joins the group"),
                  engine.layerInfo(paint).parent == folder && engine.layerInfo(paint).depth == 1);

            penTap(list, part(folder, "chevron").center());
            const bool folded = engine.layerInfo(folder).collapsed && part(sketch, "row").isEmpty();
            capture(QStringLiteral("layers-folded"));
            penTap(list, part(folder, "chevron").center());
            check(QStringLiteral("tap the arrow folds and unfolds the group"), folded && !part(sketch, "row").isEmpty());

            // A mask: tap its thumbnail to paint on it, the layer's to go back.
            engine.addLayerMask(paint);
            settle();
            penTap(list, part(paint, "thumb").center());
            const bool onLayer = !engine.maskEditing() && engine.activeLayerId() == paint;
            penTap(list, part(paint, "mask").center());
            check(QStringLiteral("tap the mask thumbnail to paint on the mask, the layer's to go back"), onLayer && engine.maskEditing());
            capture(QStringLiteral("layers-groups"));

            // With the mask targeted, a black stroke on the canvas hides the paint there.
            penWindow(QEvent::TabletPress, QPointF(700, 820), 0.4, Qt::LeftButton, Qt::LeftButton);   // closes the panel
            penWindow(QEvent::TabletRelease, QPointF(700, 820), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            engine.undo(true);   // that tap left a dot on the mask
            tools->setPreset(QStringLiteral("round-brush"));
            tools->setSize(70);
            tools->setColor(Qt::black);
            const QColor shown = canvasPixel(QPointF(600, 250));
            penDrag(QPointF(400, 250), QPointF(800, 250), 40);
            const QColor hiddenNow = canvasPixel(QPointF(600, 250));
            check(QStringLiteral("painting black on the mask hides the layer there"),
                  shown.red() > 200 && shown.green() < 160 && hiddenNow != shown && engine.layerInfo(paint).hasMask);

            // Drag Sketch back out to the very top of the stack.
            penTap(layersBtn, centre(layersBtn));
            penCarry(inWindow(part(sketch, "row").center()), inWindow(QPointF(12, 4)));
            check(QStringLiteral("drag a layer to the top edge, far left, takes it out of the group"),
                  engine.layerInfo(sketch).parent == 0 && engine.layers().last().id == sketch);

            // The group's own mask works the same way, over everything in the group.
            engine.addLayerMask(folder);
            settle();
            penTap(list, part(folder, "thumb").center());
            const bool onGroup = !engine.maskEditing() && engine.activeLayerId() == folder;
            penTap(list, part(folder, "mask").center());
            const bool onGroupMask = engine.maskEditing() && engine.activeLayerId() == folder;
            const int beforeClose = engine.history().undoCount();
            penWindow(QEvent::TabletPress, QPointF(700, 820), 0.4, Qt::LeftButton, Qt::LeftButton);   // closes the panel
            penWindow(QEvent::TabletRelease, QPointF(700, 820), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            if (engine.history().undoCount() > beforeClose)
                engine.undo(true);
            const QColor groupShown = canvasPixel(QPointF(600, 470));
            penDrag(QPointF(400, 470), QPointF(800, 470), 40);
            const QColor groupHidden = canvasPixel(QPointF(600, 470));
            check(QStringLiteral("a group's mask: its thumbnail is tapped like a layer's, and black painted on it hides the group there (%1 -> %2)")
                      .arg(groupShown.name(), groupHidden.name()),
                  onGroup && onGroupMask && groupShown.red() > 200 && groupShown.green() < 160 && groupHidden != groupShown
                      && engine.layerInfo(folder).hasMask && engine.maskEditing());
            penTap(layersBtn, centre(layersBtn));
            capture(QStringLiteral("layers-masked"));
            window.showForScreenshot(QStringLiteral("main"));
        }

        while (engine.history().undoCount() > undoBase)
            engine.undo(true);
        engine.setActiveLayer(startLayer);
        for (const QString& line : log)
            qInfo().noquote() << "selection, fill, brush settings:" << line;
        qInfo().noquote() << "selection, fill, brush settings:" << (toolsOk ? "ok" : "FAILED");
    }

    tools->setColor(QColor(20, 150, 90));
    mouse(QEvent::MouseButtonPress, QPointF(260, 860), Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 40; ++i) {
        mouse(QEvent::MouseMove, QPointF(260 + i * 9, 860 - std::sin(i * 0.25) * 30), Qt::NoButton, Qt::LeftButton);
        QApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    mouse(QEvent::MouseButtonRelease, QPointF(620, 860), Qt::LeftButton, Qt::NoButton);

    qInfo() << "input: pen stroke";
    auto pen = [&](QEvent::Type t, const QPointF& pos, qreal pressure, Qt::MouseButton b, Qt::MouseButtons held) {
        QTabletEvent ev(t, &stylus, pos, view->mapToGlobal(pos), pressure, 20.0f, 10.0f, 0.0f, 0.0, 0.0f,
                        Qt::NoModifier, b, held);
        QApplication::sendEvent(view, &ev);
    };
    tools->setTool(StrokeTool::Brush);
    tools->setPreset(QStringLiteral("studio-pen"));
    tools->setSize(40);
    tools->setColor(QColor(130, 40, 200));
    pen(QEvent::TabletPress, QPointF(900, 880), 0.05, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 60; ++i) {
        const double t = i / 60.0;
        pen(QEvent::TabletMove, QPointF(900 + t * 480, 880 - std::sin(t * 6.283) * 50), std::sin(t * 3.1416), Qt::NoButton,
            Qt::LeftButton);
        QApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    pen(QEvent::TabletRelease, QPointF(1380, 880), 0.0, Qt::LeftButton, Qt::NoButton);

    qInfo() << "input: pick";
    // Right-click picks the colour under the pointer (the pink oil stroke).
    mouse(QEvent::MouseButtonPress, QPointF(800, 235), Qt::RightButton, Qt::RightButton);
    mouse(QEvent::MouseButtonRelease, QPointF(800, 235), Qt::RightButton, Qt::NoButton);
    const QColor picked = tools->color();
    shot(QStringLiteral("input"));

    qInfo() << "input: zoom";
    const int strokesAdded = engine.history().undoCount() - undoBefore;

    // ---- QuickShape through the real input path: draw, rest, resize, edit.
    auto wait = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            QApplication::processEvents(QEventLoop::AllEvents, 4);
            QThread::msleep(3);
        }
    };
    // `pressure` maps progress along the stroke (0..1) to pen pressure.
    auto drawAndRest = [&](const std::vector<QPointF>& pts, const std::function<double(double)>& pressure = {}) {
        auto at = [&](size_t i) { return pressure ? pressure(double(i) / double(pts.size() - 1)) : 0.6; };
        pen(QEvent::TabletPress, pts.front(), at(0), Qt::LeftButton, Qt::LeftButton);
        for (size_t i = 1; i < pts.size(); ++i) {
            pen(QEvent::TabletMove, pts[i], at(i), Qt::NoButton, Qt::LeftButton);
            wait(5);
        }
        wait(800);   // pen resting on the surface
    };
    tools->setPreset(QStringLiteral("studio-pen"));
    tools->setSize(16);
    tools->setColor(QColor(24, 24, 30));
    bool shapeOk = true;
    QStringList shapeLog;

    // 1. A wobbly circle, then pull the pen outwards to grow it before lifting.
    const QPointF circleCentre(470, 500);
    std::vector<QPointF> rough = wobble(ellipseStroke(circleCentre, 100, 96, 0, 372), 2.5, 0.8);
    const int beforeCircle = engine.history().undoCount();
    drawAndRest(rough);
    const QString circleName = view->shapeName();
    shapeOk = shapeOk && circleName == QLatin1String("Circle");
    const QPointF restPos = rough.back();
    QPointF outward = restPos - circleCentre;
    outward /= std::hypot(outward.x(), outward.y());
    for (int i = 1; i <= 12; ++i) {
        pen(QEvent::TabletMove, restPos + outward * (i * 3.0), 0.6, Qt::NoButton, Qt::LeftButton);
        wait(5);
    }
    capture(QStringLiteral("shape-held"));
    pen(QEvent::TabletRelease, restPos + outward * 36.0, 0.0, Qt::LeftButton, Qt::NoButton);
    wait(30);
    const bool oneStep = engine.history().undoCount() == beforeCircle + 1;
    shapeOk = shapeOk && oneStep && view->shapeEditOffered();
    shapeLog << QStringLiteral("held stroke -> %1, one undo step %2, edit offered %3")
                    .arg(circleName, oneStep ? "yes" : "NO", view->shapeEditOffered() ? "yes" : "NO");
    shot(QStringLiteral("shape-offer"));

    // 2. Edit mode: stretch a handle into an ellipse, then ease back until it snaps.
    shot(QStringLiteral("shape"));
    const bool editing = view->shapeEditing();
    QVector<QPointF> handles = view->shapeHandlePositions();
    QString stretched, snappedBack;
    if (editing && handles.size() == 4) {
        const QPointF h0 = handles[0];
        mouse(QEvent::MouseButtonPress, h0, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 10; ++i)
            mouse(QEvent::MouseMove, h0 + QPointF(i * 7.0, 0), Qt::NoButton, Qt::LeftButton);
        settle();   // the shape follows the pointer once the moves at hand have been read
        stretched = view->shapeVariant();
        capture(QStringLiteral("shape-ellipse"));
        for (int i = 9; i >= 1; --i)
            mouse(QEvent::MouseMove, h0 + QPointF(i * 7.0, 0), Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, h0 + QPointF(3.0, 1.0), Qt::NoButton, Qt::LeftButton);   // close, not exact
        settle();   // the shape follows the pointer once the moves at hand have been read
        snappedBack = view->shapeVariant();
        capture(QStringLiteral("shape-snap"));
        mouse(QEvent::MouseButtonRelease, h0 + QPointF(3.0, 1.0), Qt::LeftButton, Qt::NoButton);
    }
    view->setShapeVariant(QStringLiteral("ellipse"));
    view->setShapeVariant(QStringLiteral("circle"));
    const QString viaButton = view->shapeVariant();
    view->finishShapeEdit(true);
    wait(20);
    const bool stillOneStep = engine.history().undoCount() == beforeCircle + 1;
    shapeOk = shapeOk && editing && stretched == QLatin1String("ellipse") && snappedBack == QLatin1String("circle")
              && viaButton == QLatin1String("circle") && stillOneStep && view->shapeEditOffered();
    shapeLog << QStringLiteral("edit mode %1: stretched -> %2, eased back -> %3, Circle button -> %4, still one undo step %5")
                    .arg(editing ? "opened" : "DID NOT OPEN", stretched, snappedBack, viaButton, stillOneStep ? "yes" : "NO");

    // 3. A rough rectangle and a straight-ish line.
    std::vector<QPointF> box = wobble(soften(densify(closedFrom(boxCorners(QPointF(1150, 470), 300, 180, 0), 0.4), false, 4.0), 4), 2.0, 1.3);
    drawAndRest(box);
    const QString boxName = view->shapeName();
    pen(QEvent::TabletRelease, box.back(), 0.0, Qt::LeftButton, Qt::NoButton);
    shot(QStringLiteral("shape"));
    const bool quadFamily = view->shapeFamily() == CanvasWidget::ShapeFamily::Quad;
    handles = view->shapeHandlePositions();
    QString squared;
    if (view->shapeEditing() && handles.size() == 4) {
        // Drag the bottom-right corner in until width and height nearly match.
        const QPointF corner = handles[2];
        mouse(QEvent::MouseButtonPress, corner, Qt::LeftButton, Qt::LeftButton);
        // The box was drawn 300 x 180: stop a few pixels short of equal and let the magnet finish.
        for (int i = 1; i <= 19; ++i)
            mouse(QEvent::MouseMove, corner - QPointF(i * 6.0, 0), Qt::NoButton, Qt::LeftButton);
        settle();   // the shape follows the pointer once the moves at hand have been read
        squared = view->shapeVariant();
        capture(QStringLiteral("shape-square"));
        mouse(QEvent::MouseButtonRelease, corner - QPointF(114, 0), Qt::LeftButton, Qt::NoButton);
    }
    view->finishShapeEdit(true);
    shapeOk = shapeOk && boxName == QLatin1String("Rectangle") && quadFamily && squared == QLatin1String("square");
    shapeLog << QStringLiteral("rectangle -> %1, corner dragged to equal sides -> %2").arg(boxName, squared);

    std::vector<QPointF> lineStroke = wobble(densify({QPointF(330, 760), QPointF(900, 771)}, false, 5.0), 2.0, 0.5);
    drawAndRest(lineStroke);
    const QString lineName = view->shapeName();
    pen(QEvent::TabletRelease, lineStroke.back(), 0.0, Qt::LeftButton, Qt::NoButton);
    wait(20);
    shapeOk = shapeOk && lineName == QLatin1String("Line");
    shapeLog << QStringLiteral("line -> %1").arg(lineName);

    // 4. A snapped shape keeps the stroke's changing line weight. Measured on
    // a layer of its own, as the run of covered pixels across the line.
    {
        const int inkLayer = engine.addLayer();
        auto thickness = [&](const QPointF& widgetPos, bool acrossVertically) {
            const QByteArray raw = engine.layerPixels(inkLayer);
            const auto* px = reinterpret_cast<const quint16*>(raw.constData());
            const QSize cs = engine.canvasSize();
            const QPointF c = view->widgetToCanvas(widgetPos);
            int covered = 0;
            for (int d = -60; d <= 60; ++d) {
                const int x = int(c.x()) + (acrossVertically ? 0 : d), y = int(c.y()) + (acrossVertically ? d : 0);
                if (x < 0 || y < 0 || x >= cs.width() || y >= cs.height())
                    continue;
                covered += px[(qsizetype(y) * cs.width() + x) * 4 + 3] >= 0x3800;   // alpha of at least one half
            }
            return covered;
        };
        tools->setPreset(QStringLiteral("studio-pen"));
        tools->setSize(30);

        // A line pressed harder and harder.
        std::vector<QPointF> swell = wobble(densify({QPointF(300, 118), QPointF(1250, 126)}, false, 6.0), 2.0, 1.1);
        drawAndRest(swell, [](double t) { return 0.1 + 0.9 * t; });
        const QString swellName = view->shapeName();
        pen(QEvent::TabletRelease, swell.back(), 0.0, Qt::LeftButton, Qt::NoButton);
        wait(20);
        const int thin = thickness(QPointF(440, 119), true);
        const int mid = thickness(QPointF(780, 122), true);
        const int thick = thickness(QPointF(1150, 125), true);
        const bool lineOk = swellName == QLatin1String("Line") && thin > 2 && thin < mid && mid < thick && thin * 10 < thick * 6;
        shapeLog << QStringLiteral("line drawn light to heavy -> %1, width %2 / %3 / %4 px along it").arg(swellName).arg(thin).arg(mid).arg(thick);

        // Tap the line of a shape just drawn and it reopens for editing; tap away from it and it does not.
        {
            const int beforeTap = engine.history().undoCount();
            std::vector<QPointF> ringStroke = wobble(ellipseStroke(QPointF(520, 430), 90, 88, 0, 370), 1.5, 0.3);
            drawAndRest(ringStroke, [](double) { return 0.7; });
            pen(QEvent::TabletRelease, ringStroke.back(), 0.0, Qt::LeftButton, Qt::NoButton);
            wait(20);
            const bool offered = view->shapeEditOffered();
            const QPointF onLine(520 + 90, 430);   // the right-hand side of the circle
            pen(QEvent::TabletPress, onLine, 0.5, Qt::LeftButton, Qt::LeftButton);
            pen(QEvent::TabletMove, onLine + QPointF(1, 1), 0.5, Qt::NoButton, Qt::LeftButton);
            pen(QEvent::TabletRelease, onLine + QPointF(1, 1), 0.0, Qt::LeftButton, Qt::NoButton);
            wait(20);
            const bool reopened = view->shapeEditing() && engine.history().undoCount() == beforeTap;   // the shape is live again, and no dot was added
            view->finishShapeEdit(true);
            wait(20);
            // A stroke that starts on the line and goes somewhere is just a stroke.
            const bool offeredAgain = view->shapeEditOffered();
            pen(QEvent::TabletPress, onLine, 0.5, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 12; ++i)
                pen(QEvent::TabletMove, onLine + QPointF(i * 6, i * 2), 0.5, Qt::NoButton, Qt::LeftButton);
            pen(QEvent::TabletRelease, onLine + QPointF(72, 24), 0.0, Qt::LeftButton, Qt::NoButton);
            wait(20);
            const bool drew = !view->shapeEditing() && !view->shapeEditOffered() && engine.history().undoCount() == beforeTap + 2;
            const bool tapOk = offered && reopened && offeredAgain && drew;
            shapeOk = shapeOk && tapOk;
            shapeLog << QStringLiteral("tap on the circle's line reopens it for editing%1; a stroke starting there is just a stroke").arg(tapOk ? QString() : QStringLiteral(" (FAILED)"));
            engine.undo(true);
            engine.undo(true);
        }

        // A squiggle of round bends, like the one that used to come out as a
        // row of straight segments: it must stay a curve, in one undo step.
        {
            const int beforeCurve = engine.history().undoCount();
            std::vector<QPointF> squiggle = wobble(meanderStroke(QPointF(330, 560), 34, 46, 7), 1.5, 0.8);
            drawAndRest(squiggle, [](double) { return 0.7; });
            const QString squiggleName = view->shapeName();
            pen(QEvent::TabletRelease, squiggle.back(), 0.0, Qt::LeftButton, Qt::NoButton);
            wait(20);
            // Beside the top of the first bend the line is there; a straightened
            // version would cut across below it and leave this spot bare.
            const int onBend = thickness(QPointF(330 + 34, 560 - 46 - 34), true);
            const bool curveOk = squiggleName == QLatin1String("Curve") && onBend > 4 && engine.history().undoCount() == beforeCurve + 1
                                 && view->shapeEditOffered();
            shapeOk = shapeOk && curveOk;
            shapeLog << QStringLiteral("squiggle of seven round bends -> %1%2, the top of a bend is still drawn (%3 px of line there)")
                            .arg(squiggleName, curveOk ? QString() : QStringLiteral(" (FAILED)")).arg(onBend);
            engine.undo(true);
        }

        // A circle that gets heavier on the way round. It starts at 20 degrees
        // and runs clockwise, so the bottom is early (light) and the right
        // side is late (heavy).
        const QPointF ringCentre(1000, 690);
        std::vector<QPointF> ring = wobble(ellipseStroke(ringCentre, 105, 102, 0, 372), 2.0, 0.4);
        drawAndRest(ring, [](double t) { return 0.2 + 0.8 * t; });
        const QString ringName = view->shapeName();
        pen(QEvent::TabletRelease, ring.back(), 0.0, Qt::LeftButton, Qt::NoButton);
        wait(20);
        const int early = thickness(ringCentre + QPointF(0, 104), true);
        const int late = thickness(ringCentre + QPointF(104, 0), false);
        const bool ringOk = ringName == QLatin1String("Circle") && early > 2 && early * 10 < late * 7;
        shapeLog << QStringLiteral("circle drawn light to heavy -> %1, width %2 px early on, %3 px near the end").arg(ringName).arg(early).arg(late);

        // Editing it keeps the profile: stretch the circle and the same sides stay light and heavy.
        shot(QStringLiteral("shape"));
        const QVector<QPointF> ringHandles = view->shapeHandlePositions();
        bool editKeeps = false;
        if (view->shapeEditing() && ringHandles.size() == 4) {
            mouse(QEvent::MouseButtonPress, ringHandles[1], Qt::LeftButton, Qt::LeftButton);   // the lower handle
            for (int i = 1; i <= 8; ++i)
                mouse(QEvent::MouseMove, ringHandles[1] + QPointF(0, i * 5.0), Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, ringHandles[1] + QPointF(0, 40), Qt::LeftButton, Qt::NoButton);
            capture(QStringLiteral("shape-dynamic"));
            view->finishShapeEdit(true);
            wait(20);
            const QVector<QPointF> after = view->shapeHandlePositions();
            const int earlyAfter = after.size() == 4 ? thickness(after[1], true) : 0;
            const int lateAfter = after.size() == 4 ? thickness(after[0], false) : 0;
            editKeeps = earlyAfter > 2 && earlyAfter * 10 < lateAfter * 7;
            shapeLog << QStringLiteral("after stretching it in edit mode: %1 px and %2 px").arg(earlyAfter).arg(lateAfter);
        }
        shapeOk = shapeOk && lineOk && ringOk && editKeeps;
    }

    // Drawing again withdraws the offer; undo removes a whole shape at once.
    engine.undo(true);
    shapeOk = shapeOk && !view->shapeEditOffered();
    engine.redo(true);
    shot(QStringLiteral("shapes-done"));
    for (const QString& line : shapeLog)
        qInfo().noquote() << "quickshape:" << line;
    qInfo().noquote() << "quickshape:" << (shapeOk ? "ok" : "FAILED");

    // ---- The points of a shape: added, taken out, made smooth or sharp, with
    // the keys on the shape itself and from the edit bar.
    bool pointsOk = true;
    {
        using Kind = QuickShape::Kind;
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            pointsOk = pointsOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        auto button = [&](QEvent::Type t, const QPointF& pos, Qt::KeyboardModifiers mods) {
            const bool move = t == QEvent::MouseMove, release = t == QEvent::MouseButtonRelease;
            QMouseEvent ev(t, pos, view->mapToGlobal(pos), move ? Qt::NoButton : Qt::LeftButton, release ? Qt::NoButton : Qt::LeftButton, mods);
            QApplication::sendEvent(view, &ev);
        };
        auto clickWith = [&](const QPointF& pos, Qt::KeyboardModifiers mods) {
            button(QEvent::MouseButtonPress, pos, mods);
            button(QEvent::MouseButtonRelease, pos, mods);
            settle();
        };
        auto dragWith = [&](const QPointF& from, const QPointF& to, Qt::KeyboardModifiers mods) {
            button(QEvent::MouseButtonPress, from, mods);
            for (int i = 1; i <= 8; ++i)
                button(QEvent::MouseMove, from + (to - from) * (i / 8.0), mods);
            settle();   // moves are taken up once per frame
            button(QEvent::MouseButtonRelease, to, mods);
            settle();
        };
        auto farthest = [](const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); };
        const int layer = engine.addLayer();
        // Which pixels of the layer the line covers.
        auto ink = [&] {
            const QByteArray raw = engine.layerPixels(layer);
            const auto* px = reinterpret_cast<const quint16*>(raw.constData());
            const qsizetype count = raw.size() / 8;
            std::vector<bool> covered(size_t(count), false);
            for (qsizetype i = 0; i < count; ++i)
                covered[size_t(i)] = px[i * 4 + 3] >= 0x3800 && px[i * 4 + 3] < 0x7c00;   // alpha of at least one half
            return covered;
        };
        // The share of the line that is somewhere else in `b` than in `a`.
        auto moved = [](const std::vector<bool>& a, const std::vector<bool>& b) {
            size_t differ = 0, total = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                differ += a[i] != b[i];
                total += a[i];
            }
            return total ? double(differ) / double(total) : 1.0;
        };
        // A place on the outline well away from every point, about `share` of the way along.
        auto onLine = [&](double share) {
            const std::vector<PathPoint> line = view->editedShape().path(2.0);
            const QVector<QPointF> handles = view->shapeHandlePositions();
            for (size_t i = size_t(share * double(line.size())); i < line.size(); ++i) {
                const QPointF at = view->canvasToWidget(line[i].pos);
                bool clear = true;
                for (const QPointF& h : handles)
                    clear = clear && farthest(h, at) > 26.0;
                if (clear)
                    return at;
            }
            return QPointF();
        };
        auto nodeAt = [&](int k) { return view->canvasToWidget(view->editedShape().nodes[size_t(k)].pos); };
        // The outline in canvas pixels, and how far two of them stray from each other at most.
        auto lineOf = [&] {
            std::vector<QPointF> out;
            for (const PathPoint& pt : view->editedShape().path(1.5))
                out.push_back(pt.pos);
            return out;
        };
        auto strays = [&](const std::vector<QPointF>& a, const std::vector<QPointF>& b) {
            double worst = 0.0;
            for (const std::vector<QPointF>* from : {&a, &b}) {
                const std::vector<QPointF>& to = from == &a ? b : a;
                for (const QPointF& q : *from) {
                    double nearest = std::numeric_limits<double>::max();
                    for (const QPointF& r : to)
                        nearest = std::min(nearest, farthest(q, r));
                    worst = std::max(worst, nearest);
                }
            }
            return worst;
        };
        auto points = [&] { return int(view->editedShape().nodes.size()); };
        tools->setPreset(QStringLiteral("studio-pen"));
        tools->setSize(14);
        tools->setColor(QColor(24, 24, 30));

        // A wave: a curve, which opens as a path of smooth points.
        std::vector<QPointF> wave;
        for (double x = 0.0; x <= 620.0; x += 3.0)
            wave.emplace_back(240.0 + x, 300.0 - 95.0 * std::sin(x / 620.0 * 9.42478));
        wave = wobble(wave, 1.2, 0.4);
        const int beforeWave = engine.history().undoCount();
        drawAndRest(wave, [](double) { return 0.7; });
        const QString waveName = view->shapeName();
        pen(QEvent::TabletRelease, wave.back(), 0.0, Qt::LeftButton, Qt::NoButton);
        wait(20);
        const std::vector<bool> drawn = ink();   // while a shape is open it is a live stroke, not yet on the layer
        const bool opened = view->startShapeEdit();
        settle();
        const int n0 = opened ? points() : 0;
        const std::vector<QPointF> waveLine = lineOf();
        check(QStringLiteral("a wave -> %1, open for editing as %2 smooth points").arg(waveName).arg(n0),
              waveName == QLatin1String("Curve") && opened && view->editedShape().kind == Kind::Path && n0 >= 4 && view->shapePoint() < 0);

        // Ctrl + click on the line: one more point, and the line stays where it is.
        const QPointF spot = onLine(0.3);
        clickWith(spot, Qt::ControlModifier);
        const int added = view->shapePoint();
        const double addedShift = strays(waveLine, lineOf());
        check(QStringLiteral("Ctrl + click on the line adds a point there (%1 points, the line within %2 px of where it was)").arg(points()).arg(addedShift, 0, 'f', 2),
              points() == n0 + 1 && added >= 0 && farthest(nodeAt(added), spot) < 4.0 && addedShift < 1.6);

        // Ctrl + press on the line and keep going: the new point comes along.
        const QPointF spot2 = onLine(0.62);
        const QPointF dropped = spot2 + QPointF(18, 70);
        dragWith(spot2, dropped, Qt::ControlModifier);
        const int pulled = view->shapePoint();
        check(QStringLiteral("Ctrl + drag from the line puts a point where the pointer lets go"),
              points() == n0 + 2 && pulled >= 0 && farthest(nodeAt(pulled), dropped) < 3.0 && strays(waveLine, lineOf()) > 30.0);

        // Alt + click: smooth to sharp and back. Alt + drag only moves.
        clickWith(nodeAt(pulled), Qt::AltModifier);
        const bool sharp = !view->editedShape().nodes[size_t(pulled)].smooth && !view->shapePointSmooth() && view->shapeArmPositions().isEmpty();
        capture(QStringLiteral("shape-corner"));
        clickWith(nodeAt(pulled), Qt::AltModifier);
        const bool smoothAgain = view->editedShape().nodes[size_t(pulled)].smooth && view->shapePointSmooth() && points() == n0 + 2;
        const QPointF before = nodeAt(pulled);
        dragWith(before, before + QPointF(-30, 22), Qt::AltModifier);
        const bool altMoved = view->editedShape().nodes[size_t(pulled)].smooth && farthest(nodeAt(pulled), before + QPointF(-30, 22)) < 3.0;
        check(QStringLiteral("Alt + click makes a point sharp, again makes it smooth; Alt + drag moves it and leaves it as it is"), sharp && smoothAgain && altMoved);

        // A chosen smooth point shows its two handles; one drags, the other stays in line.
        clickWith(nodeAt(1), Qt::NoModifier);
        const QVector<QPointF> arms = view->shapeArmPositions();
        bool armsOk = view->shapePoint() == 1 && arms.size() == 2 && farthest(arms[0], nodeAt(1)) > 8.0 && farthest(arms[1], nodeAt(1)) > 8.0;
        if (armsOk) {
            const std::vector<QPointF> beforeArm = lineOf();
            const QPointF target = arms[1] + QPointF(34, -46);
            dragWith(arms[1], target, Qt::NoModifier);
            const QVector<QPointF> after = view->shapeArmPositions();
            const QPointF at = nodeAt(1);
            const QPointF in = after.value(0) - at, out = after.value(1) - at;
            armsOk = after.size() == 2 && farthest(after[1], target) < 3.0 && std::abs(in.x() * out.y() - in.y() * out.x()) < 0.02 * farthest(after[0], at) * farthest(after[1], at)
                     && in.x() * out.x() + in.y() * out.y() < 0.0 && std::abs(farthest(after[0], at) - farthest(arms[0], at)) < 2.0 && strays(beforeArm, lineOf()) > 4.0
                     && view->shapePoint() == 1;
        }
        capture(QStringLiteral("shape-points"));
        check(QStringLiteral("a chosen smooth point shows a handle either side; dragging one bends the line and keeps the other in line"), armsOk);

        // Ctrl + click on a point takes it out.
        clickWith(nodeAt(pulled), Qt::ControlModifier);
        check(QStringLiteral("Ctrl + click on a point removes it (%1 points)").arg(points()), points() == n0 + 1 && view->shapePoint() < 0);

        // The same from the edit bar, with the pen and no keys.
        // Bars of earlier edits may still be waiting to be deleted; the one in use is the one on show.
        auto inBar = [&](const QString& name) -> QAbstractButton* {
            for (QAbstractButton* b : window.findChildren<QAbstractButton*>(name)) {
                if (b->isVisible())
                    return b;
            }
            return nullptr;
        };
        QAbstractButton* addButton = inBar(QStringLiteral("shape-point-add"));
        QAbstractButton* removeButton = inBar(QStringLiteral("shape-point-remove"));
        QAbstractButton* smoothButton = inBar(QStringLiteral("shape-point-smooth"));
        bool barOk = addButton && removeButton && smoothButton && !removeButton->isEnabled() && !smoothButton->isEnabled();
        if (barOk) {
            addButton->click();
            settle();
            const bool armed = view->shapeAddArmed();
            const QPointF spot3 = onLine(0.8);
            pen(QEvent::TabletPress, spot3, 0.5, Qt::LeftButton, Qt::LeftButton);
            pen(QEvent::TabletRelease, spot3, 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            const int byPen = view->shapePoint();
            const bool penAdded = points() == n0 + 2 && byPen >= 0 && farthest(nodeAt(byPen), spot3) < 4.0 && !view->shapeAddArmed();
            const bool enabled = removeButton->isEnabled() && smoothButton->isEnabled();
            smoothButton->click();
            settle();
            const bool madeSharp = !view->editedShape().nodes[size_t(byPen)].smooth;
            smoothButton->click();
            settle();
            const bool madeSmooth = view->editedShape().nodes[size_t(byPen)].smooth;
            removeButton->click();
            settle();
            barOk = armed && penAdded && enabled && madeSharp && madeSmooth && points() == n0 + 1 && view->shapePoint() < 0 && !removeButton->isEnabled();
            if (!barOk)
                log << QStringLiteral("       armed %1, added by pen %2, buttons on %3, sharp %4, smooth %5, points %6 of %7").arg(armed).arg(penAdded).arg(enabled).arg(madeSharp).arg(madeSmooth).arg(points()).arg(n0 + 1);
        } else {
            log << QStringLiteral("       buttons found %1 %2 %3, remove enabled %4, smooth enabled %5").arg(addButton != nullptr).arg(removeButton != nullptr).arg(smoothButton != nullptr)
                       .arg(removeButton && removeButton->isEnabled()).arg(smoothButton && smoothButton->isEnabled());
        }
        check(QStringLiteral("the edit bar does the same for the pen: add, sharp, smooth, remove"), barOk);

        // Delete takes out the chosen point, and leaves the layer alone.
        clickWith(nodeAt(added), Qt::NoModifier);
        bool deleteOk = view->shapePoint() == added;
        for (QShortcut* sc : window.findChildren<QShortcut*>()) {
            if (sc->key() == QKeySequence(Qt::Key_Delete))
                emit sc->activated();
        }
        settle();
        const int underneath = engine.history().undoCount();
        deleteOk = deleteOk && points() == n0 && view->shapeEditing() && underneath == beforeWave;   // nothing new in the history: no layer was cleared
        check(QStringLiteral("Delete removes the chosen point, not the layer"), deleteOk);

        // A tap beside the shape lets go of the point; Cancel puts back what was drawn.
        clickWith(nodeAt(2), Qt::NoModifier);
        const bool chosen = view->shapePoint() == 2;
        const QPointF beside = nodeAt(2) + QPointF(0, 150);
        clickWith(beside, Qt::NoModifier);
        const bool letGo = view->shapePoint() < 0;

        // From the bar: the two ends joined, then the shape cut open again at a chosen point.
        {
            QAbstractButton* closeButton = inBar(QStringLiteral("shape-close"));
            bool closeOk = closeButton && closeButton->isEnabled() && !view->shapeClosed();
            const int open = points();
            if (closeOk) {
                closeButton->click();
                settle();
                const bool joined = view->shapeClosed() && points() == open;
                clickWith(nodeAt(2), Qt::NoModifier);
                const QPointF cutAt = nodeAt(2);
                const std::vector<QPointF> ringBefore = lineOf();
                closeButton->click();
                settle();
                const int last = points() - 1;
                const double shift = strays(ringBefore, lineOf());
                const bool parted = !view->shapeClosed() && points() == open + 1 && farthest(nodeAt(0), cutAt) < 1.0 && farthest(nodeAt(last), cutAt) < 1.0
                                    && shift < 1.2 && view->shapePoint() == 0;
                capture(QStringLiteral("shape-opened"));
                dragWith(cutAt, cutAt + QPointF(40, -60), Qt::NoModifier);
                const bool pulledApart = farthest(nodeAt(0), nodeAt(last)) > 30.0;
                closeOk = joined && parted && pulledApart;
                if (!closeOk)
                    log << QStringLiteral("       joined %1, parted %2 (line moved %3 px), pulled apart %4").arg(joined).arg(parted).arg(shift, 0, 'f', 2).arg(pulledApart);
            }
            check(QStringLiteral("the bar closes an open shape, and cuts a closed one open at the chosen point without moving the line; an end then pulls away"), closeOk);
        }
        view->finishShapeEdit(false);
        wait(20);
        const double back = moved(drawn, ink());
        check(QStringLiteral("a tap beside the shape lets go of the point; Cancel restores the wave (%1% off), all of it one undo step").arg(back * 100.0, 0, 'f', 1),
              chosen && letGo && back < 0.02 && view->editedShape().kind == Kind::Curve && engine.history().undoCount() == beforeWave + 1);
        engine.undo(true);

        // A circle keeps its own handles and its Circle button until a point is asked for.
        const int beforeRound = engine.history().undoCount();
        std::vector<QPointF> round = wobble(ellipseStroke(QPointF(560, 320), 150, 146, 0, 370), 1.5, 0.3);
        drawAndRest(round, [](double) { return 0.7; });
        pen(QEvent::TabletRelease, round.back(), 0.0, Qt::LeftButton, Qt::NoButton);
        wait(20);
        view->startShapeEdit();
        settle();
        auto variantShown = [&](const char* id) {
            for (QAbstractButton* b : window.findChildren<QAbstractButton*>()) {
                if (b->property("variant").toString() == QLatin1String(id) && b->isVisible())
                    return true;
            }
            return false;
        };
        auto shownName = [&] {
            for (QLabel* label : window.findChildren<QLabel*>(QStringLiteral("shape-name"))) {
                if (label->isVisible())
                    return label->text();
            }
            return QString();
        };
        const bool asCircle = view->editedShape().kind == Kind::Ellipse && variantShown("circle") && shownName().isEmpty();
        const std::vector<QPointF> ring = lineOf();
        const QPointF centre = view->canvasToWidget(view->editedShape().center);
        const double radius = farthest(view->shapeHandlePositions()[0], centre);
        const QPointF onRing = centre + QPointF(radius * 0.7071, -radius * 0.7071);
        clickWith(onRing, Qt::ControlModifier);
        const double ringShift = strays(ring, lineOf());
        const bool fivePoints = view->editedShape().kind == Kind::Path && points() == 5 && view->editedShape().closed() && view->shapeName() == QLatin1String("Shape")
                                && !variantShown("circle") && shownName() == QLatin1String("Shape");
        check(QStringLiteral("a circle: Ctrl + click on it makes it a shape of five points, round as before (within %1 px), and the bar follows").arg(ringShift, 0, 'f', 2),
              asCircle && fivePoints && ringShift < 1.6 && view->shapePoint() >= 0 && farthest(nodeAt(view->shapePoint()), onRing) < 4.0);
        // Pull the new point outwards and make it sharp: a drop.
        const int tip = view->shapePoint();
        dragWith(nodeAt(tip), nodeAt(tip) + QPointF(70, -70), Qt::NoModifier);
        clickWith(nodeAt(tip), Qt::AltModifier);
        capture(QStringLiteral("shape-drop"));
        view->finishShapeEdit(true);
        wait(20);
        const bool reopens = view->shapeEditOffered() && view->startShapeEdit() && view->editedShape().kind == Kind::Path && points() == 5
                             && !view->editedShape().nodes[size_t(tip)].smooth;
        view->finishShapeEdit(true);
        wait(20);
        check(QStringLiteral("the new point pulled out and made sharp; applied, the shape opens again with its points as they were left"),
              reopens && engine.history().undoCount() == beforeRound + 1);
        engine.undo(true);

        // A rectangle: a corner rounded, another taken out, and the least a closed shape can be.
        std::vector<QPointF> boxStroke = wobble(densify(closedFrom(boxCorners(QPointF(560, 330), 360, 220, 0), 0.4), false, 4.0), 1.5, 0.6);
        drawAndRest(boxStroke, [](double) { return 0.7; });
        pen(QEvent::TabletRelease, boxStroke.back(), 0.0, Qt::LeftButton, Qt::NoButton);
        wait(20);
        view->startShapeEdit();
        settle();
        const bool asBox = view->editedShape().kind == Kind::Rectangle && variantShown("rectangle");
        const QVector<QPointF> boxCornersAt = view->shapeHandlePositions();
        clickWith(boxCornersAt.value(0), Qt::AltModifier);
        const bool rounded = view->editedShape().kind == Kind::Path && points() == 4 && view->shapePoint() >= 0 && view->shapePointSmooth()
                             && farthest(nodeAt(view->shapePoint()), boxCornersAt.value(0)) < 1.0 && !variantShown("rectangle");
        clickWith(boxCornersAt.value(2), Qt::ControlModifier);
        const bool three = points() == 3;
        clickWith(nodeAt(1), Qt::ControlModifier);
        const bool stillThree = points() == 3 && view->shapeEditing();
        if (!(asBox && rounded && three && stillThree))
            log << QStringLiteral("       as a rectangle %1 (%2), rounded %3, three %4, still three %5").arg(asBox).arg(view->shapeName()).arg(rounded).arg(three).arg(stillThree);
        check(QStringLiteral("a rectangle: Alt + click rounds a corner, Ctrl + click takes one out, and three points are the least it keeps"),
              asBox && rounded && three && stillThree);
        view->finishShapeEdit(true);
        wait(20);
        engine.undo(true);

        // A straight line: a point put on it and pulled aside in one movement.
        std::vector<QPointF> straight = wobble(densify({QPointF(300, 300), QPointF(860, 318)}, false, 5.0), 1.5, 0.5);
        drawAndRest(straight, [](double) { return 0.7; });
        pen(QEvent::TabletRelease, straight.back(), 0.0, Qt::LeftButton, Qt::NoButton);
        wait(20);
        view->startShapeEdit();
        settle();
        const bool asLine = view->editedShape().kind == Kind::Line;
        const QVector<QPointF> ends = view->shapeHandlePositions();
        const QPointF middle = (ends.value(0) + ends.value(1)) * 0.5;
        dragWith(middle, middle + QPointF(10, 90), Qt::ControlModifier);
        check(QStringLiteral("a line: Ctrl + drag from its middle bends it into a %1 of %2 points").arg(view->shapeName().toLower()).arg(points()),
              asLine && view->editedShape().kind == Kind::Path && points() == 3 && view->shapeName() == QLatin1String("Polyline")
                  && farthest(nodeAt(1), middle + QPointF(10, 90)) < 3.0 && !view->editedShape().nodes[1].smooth);
        view->finishShapeEdit(true);
        wait(20);
        engine.undo(true);
        engine.removeLayer(layer);

        for (const QString& line : log)
            qInfo().noquote() << "shape points:" << line;
        qInfo().noquote() << "shape points:" << (pointsOk ? "ok" : "FAILED");
    }

    // Zoom in on the pen stroke with the wheel, then rotate the view.
    for (int i = 0; i < 6; ++i) {
        QWheelEvent wheel(QPointF(1140, 880), view->mapToGlobal(QPointF(1140, 880)), QPoint(), QPoint(0, 120), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view, &wheel);
    }
    view->rotateStep(15.0);
    shot(QStringLiteral("zoomed"));

    qInfo() << "input: done";
    qInfo().noquote() << QStringLiteral("ui input: %1 strokes recorded (expected 2), picked colour %2, zoom %3%")
                             .arg(strokesAdded)
                             .arg(picked.name())
                             .arg(qRound(view->zoom() * 100));
    if (live) {
        settle();
        window.screen()->grabWindow(window.winId()).save(QStringLiteral("%1-live.png").arg(outPrefix));
    }

    // ---- Scrolling lists by dragging them with the pen, and panels that stay open.
    bool panelsOk = true;
    {
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            panelsOk = panelsOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        auto pause = [](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms)
                QApplication::processEvents(QEventLoop::AllEvents, 5);
        };
        // A drag at a human pace: `msPerStep` apart, so there is a speed to speak of.
        auto penStroke = [&](const QPointF& from, const QPointF& to, int steps, int msPerStep) {
            penWindow(QEvent::TabletMove, from, 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, from, 0.5, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= steps; ++i) {
                if (msPerStep > 0)
                    pause(msPerStep);
                penWindow(QEvent::TabletMove, from + (to - from) * (double(i) / steps), 0.6, Qt::NoButton, Qt::LeftButton);
            }
            penWindow(QEvent::TabletRelease, to, 0.0, Qt::LeftButton, Qt::NoButton);
        };
        // Bars are rebuilt each time they open, and the old one lingers until the
        // event loop gets to delete it: go by the one that is on screen.
        auto named = [&](const char* name) -> QWidget* {
            QWidget* found = nullptr;
            for (QWidget* w : window.findChildren<QWidget*>(QLatin1String(name))) {
                if (w->isVisible())
                    found = w;
            }
            return found;
        };
        view->fitToView();
        view->resetRotation();
        window.showForScreenshot(QStringLiteral("main"));
        tools->setTool(StrokeTool::Brush);
        tools->setPreset(QStringLiteral("studio-pen"));

        // Brush library: drag the list, the brush stays; a tap still picks one.
        penTap(named("tool-brush"), centre(named("tool-brush")));
        QWidget* brushList = named("brush-list");
        auto* brushScroll = qobject_cast<QScrollArea*>(brushList->parentWidget()->parentWidget());
        const QPointF listMid = brushScroll->viewport()->mapTo(&window, brushScroll->viewport()->rect().center());
        const int top = brushScroll->verticalScrollBar()->value();
        penStroke(listMid + QPointF(0, 130), listMid - QPointF(0, 130), 20, 0);
        settle();
        const int dragged = brushScroll->verticalScrollBar()->value();
        const bool brushKept = tools->preset().id == QLatin1String("studio-pen");
        penTap(brushScroll->viewport(), brushScroll->viewport()->rect().center());
        const QString tapped = tools->preset().id;
        check(QStringLiteral("drag the brush list with the pen: it scrolls %1 px and the brush stays; a tap then picks \"%2\"")
                  .arg(dragged - top).arg(tools->preset().name),
              dragged - top > 200 && brushKept && tapped != QLatin1String("studio-pen"));

        // A flick keeps going after the pen has left.
        brushScroll->verticalScrollBar()->setValue(0);
        penStroke(listMid + QPointF(0, 120), listMid - QPointF(0, 60), 9, 7);
        const int atRelease = brushScroll->verticalScrollBar()->value();
        pause(350);
        const int afterGlide = brushScroll->verticalScrollBar()->value();
        penTap(brushScroll->viewport(), brushScroll->viewport()->rect().center());   // a touch stops the glide
        check(QStringLiteral("a flick glides on: %1 px when the pen lifts, %2 px a moment later").arg(atRelease).arg(afterGlide),
              atRelease > 100 && afterGlide > atRelease + 40);

        // The sets: more of them than fit, so their column scrolls under the pen, and a tap still picks one.
        {
            auto* sets = qobject_cast<QScrollArea*>(named("brush-category-scroll"));
            QWidget* setList = named("brush-categories");
            const QStringList setNames = BrushLibrary::instance().categories();
            const QPointF onSets = sets->viewport()->mapTo(&window, sets->viewport()->rect().center());
            const QString brushWas = tools->preset().id;
            penStroke(onSets + QPointF(0, 110), onSets - QPointF(0, 110), 16, 0);
            settle();
            const int setsScrolled = sets->verticalScrollBar()->value();
            const bool dragKept = tools->preset().id == brushWas;
            sets->verticalScrollBar()->setValue(sets->verticalScrollBar()->maximum());
            settle();
            capture(QStringLiteral("brush-sets"));
            penTap(setList, QPointF(40, (setNames.size() - 1) * 40 + 18));
            penTap(brushScroll->viewport(), QPointF(brushScroll->viewport()->width() / 2.0, 40));
            check(QStringLiteral("%1 sets of brushes: dragging their column scrolls it %2 px; a tap on the last one, then on a brush, picks \"%3\" from \"%4\"")
                      .arg(setNames.size()).arg(setsScrolled).arg(tools->preset().name, tools->preset().category),
                  setNames.size() == 14 && setsScrolled > 40 && dragKept && tools->preset().category == setNames.last());

            // Rest on a brush for a moment and it comes up, to be put somewhere else in its set.
            BrushLibrary& library = BrushLibrary::instance();
            const QString setNow = tools->preset().category;
            QStringList orderWas;
            for (const BrushPreset* b : library.inCategory(setNow))
                orderWas << b->id;
            QWidget* list = named("brush-list");
            const QPointF onFirst = list->mapTo(&window, QPoint(list->width() / 2, 42));
            penWindow(QEvent::TabletMove, onFirst, 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, onFirst, 0.5, Qt::LeftButton, Qt::LeftButton);
            pause(480);
            for (int i = 1; i <= 12; ++i)
                penWindow(QEvent::TabletMove, onFirst + QPointF(0, 84.0 * 2 * i / 12.0), 0.5, Qt::NoButton, Qt::LeftButton);
            capture(QStringLiteral("brush-reorder"));
            penWindow(QEvent::TabletRelease, onFirst + QPointF(0, 168), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            QStringList orderNow;
            for (const BrushPreset* b : library.inCategory(setNow))
                orderNow << b->id;
            const int scrolledBy = brushScroll->verticalScrollBar()->value();
            check(QStringLiteral("rest on the first brush of the set, then drag it down two places: it is third now, and the list did not scroll instead"),
                  orderWas.size() > 3 && orderNow.value(2) == orderWas.value(0) && orderNow.value(0) == orderWas.value(1)
                      && orderNow.value(1) == orderWas.value(2) && orderNow.mid(3) == orderWas.mid(3) && scrolledBy == 0);
            library.moveInCategory(orderWas.value(0), 0);
            library.save();
        }

        // Brush lab: a page longer than the window scrolls under the pen, and nothing on it is picked on the way.
        window.showForScreenshot(QStringLiteral("main"));
        tools->setPreset(QStringLiteral("studio-pen"));
        window.showBrushLab();
        settle();
        {
            BrushLab* lab = window.brushLab();
            lab->setSection(BrushLab::Shape);
            settle();
            auto* page = qobject_cast<QScrollArea*>(named("lab-page-3"));
            const QPointF onPage = page->viewport()->mapTo(&window, QPoint(page->viewport()->width() / 2, page->viewport()->height() / 2));
            penStroke(onPage + QPointF(0, 160), onPage - QPointF(0, 160), 20, 0);
            settle();
            const int pageScrolled = page->verticalScrollBar()->value();
            check(QStringLiteral("drag the Shape page of the brush lab with the pen: it scrolls %1 px and no shape is picked on the way").arg(pageScrolled),
                  pageScrolled > 150 && tools->preset().tip.isEmpty() && !BrushLibrary::instance().isModified(QStringLiteral("studio-pen")));
            capture(QStringLiteral("brush-lab-scrolled"));
            penTap(named("lab-done"), centre(named("lab-done")));
            check(QStringLiteral("Done closes the lab"), !lab->isVisible());
        }
        window.showForScreenshot(QStringLiteral("main"));

        // Colours: tear the panel off by its grip, and it stays open while painting.
        auto* chip = named("color-chip");
        auto* colours = window.findChild<Popover*>(QStringLiteral("color-popover"));
        penTap(chip, centre(chip));
        const bool hanging = colours->isVisible() && !colours->isPinned();
        const QPointF grip = colours->mapTo(&window, colours->gripRect().topLeft() + QPoint(60, 8));
        penStroke(grip, grip + QPointF(-520, 180), 20, 0);
        settle();
        const QPoint parked = colours->pinnedPos();
        const int undoWas = engine.history().undoCount();
        penStroke(QPointF(300, 780), QPointF(520, 820), 20, 0);   // a stroke on the canvas
        settle();
        check(QStringLiteral("drag the Colours panel by its grip: it is pinned, and still open after a stroke on the canvas"),
              hanging && colours->isPinned() && colours->isVisible() && engine.history().undoCount() == undoWas + 1
                  && parked.x() > 300 && parked.x() < 1000 && parked.y() > 150);
        capture(QStringLiteral("panel-pinned"));

        // Layers opens and closes beside it without disturbing it; its own button hides and shows it.
        penTap(named("layers-button"), centre(named("layers-button")));
        auto* layersPop = window.findChild<Popover*>(QStringLiteral("layer-popover"));
        const bool both = layersPop->isVisible() && colours->isVisible();
        capture(QStringLiteral("panel-pinned-with-layers"));
        penStroke(QPointF(300, 840), QPointF(420, 860), 12, 0);
        settle();
        const bool layersGone = !layersPop->isVisible() && colours->isVisible();
        penTap(chip, centre(chip));
        const bool hidden = !colours->isVisible() && colours->isPinned();
        penTap(chip, centre(chip));
        check(QStringLiteral("another panel comes and goes beside it; its own button hides it and brings it back where it was"),
              both && layersGone && hidden && colours->isVisible() && colours->pinnedPos() == parked);

        // Dragged to the edge it snaps there; the pin lets it go back under its button.
        const QPointF gripNow = colours->mapTo(&window, colours->gripRect().topLeft() + QPoint(60, 8));
        penStroke(gripNow, QPointF(60 + 17, gripNow.y() + 40), 20, 0);
        settle();
        const int snappedX = colours->pinnedPos().x();
        penTap(colours, colours->pinRect().center());
        const bool unpinned = !colours->isPinned() && colours->isVisible();
        penStroke(QPointF(300, 900), QPointF(360, 905), 8, 0);
        settle();
        check(QStringLiteral("near the left edge it snaps to it (x = %1); unpinned, the next stroke puts it away as before").arg(snappedX),
              snappedX == 8 && unpinned && !colours->isVisible());
        // Reference picture: open the panel, pick colours from it, pan, zoom, flip, and keep it up while painting.
        {
            QImage card(800, 600, QImage::Format_RGB32);
            card.fill(QColor(220, 40, 40));
            {
                QPainter cp(&card);
                cp.fillRect(400, 0, 400, 600, QColor(40, 60, 220));
            }
            ReferencePanel* reference = window.referencePanel();
            auto* referencePop = window.findChild<Popover*>(QStringLiteral("reference-popover"));
            penTap(named("reference-button"), centre(named("reference-button")));
            const bool opened = referencePop->isVisible() && !reference->hasImage();
            reference->setImage(card, QStringLiteral("Test card"));
            settle();
            const QRect area = reference->viewRect();
            const QPointF leftSide(area.left() + area.width() * 0.25, area.center().y()), rightSide(area.left() + area.width() * 0.75, area.center().y());
            penTap(named("reference-pick"), centre(named("reference-pick")));
            penTap(reference, leftSide);
            const QColor pickedLeft = tools->color();
            penTap(reference, rightSide);
            const QColor pickedRight = tools->color();
            penTap(named("reference-pick"), centre(named("reference-pick")));
            // With the eyedropper off, a drag moves the picture instead.
            const QPointF centreBefore = reference->viewCentre();
            const QPointF middle = reference->mapTo(&window, area.center());
            penStroke(middle, middle + QPointF(90, 0), 10, 0);
            settle();
            const bool panned = reference->viewCentre().x() < centreBefore.x() - 60 && tools->color() == pickedRight;
            const double scaleBefore = reference->viewScale();
            if (auto* zoomSlider = window.findChild<Slider*>(QStringLiteral("reference-zoom"))) {
                const QPointF knob = zoomSlider->mapTo(&window, QPoint(int(zoomSlider->width() * zoomSlider->value()), zoomSlider->height() / 2));
                penStroke(knob, knob + QPointF(80, 0), 8, 0);
            }
            settle();
            const bool zoomed = reference->viewScale() > scaleBefore * 1.5;
            penTap(named("reference-fit"), centre(named("reference-fit")));
            penTap(named("reference-flip"), centre(named("reference-flip")));
            const bool flipped = reference->colorAt(leftSide) == QColor(40, 60, 220) && reference->colorAt(rightSide) == QColor(220, 40, 40);
            // Torn off, it stays through a stroke on the canvas.
            const QPointF grip = referencePop->mapTo(&window, referencePop->gripRect().topLeft() + QPoint(60, 8));
            penStroke(grip, grip + QPointF(-400, 160), 16, 0);
            settle();
            const int strokesBefore = engine.history().undoCount();
            penStroke(QPointF(300, 780), QPointF(480, 800), 14, 0);
            settle();
            capture(QStringLiteral("reference"));
            check(QStringLiteral("reference picture: the eyedropper takes its red and its blue, a drag pans, the slider zooms, Flip mirrors, and pinned it stays up while painting"),
                  opened && pickedLeft == QColor(220, 40, 40) && pickedRight == QColor(40, 60, 220) && panned && zoomed && flipped
                      && referencePop->isPinned() && referencePop->isVisible() && engine.history().undoCount() == strokesBefore + 1);
            engine.undo(true);
            // Remove takes the picture away, and it is not remembered for the next start either.
            QSettings().setValue(QStringLiteral("reference/path"), QStringLiteral("/somewhere/card.png"));
            QWidget* removeBtn = named("reference-remove");
            const bool offered = removeBtn && removeBtn->isEnabled();
            if (removeBtn)
                penTap(removeBtn, centre(removeBtn));
            const bool gone = !reference->hasImage() && !reference->colorAt(leftSide).isValid()
                              && !QSettings().contains(QStringLiteral("reference/path"));
            const QColor colourKept = tools->color();
            penTap(reference, leftSide);   // nothing there to pick or to drag
            const bool inert = tools->color() == colourKept && removeBtn && !removeBtn->isEnabled() && !named("reference-fit")->isEnabled();
            capture(QStringLiteral("reference-removed"));
            reference->setImage(card, QStringLiteral("Test card"));
            settle();
            check(QStringLiteral("reference picture: Remove takes it away and forgets it; the buttons that need a picture wait for the next one, which loads as before"),
                  offered && gone && inert && reference->hasImage() && removeBtn->isEnabled());
            reference->clearImage();
            penTap(referencePop, referencePop->pinRect().center());
            penTap(named("reference-button"), centre(named("reference-button")));
            window.showForScreenshot(QStringLiteral("main"));
        }

        // Symmetry from its bar: pick a mode, draw on one side, find it on the other.
        penTap(named("guides-button"), centre(named("guides-button")));
        const bool barUp = named("guide-bar") && named("guide-bar")->isVisible();
        penTap(named("guide-vertical"), centre(named("guide-vertical")));
        tools->setPreset(QStringLiteral("studio-pen"));
        tools->setSize(24);
        tools->setColor(QColor(200, 40, 60));
        const QPointF canvasMid = view->canvasToWidget(QPointF(engine.canvasSize().width() / 2.0, engine.canvasSize().height() / 2.0));
        const QPointF strokeFrom = canvasMid + QPointF(-320, -150), strokeTo = canvasMid + QPointF(-120, -60);
        const QPointF sample = view->widgetToCanvas(strokeFrom + (strokeTo - strokeFrom) * 0.5);
        const QPoint mirrored(engine.canvasSize().width() - 1 - int(sample.x()), int(sample.y()));
        const QColor mirrorBefore = engine.renderImage().pixelColor(mirrored);
        penStroke(strokeFrom, strokeTo, 30, 0);
        settle();
        const QColor mirrorAfter = engine.renderImage().pixelColor(mirrored);
        check(QStringLiteral("symmetry bar: Vertical, then a stroke left of centre also lands right of it"),
              barUp && engine.guides().symmetry == Guides::Symmetry::Vertical && mirrorAfter != mirrorBefore && mirrorAfter.red() > 150
                  && mirrorAfter.green() < 110);
        engine.undo(true);
        penTap(named("guide-radial"), centre(named("guide-radial")));
        penTap(named("guide-more"), centre(named("guide-more")));
        penTap(named("guide-more"), centre(named("guide-more")));
        penTap(named("guide-grid"), centre(named("guide-grid")));
        const int strokesBefore = engine.history().undoCount();
        tools->setPreset(QStringLiteral("brush-pen"));
        tools->setSize(18);
        penWindow(QEvent::TabletPress, canvasMid + QPointF(40, -30), 0.3, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 40; ++i) {
            const double t = i / 40.0;
            penWindow(QEvent::TabletMove, canvasMid + QPointF(40 + 250 * t, -30 - 150 * std::sin(t * 3.0)), 0.3 + 0.6 * std::sin(t * 3.14159),
                      Qt::NoButton, Qt::LeftButton);
        }
        penWindow(QEvent::TabletRelease, canvasMid + QPointF(290, -50), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        check(QStringLiteral("Radial with two taps on +: %1 repeats, grid on, and the bar stays up while painting").arg(engine.guides().segments),
              engine.guides().symmetry == Guides::Symmetry::Radial && engine.guides().segments == 8 && engine.guides().grid
                  && named("guide-bar")->isVisible() && engine.history().undoCount() == strokesBefore + 1);
        capture(QStringLiteral("symmetry"));
        engine.undo(true);
        penTap(named("guide-grid"), centre(named("guide-grid")));
        {
            // Symmetry put somewhere else: drag its centre, turn it by its green point, draw, and send it back.
            penTap(named("guide-vertical"), centre(named("guide-vertical")));
            const QVector<QPointF> atRest = view->symmetryHandlePositions();
            const bool twoPoints = atRest.size() == 2 && std::hypot(atRest[0].x() - canvasMid.x(), atRest[0].y() - canvasMid.y()) < 1.0
                                   && atRest[1].y() < atRest[0].y() - 80 && !named("guide-recentre") && named("guide-variant");
            penStroke(atRest[0], atRest[0] + QPointF(-200, 60), 16, 0);
            settle();
            const QPointF wanted = view->widgetToCanvas(atRest[0] + QPointF(-200, 60));
            const QPointF movedTo = engine.symmetryCentre();
            const bool moved = std::hypot(movedTo.x() - wanted.x(), movedTo.y() - wanted.y()) < 3.0 && named("guide-recentre")
                               && engine.history().undoCount() == strokesBefore;
            // A stroke left of the new axis comes back on its other side.
            tools->setPreset(QStringLiteral("studio-pen"));
            tools->setSize(24);
            const QPointF axisNow = view->symmetryHandlePositions()[0];
            const QPointF from = axisNow + QPointF(-260, -120), to = axisNow + QPointF(-90, -40);
            const QPointF onStroke = view->widgetToCanvas(from + (to - from) * 0.5);
            const QPoint reflected(int(std::lround(2.0 * movedTo.x() - onStroke.x())), int(onStroke.y()));
            const QColor reflectedBefore = engine.renderImage().pixelColor(reflected);
            penStroke(from, to, 30, 0);
            settle();
            const QColor reflectedAfter = engine.renderImage().pixelColor(reflected);
            const bool mirroredThere = reflectedAfter != reflectedBefore && reflectedAfter.red() > 150 && reflectedAfter.green() < 110;
            engine.undo(true);
            // The green point, swung round to three o'clock of the centre: a quarter turn.
            const QVector<QPointF> held = view->symmetryHandlePositions();
            penWindow(QEvent::TabletMove, held[1], 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, held[1], 0.5, Qt::LeftButton, Qt::LeftButton);
            for (int i = 1; i <= 18; ++i) {
                const double a = -1.5707963 + 1.5707963 * i / 18.0;
                penWindow(QEvent::TabletMove, held[0] + QPointF(std::cos(a), std::sin(a)) * 96.0, 0.5, Qt::NoButton, Qt::LeftButton);
            }
            penWindow(QEvent::TabletRelease, held[0] + QPointF(96, 0), 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            const float turnedTo = engine.guides().symAngle;
            const QLabel* turnLabel = qobject_cast<QLabel*>(named("guide-turn"));
            const bool shown = turnLabel && turnLabel->text() == QStringLiteral("90°");
            // Part of the way back, to an angle of its own.
            const QVector<QPointF> again = view->symmetryHandlePositions();
            const double part = -30.0 * 3.14159265 / 180.0;
            penStroke(again[1], again[0] + QPointF(std::cos(part), std::sin(part)) * 96.0, 12, 0);
            settle();
            const float slanted = engine.guides().symAngle;
            capture(QStringLiteral("symmetry-moved"));
            penTap(named("guide-variant"), centre(named("guide-variant")));
            const bool rotational = engine.guides().symRotational;
            penTap(named("guide-variant"), centre(named("guide-variant")));
            penTap(named("guide-radial"), centre(named("guide-radial")));
            penTap(named("guide-variant"), centre(named("guide-variant")));
            const bool kaleidoscope = engine.guides().symMirrored && !engine.guides().symRotational;
            const int kaleidoscopeStrokes = engine.history().undoCount();
            tools->setPreset(QStringLiteral("brush-pen"));
            tools->setSize(14);
            const QPointF hubNow = view->symmetryHandlePositions()[0];
            penStroke(hubNow + QPointF(30, -40), hubNow + QPointF(150, -110), 24, 0);
            settle();
            capture(QStringLiteral("symmetry-kaleidoscope"));
            const bool drew = engine.history().undoCount() == kaleidoscopeStrokes + 1;
            engine.undo(true);
            penTap(named("guide-variant"), centre(named("guide-variant")));
            penTap(named("guide-recentre"), centre(named("guide-recentre")));
            check(QStringLiteral("symmetry moved and turned with the pen: the blue point takes the axis %1 px left, a stroke mirrors about it there; "
                                 "the green point swung to the side turns it %2° (the bar says so), part of the way back %3°; "
                                 "Rotational and Mirrored switch, and Reset puts it back in the middle")
                      .arg(qRound(engine.canvasSize().width() / 2.0 - movedTo.x())).arg(qRound(turnedTo)).arg(qRound(slanted)),
                  twoPoints && moved && mirroredThere && std::abs(turnedTo - 90.0f) < 0.01f && shown && std::abs(slanted - 60.0f) < 3.0f
                      && rotational && kaleidoscope && drew && engine.guides().symOffset.isNull()
                      && engine.guides().symAngle == 0.0f && !engine.guides().symMirrored && !named("guide-recentre"));
        }
        penTap(named("guide-off"), centre(named("guide-off")));
        penTap(named("guide-done"), centre(named("guide-done")));
        Guides eightFold;
        eightFold.segments = 8;
        check(QStringLiteral("Off and Done: no symmetry, no grid, bar gone"),
              engine.guides() == eightFold && !named("guide-bar"));
        // Perspective: pick one-point from the bar, drag its point, then draw a shaky line.
        penTap(named("guides-button"), centre(named("guides-button")));
        penTap(named("guide-1-point"), centre(named("guide-1-point")));
        const QVector<QPointF> points = view->guideHandlePositions();
        const bool onePoint = engine.guides().perspective == Guides::Perspective::OnePoint && points.size() == 1 && engine.guides().snap;
        const int strokesAtDrag = engine.history().undoCount();
        if (!points.isEmpty())
            penStroke(points[0], points[0] + QPointF(120, 60), 12, 0);
        settle();
        const QPointF vpNow = view->canvasToWidget(engine.guides().vp[0]);
        check(QStringLiteral("perspective bar: 1-point, then dragging its point moves the vanishing point and paints nothing"),
              onePoint && std::hypot(vpNow.x() - points.value(0).x() - 120, vpNow.y() - points.value(0).y() - 60) < 2.0
                  && engine.history().undoCount() == strokesAtDrag);

        // A wavering stroke that sets off level comes out dead level.
        const int traced = engine.addLayer();
        tools->setPreset(QStringLiteral("technical-pen"));
        tools->setSize(8);
        tools->setColor(Qt::black);
        const QPointF lineFrom = vpNow + QPointF(-420, 190);
        penWindow(QEvent::TabletMove, lineFrom, 0.0, Qt::NoButton, Qt::NoButton);
        penWindow(QEvent::TabletPress, lineFrom, 0.6, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 40; ++i)
            penWindow(QEvent::TabletMove, lineFrom + QPointF(i * 9, 14 * std::sin(i * 0.6) + i * 0.4), 0.6, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, lineFrom + QPointF(360, 22), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        const QRect levelBounds = engine.layerContentBounds(traced);
        // And one that sets off towards the vanishing point runs straight at it.
        engine.undo(true);
        const QPointF rayFrom = vpNow + QPointF(-380, 260);
        penWindow(QEvent::TabletPress, rayFrom, 0.6, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 30; ++i) {
            const double t = i / 40.0;
            penWindow(QEvent::TabletMove, rayFrom + (vpNow - rayFrom) * t + QPointF(10 * std::sin(i * 0.7), -8 * std::sin(i * 0.5)), 0.6, Qt::NoButton, Qt::LeftButton);
        }
        penWindow(QEvent::TabletRelease, rayFrom + (vpNow - rayFrom) * 0.75 + QPointF(6, 6), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        // Every painted pixel lies within the pen's width of the line from where it began to the vanishing point.
        const QPointF a = view->widgetToCanvas(rayFrom), b = engine.guides().vp[0];
        const QByteArray tracedPx = engine.layerPixels(traced);
        const auto* tracedHalf = reinterpret_cast<const qfloat16*>(tracedPx.constData());
        const QRect rayBounds = engine.layerContentBounds(traced);
        double furthest = 0.0;
        int inked = 0;
        const int cw = engine.canvasSize().width();
        for (int y = rayBounds.top(); y <= rayBounds.bottom(); ++y) {
            for (int x = rayBounds.left(); x <= rayBounds.right(); ++x) {
                if (float(tracedHalf[(size_t(y) * cw + size_t(x)) * 4 + 3]) < 0.5f)
                    continue;
                ++inked;
                const double off = std::abs((x + 0.5 - a.x()) * (b.y() - a.y()) - (y + 0.5 - a.y()) * (b.x() - a.x()))
                                   / std::hypot(b.x() - a.x(), b.y() - a.y());
                furthest = std::max(furthest, off);
            }
        }
        check(QStringLiteral("snapping: a shaky level stroke is %1 px tall, and one towards the point strays %2 px at most from the true line")
                  .arg(levelBounds.height()).arg(furthest, 0, 'f', 1),
              levelBounds.width() > 400 && levelBounds.height() <= 12 && inked > 500 && furthest < 6.5);
        capture(QStringLiteral("perspective"));
        engine.undo(true);

        // A pen that lands with a hook pointing at the vanishing point, then
        // runs level: the hook must not decide the line.
        penWindow(QEvent::TabletPress, lineFrom, 0.6, Qt::LeftButton, Qt::LeftButton);
        const QPointF toVp = (vpNow - lineFrom) / std::hypot(vpNow.x() - lineFrom.x(), vpNow.y() - lineFrom.y());
        for (int i = 1; i <= 4; ++i)
            penWindow(QEvent::TabletMove, lineFrom + (toVp * 0.75 + QPointF(0.25, 0.0)) * (i * 3.0), 0.6, Qt::NoButton, Qt::LeftButton);
        for (int i = 1; i <= 36; ++i)
            penWindow(QEvent::TabletMove, lineFrom + QPointF(8 + i * 9, -4 + 3 * std::sin(i * 0.5)), 0.6, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, lineFrom + QPointF(332, -3), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        const QRect hooked = engine.layerContentBounds(traced);
        engine.undo(true);
        // And a short flick that lifts before that is settled still leaves its mark.
        penWindow(QEvent::TabletPress, lineFrom, 0.6, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 5; ++i)
            penWindow(QEvent::TabletMove, lineFrom + (toVp * 0.75 + QPointF(0.25, 0.0)) * (i * 4.0), 0.6, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, lineFrom + (toVp * 0.75 + QPointF(0.25, 0.0)) * 20.0, 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        const QRect flick = engine.layerContentBounds(traced);
        engine.undo(true);
        check(QStringLiteral("a stroke that lands with a hook towards the point and then runs level comes out level (%1 px tall); a short flick is still drawn (%2 px long)")
                  .arg(hooked.height()).arg(flick.width()),
              hooked.width() > 400 && hooked.height() <= 12 && flick.width() > 20);

        // Snap off: the same hand draws the wobble it makes.
        penTap(named("guide-snap"), centre(named("guide-snap")));
        penWindow(QEvent::TabletPress, lineFrom, 0.6, Qt::LeftButton, Qt::LeftButton);
        for (int i = 1; i <= 40; ++i)
            penWindow(QEvent::TabletMove, lineFrom + QPointF(i * 9, 14 * std::sin(i * 0.6) + i * 0.4), 0.6, Qt::NoButton, Qt::LeftButton);
        penWindow(QEvent::TabletRelease, lineFrom + QPointF(360, 22), 0.0, Qt::LeftButton, Qt::NoButton);
        settle();
        const int freeHeight = engine.layerContentBounds(traced).height();
        engine.undo(true);
        penTap(named("guide-snap"), centre(named("guide-snap")));
        penTap(named("guide-isometric"), centre(named("guide-isometric")));
        auto* isoLeft = window.findChild<Slider*>(QStringLiteral("guide-iso-left"));
        const bool anglesShown = isoLeft && isoLeft->isVisible();
        if (anglesShown) {
            const QPointF knob = isoLeft->mapTo(&window, QPoint(8, isoLeft->height() / 2));
            penStroke(knob, knob + QPointF(isoLeft->width() * 0.75, 0), 12, 0);
        }
        settle();
        capture(QStringLiteral("isometric"));
        check(QStringLiteral("with Snap off the wobble stays (%1 px tall); Isometric shows its angle sliders, and dragging one sets %2°")
                  .arg(freeHeight).arg(qRound(engine.guides().isoLeft)),
              freeHeight > 30 && engine.guides().snap && anglesShown && engine.guides().perspective == Guides::Perspective::Isometric
                  && engine.guides().isoLeft > 55.0f);
        penTap(named("guide-fisheye"), centre(named("guide-fisheye")));
        settle();
        capture(QStringLiteral("fisheye"));
        const bool fisheyeUp = view->guideHandlePositions().size() == 2;
        penTap(named("guide-perspective-off"), centre(named("guide-perspective-off")));
        penTap(named("guide-done"), centre(named("guide-done")));

        // Shift + drag sideways sizes the brush and leaves no mark.
        const float sizeBefore = tools->size();
        const int strokesAtSize = engine.history().undoCount();
        QKeyEvent shiftDown(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QApplication::sendEvent(view, &shiftDown);
        penStroke(QPointF(700, 600), QPointF(880, 610), 12, 0);
        const float sizeGrown = tools->size();
        penStroke(QPointF(700, 600), QPointF(560, 600), 12, 0);
        const float sizeShrunk = tools->size();
        QKeyEvent shiftUp(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QApplication::sendEvent(view, &shiftUp);
        settle();
        check(QStringLiteral("Shift + drag: brush size %1 -> %2 px to the right, then %3 px to the left; fisheye has its two handles")
                  .arg(sizeBefore, 0, 'f', 0).arg(sizeGrown, 0, 'f', 0).arg(sizeShrunk, 0, 'f', 0),
              sizeGrown > sizeBefore * 2 && sizeShrunk < sizeGrown && engine.history().undoCount() == strokesAtSize && fisheyeUp
                  && engine.guides().perspective == Guides::Perspective::Off);
        // Levels and Curves, worked from their bars with the pen.
        int colourLayer = 0;
        for (const LayerInfo& l : engine.layers()) {
            if (l.name == QLatin1String("Colour"))
                colourLayer = l.id;
        }
        engine.setActiveLayer(colourLayer);
        const QPoint probe(1200, 1180);   // in the blue stroke on that layer
        const QColor original = engine.renderImage().pixelColor(probe);
        const int beforeLevels = engine.history().undoCount();
        window.showForScreenshot(QStringLiteral("levels"));
        settle();
        QWidget* levelsBar = named("adjust-bar");
        const QList<Slider*> levelSliders = levelsBar ? levelsBar->findChildren<Slider*>() : QList<Slider*>();
        if (levelSliders.size() == 5) {
            Slider* white = levelSliders[1];
            const QPointF knob = white->mapTo(&window, QPoint(white->width() - 9, white->height() / 2));
            penStroke(knob, knob - QPointF(white->width() * 0.45, 0), 14, 0);
        }
        settle();
        const QColor previewed = engine.renderImage().pixelColor(probe);
        // The histogram over the sliders: the layer's tones, which a blue stroke on a clear layer has few of.
        auto* tonesView = dynamic_cast<HistogramView*>(named("adjust-histogram"));
        int stepsInUse = 0;
        for (float bin : tonesView ? tonesView->bins() : QVector<float>())
            stepsInUse += bin > 0.02f;
        const bool histogramShown = tonesView && tonesView->bins().size() == 256 && stepsInUse > 3 && stepsInUse < 200;
        capture(QStringLiteral("levels"));
        penTap(named("adjust-apply"), centre(named("adjust-apply")));
        const QColor levelled = engine.renderImage().pixelColor(probe);
        check(QStringLiteral("Levels bar: dragging the white point lightens the picture as it moves (blue %1 -> %2); the tick applies it as one step")
                  .arg(original.blue()).arg(levelled.blue()),
              levelSliders.size() == 5 && previewed != original && previewed.blue() > original.blue() + 10 && levelled == previewed
                  && engine.history().undoCount() == beforeLevels + 1 && !named("adjust-bar"));
        check(QStringLiteral("Levels shows the layer's histogram over its sliders (%1 of 256 tones in use)").arg(stepsInUse), histogramShown);
        engine.undo(true);

        window.showForScreenshot(QStringLiteral("curves"));
        settle();
        QWidget* curve = named("adjust-curve");
        auto* curveEditor = qobject_cast<CurveEditor*>(curve);
        const QVector<float> allTones = curveEditor ? curveEditor->histogram() : QVector<float>();
        penTap(named("adjust-curve-red"), centre(named("adjust-curve-red")));
        const QVector<float> redTones = curveEditor ? curveEditor->histogram() : QVector<float>();
        penTap(named("adjust-curve-all"), centre(named("adjust-curve-all")));
        check(QStringLiteral("Curves has the histogram behind its line, and Red shows the red channel's own"),
              allTones.size() == 256 && redTones.size() == 256 && allTones != redTones && curveEditor->histogram() == allTones);
        if (curve) {
            const QPointF mid = curve->mapTo(&window, curve->rect().center());
            penStroke(mid, mid + QPointF(-34, -44), 10, 0);   // lift the middle of the curve
        }
        settle();
        const QColor curvedLive = engine.renderImage().pixelColor(probe);
        capture(QStringLiteral("curves"));
        penTap(named("adjust-cancel"), centre(named("adjust-cancel")));
        check(QStringLiteral("Curves bar: lifting the curve lightens the picture (blue %1 -> %2); the cross puts everything back")
                  .arg(original.blue()).arg(curvedLive.blue()),
              curve && curvedLive.blue() > original.blue() + 8 && engine.renderImage().pixelColor(probe) == original
                  && engine.history().undoCount() == beforeLevels && !named("adjust-bar"));
        // Text, typed straight onto the canvas: tap where it goes, type, style it from the bar, drag it, done.
        {
            const int layersBefore = engine.layerCount(), stepsBefore = engine.history().undoCount();
            QSettings().remove(QStringLiteral("text"));
            tools->setColor(QColor(30, 30, 40));
            window.showForScreenshot(QStringLiteral("text"));
            settle();
            const bool barUp = named("text-bar") != nullptr && view->textMode() && !view->textPlaced();
            const QPointF spot(520, 380);
            penTap(view, spot);
            auto* editor = view->findChild<QPlainTextEdit*>(QStringLiteral("text-editor"));
            auto type = [&](const QString& text) {
                for (const QChar ch : text) {
                    const bool enter = ch == QLatin1Char('\n');
                    QKeyEvent press(QEvent::KeyPress, enter ? Qt::Key_Return : Qt::Key_unknown, Qt::NoModifier, enter ? QStringLiteral("\r") : QString(ch));
                    QApplication::sendEvent(editor, &press);
                }
                settle();
            };
            if (editor)
                type(QStringLiteral("Helo"));
            // A slip of the finger: back up, fix it, carry on to a second line.
            if (editor) {
                QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
                QApplication::sendEvent(editor, &back);
                type(QStringLiteral("lo\nImpasto"));
            }
            const bool typed = view->textContent() == QLatin1String("Hello\nImpasto") && view->textPlaced();
            const int sizeBefore = view->textFont().pixelSize();
            penTap(named("text-bold"), centre(named("text-bold")));
            penTap(named("text-centre"), centre(named("text-centre")));
            if (auto* sizeSlider = window.findChild<Slider*>(QStringLiteral("text-size"))) {
                const QPointF knob = sizeSlider->mapTo(&window, QPoint(int(sizeSlider->width() * sizeSlider->value()), sizeSlider->height() / 2));
                penStroke(knob, knob + QPointF(14, 0), 6, 0);
            }
            // More room between the two lines.
            if (auto* lineSlider = window.findChild<Slider*>(QStringLiteral("text-line-spacing"))) {
                const QPointF knob = lineSlider->mapTo(&window, QPoint(int(lineSlider->width() * lineSlider->value()), lineSlider->height() / 2));
                penStroke(knob, knob + QPointF(16, 0), 6, 0);
            }
            settle();
            const double lineSpacing = view->textLineSpacing();
            const QFont styled = view->textFont();
            // Pick the text up and carry it somewhere else.
            penStroke(spot + QPointF(10, 20), spot + QPointF(190, 110), 12, 0);
            settle();
            capture(QStringLiteral("text"));
            penTap(named("text-apply"), centre(named("text-apply")));
            settle();
            const int textLayer = engine.activeLayerId();
            const QRect inked = engine.layerContentBounds(textLayer);
            const bool boxUp = view->transforming();
            penTap(named("transform-apply"), centre(named("transform-apply")));
            // Centred text hangs from the point it was put at, which the drag moved along with the pen.
            const QPointF expected = view->widgetToCanvas(spot) + (view->widgetToCanvas(QPointF(180, 90)) - view->widgetToCanvas(QPointF(0, 0)));
            check(QStringLiteral("text on the canvas: typed \"%1\", made bold and %2 px, lines %3 times apart from the bar, dragged, and it lands where it was put (%4 x %5 px of lettering)")
                      .arg(view->textContent().isEmpty() ? QStringLiteral("Hello / Impasto") : view->textContent()).arg(styled.pixelSize())
                      .arg(lineSpacing, 0, 'f', 2).arg(inked.width()).arg(inked.height()),
                  barUp && editor && typed && styled.bold() && styled.pixelSize() > sizeBefore && view->textAlignment() == Qt::AlignHCenter
                      && lineSpacing > 1.05 && engine.layerText(textLayer).lineSpacing == lineSpacing
                      && engine.layerCount() == layersBefore + 1 && boxUp && !view->textMode() && !named("text-bar")
                      && engine.layerInfo(textLayer).name == QLatin1String("Hello Impasto") && inked.width() > 300
                      && std::abs(inked.center().x() - expected.x()) < 30 && std::abs(inked.top() - expected.y()) < styled.pixelSize());
            // It is still text: with the text tool, a tap on it opens it again, as it was set.
            const int stepsPlaced = engine.history().undoCount();
            const bool isText = engine.layerIsText(textLayer);
            window.showForScreenshot(QStringLiteral("text"));
            settle();
            const QRect placed = engine.layerContentBounds(textLayer);
            penTap(view, view->canvasToWidget(QPointF(placed.center())));
            const bool reopened = view->textEditLayer() == textLayer && view->textContent() == QLatin1String("Hello\nImpasto")
                                  && view->textFont().bold() && view->textAlignment() == Qt::AlignHCenter && tools->color() == QColor(30, 30, 40);
            if (editor)
                type(QStringLiteral("!"));
            tools->setColor(QColor(190, 40, 60));
            settle();
            capture(QStringLiteral("text-retyped"));
            penTap(named("text-apply"), centre(named("text-apply")));
            settle();
            const TextInfo after = engine.layerText(textLayer);
            check(QStringLiteral("text stays text: a tap on it with the text tool opens it in its own font and colour; a \"!\" and another colour later it is the same layer, set afresh"),
                  isText && reopened && engine.layerIsText(textLayer) && after.text == QLatin1String("Hello\nImpasto!") && after.color == QColor(190, 40, 60)
                      && engine.layerCount() == layersBefore + 1 && engine.history().undoCount() == stepsPlaced + 1 && !view->textMode()
                      && !view->transforming() && engine.layerContentBounds(textLayer).width() >= placed.width());
            while (engine.history().undoCount() > stepsBefore)
                engine.undo(true);
            // Escape, or the cross, leaves nothing behind.
            window.showForScreenshot(QStringLiteral("text"));
            penTap(view, spot);
            if (editor)
                type(QStringLiteral("never mind"));
            penTap(named("text-cancel"), centre(named("text-cancel")));
            check(QStringLiteral("cancelling text leaves no layer and no undo step"),
                  !view->textMode() && engine.layerCount() == layersBefore && engine.history().undoCount() == stepsBefore);
        }

        // Liquify: push paint about with the pen, then apply.
        {
            int inkLayer = 0;
            for (const LayerInfo& l : engine.layers()) {
                if (l.name == QLatin1String("Ink"))
                    inkLayer = l.id;
            }
            engine.setActiveLayer(inkLayer);
            const int stepsBefore = engine.history().undoCount();
            const QImage unbent = engine.renderImage();
            window.showForScreenshot(QStringLiteral("liquify"));
            settle();
            const bool barUp = named("liquify-bar") != nullptr && view->liquifying();
            // Shift + drag sizes the liquify brush and moves no paint; the bar's slider follows.
            const float liquifySizeBefore = view->liquifySize();
            QKeyEvent shiftDown(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
            QApplication::sendEvent(view, &shiftDown);
            penStroke(QPointF(700, 600), QPointF(900, 600), 12, 0);
            QKeyEvent shiftUp(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
            QApplication::sendEvent(view, &shiftUp);
            settle();
            auto* liquifySlider = window.findChild<Slider*>(QStringLiteral("liquify-size"));
            const bool sized = view->liquifySize() > liquifySizeBefore * 2 && engine.renderImage() == unbent && liquifySlider
                               && std::abs(liquifySlider->value() - view->liquifySizeFraction()) < 0.001f;
            const float liquifySizeGrown = view->liquifySize();
            view->setLiquifySize(liquifySizeBefore);
            // Down through the heavy brush-pen line.
            const QPointF across = view->canvasToWidget(QPointF(1200, 380));
            penStroke(across, across + QPointF(30, 150), 24, 0);
            settle();
            const QImage bentLive = engine.renderImage();
            penTap(named("liquify-twirl-right"), centre(named("liquify-twirl-right")));
            const bool twirlPicked = view->liquifyMode() == LiquifyMode::TwirlRight;
            // Held still, a twirl keeps turning.
            const QPointF hold = view->canvasToWidget(QPointF(800, 540));
            penWindow(QEvent::TabletPress, hold, 0.8, Qt::LeftButton, Qt::LeftButton);
            pause(260);
            penWindow(QEvent::TabletRelease, hold, 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
            const QImage twirledLive = engine.renderImage();
            capture(QStringLiteral("liquify"));
            penTap(named("liquify-apply"), centre(named("liquify-apply")));
            int moved = 0, turned = 0;
            for (int y = 300; y < 700; y += 4) {
                for (int x = 600; x < 1500; x += 4) {
                    moved += bentLive.pixelColor(x, y) != unbent.pixelColor(x, y);
                    turned += twirledLive.pixelColor(x, y) != bentLive.pixelColor(x, y);
                }
            }
            check(QStringLiteral("liquify: Shift + drag sizes its brush (%1 -> %2 px) and moves no paint").arg(liquifySizeBefore, 0, 'f', 0).arg(liquifySizeGrown, 0, 'f', 0),
                  sized);
            check(QStringLiteral("liquify: a push through the line bends it (%1 samples changed), a held twirl goes on turning (%2 more), and the tick applies it all as one step")
                      .arg(moved).arg(turned),
                  barUp && moved > 150 && twirlPicked && turned > 150 && !view->liquifying() && !named("liquify-bar")
                      && engine.history().undoCount() == stepsBefore + 1 && engine.renderImage() == twirledLive);
            engine.undo(true);
            view->setLiquifyMode(LiquifyMode::Push);
        }

        // Distort: pull one corner of the transform box and the layer leans into perspective.
        {
            const int card = engine.addLayer();
            QPainterPath square;
            square.addRect(800, 500, 400, 400);
            engine.setSelection(square, QStringLiteral("Select"));
            engine.fillLayer(card, QColor(40, 120, 220));
            engine.clearSelection();
            const int beforeDistort = engine.history().undoCount();
            window.showForScreenshot(QStringLiteral("transform"));
            settle();
            penTap(named("transform-distort"), centre(named("transform-distort")));
            const bool distorting = view->transformDistort();
            const QVector<QPointF> corners = view->transformHandlePositions();
            // The top right corner goes down and in; the others stay.
            if (corners.size() == 8)
                penStroke(corners[2], corners[2] + (view->canvasToWidget(QPointF(1100, 650)) - view->canvasToWidget(QPointF(1200, 500))), 14, 0);
            settle();
            capture(QStringLiteral("distort"));
            penTap(named("transform-apply"), centre(named("transform-apply")));
            auto alphaAt = [&](int x, int y) {
                const QByteArray px = engine.layerPixels(card);
                return float(reinterpret_cast<const qfloat16*>(px.constData())[(size_t(y) * engine.canvasSize().width() + size_t(x)) * 4 + 3]);
            };
            // Where the corner used to be is empty now; the three that stayed still hold; the new corner is filled.
            const float oldCorner = alphaAt(1190, 510), newCorner = alphaAt(1090, 660), topLeft = alphaAt(808, 508),
                        bottomRight = alphaAt(1192, 892), bottomLeft = alphaAt(808, 892);
            check(QStringLiteral("distort: one corner dragged in leaves the other three where they were (old corner %1, new corner %2)")
                      .arg(oldCorner, 0, 'f', 2).arg(newCorner, 0, 'f', 2),
                  distorting && corners.size() == 8 && oldCorner == 0.0f && newCorner > 0.9f && topLeft > 0.9f && bottomRight > 0.9f
                      && bottomLeft > 0.9f && engine.history().undoCount() == beforeDistort + 1 && !view->transforming());
            engine.undo(true);
            while (engine.history().undoCount() > beforeDistort - 3)   // the fill, the selection steps and the layer
                engine.undo(true);
        }

        // Warp: drag one point of the sheet and the paint around it follows.
        {
            const int sheetLayer = engine.addLayer();
            QPainterPath square;
            square.addRect(800, 500, 480, 480);
            engine.setSelection(square, QStringLiteral("Select"));
            engine.fillLayer(sheetLayer, QColor(220, 130, 40));
            engine.clearSelection();
            const int beforeWarp = engine.history().undoCount();
            window.showForScreenshot(QStringLiteral("transform"));
            settle();
            penTap(named("transform-warp"), centre(named("transform-warp")));
            const QVector<QPointF> sheet = view->transformWarpPositions();
            // The right-hand end of the second row goes out to the right.
            if (sheet.size() == 16)
                penStroke(sheet[7], sheet[7] + (view->canvasToWidget(QPointF(1400, 660)) - view->canvasToWidget(QPointF(1280, 660))), 14, 0);
            settle();
            capture(QStringLiteral("warp"));
            penTap(named("transform-apply"), centre(named("transform-apply")));
            auto alphaAt = [&](int x, int y) {
                const QByteArray px = engine.layerPixels(sheetLayer);
                return float(reinterpret_cast<const qfloat16*>(px.constData())[(size_t(y) * engine.canvasSize().width() + size_t(x)) * 4 + 3]);
            };
            check(QStringLiteral("warp: dragging one point of the sheet bulges the paint beside it and leaves the far corners alone"),
                  sheet.size() == 16 && alphaAt(1360, 660) > 0.9f && alphaAt(1360, 960) == 0.0f && alphaAt(810, 510) > 0.9f
                      && alphaAt(1270, 970) > 0.9f && engine.history().undoCount() == beforeWarp + 1 && !view->transforming());
            while (engine.history().undoCount() > beforeWarp - 3)
                engine.undo(true);
        }

        // Gradient mode: a drag lays one down, and the mode stays for the next.
        {
            const int gradientLayer = engine.addLayer();
            const int beforeGradient = engine.history().undoCount();
            tools->setColor(QColor(230, 60, 120));
            window.showForScreenshot(QStringLiteral("gradient"));
            settle();
            const bool barUp = named("gradient-bar") != nullptr && view->gradientMode();
            const QPointF from = view->canvasToWidget(QPointF(400, 400)), to = view->canvasToWidget(QPointF(2000, 1200));
            penStroke(from, to, 16, 0);
            settle();
            const QByteArray px = engine.layerPixels(gradientLayer);
            const auto* half = reinterpret_cast<const qfloat16*>(px.constData());
            const int cw = engine.canvasSize().width();
            const float atStart = float(half[(size_t(400) * cw + 400) * 4 + 3]), halfWay = float(half[(size_t(800) * cw + 1200) * 4 + 3]);
            capture(QStringLiteral("gradient"));
            penTap(named("gradient-radial"), centre(named("gradient-radial")));
            penTap(view, QPointF(700, 500));   // a tap is not a gradient
            const bool tapIgnored = engine.history().undoCount() == beforeGradient + 1;
            penTap(named("gradient-done"), centre(named("gradient-done")));
            check(QStringLiteral("gradient mode: a drag lays the colour down fading out (%1 at its start, %2 half way); a tap does nothing; the tick leaves the mode")
                      .arg(atStart, 0, 'f', 2).arg(halfWay, 0, 'f', 2),
                  barUp && atStart > 0.98f && std::abs(halfWay - 0.5f) < 0.03f && tapIgnored && view->gradientKind() == GradientKind::Radial
                      && !view->gradientMode() && !named("gradient-bar"));
            view->setGradientKind(GradientKind::Linear);
            engine.undo(true);

            // Colours of one's own: the strip in the bar, a third colour put on it with the pen, and a drag across the canvas.
            view->setGradientStops({{0.0f, QColor(20, 40, 200)}, {1.0f, QColor(250, 250, 250)}});
            window.showForScreenshot(QStringLiteral("gradient"));
            settle();
            penTap(named("gradient-colours"), centre(named("gradient-colours")));
            QWidget* strip = named("gradient-strip");
            const bool stripUp = strip && view->gradientCustom() && view->gradientStops().size() == 2 && named("gradient-stop-set");
            tools->setColor(QColor(30, 200, 60));
            if (strip)
                penTap(strip, QPointF(strip->width() / 2.0, 10));
            const QVector<GradientStop> three = view->gradientStops();
            const bool added = three.size() == 3 && std::abs(three[2].at - 0.5f) < 0.03f && three[2].color == QColor(30, 200, 60);
            const int beforeOwn = engine.history().undoCount();
            penStroke(view->canvasToWidget(QPointF(400, 800)), view->canvasToWidget(QPointF(2000, 800)), 16, 0);
            settle();
            const QColor atMiddle = engine.renderImage().pixelColor(1200, 800), atStartNow = engine.renderImage().pixelColor(420, 800);
            capture(QStringLiteral("gradient-colours"));
            // The middle handle, dragged a quarter of the strip to the left, then pulled away altogether.
            const QPointF handle = strip->mapTo(&window, QPoint(strip->width() / 2, 29));
            penStroke(handle, handle - QPointF((strip->width() - 20) * 0.25, 0), 8, 0);
            const float movedTo = view->gradientStops().value(2).at;
            const QPointF handleNow = handle - QPointF((strip->width() - 20) * 0.25, 0);
            penStroke(handleNow, handleNow + QPointF(0, -90), 8, 0);
            check(QStringLiteral("gradient of one's own colours: a press on the strip puts the current colour half way; a drag on the canvas then runs through it "
                                 "(green %1 in the middle); the handle moves to %2% and, pulled away, takes its colour out")
                      .arg(atMiddle.green()).arg(qRound(movedTo * 100)),
                  stripUp && added && engine.history().undoCount() == beforeOwn + 1 && atMiddle.green() > 170 && atMiddle.red() < 80 && atMiddle.blue() < 110
                      && atStartNow.blue() > 150 && std::abs(movedTo - 0.25f) < 0.04f && view->gradientStops().size() == 2);
            penTap(named("gradient-to-nothing"), centre(named("gradient-to-nothing")));
            const bool backToPlain = !view->gradientCustom() && view->gradientToTransparent() && !named("gradient-strip");
            penTap(named("gradient-done"), centre(named("gradient-done")));
            check(QStringLiteral("Nothing puts the plain fade back and the strip away"), backToPlain && !view->gradientMode());
            engine.undo(true);
            engine.undo(true);

            // Angle: the colour sweeps anticlockwise round where the drag starts, from the direction
            // dragged, as Photoshop's does.
            const int angleLayer = engine.addLayer();
            const int beforeAngle = engine.history().undoCount();
            window.showForScreenshot(QStringLiteral("gradient"));
            settle();
            penTap(named("gradient-angle"), centre(named("gradient-angle")));
            penStroke(view->canvasToWidget(QPointF(1200, 800)), view->canvasToWidget(QPointF(1800, 800)), 16, 0);
            settle();
            const QByteArray swept = engine.layerPixels(angleLayer);
            const auto* sweptHalf = reinterpret_cast<const qfloat16*>(swept.constData());
            auto alphaOf = [&](int x, int y) { return float(sweptHalf[(size_t(y) * cw + size_t(x)) * 4 + 3]); };
            const float justPast = alphaOf(1600, 770), justBefore = alphaOf(1600, 830), opposite = alphaOf(800, 800);
            capture(QStringLiteral("gradient-angle"));
            const bool angleKept = QSettings().value(QStringLiteral("gradient/kind")).toInt() == int(GradientKind::Angle);
            penTap(named("gradient-linear"), centre(named("gradient-linear")));
            penTap(named("gradient-done"), centre(named("gradient-done")));
            check(QStringLiteral("angle gradient: just above the line dragged it is the colour (%1), just below it nearly nothing (%2), opposite half way (%3); the kind is remembered")
                      .arg(justPast, 0, 'f', 2).arg(justBefore, 0, 'f', 2).arg(opposite, 0, 'f', 2),
                  engine.history().undoCount() == beforeAngle + 1 && justPast > 0.97f && justBefore < 0.05f && std::abs(opposite - 0.5f) < 0.03f
                      && angleKept && view->gradientKind() == GradientKind::Linear);
            engine.undo(true);
            engine.removeLayer(angleLayer);
        }

        // Stabilisation in a panel of its own, for the brush in hand, without the Brush Lab.
        {
            tools->setTool(StrokeTool::Brush);
            tools->setPreset(QStringLiteral("studio-pen"));
            settle();
            penTap(named("steady-button"), centre(named("steady-button")));
            auto* steadyPop = window.findChild<Popover*>(QStringLiteral("steady-popover"));
            auto* streamline = window.findChild<Slider*>(QStringLiteral("steady-streamline"));
            auto* every = window.findChild<Slider*>(QStringLiteral("steady-every"));
            auto* brushName = window.findChild<QLabel*>(QStringLiteral("steady-brush"));
            const bool up = steadyPop && steadyPop->isVisible() && streamline && streamline->isVisible() && every && brushName;
            const bool shown = up && std::abs(streamline->value() - tools->preset().streamline) < 1e-4f
                               && brushName->text().contains(tools->preset().name);
            float pulled = 0.0f, everyNow = 0.0f;
            bool kept = false, followed = false;
            if (up) {
                // StreamLine pulled to the end of its slider: the brush itself has all of it.
                const int mid = streamline->height() / 2;
                penStroke(streamline->mapTo(&window, QPoint(10 + int((streamline->width() - 20) * streamline->value()), mid)),
                          streamline->mapTo(&window, QPoint(streamline->width() + 30, mid)), 10, 0);
                pulled = tools->preset().streamline;
                kept = BrushLibrary::instance().find(QStringLiteral("studio-pen"))->streamline == pulled;
                // Another brush in hand, and the panel shows that one.
                tools->setPreset(QStringLiteral("round-brush"));
                settle();
                followed = std::abs(streamline->value() - tools->preset().streamline) < 1e-4f && brushName->text().contains(tools->preset().name);
                // Half of the smoothing every brush gets, kept with the preferences.
                const int everyMid = every->height() / 2;
                penStroke(every->mapTo(&window, QPoint(10, everyMid)), every->mapTo(&window, QPoint(10 + (every->width() - 20) / 2, everyMid)), 10, 0);
                everyNow = QSettings().value(QStringLiteral("prefs/extraStabilization")).toFloat();
                capture(QStringLiteral("steady"));
            }
            penTap(named("steady-button"), centre(named("steady-button")));
            const bool away = steadyPop && !steadyPop->isVisible();
            check(QStringLiteral("stabilisation panel: the button opens it on the brush in hand; StreamLine pulled to the end gives that brush %1%, kept in the library; "
                                 "with another brush it shows that one; All brushes at %2% is kept with the preferences")
                      .arg(qRound(pulled * 100)).arg(qRound(everyNow * 100)),
                  shown && pulled > 0.99f && kept && followed && std::abs(everyNow - 0.5f) < 0.05f && away);
            BrushLibrary::instance().reset(QStringLiteral("studio-pen"));
            QSettings().setValue(QStringLiteral("prefs/extraStabilization"), 0.0);
            engine.setExtraStabilization(0.0f);
            if (every)
                every->setValue(0.0f);
            tools->setPreset(QStringLiteral("studio-pen"));
        }

        // Crop: drag a corner of the frame in, apply, and the canvas is that size.
        const QSize fullSize = engine.canvasSize();
        const int beforeCrop = engine.history().undoCount();
        window.showForScreenshot(QStringLiteral("crop"));
        settle();
        const QVector<QPointF> cropHandles = view->cropHandlePositions();
        if (cropHandles.size() == 8)
            penStroke(cropHandles[4], cropHandles[4] - QPointF(300, 180), 16, 0);
        settle();
        const QRect frame = view->cropRect();
        auto* cropWidth = window.findChild<QSpinBox*>(QStringLiteral("crop-width"));
        const bool barFollows = cropWidth && cropWidth->isVisible() && cropWidth->value() == frame.width();
        capture(QStringLiteral("crop"));
        penTap(named("crop-apply"), centre(named("crop-apply")));
        const QSize croppedSize = engine.canvasSize();
        engine.undo(true);
        check(QStringLiteral("crop: dragging a corner in gives a %1 x %2 frame, the bar shows it, and the tick makes the canvas that size")
                  .arg(frame.width()).arg(frame.height()),
              cropHandles.size() == 8 && frame.width() < fullSize.width() - 300 && frame.height() < fullSize.height() - 150 && barFollows
                  && croppedSize == frame.size() && engine.history().undoCount() == beforeCrop && engine.canvasSize() == fullSize
                  && !view->cropping() && !named("crop-bar"));
        for (const QString& line : log)
            qInfo().noquote() << "pen scroll and pinned panels:" << line;
        qInfo().noquote() << "pen scroll and pinned panels:" << (panelsOk ? "ok" : "FAILED");
    }

    // ---- Colours: the pickers, harmonies and palettes, with the pen at window level.
    bool coloursOk = true;
    {
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            coloursOk = coloursOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        auto pause = [](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms)
                QApplication::processEvents(QEventLoop::AllEvents, 5);
        };
        // `restMs` is how long the pen stays where it landed before it sets off.
        auto penStroke = [&](const QPointF& from, const QPointF& to, int steps, int restMs = 0) {
            penWindow(QEvent::TabletMove, from, 0.0, Qt::NoButton, Qt::NoButton);
            penWindow(QEvent::TabletPress, from, 0.5, Qt::LeftButton, Qt::LeftButton);
            if (restMs > 0)
                pause(restMs);
            for (int i = 1; i <= steps; ++i)
                penWindow(QEvent::TabletMove, from + (to - from) * (double(i) / steps), 0.6, Qt::NoButton, Qt::LeftButton);
            penWindow(QEvent::TabletRelease, to, 0.0, Qt::LeftButton, Qt::NoButton);
            settle();
        };
        auto at = [&](QWidget* w, const QPointF& local) { return QPointF(w->mapTo(&window, local.toPoint())); };
        auto hueOff = [](const QColor& c, float degrees) {
            const float d = std::abs(c.hsvHueF() * 360.0f - degrees);
            return std::min(d, 360.0f - d);
        };
        view->fitToView();
        view->resetRotation();
        window.showForScreenshot(QStringLiteral("main"));
        PaletteLibrary& lib = PaletteLibrary::instance();
        lib.resetToStarters();
        const int undoBase = engine.history().undoCount();
        auto* panel = window.findChild<ColorPanel*>();
        auto* pop = window.findChild<Popover*>(QStringLiteral("color-popover"));
        auto* chip = window.findChild<QWidget*>(QStringLiteral("color-chip"));
        auto* tabs = window.findChild<SegmentBar*>(QStringLiteral("color-tabs"));
        auto tapTab = [&](int index) { penTap(tabs, tabs->segmentRect(index).center()); };
        panel->setTab(ColorPanel::Disc);
        tools->setColor(QColor(200, 60, 40));
        penTap(chip, centre(chip));

        // Classic: the square gives saturation and brightness, the hue stays.
        tapTab(ColorPanel::Classic);
        auto* square = window.findChild<ColorSquare*>(QStringLiteral("color-square"));
        const bool squareUp = pop->isVisible() && panel->tab() == ColorPanel::Classic && square->isVisible();
        const float hueWas = tools->color().hsvHueF() * 360.0f;
        const QRectF field = square->field();
        penTap(square, QPointF(field.left() + field.width() * 0.75, field.top() + field.height() * 0.25));
        const QColor fromSquare = tools->color();
        penStroke(at(square, field.center()), at(square, field.topRight() + QPointF(40, -40)), 10);
        const QColor corner = tools->color();
        check(QStringLiteral("Classic: a tap three quarters across and up gives %1% saturation, %2% brightness at the same hue; dragged out past the top right corner it is the full colour")
                  .arg(qRound(fromSquare.hsvSaturationF() * 100)).arg(qRound(fromSquare.valueF() * 100)),
              squareUp && std::abs(fromSquare.hsvSaturationF() - 0.75f) < 0.03f && std::abs(fromSquare.valueF() - 0.75f) < 0.03f
                  && hueOff(fromSquare, hueWas) < 2.0f && corner.hsvSaturationF() > 0.99f && corner.valueF() > 0.99f && hueOff(corner, hueWas) < 2.0f);
        penTap(square, QPointF(field.left() + field.width() * 0.7, field.top() + field.height() * 0.3));
        capture(QStringLiteral("colors-classic"));

        // Harmony: the colour's own mark sets it; a companion's mark turns the whole set.
        tapTab(ColorPanel::Harmonies);
        panel->setHarmony(Harmony::Triadic);   // the dropdown is a popup of its own, which the run does not open
        auto* wheel = window.findChild<HarmonyWheel*>(QStringLiteral("harmony-wheel"));
        auto* family = window.findChild<SwatchGrid*>(QStringLiteral("harmony-colors"));
        const QPointF hub(wheel->width() / 2.0, wheel->height() / 2.0);
        const double reach = std::min(wheel->width(), wheel->height()) / 2.0 - 13.0;
        penStroke(at(wheel, wheel->markPos(0)), at(wheel, hub + QPointF(0.8 * reach, 0)), 12);
        const QColor onWheel = tools->color();
        const bool triad = family->colors().size() == 3 && hueOff(family->colors()[1], 120) < 3.0f && hueOff(family->colors()[2], 240) < 3.0f;
        // The companion a third of the way round, pulled to nine o'clock: the colour follows to 60 degrees.
        penStroke(at(wheel, wheel->markPos(1)), at(wheel, hub - QPointF(0.8 * reach, 0)), 12);
        const QColor turned = tools->color();
        capture(QStringLiteral("colors-harmony"));
        penTap(family, family->cellRect(1).center());
        const QColor companion = tools->color();
        check(QStringLiteral("Harmony: the mark dragged to three o'clock gives red at %1% saturation with companions at 120° and 240°; "
                             "a companion dragged to nine o'clock turns the colour to %2°; a tap on its chip takes that colour (%3°)")
                  .arg(qRound(onWheel.hsvSaturationF() * 100)).arg(qRound(turned.hsvHueF() * 360)).arg(qRound(companion.hsvHueF() * 360)),
              wheel->isVisible() && hueOff(onWheel, 0) < 3.0f && std::abs(onWheel.hsvSaturationF() - 0.8f) < 0.04f && triad
                  && hueOff(turned, 60) < 3.0f && std::abs(turned.hsvSaturationF() - 0.8f) < 0.04f && hueOff(companion, 180) < 3.0f);

        // Value: red, green and blue as sliders, and the pair that swaps back.
        tapTab(ColorPanel::Value);
        auto* redSlider = window.findChild<Slider*>(QStringLiteral("color-red"));
        auto* pair = window.findChild<QWidget*>(QStringLiteral("color-pair"));
        const QColor beforeRed = tools->color();
        const QPointF knob(10 + (redSlider->width() - 20) * redSlider->value(), redSlider->height() / 2.0);
        penStroke(at(redSlider, knob), at(redSlider, QPointF(redSlider->width() + 30, redSlider->height() / 2.0)), 12);
        const QColor reddened = tools->color();
        // Paint with it, pick another, and the pair takes the first one back.
        penStroke(QPointF(420, 800), QPointF(560, 820), 12);
        const bool painted = engine.history().undoCount() == undoBase + 1 && !pop->isVisible();   // not pinned: the stroke put it away
        penTap(chip, centre(chip));
        tools->setColor(QColor(20, 90, 60));
        settle();
        capture(QStringLiteral("colors-value"));
        penTap(pair, centre(pair));
        const QColor swapped = tools->color();
        penTap(pair, centre(pair));
        check(QStringLiteral("Value: the red slider pulled to its end makes red 255 and leaves green and blue; after a stroke and another colour, a tap on the pair goes back to it and a second tap returns"),
              pop->isVisible() && panel->tab() == ColorPanel::Value && reddened.red() == 255 && std::abs(reddened.green() - beforeRed.green()) <= 1
                  && std::abs(reddened.blue() - beforeRed.blue()) <= 1 && painted && swapped == reddened && tools->color() == QColor(20, 90, 60));

        // The default palette under the pickers.
        tapTab(ColorPanel::Disc);
        auto* compact = window.findChild<PaletteGrid*>(QStringLiteral("palette-compact"));
        const QString starter = lib.currentId();
        const QColor mine = tools->color();
        penTap(compact, compact->slotRect(30).center());
        const bool kept = lib.find(starter)->colors[30] == mine && lib.find(starter)->filled() == 31;
        penTap(compact, compact->slotRect(11).center());
        const bool taken = tools->color() == lib.find(starter)->colors[11] && tools->color() == QColor(QStringLiteral("#d32f2f"));
        check(QStringLiteral("palette: a tap on an empty slot keeps the current colour there; a tap on a swatch takes its colour"), compact->isVisible() && kept && taken);
        // Resting on a swatch picks it up; it goes where it is dropped.
        penStroke(at(compact, compact->slotRect(30).center()), at(compact, compact->slotRect(33).center()), 10, 560);
        const bool movedOn = lib.find(starter)->colors[33] == mine && !lib.find(starter)->colors[30].isValid();
        const QColor zero = lib.find(starter)->colors[0], one = lib.find(starter)->colors[1];
        penStroke(at(compact, compact->slotRect(0).center()), at(compact, compact->slotRect(1).center()), 10, 560);
        const bool exchanged = lib.find(starter)->colors[0] == one && lib.find(starter)->colors[1] == zero;
        // Dropped outside the grid, it goes back.
        penStroke(at(compact, compact->slotRect(5).center()), at(compact, compact->slotRect(5).center() - QPointF(0, 90)), 10, 560);
        // Without the rest, a pen sliding from one swatch onto the next rearranges nothing and takes neither colour.
        const QVector<QColor> arranged = lib.find(starter)->colors;
        penStroke(at(compact, compact->slotRect(12).center()), at(compact, compact->slotRect(13).center()), 6);
        check(QStringLiteral("palette: rest on a swatch, then drag: onto an empty slot it moves, onto another swatch the two change places, let go elsewhere it stays put; "
                             "a slide without the rest changes nothing"),
              movedOn && exchanged && lib.find(starter)->filled() == 31 && lib.find(starter)->colors[5] == QColor(QStringLiteral("#ffffff"))
                  && lib.find(starter)->colors == arranged && tools->color() == QColor(QStringLiteral("#d32f2f")));
        capture(QStringLiteral("colors-palette"));

        // All the palettes: a name makes one the default, the list scrolls under the pen.
        tapTab(ColorPanel::Palettes);
        QWidget* list = panel->paletteList();
        auto* listScroll = qobject_cast<QScrollArea*>(list->parentWidget()->parentWidget());
        const bool longList = listScroll && listScroll->verticalScrollBar()->maximum() > 60 && !compact->isVisible();
        const QString second = lib.palettes()[1].id;
        penTap(list, QPointF(60, panel->paletteHeaderRect(1).center().y()));
        const bool madeDefault = lib.currentId() == second;
        auto snapshot = [&] {
            QVector<QVector<QColor>> all;
            for (const Palette& p : lib.palettes())
                all.push_back(p.colors);
            return all;
        };
        const auto coloursBefore = snapshot();
        const QColor colourBefore = tools->color();
        const QPointF listMid = at(listScroll->viewport(), listScroll->viewport()->rect().center());
        penStroke(listMid + QPointF(0, 110), listMid - QPointF(0, 110), 20);
        const int scrolled = listScroll->verticalScrollBar()->value();
        check(QStringLiteral("palettes: a tap on a name makes \"%1\" the default; dragging the list with the pen scrolls it %2 px and changes no swatch and no colour")
                  .arg(lib.current().name).arg(scrolled),
              longList && madeDefault && scrolled > 60 && snapshot() == coloursBefore && tools->color() == colourBefore);
        // Even in the list that scrolls, resting on a swatch first moves the swatch rather than the list.
        listScroll->verticalScrollBar()->setValue(0);
        settle();
        PaletteGrid* firstGrid = panel->paletteGrid(0);
        const QString firstId = lib.palettes()[0].id;
        const QColor carried = lib.find(firstId)->colors[12];
        penStroke(at(firstGrid, firstGrid->slotRect(12).center()), at(firstGrid, firstGrid->slotRect(32).center()), 12, 560);
        check(QStringLiteral("palettes: rest on a swatch in the list, then drag down two rows: the swatch moves, the list stays where it was"),
              lib.find(firstId)->colors[32] == carried && !lib.find(firstId)->colors[12].isValid() && listScroll->verticalScrollBar()->value() == 0);

        // Carried over to another palette and let go there: a copy goes in, and the first palette keeps its own.
        {
            PaletteGrid* secondGrid = panel->paletteGrid(1);
            const QString secondId = lib.palettes()[1].id;
            const int filledBefore = lib.find(secondId)->filled();
            int target = -1;
            for (int k = 0; secondGrid && k < lib.find(secondId)->colors.size(); ++k) {
                const QPoint centre = secondGrid->slotRect(k).center().toPoint();
                if (!lib.find(secondId)->colors[k].isValid() && secondGrid->visibleRegion().contains(centre)) {
                    target = k;
                    break;
                }
            }
            bool copied = false;
            if (target >= 0) {
                penStroke(at(firstGrid, firstGrid->slotRect(32).center()), at(secondGrid, secondGrid->slotRect(target).center()), 14, 560);
                copied = lib.find(secondId)->colors[target] == carried && lib.find(secondId)->filled() == filledBefore + 1
                         && lib.find(firstId)->colors[32] == carried && secondGrid->incomingSlot() < 0;
            }
            check(QStringLiteral("palettes: a swatch carried onto another palette goes into it as a copy (slot %1), and stays in the palette it came from").arg(target),
                  target >= 0 && copied);
        }

        // New palettes: an empty one to fill by tapping, and one taken from what is on the canvas.
        const int countBefore = int(lib.palettes().size());
        const QString fresh = panel->newPalette();   // the + menu is a popup; this is what its first entry calls
        settle();
        tools->setColor(QColor(12, 140, 200));
        PaletteGrid* freshGrid = panel->paletteGrid(0);
        penTap(freshGrid, freshGrid->slotRect(0).center());
        penTap(freshGrid, freshGrid->slotRect(4).center());
        const Palette* made = lib.find(fresh);
        const bool filledByPen = made && lib.palettes().first().id == fresh && lib.currentId() == fresh && made->filled() == 2
                                 && made->colors[0] == QColor(12, 140, 200) && made->colors[4] == QColor(12, 140, 200)
                                 && listScroll->verticalScrollBar()->value() == 0;
        const QString fromCanvas = panel->newPaletteFrom(engine.renderImage(), QStringLiteral("From the Canvas"));
        settle();
        const Palette* canvasPalette = lib.find(fromCanvas);
        const int canvasColours = canvasPalette ? canvasPalette->filled() : 0;
        capture(QStringLiteral("colors-palettes"));
        check(QStringLiteral("palettes: a new palette is on top and the default, and two taps put the current colour in its slots; one made from the canvas has %1 colours")
                  .arg(canvasColours),
              filledByPen && int(lib.palettes().size()) == countBefore + 2 && canvasColours >= 8 && canvasColours <= 30 && lib.currentId() == fromCanvas);
        tapTab(ColorPanel::Disc);
        capture(QStringLiteral("colors-from-canvas"));

        window.showForScreenshot(QStringLiteral("main"));
        while (engine.history().undoCount() > undoBase)
            engine.undo(true);
        lib.resetToStarters();
        check(QStringLiteral("the stroke made on the way is undone and the panel is put away"), !pop->isVisible() && engine.history().undoCount() == undoBase);
        for (const QString& line : log)
            qInfo().noquote() << "colours:" << line;
        qInfo().noquote() << "colours:" << (coloursOk ? "ok" : "FAILED");
    }

    // ---- The shortcuts window and a dialog's buttons, drawn without being put on screen.
    bool windowsOk = true;
    {
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            windowsOk = windowsOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        ShortcutSheet sheet;
        sheet.setAttribute(Qt::WA_DontShowOnScreen);
        sheet.resize(880, 640);
        sheet.show();
        settle();
        int listed = 0, onSymmetry = 0;
        const QVector<ShortcutSheet::Section> sections = ShortcutSheet::sections();
        for (const ShortcutSheet::Section& section : sections) {
            listed += int(section.rows.size());
            for (const ShortcutSheet::Row& row : section.rows) {
                onSymmetry += row.what.contains(QLatin1String("symmetry"), Qt::CaseInsensitive) || row.keys.contains(QLatin1String("symmetry"), Qt::CaseInsensitive)
                              || section.title.contains(QLatin1String("symmetry"), Qt::CaseInsensitive);
            }
        }
        const bool whole = sheet.sectionCount() == sections.size() && sheet.visibleRows() == listed && !sheet.isModal();
        sheet.grab().save(QStringLiteral("%1-shortcuts.png").arg(outPrefix));
        QWidget* first = sheet.sectionHeader(0);
        auto press = [&](QWidget* w) {
            const QPointF at(w->width() / 2.0, w->height() / 2.0);
            QMouseEvent down(QEvent::MouseButtonPress, at, w->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(w, &down);
            QMouseEvent up(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(w, &up);
            settle();
        };
        press(first);
        const bool folded = sheet.isFolded(0) && sheet.visibleRows() == listed - int(sections[0].rows.size());
        sheet.setFilter(QStringLiteral("symmetry"));
        settle();
        const int found = sheet.visibleRows();
        sheet.setFilter(QString());
        settle();
        const bool stillFolded = sheet.isFolded(0) && sheet.visibleRows() == listed - int(sections[0].rows.size());
        sheet.grab().save(QStringLiteral("%1-shortcuts-folded.png").arg(outPrefix));
        press(first);
        check(QStringLiteral("shortcuts window: %1 shortcuts in %2 sections, in a window of %3 x %4 that is not modal; a press on a heading folds it; "
                             "looking for \"symmetry\" leaves %5 lines, and clearing that gives back what was open")
                  .arg(listed).arg(sections.size()).arg(sheet.width()).arg(sheet.height()).arg(found),
              whole && sheet.height() <= 700 && folded && found == onSymmetry && found > 0 && stillFolded && !sheet.isFolded(0)
                  && sheet.visibleRows() == listed);
        sheet.hide();

        QMessageBox box(QMessageBox::NoIcon, QStringLiteral("Unsaved Changes"), QStringLiteral("Save changes to this canvas before closing it?"),
                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        box.setAttribute(Qt::WA_DontShowOnScreen);
        box.show();
        settle();
        box.grab().save(QStringLiteral("%1-dialog.png").arg(outPrefix));
        auto wears = [&](QMessageBox::StandardButton which, const char* icon) {
            const QAbstractButton* b = box.button(which);
            return b && b->icon().pixmap(QSize(16, 16), 1.0).toImage() == theme::icon(QLatin1String(icon), 16, theme::text, 1.0).toImage();
        };
        check(QStringLiteral("dialog buttons carry the app's own line icons: Save, Discard and Cancel"),
              wears(QMessageBox::Save, "save") && wears(QMessageBox::Discard, "trash") && wears(QMessageBox::Cancel, "close"));
        box.hide();

        // New Canvas: a size on paper at a resolution, and what this card has room for.
        {
            NewCanvasDialog dlg(QSize(2560, 1440));
            dlg.setAttribute(Qt::WA_DontShowOnScreen);
            dlg.show();
            settle();
            const qint64 limit = CanvasEngine::maxCanvasPixels();
            dlg.setUnit(NewCanvasDialog::Pixels);
            dlg.setResolution(300);
            dlg.setSize(2480, 3508);
            const bool a4 = dlg.pixelSize() == QSize(2480, 3508) && dlg.presetText(dlg.currentPreset()).startsWith(QLatin1String("A4")) && dlg.fits()
                            && dlg.roomText().contains(QStringLiteral("21 × 29.7 cm at 300 dpi"));
            // The same sheet said in centimetres; a coarser print of it is fewer pixels.
            dlg.setUnit(NewCanvasDialog::Centimetres);
            const bool inCm = std::abs(dlg.shownWidth() - 21.0) < 0.01 && std::abs(dlg.shownHeight() - 29.7) < 0.01 && dlg.pixelSize() == QSize(2480, 3508);
            dlg.setResolution(150);
            const bool coarser = dlg.pixelSize() == QSize(1240, 1754) && std::abs(dlg.shownWidth() - 21.0) < 0.01;
            check(QStringLiteral("new canvas: A4 is 2480 × 3508 px at 300 dpi, shows as 21 × 29.7 cm, and is 1240 × 1754 px at 150 dpi"), a4 && inCm && coarser);

            // A metre square: fine at poster resolution, and at 300 dpi the dialog says what would fit instead.
            dlg.setSize(100.0, 100.0);
            const bool poster = dlg.pixelSize() == QSize(5906, 5906) && dlg.fits() && dlg.presetText(dlg.currentPreset()).startsWith(QLatin1String("1 m square"));
            dlg.setResolution(300);
            const QSize metre = dlg.pixelSize();
            const bool metreFits = qint64(metre.width()) * metre.height() <= limit;
            const int finest = NewCanvasDialog::finestResolution(100.0 / 2.54, 100.0 / 2.54);
            const double atFinest = 100.0 / 2.54 * finest, pastFinest = 100.0 / 2.54 * (finest + 1);
            const bool honest = metre == QSize(11811, 11811) && dlg.fits() == metreFits
                                && (metreFits || dlg.roomText().contains(QStringLiteral("fits at up to %1 dpi").arg(finest)))
                                && atFinest * atFinest <= double(limit) && (pastFinest * pastFinest > double(limit) || pastFinest > CanvasEngine::maxCanvasDimension());
            dlg.accept();
            const bool refused = metreFits || dlg.result() != QDialog::Accepted;
            check(QStringLiteral("new canvas: 1 m × 1 m is 5906 px a side at 150 dpi; at 300 dpi it is 11811 px a side, which %1 here (a metre square fits at up to %2 dpi)")
                      .arg(metreFits ? QStringLiteral("fits") : QStringLiteral("is refused with the reason")).arg(finest),
                  poster && honest && refused);

            // The largest square is offered by name, and is as large as it says.
            dlg.choosePreset(dlg.presetCount() - 1);
            const QSize largest = dlg.pixelSize();
            const bool largestOk = dlg.fits() && dlg.unit() == NewCanvasDialog::Pixels && largest.width() == largest.height()
                                   && qint64(largest.width() + 1) * (largest.width() + 1) > limit;
            dlg.grab().save(QStringLiteral("%1-new-canvas.png").arg(outPrefix));
            dlg.setUnit(NewCanvasDialog::Inches);
            const bool inInches = std::abs(dlg.shownWidth() - largest.width() / 300.0) < 0.001 && dlg.pixelSize() == largest;
            check(QStringLiteral("new canvas: the largest square this card holds is %1 px a side, %2 cm at 300 dpi").arg(largest.width()).arg(largest.width() / 300.0 * 2.54, 0, 'f', 1),
                  largestOk && inInches);
            dlg.hide();
        }
        for (const QString& line : log)
            qInfo().noquote() << "windows:" << line;
        qInfo().noquote() << "windows:" << (windowsOk ? "ok" : "FAILED");
    }

    // ---- The gallery, last of all: it swaps the document on the canvas.
    bool galleryOk = true;
    {
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            galleryOk = galleryOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        auto pause = [](int ms) {
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < ms)
                QApplication::processEvents(QEventLoop::AllEvents, 10);
        };
        Gallery* gallery = window.gallery();
        view->zoomTo(view->zoom());
        view->fitToView();
        view->resetRotation();

        // Two artworks are on the wall already, made elsewhere.
        const BrushLibrary& brushes = BrushLibrary::instance();
        {
            CanvasEngine other;
            other.initialize();
            other.newDocument(QSize(1200, 1600), QColor(246, 240, 228));
            wave(other, StrokeTool::Brush, *brushes.find(QStringLiteral("sumi")), 120, 1.0f, QColor(30, 30, 40), QPointF(200, 300), QPointF(1000, 1300), 160);
            wave(other, StrokeTool::Brush, *brushes.find(QStringLiteral("watercolor")), 420, 0.8f, QColor(210, 80, 60), QPointF(250, 1100), QPointF(950, 500), 90);
            docio::save(other, QDir(wallDir.path()).filePath(QStringLiteral("Red crane.imp")), nullptr);
            other.newDocument(QSize(2000, 1200), QColor(24, 28, 44));
            wave(other, StrokeTool::Brush, *brushes.find(QStringLiteral("airbrush-soft")), 600, 0.9f, QColor(250, 190, 90), QPointF(300, 800), QPointF(1700, 500), 120);
            wave(other, StrokeTool::Brush, *brushes.find(QStringLiteral("studio-pen")), 26, 1.0f, QColor(240, 240, 250), QPointF(300, 400), QPointF(1700, 760), 140);
            docio::save(other, QDir(wallDir.path()).filePath(QStringLiteral("Night harbour.sbk")), nullptr);
        }

        // Crash recovery: unsaved work is copied aside, and a session that
        // never closed properly leaves that copy behind to be brought back.
        {
            tools->setPreset(QStringLiteral("round-brush"));
            wave(engine, StrokeTool::Brush, tools->preset(), 60, 1.0f, QColor(200, 60, 40), QPointF(300, 300), QPointF(900, 420), 40);
            const QImage unsaved = engine.renderImage();
            const QSize unsavedSize = engine.canvasSize();
            const bool copied = window.saveRecoveryNow();
            const bool notTwice = !window.saveRecoveryNow();   // nothing new since
            const QStringList left = MainWindow::leftoverRecoveries(true);
            const bool hiddenFromSelf = MainWindow::leftoverRecoveries(false).isEmpty();   // this session is alive: not offered to itself
            // The "crash": the canvas is gone without a save or a goodbye.
            engine.newDocument(QSize(640, 480), Qt::white);
            const bool back = !left.isEmpty() && window.recover(left.first());
            check(QStringLiteral("recovery: unsaved work is copied aside once, and after a crash it comes back whole and still counts as unsaved"),
                  copied && notTwice && left.size() == 1 && hiddenFromSelf && back && engine.canvasSize() == unsavedSize
                      && maxDiff(engine.renderImage(), unsaved) <= 1 && engine.isModified()
                      && MainWindow::leftoverRecoveries(true).size() == 1);
        }

        // Saving an untitled canvas needs no dialog: it lands in the gallery.
        engine.setModified(true);
        const bool saved = window.saveDocument();
        const QString untitled = window.documentPath();
        check(QStringLiteral("saving an untitled canvas puts it in the gallery as \"%1\"").arg(QFileInfo(untitled).completeBaseName()),
              saved && QFileInfo(untitled).absolutePath() == QDir(wallDir.path()).absolutePath() && QFileInfo::exists(untitled)
                  && QFileInfo(untitled).completeBaseName() == QLatin1String("Untitled Artwork") && !engine.isModified()
                  && MainWindow::leftoverRecoveries(true).isEmpty());   // saved properly: no recovery copy is kept

        auto* galleryBtn = window.findChild<QWidget*>(QStringLiteral("gallery-button"));
        penTap(galleryBtn, centre(galleryBtn));
        pause(250);   // the thumbnails come in one at a time
        check(QStringLiteral("tap Gallery: %1 artworks on the wall").arg(gallery->paths().size()),
              gallery->isVisible() && gallery->paths().size() == 3 && gallery->paths().contains(untitled)
                  && !gallery->cardRect(untitled).isEmpty());

        // Rename the open one, duplicate another.
        QString error;
        const QString renamed = gallery->renameArtwork(untitled, QStringLiteral("Test sheet"), &error);
        const QString crane = QDir(wallDir.path()).filePath(QStringLiteral("Red crane.imp"));
        const bool clash = gallery->renameArtwork(crane, QStringLiteral("Night harbour"), &error).isEmpty();
        const QString twin = gallery->duplicateArtwork(crane, &error);
        const bool keptOpen = !gallery->trashArtwork(renamed, &error);
        pause(250);
        check(QStringLiteral("rename, duplicate: \"%1\" follows the open canvas, \"%2\" appears; a taken name and trashing the open artwork are refused")
                  .arg(QFileInfo(renamed).completeBaseName(), QFileInfo(twin).completeBaseName()),
              QFileInfo::exists(renamed) && !QFileInfo::exists(untitled) && window.documentPath() == renamed && clash
                  && QFileInfo(twin).completeBaseName() == QLatin1String("Red crane copy") && QFileInfo::exists(twin) && keptOpen
                  && QFileInfo::exists(renamed) && gallery->paths().size() == 4);
        capture(QStringLiteral("gallery"));

        // Tap a card: that artwork opens and the wall gets out of the way.
        const QRect card = gallery->cardRect(crane);
        penTap(gallery, QPointF(card.center().x(), card.top() + 90));
        check(QStringLiteral("tap a card opens that artwork (%1 x %2)").arg(engine.canvasSize().width()).arg(engine.canvasSize().height()),
              !gallery->isVisible() && window.documentPath() == crane && engine.canvasSize() == QSize(1200, 1600));
        capture(QStringLiteral("gallery-opened"));

        // Back and forth without choosing anything.
        penTap(galleryBtn, centre(galleryBtn));
        const bool up = gallery->isVisible();
        auto* back = window.findChild<QWidget*>(QStringLiteral("gallery-back"));
        penTap(back, centre(back));
        check(QStringLiteral("Gallery, then Back to canvas, leaves the canvas as it was"),
              up && !gallery->isVisible() && window.documentPath() == crane && !engine.isModified());
        for (const QString& line : log)
            qInfo().noquote() << "gallery:" << line;
        qInfo().noquote() << "gallery:" << (galleryOk ? "ok" : "FAILED");
    }

    // ---- Layered files from other programs, opened through the window.
    bool layeredOk = true;
    {
        QStringList log;
        auto check = [&](const QString& what, bool ok) {
            layeredOk = layeredOk && ok;
            log << QStringLiteral("%1 %2").arg(ok ? "ok    " : "FAILED", what);
        };
        buildInterchangeSample(engine, false);
        const QImage original = engine.renderImage();
        const QString psd = outPrefix + QStringLiteral("-sample.psd"), ora = outPrefix + QStringLiteral("-sample.ora");
        QString error;
        const bool written = interchange::exportPsd(engine, psd, 8, &error, nullptr) && interchange::exportOra(engine, ora, &error, nullptr);
        for (const QString& path : {psd, ora}) {
            engine.newDocument(QSize(300, 200), Qt::white);
            engine.setModified(false);
            const bool opened = written && window.openPath(path);
            settle();
            const QImage shown = engine.renderImage().convertToFormat(QImage::Format_ARGB32);
            const QImage want = original.convertToFormat(QImage::Format_ARGB32);
            int worst = shown.size() == want.size() ? 0 : 255;
            for (int y = 0; worst < 255 && y < want.height(); ++y) {
                for (int x = 0; x < want.width(); ++x) {
                    const QRgb a = shown.pixel(x, y), b = want.pixel(x, y);
                    worst = std::max({worst, std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
                }
            }
            check(QStringLiteral("%1 opens through the window as an untitled canvas of %2 layers, the picture within %3/255")
                      .arg(QFileInfo(path).suffix().toUpper()).arg(engine.layerCount()).arg(worst),
                  opened && engine.layerCount() == 8 && window.documentPath().isEmpty() && worst <= 2 && !engine.isModified());
        }
        window.showForScreenshot(QStringLiteral("layers"));
        capture(QStringLiteral("layers-from-psd"));
        for (const QString& line : log)
            qInfo().noquote() << "layered files:" << line;
        qInfo().noquote() << "layered files:" << (layeredOk ? "ok" : "FAILED");
    }

    const bool animationOk = runAnimationUi(window, engine, penTap, capture, settle, outPrefix);
    const bool modelOk = runModelUi(window, engine, penTap, penWindow, capture, settle);
    engine.setModified(false);
    return strokesAdded == 2 && shapeOk && pointsOk && penPanelsOk && toolsOk && panelsOk && coloursOk && windowsOk && galleryOk && layeredOk && animationOk && modelOk ? 0 : 2;
}

int runProductShots(const QString& outputDir)
{
    isolateSettings();
    theme::apply(*qApp);
    QDir out(QDir(outputDir).absolutePath());
    if (!out.mkpath(QStringLiteral("Artworks")))
        return 1;
    Gallery::setFolder(out.filePath(QStringLiteral("Artworks")));
    CanvasEngine engine;
    QString error;
    if (!engine.initialize(&error)) {
        qCritical().noquote() << error;
        return 1;
    }
    const auto& brush = *BrushLibrary::instance().find(QStringLiteral("pencil-6b"));
    auto fill = [&](const QPainterPath& path, const QColor& color, const QString& name) {
        const int layer = engine.addLayer();
        engine.setLayerName(layer, name);
        engine.setSelection(path, QStringLiteral("Selection"));
        engine.fillLayer(layer, color);
        engine.clearSelection();
    };
    const QStringList names = {QStringLiteral("Alpine light"), QStringLiteral("Afterglow"),
                               QStringLiteral("Blue hour"), QStringLiteral("Rose valley")};
    const QColor skies[] = {QColor("#c8d9dc"), QColor("#c4a7be"), QColor("#233959"), QColor("#ebc6bb")};
    for (int scene = 0; scene < names.size(); ++scene) {
        engine.newDocument(QSize(1800, 1200), skies[scene]);
        engine.setLayerName(engine.activeLayerId(), QStringLiteral("Sky"));
        engine.beginGradient();
        engine.setGradient(QPointF(900, 0), QPointF(900, 800), skies[scene], QColor("#f4d7ad"), GradientKind::Linear);
        engine.endGradient(true);
        QPainterPath sun;
        sun.addEllipse(QPointF(1240 - scene * 90, 330), 105, 105);
        fill(sun, QColor("#fff0c9"), QStringLiteral("Sun"));
        const QColor hills[] = {QColor("#8aabae"), QColor("#517e87"), QColor("#285461"), QColor("#143c47")};
        for (int ridge = 0; ridge < 4; ++ridge) {
            QPainterPath path;
            path.moveTo(-20, 1200);
            for (int i = 0; i <= 24; ++i) {
                const qreal x = i * 80 - 20;
                const qreal y = 560 + ridge * 135 + 85 * std::sin(i * 0.48 + ridge * 1.5 + scene * 0.7)
                                + 42 * std::sin(i * 1.16 + ridge);
                path.lineTo(x, y);
            }
            path.lineTo(1850, 1200);
            path.closeSubpath();
            QColor tone = hills[ridge];
            if (scene == 1 || scene == 3)
                tone = QColor::fromHslF(0.91 - ridge * 0.07, 0.15 + ridge * 0.05, 0.62 - ridge * 0.12);
            fill(path, tone, QStringLiteral("Ridge %1").arg(ridge + 1));
        }
        QPainterPath water;
        water.moveTo(690, 1200);
        water.cubicTo(1640, 980, 810, 1000, 1110, 820);
        water.cubicTo(1170, 795, 1280, 790, 1310, 780);
        water.cubicTo(1090, 820, 1020, 835, 1030, 875);
        water.cubicTo(960, 1000, 1440, 950, 1080, 1200);
        water.closeSubpath();
        fill(water, QColor("#b1d4ce"), QStringLiteral("River"));
        // Real brush marks on a separate paint layer, using the same stroke
        // engine as the viewport. No UI or artwork pixels are mocked.
        const int marks = engine.addLayer();
        engine.setLayerName(marks, QStringLiteral("Pencil highlights"));
        for (int i = 0; i < 16; ++i)
            wave(engine, StrokeTool::Brush, brush, 5, 0.45f, QColor("#f6ddb3"),
                 QPointF(180 + i * 67, 830 + (i % 3) * 19),
                 QPointF(290 + i * 67, 805 + (i % 3) * 19), 8);
        QPainterPath trees;
        for (int i = 0; i < 28; ++i) {
            const qreal x = i * 72 - 10;
            const qreal base = 1110 + 55 * std::sin(i * 0.85);
            const qreal h = 60 + (i * 37 % 100);
            if (x > 700 && x < 1270)
                continue;
            trees.moveTo(x, base - h);
            trees.lineTo(x + h * 0.3, base);
            trees.lineTo(x - h * 0.3, base);
            trees.closeSubpath();
        }
        fill(trees, QColor("#102e38"), QStringLiteral("Pines"));
        if (!docio::save(engine, QDir(Gallery::folder()).filePath(names[scene] + QStringLiteral(".imp")), &error)) {
            qCritical().noquote() << error;
            return 1;
        }
    }
    MainWindow window(&engine);
    window.resize(1600, 1000);
    if (!window.openPath(QDir(Gallery::folder()).filePath(names.first() + QStringLiteral(".imp"))))
        return 1;
    window.show();
    auto settle = [] {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 400) {
            QApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(5);
        }
    };
    auto capture = [&](const QString& name) {
        settle();
        auto* host = window.centralWidget();
        auto* canvas = host->findChild<CanvasWidget*>();
        QImage image = canvas->grabFramebuffer().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        for (QObject* child : host->children()) {
            auto* widget = qobject_cast<QWidget*>(child);
            if (widget && widget != canvas && widget->isVisible())
                widget->render(&painter, widget->pos(), QRegion(), QWidget::DrawChildren);
        }
        painter.end();
        return image.save(out.filePath(name + QStringLiteral(".png")));
    };
    window.showForScreenshot(QStringLiteral("layers"));
    bool ok = capture(QStringLiteral("painting"));
    window.showForScreenshot(QStringLiteral("brushes"));
    ok = capture(QStringLiteral("brushes")) && ok;
    window.resize(1280, 620);
    window.showGallery();
    ok = capture(QStringLiteral("gallery")) && ok;
    auto* more = window.findChild<QAbstractButton*>(QStringLiteral("gallery-menu"));
    bool opened = false, closed = false, escaped = false;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &window, [&] {
        ok = false;
        if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();
        if (auto* popup = QApplication::activePopupWidget()) popup->close();
    });
    watchdog.start(10000);
    QTimer::singleShot(100, &window, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        auto* action = menu ? menu->findChild<QAction*>(QStringLiteral("gallery-about")) : nullptr;
        if (!action) {
            if (menu) menu->close();
            return;
        }
        menu->close();
        QTimer::singleShot(100, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("about-dialog"));
            if (!dialog) return;
            const auto* version = dialog->findChild<QLabel*>(QStringLiteral("about-version"));
            const auto* logo = dialog->findChild<QLabel*>(QStringLiteral("about-logo"));
            const auto* support = dialog->findChild<QLabel*>(QStringLiteral("about-support"));
            const auto* repo = dialog->findChild<QLabel*>(QStringLiteral("about-repository"));
            const auto* stack = dialog->findChild<QLabel*>(QStringLiteral("about-stack"));
            const auto* credit = dialog->findChild<QLabel*>(QStringLiteral("about-credit"));
            opened = dialog->isVisible() && version && version->text().contains(QApplication::applicationVersion())
                     && logo && !logo->pixmap().isNull()
                     && support && support->openExternalLinks() && support->text().contains(QStringLiteral("mailto:impasto@ozzy.co.nl"))
                     && repo && repo->openExternalLinks() && repo->text().contains(QStringLiteral("https://github.com/ozzy-apps/impasto"))
                     && stack && stack->text().contains(QStringLiteral("C++20 · Qt 6 · OpenGL"))
                     && credit && credit->text().contains(QStringLiteral("Made with love by Ozzy")) && credit->text().contains(QStringLiteral("❤️"));
            ok = dialog->grab().save(out.filePath(QStringLiteral("about.png"))) && ok;
            auto* close = dialog->findChild<QPushButton*>(QStringLiteral("about-close"));
            if (close) close->click();
            else dialog->reject();
            closed = !dialog->isVisible();
        });
        action->trigger();
        QTimer::singleShot(100, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(QStringLiteral("about-dialog"));
            if (!dialog) return;
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(dialog, &escape);
            escaped = !dialog->isVisible();
        });
        action->trigger();
    });
    if (more) more->click();
    watchdog.stop();
    qInfo() << "Product screenshots:" << ok << "Gallery About/version/logo/links/credit/stack:" << opened << "Close:" << closed << "Escape:" << escaped;
    // Closing About must leave the artwork and gallery intact.
    ok = ok && opened && closed && escaped && window.gallery()->isVisible() && engine.canvasSize() == QSize(1800, 1200);
    engine.setModified(false);
    return ok ? 0 : 2;
}
