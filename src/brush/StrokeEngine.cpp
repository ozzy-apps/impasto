#include "brush/StrokeEngine.h"

#include "core/Curve.h"

#include <QRandomGenerator>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

double dist(const QPointF& a, const QPointF& b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

// Centripetal Catmull-Rom (Barry-Goldman form). Unlike the uniform variant it
// never loops or overshoots when samples are unevenly spaced.
QPointF catmullRom(const QPointF& p0, const QPointF& p1, const QPointF& p2, const QPointF& p3, double u)
{
    auto knot = [](const QPointF& a, const QPointF& b) { return std::max(std::sqrt(dist(a, b)), 1e-4); };
    const double t0 = 0.0;
    const double t1 = t0 + knot(p0, p1);
    const double t2 = t1 + knot(p1, p2);
    const double t3 = t2 + knot(p2, p3);
    const double t = t1 + (t2 - t1) * u;

    const QPointF a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
    const QPointF a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
    const QPointF a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
    const QPointF b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
    const QPointF b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
    return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
}

float mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

} // namespace

float StrokeEngine::rand01()
{
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return float(m_rng & 0xffffffu) / float(0x1000000);
}

float StrokeEngine::nextSpacing() const
{
    const float fraction = m_spacingOverride > 0.0f ? m_spacingOverride : m_preset.spacing;
    return std::max(0.3f, fraction * std::max(m_lastDabSize, 1.0f));
}

StrokeEngine::Node StrokeEngine::makeNode(const QPointF& pos, float pressure) const
{
    Node n;
    n.pos = pos;
    n.pressure = std::clamp(pressure, 0.0f, 1.0f);
    n.tilt = std::clamp(std::hypot(m_lastRaw.tiltX, m_lastRaw.tiltY) / 60.0f, 0.0f, 1.0f);
    n.azimuth = n.tilt > 0.02f ? std::atan2(m_lastRaw.tiltY, m_lastRaw.tiltX) : 0.0f;
    n.speed = m_speed;
    return n;
}

void StrokeEngine::begin(const BrushPreset& preset, float size, const QColor& color, const StrokeSample& s)
{
    m_preset = preset;
    m_size = size;
    // What the brush makes of pen pressure: its own curve if it has one, or
    // else the plain power curve. Looked up for every dab, so worked out once.
    for (int i = 0; i <= 256; ++i) {
        const float x = float(i) / 256.0f;
        m_pressureLut[i] = preset.pressureCurve.size() >= 2 ? curveValue(preset.pressureCurve, x) : std::pow(x, preset.pressureGamma);
    }
    m_rgb[0] = color.redF();
    m_rgb[1] = color.greenF();
    m_rgb[2] = color.blueF();
    float h = 0, sat = 0, v = 0;
    color.getHsvF(&h, &sat, &v);
    m_hsv[0] = h < 0.0f ? 0.0f : h;
    m_hsv[1] = sat;
    m_hsv[2] = v;

    m_nodes.clear();
    m_dabs.clear();
    m_rendered = 0;
    m_emitted = 0;
    m_lastRaw = s;
    m_smoothPos = s.pos;
    m_smoothPressure = s.pressure;
    m_speed = 0.0f;
    m_lastTime = s.time;
    m_distToNext = 0.0;   // first dab lands exactly on the start point
    m_travel = 0.0;
    m_lastDabSize = size;
    m_haveDir = false;
    m_rng = m_rngStart = (m_seed ? m_seed : QRandomGenerator::global()->generate()) | 1u;

    const double stab = std::clamp(double(preset.stabilization) + double(m_extraStabilization), 0.0, 1.0);
    m_window = stab * stab * 0.22;
    m_stringLength = double(std::clamp(preset.stringLength, 0.0f, 1.0f)) * kMaxStringPx * m_inputScale;
    m_stringPos = s.pos;
    m_recent.clear();
    m_direct = false;
    m_endTaper = false;
    m_totalLength = 0.0;

    m_nodes.push_back(makeNode(s.pos, s.pressure));
}

void StrokeEngine::add(const StrokeSample& s)
{
    const double dt = std::clamp(s.time - m_lastTime, 0.001, 0.05);
    m_lastTime = s.time;

    const float instSpeed = float(dist(s.pos, m_lastRaw.pos) / dt);
    m_speed += (instSpeed - m_speed) * float(1.0 - std::exp(-dt / 0.03));
    m_lastRaw = s;

    // 1. Pull string: the drawing end only moves once the pen has pulled the
    //    string taut, and then follows exactly. Slow, deliberate lines with
    //    sharp corners, and no lag to wait out.
    QPointF target = s.pos;
    if (m_stringLength > 0.0) {
        const QPointF d = s.pos - m_stringPos;
        const double len = std::hypot(d.x(), d.y());
        if (len > m_stringLength)
            m_stringPos += d * ((len - m_stringLength) / len);
        target = m_stringPos;
    }

    // 2. Stabilisation: the average position over a short stretch of time. A
    //    fast stroke covers more ground in that time, so it is smoothed more.
    if (m_window > 0.0) {
        m_recent.push_back({s.time, target});
        while (m_recent.size() > 1 && m_recent.front().time < s.time - m_window)
            m_recent.pop_front();
        QPointF sum;
        for (const Timed& r : m_recent)
            sum += r.pos;
        target = sum / double(m_recent.size());
    }

    // 3. StreamLine: the drawn point chases that target with a time constant.
    //    A small floor keeps sensor noise out even when everything is off.
    const double sl = std::clamp(double(m_preset.streamline), 0.0, 1.0);
    const double tau = 0.004 + sl * sl * 0.30;
    m_smoothPos += (target - m_smoothPos) * (1.0 - std::exp(-dt / tau));

    // Line weight gets its own smoothing, so a steady width does not have to
    // cost a laggy line.
    const double ps = std::clamp(double(m_preset.pressureSmoothing), 0.0, 1.0);
    const double tauP = 0.012 + sl * 0.05 + ps * ps * 0.35;
    m_smoothPressure += (s.pressure - m_smoothPressure) * float(1.0 - std::exp(-dt / tauP));

    pushNode(makeNode(m_smoothPos, m_smoothPressure));
}

void StrokeEngine::addDirect(const PathPoint& point)
{
    m_lastRaw.pos = point.pos;
    m_lastRaw.pressure = point.pressure;
    m_lastRaw.tiltX = point.tiltX;
    m_lastRaw.tiltY = point.tiltY;
    m_smoothPos = point.pos;
    m_smoothPressure = point.pressure;
    m_speed = point.speed;
    m_direct = true;
    Node n = makeNode(point.pos, point.pressure);
    n.corner = point.corner;
    pushNode(n);
}

void StrokeEngine::pushNode(const Node& n)
{
    if (!m_nodes.empty() && dist(n.pos, m_nodes.back().pos) < 0.2)
        return;
    m_nodes.push_back(n);
    // A segment needs the node after it for its exit tangent, so drawing runs
    // one sample behind the pen.
    while (m_rendered + 2 < m_nodes.size()) {
        renderSegment(m_rendered);
        ++m_rendered;
    }
}

bool StrokeEngine::end()
{
    // Close the gap that smoothing left between the line and the pen. A pulled
    // string is the exception: the line stops where the string holds it.
    const QPointF finish = m_stringLength > 0.0 ? m_stringPos : m_lastRaw.pos;
    const double gap = m_direct ? 0.0 : dist(m_smoothPos, finish);
    if (gap > 0.2) {
        const int steps = std::clamp(int(gap / 4.0), 1, 24);
        const QPointF from = m_smoothPos;
        const float p0 = m_smoothPressure;
        for (int k = 1; k <= steps; ++k) {
            const float t = float(k) / float(steps);
            pushNode(makeNode(from + (finish - from) * t, mix(p0, m_lastRaw.pressure, t)));
        }
    }
    while (m_rendered + 1 < m_nodes.size()) {
        renderSegment(m_rendered);
        ++m_rendered;
    }
    // A tap without movement still leaves a mark.
    if (m_emitted == 0 && !m_nodes.empty()) {
        Node n = m_nodes.front();
        n.pressure = std::max(n.pressure, m_lastRaw.pressure);
        emitDab(n, 0.0f, 1e9);
    }
    if (m_taper && m_preset.taperEnd > 0.0f && m_nodes.size() > 1) {
        regenerate();
        return true;
    }
    return false;
}

// Runs the whole path again, now that its length is known, so the last stretch
// can thin out. The seed is rewound, so everything before the taper comes out
// exactly as it was drawn.
void StrokeEngine::regenerate()
{
    m_totalLength = 0.0;
    for (size_t i = 1; i < m_nodes.size(); ++i)
        m_totalLength += dist(m_nodes[i].pos, m_nodes[i - 1].pos);
    m_dabs.clear();
    m_emitted = 0;
    m_distToNext = 0.0;
    m_travel = 0.0;
    m_lastDabSize = m_size;
    m_haveDir = false;
    m_rng = m_rngStart;
    m_endTaper = true;
    for (size_t i = 0; i + 1 < m_nodes.size(); ++i)
        renderSegment(i);
    m_rendered = m_nodes.size() - 1;
    m_endTaper = false;
}

void StrokeEngine::renderSegment(size_t i)
{
    const size_t last = m_nodes.size() - 1;
    const Node& p1 = m_nodes[i];
    const Node& p2 = m_nodes[i + 1];
    // At a corner the neighbouring segment must not bend this one's tangent.
    const Node& p0 = m_nodes[(i > 0 && !p1.corner) ? i - 1 : i];
    const Node& p3 = m_nodes[(i + 2 <= last && !p2.corner) ? i + 2 : i + 1];

    const double chord = dist(p1.pos, p2.pos);
    const int steps = std::clamp(int(std::ceil(chord / 1.5)), 1, 4000);
    QPointF prev = p1.pos;
    double tPrev = 0.0;
    int budget = 20000;   // guards against runaway dab counts on huge jumps

    for (int k = 1; k <= steps; ++k) {
        const double t = double(k) / double(steps);
        const QPointF cur = k == steps ? p2.pos : catmullRom(p0.pos, p1.pos, p2.pos, p3.pos, t);
        const QPointF d = cur - prev;
        const double len = std::hypot(d.x(), d.y());
        if (len > 1e-6) {
            const QPointF unit = d / len;
            m_dir = m_haveDir ? m_dir * 0.6 + unit * 0.4 : unit;
            m_haveDir = true;
            const float direction = float(std::atan2(m_dir.y(), m_dir.x()));

            double walked = 0.0;
            while (m_distToNext <= len - walked && budget-- > 0) {
                walked += m_distToNext;
                const double f = walked / len;
                const float tt = float(tPrev + (t - tPrev) * f);
                Node n;
                n.pos = prev + d * f;
                n.pressure = mix(p1.pressure, p2.pressure, tt);
                n.tilt = mix(p1.tilt, p2.tilt, tt);
                n.azimuth = p2.azimuth;
                n.speed = mix(p1.speed, p2.speed, tt);
                emitDab(n, direction, m_travel + walked);
                m_distToNext = nextSpacing();
            }
            m_distToNext -= (len - walked);
            m_travel += len;
        }
        prev = cur;
        tPrev = t;
    }
}

void StrokeEngine::emitDab(const Node& n, float direction, double travel)
{
    const BrushPreset& b = m_preset;
    const float at = std::clamp(n.pressure, 0.0f, 1.0f) * 256.0f;
    const int cell = std::min(255, int(at));
    const float p = m_pressureLut[cell] + (m_pressureLut[cell + 1] - m_pressureLut[cell]) * (at - float(cell));

    float size = m_size * (1.0f - b.pressureSize * (1.0f - p));
    size *= 1.0f + b.tiltSize * n.tilt;
    if (b.speedSize != 0.0f)
        size *= std::clamp(1.0f - b.speedSize * std::clamp(n.speed / 4000.0f, 0.0f, 1.0f), 0.1f, 2.0f);
    if (m_taper) {
        const float tip = std::clamp(b.taperTip, 0.0f, 1.0f);
        if (b.taper > 0.0f) {
            const float lead = std::clamp(float(travel) / (b.taper * m_size), 0.0f, 1.0f);
            size *= tip + (1.0f - tip) * std::sqrt(lead);
        }
        if (m_endTaper && b.taperEnd > 0.0f) {
            const float left = std::clamp(float(m_totalLength - travel) / (b.taperEnd * m_size), 0.0f, 1.0f);
            size *= tip + (1.0f - tip) * std::sqrt(left);
        }
    }

    float flow = b.flow * (1.0f - b.pressureFlow * (1.0f - p));
    flow *= 1.0f - b.tiltFlow * n.tilt;
    if (b.speedFlow > 0.0f)
        flow *= std::clamp(1.0f - b.speedFlow * std::clamp(n.speed / 4000.0f, 0.0f, 1.0f), 0.03f, 1.0f);

    m_lastDabSize = size;
    ++m_emitted;

    const QPointF along(std::cos(direction), std::sin(direction));
    const QPointF across(-along.y(), along.x());

    const int count = std::max(1, b.count);
    for (int c = 0; c < count; ++c) {
        Dab d;
        float s = size;
        if (b.sizeJitter > 0.0f)
            s *= 1.0f - b.sizeJitter * rand01();
        float fl = flow;
        if (b.flowJitter > 0.0f)
            fl *= 1.0f - b.flowJitter * rand01();
        if (s < 0.02f || fl <= 0.0f)
            continue;

        QPointF pos = n.pos;
        if (b.scatter > 0.0f) {
            pos += across * double(randSigned() * b.scatter * size);
            pos += along * double(randSigned() * b.scatter * size * 0.5f);
        }

        float angle = b.angle * float(kPi / 180.0);
        if (b.angleSource == BrushPreset::AngleSource::Direction)
            angle += direction;
        else if (b.angleSource == BrushPreset::AngleSource::Azimuth)
            angle += n.tilt > 0.02f ? n.azimuth : direction;
        if (b.angleJitter > 0.0f)
            angle += randSigned() * b.angleJitter * float(kPi);

        d.x = float(pos.x());
        d.y = float(pos.y());
        d.size = s;
        d.angle = angle;
        if (b.hueJitter > 0.0f || b.satJitter > 0.0f || b.valJitter > 0.0f) {
            float h = m_hsv[0] + randSigned() * b.hueJitter * 0.5f;
            h -= std::floor(h);
            const float sa = std::clamp(m_hsv[1] + randSigned() * b.satJitter, 0.0f, 1.0f);
            const float va = std::clamp(m_hsv[2] + randSigned() * b.valJitter, 0.0f, 1.0f);
            const QColor c2 = QColor::fromHsvF(h, sa, va);
            d.r = c2.redF();
            d.g = c2.greenF();
            d.b = c2.blueF();
        } else {
            d.r = m_rgb[0];
            d.g = m_rgb[1];
            d.b = m_rgb[2];
        }
        d.flow = std::clamp(fl, 0.0f, 1.0f);
        d.roundness = b.roundness;
        d.seedX = rand01();
        d.seedY = rand01();
        d.pad = 0.0f;
        m_dabs.push_back(d);
    }
}
