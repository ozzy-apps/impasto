#include "brush/TextureGen.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace texgen {
namespace {

constexpr float kPi = 3.14159265358979f;

inline uint32_t hash(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline float rnd(int x, int y, uint32_t seed)
{
    return float(hash(uint32_t(x), uint32_t(y), seed) & 0xffffffu) / float(0x1000000);
}

inline float smooth(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Value noise that wraps every `period` lattice cells, so textures tile.
float vnoise(float x, float y, int period, uint32_t seed)
{
    const int xi = int(std::floor(x));
    const int yi = int(std::floor(y));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    auto wrap = [period](int a) { return ((a % period) + period) % period; };
    const float u = fx * fx * (3.0f - 2.0f * fx);
    const float v = fy * fy * (3.0f - 2.0f * fy);
    const float a = rnd(wrap(xi), wrap(yi), seed);
    const float b = rnd(wrap(xi + 1), wrap(yi), seed);
    const float c = rnd(wrap(xi), wrap(yi + 1), seed);
    const float d = rnd(wrap(xi + 1), wrap(yi + 1), seed);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

// The same with a period of its own each way, for textures that run in one direction.
float vnoise2(float x, float y, int periodX, int periodY, uint32_t seed)
{
    const int xi = int(std::floor(x));
    const int yi = int(std::floor(y));
    const float fx = x - float(xi);
    const float fy = y - float(yi);
    auto wrapX = [periodX](int a) { return ((a % periodX) + periodX) % periodX; };
    auto wrapY = [periodY](int a) { return ((a % periodY) + periodY) % periodY; };
    const float u = fx * fx * (3.0f - 2.0f * fx);
    const float v = fy * fy * (3.0f - 2.0f * fy);
    const float a = rnd(wrapX(xi), wrapY(yi), seed);
    const float b = rnd(wrapX(xi + 1), wrapY(yi), seed);
    const float c = rnd(wrapX(xi), wrapY(yi + 1), seed);
    const float d = rnd(wrapX(xi + 1), wrapY(yi + 1), seed);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

// One point scattered in every cell of a grid that wraps: how far it is to
// the nearest of them, and to the next nearest. Their difference is small
// along the borders between cells, which draws a network of cracks.
void cellDistances(float u, float v, int cells, uint32_t seed, float* nearest, float* second)
{
    const float x = u * float(cells), y = v * float(cells);
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    auto wrap = [cells](int a) { return ((a % cells) + cells) % cells; };
    *nearest = *second = 9.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int cx = xi + dx, cy = yi + dy;
            const float px = float(cx) + rnd(wrap(cx), wrap(cy), seed);
            const float py = float(cy) + rnd(wrap(cx), wrap(cy), seed + 7u);
            const float d = std::hypot(px - x, py - y);
            if (d < *nearest) {
                *second = *nearest;
                *nearest = d;
            } else if (d < *second) {
                *second = d;
            }
        }
    }
}

// u, v in 0..1 across the tile. Returns roughly 0..1.
float fbm(float u, float v, int basePeriod, int octaves, float gain, uint32_t seed)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    int period = basePeriod;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * vnoise(u * float(period), v * float(period), period, seed + uint32_t(o) * 101u);
        norm += amp;
        amp *= gain;
        period *= 2;
    }
    return sum / norm;
}

// Gradient noise that wraps every `px` by `py` lattice cells. Unlike value
// noise it has no grid showing through, which thresholded paper tooth
// would otherwise turn into little squares. Returns roughly -1..1.
float gnoise(float x, float y, int px, int py, uint32_t seed)
{
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    const float fx = x - float(xi), fy = y - float(yi);
    auto wrapX = [px](int a) { return ((a % px) + px) % px; };
    auto wrapY = [py](int a) { return ((a % py) + py) % py; };
    auto grad = [&](int cx, int cy, float dx, float dy) {
        const float a = rnd(wrapX(cx), wrapY(cy), seed) * 2.0f * kPi;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const float u = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float v = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const float n00 = grad(xi, yi, fx, fy), n10 = grad(xi + 1, yi, fx - 1.0f, fy);
    const float n01 = grad(xi, yi + 1, fx, fy - 1.0f), n11 = grad(xi + 1, yi + 1, fx - 1.0f, fy - 1.0f);
    const float a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
    return (a + (b - a) * v) * 1.41f;
}

// Octaves of gradient noise over the tile (u, v in 0..1); `stretch` makes the
// cells longer across x than y (fibres, laid lines). Roughly -1..1.
float gfbm(float u, float v, int base, int octaves, float gain, uint32_t seed, int stretch = 1)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    int period = base;
    for (int o = 0; o < octaves; ++o) {
        const int py = period, pxp = std::max(1, period / stretch);
        sum += amp * gnoise(u * float(pxp), v * float(py), pxp, py, seed + uint32_t(o) * 131u);
        norm += amp;
        amp *= gain;
        period *= 2;
    }
    return sum / norm;
}

// Soft bumps, one in each cell of a wrapping grid: the raised tooth of a
// paper. 1 on top of a bump, falling to 0 between them.
float bumps(float u, float v, int cells, uint32_t seed)
{
    float nearest = 0.0f, second = 0.0f;
    cellDistances(u, v, cells, seed, &nearest, &second);
    return std::clamp(1.0f - nearest * 1.25f, 0.0f, 1.0f);
}

struct Rng {
    uint32_t s;
    float next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(s & 0xffffffu) / float(0x1000000);
    }
    float range(float a, float b) { return a + (b - a) * next(); }
};

using Field = std::vector<float>;

QImage toImage(const Field& f)
{
    QImage img(kSize, kSize, QImage::Format_Grayscale8);
    for (int y = 0; y < kSize; ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < kSize; ++x)
            row[x] = uchar(std::clamp(f[size_t(y) * kSize + x], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return img;
}

// Spreads the values of a texture evenly over 0..1, keeping their order.
// Paper tooth is used by threshold: light pressure catches the top tenth of
// it, harder pressure the top half. Even values make that response even, so
// a pressure ramp shades smoothly instead of jumping.
void equalize(Field& f)
{
    std::vector<std::pair<float, uint32_t>> order(f.size());
    for (uint32_t i = 0; i < f.size(); ++i)
        order[i] = {f[i], i};
    std::sort(order.begin(), order.end());
    const float n = float(f.size() - 1);
    for (size_t r = 0; r < order.size(); ++r)
        f[order[r].second] = float(r) / n;
}

QImage renderEven(const std::function<float(float, float, float, float)>& fn)
{
    Field f(size_t(kSize) * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float u = (float(x) + 0.5f) / kSize;
            const float v = (float(y) + 0.5f) / kSize;
            f[size_t(y) * kSize + x] = fn(u * 2.0f - 1.0f, v * 2.0f - 1.0f, u, v);
        }
    }
    equalize(f);
    return toImage(f);
}

// fn receives pixel-centre coordinates mapped to -1..1 (x, y) and 0..1 (u, v).
QImage render(const std::function<float(float, float, float, float)>& fn)
{
    Field f(size_t(kSize) * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float u = (float(x) + 0.5f) / kSize;
            const float v = (float(y) + 0.5f) / kSize;
            f[size_t(y) * kSize + x] = fn(u * 2.0f - 1.0f, v * 2.0f - 1.0f, u, v);
        }
    }
    return toImage(f);
}

// Max-blends a soft disc into the field; coordinates are in pixels.
void splat(Field& f, float cx, float cy, float r, float intensity, float hardness)
{
    const int x0 = std::max(0, int(cx - r - 1)), x1 = std::min(kSize - 1, int(cx + r + 1));
    const int y0 = std::max(0, int(cy - r - 1)), y1 = std::min(kSize - 1, int(cy + r + 1));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy) / r;
            const float a = intensity * smooth(1.0f, hardness, d);
            float& p = f[size_t(y) * kSize + x];
            p = std::max(p, a);
        }
    }
}

// A soft ellipse, max-blended: rx across, ry down, in pixels, turned by `angle`.
void splatOval(Field& f, float cx, float cy, float rx, float ry, float angle, float intensity, float hardness)
{
    const float r = std::max(rx, ry);
    const float c = std::cos(angle), sn = std::sin(angle);
    const int x0 = std::max(0, int(cx - r - 1)), x1 = std::min(kSize - 1, int(cx + r + 1));
    const int y0 = std::max(0, int(cy - r - 1)), y1 = std::min(kSize - 1, int(cy + r + 1));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = float(x) + 0.5f - cx, dy = float(y) + 0.5f - cy;
            const float lx = (c * dx + sn * dy) / rx, ly = (-sn * dx + c * dy) / ry;
            const float a = intensity * smooth(1.0f, hardness, std::hypot(lx, ly));
            float& p = f[size_t(y) * kSize + x];
            p = std::max(p, a);
        }
    }
}

// A stroke of the splat from one point to another, its radius changing on the way.
void drag(Field& f, float x0, float y0, float x1, float y1, float r0, float r1, float intensity, float hardness)
{
    const int steps = std::max(2, int(std::hypot(x1 - x0, y1 - y0) / std::max(0.5f, std::min(r0, r1) * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        splat(f, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, r0 + (r1 - r0) * t, intensity, hardness);
    }
}

// Round dots thrown into a disc: evenly, or crowding towards the middle.
QImage dots(uint32_t seed, int count, float spread, float rMin, float rMax, float hardness, bool crowded)
{
    Field f(size_t(kSize) * kSize, 0.0f);
    Rng rng{seed};
    for (int i = 0; i < count; ++i) {
        const float ang = rng.range(0.0f, 2.0f * kPi);
        const float m = crowded ? spread * std::sqrt(-2.0f * std::log(std::max(rng.next(), 1e-5f))) : spread * std::sqrt(rng.next());
        const float t = rng.next();
        const float r = (rMin + (rMax - rMin) * t * t) * kSize;
        const float x = 0.5f + std::cos(ang) * m, y = 0.5f + std::sin(ang) * m;
        if (std::hypot(x - 0.5f, y - 0.5f) + r / kSize > 0.49f)
            continue;
        splat(f, x * kSize, y * kSize, r, rng.range(0.55f, 1.0f), hardness);
    }
    return toImage(f);
}

// Blades or hairs growing from a base line, each bending its own way.
QImage blades(uint32_t seed, int count, float baseY, float spreadX, float length, float lean, float thick)
{
    Field f(size_t(kSize) * kSize, 0.0f);
    Rng rng{seed};
    for (int i = 0; i < count; ++i) {
        float x = (0.5f + rng.range(-spreadX, spreadX)) * kSize, y = baseY * kSize;
        const float reach = length * rng.range(0.55f, 1.0f) * kSize;
        float heading = -kPi / 2.0f + rng.range(-lean, lean);
        const float bend = rng.range(-0.9f, 0.9f) / 26.0f;
        const float root = thick * rng.range(0.7f, 1.2f) * kSize;
        const float tone = rng.range(0.7f, 1.0f);
        constexpr int steps = 26;
        for (int k = 0; k < steps; ++k) {
            const float t = float(k) / float(steps);
            const float nx = x + std::cos(heading) * reach / steps, ny = y + std::sin(heading) * reach / steps;
            drag(f, x, y, nx, ny, root * (1.0f - t * 0.92f), root * (1.0f - (t + 1.0f / steps) * 0.92f), tone, 0.5f);
            x = nx;
            y = ny;
            heading += bend;
        }
    }
    return toImage(f);
}

// Clusters of bristle ends inside an ellipse. Dragged along a stroke they leave
// parallel streaks, which is where the painterly look comes from.
// Each bristle end is drawn a little longer along x than across: dragged
// along the stroke the stamps run together into streaks instead of beading.
QImage bristles(uint32_t seed, float rx, float ry, int count, float dotMin, float dotMax, float stretch = 1.8f)
{
    Field f(size_t(kSize) * kSize, 0.0f);
    Rng rng{seed};
    for (int i = 0; i < count; ++i) {
        const float rad = std::sqrt(rng.next());
        const float ang = rng.range(0.0f, 2.0f * kPi);
        const float x = 0.5f + std::cos(ang) * rad * rx;
        const float y = 0.5f + std::sin(ang) * rad * ry;
        const float r = rng.range(dotMin, dotMax) * kSize;
        // Bristles near the rim carry less paint.
        const float load = rng.range(0.55f, 1.0f) * (1.0f - 0.35f * rad * rad);
        const float along = std::min(r * stretch, (0.49f - std::abs(x - 0.5f)) * kSize);
        splatOval(f, x * kSize, y * kSize, std::max(along, r), r, 0.0f, load, 0.35f);
    }
    return toImage(f);
}

// Paint thrown at the paper: a few big drops, each with a crown of small
// ones and some tails flung outwards, and a fine mist between them.
QImage splatter(uint32_t seed)
{
    Field f(size_t(kSize) * kSize, 0.0f);
    Rng rng{seed};
    auto inside = [](float x, float y, float r) { return std::hypot(x - 0.5f, y - 0.5f) + r / kSize < 0.48f; };
    for (int i = 0; i < 7; ++i) {
        const float m = 0.2f * std::sqrt(rng.next());
        const float ang = rng.range(0.0f, 2.0f * kPi);
        const float x = 0.5f + std::cos(ang) * m, y = 0.5f + std::sin(ang) * m;
        const float r = rng.range(0.025f, 0.06f) * kSize;
        if (!inside(x, y, r))
            continue;
        // The drop, not quite round.
        splatOval(f, x * kSize, y * kSize, r * rng.range(0.85f, 1.2f), r, rng.range(0.0f, kPi), 1.0f, 0.75f);
        const int crown = 5 + int(rng.next() * 9);
        for (int k = 0; k < crown; ++k) {
            const float a = rng.range(0.0f, 2.0f * kPi);
            const float d = r * rng.range(1.2f, 2.6f);
            const float cx = x * kSize + std::cos(a) * d, cy = y * kSize + std::sin(a) * d;
            const float cr = r * rng.range(0.08f, 0.28f);
            if (!inside(cx / kSize, cy / kSize, cr * 3.0f))
                continue;
            if (rng.next() < 0.4f) {
                // A tail flung outwards, thinning to its end.
                drag(f, x * kSize + std::cos(a) * r * 0.8f, y * kSize + std::sin(a) * r * 0.8f, cx, cy, r * 0.22f, cr * 0.6f, 1.0f, 0.7f);
            }
            splat(f, cx, cy, cr, 1.0f, 0.7f);
        }
    }
    for (int i = 0; i < 140; ++i) {
        const float m = 0.16f * std::sqrt(-2.0f * std::log(std::max(rng.next(), 1e-5f)));
        const float ang = rng.range(0.0f, 2.0f * kPi);
        const float x = 0.5f + std::cos(ang) * m, y = 0.5f + std::sin(ang) * m;
        const float t = rng.next();
        const float r = (0.003f + 0.012f * t * t) * kSize;
        if (!inside(x, y, r))
            continue;
        splat(f, x * kSize, y * kSize, r, rng.range(0.7f, 1.0f), 0.65f);
    }
    return toImage(f);
}

} // namespace

namespace {

struct Named {
    const char* name;
    const char* title;
};

// In the order the brush lab shows them.
const Named kTips[] = {
    {"bristle", QT_TRANSLATE_NOOP("texgen", "Bristles")},       {"flat_bristle", QT_TRANSLATE_NOOP("texgen", "Flat bristles")},
    {"dry", QT_TRANSLATE_NOOP("texgen", "Dry bristles")},       {"fan", QT_TRANSLATE_NOOP("texgen", "Fan")},
    {"rake", QT_TRANSLATE_NOOP("texgen", "Rake")},              {"knife", QT_TRANSLATE_NOOP("texgen", "Knife")},
    {"chalk", QT_TRANSLATE_NOOP("texgen", "Chalk")},            {"charcoal", QT_TRANSLATE_NOOP("texgen", "Charcoal")},
    {"crayon", QT_TRANSLATE_NOOP("texgen", "Crayon")},          {"pastel", QT_TRANSLATE_NOOP("texgen", "Pastel")},
    {"square", QT_TRANSLATE_NOOP("texgen", "Square")},          {"ink_blot", QT_TRANSLATE_NOOP("texgen", "Ink blot")},
    {"hatch", QT_TRANSLATE_NOOP("texgen", "Hatching")},         {"scratch", QT_TRANSLATE_NOOP("texgen", "Scratches")},
    {"stipple", QT_TRANSLATE_NOOP("texgen", "Stipple")},        {"splatter", QT_TRANSLATE_NOOP("texgen", "Splatter")},
    {"spray_fine", QT_TRANSLATE_NOOP("texgen", "Fine spray")},  {"sponge", QT_TRANSLATE_NOOP("texgen", "Sponge")},
    {"cloud", QT_TRANSLATE_NOOP("texgen", "Cloud")},            {"smoke", QT_TRANSLATE_NOOP("texgen", "Smoke")},
    {"leaf", QT_TRANSLATE_NOOP("texgen", "Leaf")},              {"grass", QT_TRANSLATE_NOOP("texgen", "Grass")},
    {"fur", QT_TRANSLATE_NOOP("texgen", "Fur")},                {"flower", QT_TRANSLATE_NOOP("texgen", "Flower")},
    {"drop", QT_TRANSLATE_NOOP("texgen", "Raindrop")},          {"snow", QT_TRANSLATE_NOOP("texgen", "Snowflake")},
    {"star", QT_TRANSLATE_NOOP("texgen", "Sparkle")},           {"glow", QT_TRANSLATE_NOOP("texgen", "Glow")},
    {"bokeh", QT_TRANSLATE_NOOP("texgen", "Bokeh")},            {"bubble", QT_TRANSLATE_NOOP("texgen", "Bubble")},
    {"flick", QT_TRANSLATE_NOOP("texgen", "Flick")},            {"strands", QT_TRANSLATE_NOOP("texgen", "Strands")},
};

const Named kGrains[] = {
    {"paper", QT_TRANSLATE_NOOP("texgen", "Paper")},              {"fine", QT_TRANSLATE_NOOP("texgen", "Fine paper")},
    {"cold_press", QT_TRANSLATE_NOOP("texgen", "Cold press")},    {"fibre", QT_TRANSLATE_NOOP("texgen", "Fibres")},
    {"canvas", QT_TRANSLATE_NOOP("texgen", "Canvas")},            {"linen", QT_TRANSLATE_NOOP("texgen", "Linen")},
    {"burlap", QT_TRANSLATE_NOOP("texgen", "Burlap")},            {"charcoal", QT_TRANSLATE_NOOP("texgen", "Charcoal paper")},
    {"sand", QT_TRANSLATE_NOOP("texgen", "Sand")},                {"speckle", QT_TRANSLATE_NOOP("texgen", "Speckle")},
    {"concrete", QT_TRANSLATE_NOOP("texgen", "Concrete")},        {"blotch", QT_TRANSLATE_NOOP("texgen", "Blotches")},
    {"clouds", QT_TRANSLATE_NOOP("texgen", "Clouds")},            {"wood", QT_TRANSLATE_NOOP("texgen", "Wood")},
    {"crackle", QT_TRANSLATE_NOOP("texgen", "Crackle")},          {"halftone", QT_TRANSLATE_NOOP("texgen", "Halftone dots")},
    {"halftone_coarse", QT_TRANSLATE_NOOP("texgen", "Coarse dots")}, {"lines", QT_TRANSLATE_NOOP("texgen", "Line screen")},
    {"hatch", QT_TRANSLATE_NOOP("texgen", "Hatching")},           {"crosshatch", QT_TRANSLATE_NOOP("texgen", "Crosshatching")},
    {"waves", QT_TRANSLATE_NOOP("texgen", "Waves")},              {"halftone_cone", QT_TRANSLATE_NOOP("texgen", "Halftone screen")},
    {"pen_hatch", QT_TRANSLATE_NOOP("texgen", "Inked hatching")},
};

template <size_t N> QStringList namesOf(const Named (&table)[N])
{
    QStringList out;
    for (const Named& n : table)
        out << QString::fromLatin1(n.name);
    return out;
}

template <size_t N> QString titleOf(const Named (&table)[N], const QString& name)
{
    for (const Named& n : table) {
        if (name == QLatin1String(n.name))
            return QCoreApplication::translate("texgen", n.title);
    }
    return name;
}

} // namespace

QStringList tipNames()
{
    return namesOf(kTips);
}

QStringList grainNames()
{
    return namesOf(kGrains);
}

QString tipTitle(const QString& name)
{
    return titleOf(kTips, name);
}

QString grainTitle(const QString& name)
{
    return titleOf(kGrains, name);
}

QImage tip(const QString& name)
{
    if (name == QLatin1String("bristle"))
        return bristles(11, 0.44f, 0.44f, 320, 0.014f, 0.04f);
    if (name == QLatin1String("flat_bristle"))
        return bristles(23, 0.15f, 0.45f, 260, 0.011f, 0.028f, 2.2f);
    if (name == QLatin1String("dry"))
        return bristles(37, 0.3f, 0.45f, 110, 0.008f, 0.02f, 6.0f);
    if (name == QLatin1String("splatter"))
        return splatter(53);
    if (name == QLatin1String("chalk")) {
        return render([](float x, float y, float u, float v) {
            const float ax = std::abs(x) / 0.82f, ay = std::abs(y) / 0.9f;
            const float d = std::pow(std::pow(ax, 4.0f) + std::pow(ay, 4.0f), 0.25f);
            const float edge = smooth(1.0f, 0.82f, d + (fbm(u, v, 12, 4, 0.6f, 5) - 0.5f) * 0.45f);
            const float body = 0.5f + 0.5f * smooth(0.3f, 0.7f, fbm(u, v, 40, 3, 0.7f, 9));
            return edge * body;
        });
    }
    if (name == QLatin1String("charcoal")) {
        return render([](float x, float y, float u, float v) {
            const float d = std::hypot(x / 0.34f, y / 0.92f);
            const float edge = smooth(1.0f, 0.6f, d + (fbm(u, v, 10, 4, 0.65f, 15) - 0.5f) * 0.7f);
            const float body = 0.25f + 0.75f * smooth(0.36f, 0.62f, fbm(u, v, 56, 3, 0.75f, 19));
            return edge * body;
        });
    }
    if (name == QLatin1String("sponge")) {
        return render([](float x, float y, float u, float v) {
            const float d = std::hypot(x, y);
            const float edge = smooth(0.98f, 0.45f, d + (fbm(u, v, 6, 4, 0.6f, 25) - 0.5f) * 0.6f);
            const float cells = smooth(0.4f, 0.62f, fbm(u, v, 18, 4, 0.6f, 29));
            return edge * (0.15f + 0.85f * cells);
        });
    }
    if (name == QLatin1String("leaf")) {
        // A pointed leaf lying along x, widest a little before the middle, with
        // a midrib and slanting veins left slightly thinner so they read when stamped.
        return render([](float x, float y, float, float) {
            const float t = (x + 0.92f) / 1.84f;   // 0 at the stem, 1 at the tip
            if (t <= 0.0f || t >= 1.0f)
                return 0.0f;
            const float half = 0.36f * std::pow(std::sin(kPi * std::pow(t, 0.8f)), 0.85f);
            const float inside = (half - std::abs(y)) * (kSize * 0.5f);
            const float body = std::clamp(inside + 0.5f, 0.0f, 1.0f);
            const float rib = smooth(0.014f, 0.004f, std::abs(y));
            const float slant = std::abs(y) * 2.2f - x;
            const float band = std::abs(slant * 5.0f - std::floor(slant * 5.0f) - 0.5f);
            const float veins = smooth(0.07f, 0.02f, band) * smooth(0.0f, 0.06f, std::abs(y));
            return body * (1.0f - 0.24f * std::max(rib, veins * 0.7f));
        });
    }
    if (name == QLatin1String("fan")) {
        // Bristle ends along a shallow arc, each a short dash lying along the
        // stroke (y here: the fan is turned across it), so that dragged they
        // leave unbroken fine streaks.
        Field f(size_t(kSize) * kSize, 0.0f);
        Rng rng{61};
        for (int i = 0; i < 170; ++i) {
            const float t = rng.range(-1.0f, 1.0f);
            const float x = 0.5f + t * 0.45f, y = 0.5f + t * t * 0.09f - 0.03f + rng.range(-0.02f, 0.02f);
            const float r = rng.range(0.006f, 0.016f) * kSize;
            splatOval(f, x * kSize, y * kSize, r, r * 3.5f, 0.0f, rng.range(0.5f, 1.0f) * (1.0f - 0.4f * t * t), 0.4f);
        }
        return toImage(f);
    }
    if (name == QLatin1String("rake")) {
        // Six tines, each a rounded stub along the stroke, none quite like the others.
        Field f(size_t(kSize) * kSize, 0.0f);
        Rng rng{67};
        for (int i = 0; i < 6; ++i) {
            const float r = 0.045f * kSize * rng.range(0.9f, 1.08f);
            splatOval(f, (0.5f + (float(i) - 2.5f) * 0.165f) * kSize, 0.5f * kSize, r, r * 1.9f, 0.0f, rng.range(0.9f, 1.0f), 0.7f);
        }
        return toImage(f);
    }
    if (name == QLatin1String("knife")) {
        // A blade across the stroke: flat, sharp-edged, uneven only along its length.
        return render([](float x, float y, float u, float) {
            const float edge = smooth(1.0f, 0.93f, std::max(std::abs(x) / 0.95f, std::abs(y) / 0.2f));
            return edge * (0.5f + 0.5f * smooth(0.25f, 0.7f, fbm(u, 0.37f, 36, 3, 0.7f, 71)));
        });
    }
    if (name == QLatin1String("crayon")) {
        return render([](float x, float y, float u, float v) {
            const float ax = std::abs(x) / 0.7f, ay = std::abs(y) / 0.88f;
            const float d = std::pow(std::pow(ax, 3.0f) + std::pow(ay, 3.0f), 1.0f / 3.0f);
            const float edge = smooth(1.0f, 0.86f, d + (fbm(u, v, 14, 3, 0.6f, 75) - 0.5f) * 0.3f);
            const float wax = 0.4f + 0.6f * smooth(0.34f, 0.66f, fbm(u * 0.35f, v, 52, 3, 0.75f, 79));
            return edge * wax;
        });
    }
    if (name == QLatin1String("pastel")) {
        return render([](float x, float y, float u, float v) {
            const float edge = smooth(1.0f, 0.5f, std::hypot(x, y) + (fbm(u, v, 9, 4, 0.6f, 83) - 0.5f) * 0.5f);
            return edge * (0.3f + 0.7f * smooth(0.3f, 0.7f, fbm(u, v, 72, 3, 0.8f, 87)));
        });
    }
    if (name == QLatin1String("square"))
        return render([](float x, float y, float, float) { return smooth(0.9f, 0.885f, std::max(std::abs(x), std::abs(y))); });
    if (name == QLatin1String("ink_blot")) {
        return render([](float x, float y, float u, float v) {
            return smooth(0.74f, 0.7f, std::hypot(x, y) + (fbm(u, v, 3, 5, 0.55f, 91) - 0.5f) * 0.95f);
        });
    }
    if (name == QLatin1String("hatch")) {
        // Ruled strokes inside a rough disc, each a little heavier or lighter than the next.
        return render([](float x, float y, float u, float v) {
            const float row = v * 9.0f;
            const float band = std::abs(row - std::floor(row) - 0.5f);
            const float weight = 0.55f + 0.45f * rnd(int(std::floor(row)), 3, 95);
            const float rule = smooth(0.2f, 0.1f, band) * weight;
            const float edge = smooth(0.96f, 0.8f, std::hypot(x, y) + (fbm(u, v, 8, 3, 0.6f, 97) - 0.5f) * 0.35f);
            return rule * edge;
        });
    }
    if (name == QLatin1String("scratch")) {
        Field f(size_t(kSize) * kSize, 0.0f);
        Rng rng{103};
        for (int i = 0; i < 16; ++i) {
            const float cx = rng.range(0.2f, 0.8f) * kSize, cy = rng.range(0.14f, 0.86f) * kSize;
            const float a = rng.range(-0.22f, 0.22f), half = rng.range(0.1f, 0.34f) * kSize;
            drag(f, cx - std::cos(a) * half, cy - std::sin(a) * half, cx + std::cos(a) * half, cy + std::sin(a) * half,
                 rng.range(1.2f, 2.6f), 0.8f, rng.range(0.6f, 1.0f), 0.5f);
        }
        return toImage(f);
    }
    if (name == QLatin1String("stipple"))
        return dots(109, 30, 0.43f, 0.014f, 0.042f, 0.7f, false);
    if (name == QLatin1String("spray_fine"))
        return dots(113, 900, 0.15f, 0.0025f, 0.007f, 0.5f, true);
    if (name == QLatin1String("cloud")) {
        return render([](float x, float y, float u, float v) {
            const float edge = smooth(0.98f, 0.2f, std::hypot(x, y) + (fbm(u, v, 5, 5, 0.55f, 121) - 0.5f) * 0.95f);
            return edge * (0.5f + 0.5f * fbm(u, v, 10, 4, 0.6f, 123));
        });
    }
    if (name == QLatin1String("smoke")) {
        // Noise bent by more noise: wisps rather than lumps.
        return render([](float x, float y, float u, float v) {
            const float wu = u + (fbm(u, v, 4, 4, 0.6f, 127) - 0.5f) * 0.5f, wv = v + (fbm(u, v, 4, 4, 0.6f, 129) - 0.5f) * 0.5f;
            const float wisp = smooth(0.42f, 0.76f, fbm(wu - std::floor(wu), wv - std::floor(wv), 6, 5, 0.6f, 131));
            return wisp * smooth(1.0f, 0.25f, std::hypot(x, y));
        });
    }
    if (name == QLatin1String("grass"))
        return blades(137, 9, 0.94f, 0.2f, 0.82f, 0.42f, 0.02f);
    if (name == QLatin1String("fur")) {
        // Fine hairs lying along x, so that they follow the stroke: each one
        // thickest a third of the way along and tapering to both ends, with a
        // slight curve. They thin out towards the rim, so nothing is cut off.
        Field f(size_t(kSize) * kSize, 0.0f);
        Rng rng{139};
        for (int i = 0; i < 70; ++i) {
            const float cy = 0.5f + rng.range(-0.36f, 0.36f), cx = 0.5f + rng.range(-0.16f, 0.16f);
            const float len = rng.range(0.26f, 0.42f) * (1.0f - 1.2f * std::abs(cy - 0.5f));
            const float bend = rng.range(-0.06f, 0.06f), lean = rng.range(-0.12f, 0.12f);
            const float thick = rng.range(1.0f, 2.2f);
            const float tone = rng.range(0.55f, 1.0f);
            constexpr int steps = 18;
            float px = (cx - len * 0.5f) * kSize, py = cy * kSize;
            for (int k = 1; k <= steps; ++k) {
                const float t = float(k) / float(steps);
                const float nx = (cx - len * 0.5f + len * t) * kSize;
                const float ny = (cy + lean * len * (t - 0.5f) + bend * std::sin(t * kPi)) * kSize;
                const float w0 = thick * std::sin(kPi * std::pow(float(k - 1) / steps, 0.7f)) + 0.35f;
                const float w1 = thick * std::sin(kPi * std::pow(t, 0.7f)) + 0.35f;
                drag(f, px, py, nx, ny, w0, w1, tone, 0.3f);
                px = nx;
                py = ny;
            }
        }
        return toImage(f);
    }
    if (name == QLatin1String("flower")) {
        return render([](float x, float y, float, float) {
            const float r = std::hypot(x, y), a = std::atan2(y, x);
            const float petal = 0.3f + 0.6f * std::pow(std::abs(std::cos(2.5f * a)), 0.7f);
            const float heart = 1.0f - 0.55f * smooth(0.17f, 0.1f, r);
            return smooth(petal, petal - 0.035f, r) * heart * (0.82f + 0.18f * smooth(0.0f, petal, r));
        });
    }
    if (name == QLatin1String("drop")) {
        // A streak that thickens towards its lower end.
        return render([](float x, float y, float, float) {
            const float along = (y + 0.95f) / 1.9f;
            if (along <= 0.0f || along >= 1.0f)
                return 0.0f;
            const float half = 0.012f + 0.05f * along * along;
            return smooth(half, half * 0.4f, std::abs(x)) * (0.25f + 0.75f * along) * smooth(1.0f, 0.93f, along);
        });
    }
    if (name == QLatin1String("snow")) {
        // Six arms with a pair of barbs on each.
        Field f(size_t(kSize) * kSize, 0.0f);
        const float c = kSize * 0.5f;
        for (int k = 0; k < 6; ++k) {
            const float a = kPi / 3.0f * float(k), dx = std::cos(a), dy = std::sin(a);
            drag(f, c, c, c + dx * 0.45f * kSize, c + dy * 0.45f * kSize, 6.0f, 3.0f, 1.0f, 0.5f);
            for (float side : {-1.0f, 1.0f}) {
                const float b = a + side * kPi / 3.0f;
                const float bx = c + dx * 0.27f * kSize, by = c + dy * 0.27f * kSize;
                drag(f, bx, by, bx + std::cos(b) * 0.11f * kSize, by + std::sin(b) * 0.11f * kSize, 4.0f, 2.2f, 0.9f, 0.5f);
            }
        }
        return toImage(f);
    }
    if (name == QLatin1String("star")) {
        return render([](float x, float y, float, float) {
            const float d = std::hypot(x, y);
            const float core = smooth(0.2f, 0.0f, d);
            const float rays = std::pow(std::max(0.0f, 1.0f - std::abs(x) / 0.96f), 3.0f) * std::exp(-std::abs(y) * 42.0f)
                               + std::pow(std::max(0.0f, 1.0f - std::abs(y) / 0.96f), 3.0f) * std::exp(-std::abs(x) * 42.0f);
            return std::clamp(core * 0.9f + rays + 0.25f * std::exp(-d * d * 14.0f), 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("glow")) {
        return render([](float x, float y, float, float) {
            const float d = std::hypot(x, y);
            return std::clamp(0.55f * std::exp(-d * d * 5.5f) * smooth(1.0f, 0.7f, d) + smooth(0.2f, 0.04f, d), 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("bokeh")) {
        return render([](float x, float y, float, float) {
            const float d = std::hypot(x, y);
            return smooth(0.93f, 0.87f, d) * (0.5f + 0.5f * smooth(0.55f, 0.9f, d));
        });
    }
    if (name == QLatin1String("bubble")) {
        return render([](float x, float y, float, float) {
            const float d = std::hypot(x, y);
            const float rim = smooth(0.1f, 0.03f, std::abs(d - 0.82f));
            const float shine = 0.7f * smooth(0.16f, 0.06f, std::hypot(x + 0.36f, y + 0.4f));
            return std::clamp(rim + 0.14f * smooth(0.82f, 0.78f, d) + shine, 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("flick")) {
        // Paint flicked off a brush: a drop with a tail, the drop ahead (+x).
        Field f(size_t(kSize) * kSize, 0.0f);
        const float c = kSize * 0.5f;
        drag(f, c - 0.42f * kSize, c + 0.01f * kSize, c + 0.22f * kSize, c, 1.2f, 0.07f * kSize, 1.0f, 0.75f);
        splatOval(f, c + 0.28f * kSize, c, 0.1f * kSize, 0.085f * kSize, 0.0f, 1.0f, 0.8f);
        splat(f, c + 0.44f * kSize, c - 0.06f * kSize, 0.022f * kSize, 1.0f, 0.7f);
        return toImage(f);
    }
    if (name == QLatin1String("strands")) {
        // A dozen fine lines side by side, lying along the stroke (x), of
        // uneven weight: drawn they become a lock of hair.
        Field f(size_t(kSize) * kSize, 0.0f);
        Rng rng{263};
        for (int i = 0; i < 13; ++i) {
            const float y = 0.5f + (float(i) - 6.0f) / 6.0f * 0.4f + rng.range(-0.02f, 0.02f);
            const float w = rng.range(4.0f, 8.0f) * (1.0f - 0.5f * std::abs(y - 0.5f) / 0.42f);
            splatOval(f, 0.5f * kSize, y * kSize, 0.2f * kSize, w, 0.0f, rng.range(0.6f, 1.0f), 0.55f);
        }
        return toImage(f);
    }
    return render([](float x, float y, float, float) { return smooth(1.0f, 0.9f, std::hypot(x, y)); });
}

// Dots on a 45-degree screen: what `dot` makes of the distance from the
// nearest dot's centre, in cells (0 at the centre, 0.71 between dots). The
// screen is turned by measuring along the two diagonals, which tiles exactly.
static QImage screen(int cells, const std::function<float(float)>& dot)
{
    return render([cells, &dot](float, float, float u, float v) {
        const float a = (u + v) * float(cells), b = (u - v) * float(cells);
        const float da = a - std::floor(a) - 0.5f, db = b - std::floor(b) - 0.5f;
        return dot(std::hypot(da, db));
    });
}

QImage grain(const QString& name)
{
    if (name == QLatin1String("paper")) {
        // Drawing paper: a close tooth of small bumps, uneven in height, over
        // a gentle unevenness of the sheet.
        return renderEven([](float, float, float u, float v) {
            return 0.55f * bumps(u, v, 96, 101) + 0.3f * gfbm(u, v, 128, 2, 0.6f, 103) + 0.15f * gfbm(u, v, 16, 3, 0.5f, 107);
        });
    }
    if (name == QLatin1String("fine")) {
        // Smooth bristol: the tooth is there, but small and shallow.
        return renderEven([](float, float, float u, float v) {
            return 0.5f * bumps(u, v, 160, 131) + 0.4f * gfbm(u, v, 256, 1, 0.5f, 133) + 0.1f * gfbm(u, v, 32, 2, 0.5f, 137);
        });
    }
    if (name == QLatin1String("charcoal")) {
        // Charcoal paper (laid): a fine tooth crossed by the faint ribbing the
        // sheet took from its mould, which is what charcoal catches on.
        return renderEven([](float, float, float u, float v) {
            const float laid = 0.5f + 0.5f * std::sin(2.0f * kPi * (64.0f * v + 0.6f * gfbm(u, v, 8, 2, 0.5f, 149)));
            return 0.45f * bumps(u, v, 112, 151) + 0.3f * gfbm(u, v, 160, 2, 0.6f, 153) + 0.25f * laid;
        });
    }
    if (name == QLatin1String("blotch")) {
        return renderEven([](float, float, float u, float v) { return gfbm(u, v, 6, 5, 0.55f, 171); });
    }
    if (name == QLatin1String("canvas")) {
        // Plain weave: alternating over/under threads plus a little fibre noise.
        return render([](float, float, float u, float v) {
            constexpr float threads = 64.0f;
            const float tx = u * threads, ty = v * threads;
            const float fx = tx - std::floor(tx), fy = ty - std::floor(ty);
            const bool over = ((int(tx) + int(ty)) & 1) != 0;
            const float across = std::sin(kPi * (over ? fy : fx));
            const float along = 0.6f + 0.4f * std::sin(kPi * (over ? fx : fy));
            const float weave = across * along;
            return std::clamp(0.2f + 0.65f * weave + (fbm(u, v, 96, 3, 0.7f, 181) - 0.5f) * 0.35f, 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("halftone")) {
        // Round dots on a 45-degree screen, as in printed comics: about a
        // quarter of the area inked, with half a pixel of soft edge.
        return screen(32, [](float d) { return smooth(0.3f, 0.265f, d); });
    }
    if (name == QLatin1String("cold_press")) {
        // Watercolour paper: broad, soft hills with a fine tooth on them.
        return renderEven([](float, float, float u, float v) {
            return 0.3f * bumps(u, v, 40, 201) + 0.45f * gfbm(u, v, 20, 4, 0.55f, 203) + 0.25f * gfbm(u, v, 160, 2, 0.5f, 205);
        });
    }
    if (name == QLatin1String("fibre")) {
        // Long fibres lying along x, over a light tooth.
        return renderEven([](float, float, float u, float v) {
            return 0.65f * gfbm(u, v, 96, 3, 0.6f, 205, 12) + 0.35f * gfbm(u, v, 128, 2, 0.6f, 209);
        });
    }
    if (name == QLatin1String("linen")) {
        return render([](float, float, float u, float v) {
            constexpr float threads = 112.0f;
            const float tx = u * threads, ty = v * threads;
            const float fx = tx - std::floor(tx), fy = ty - std::floor(ty);
            const bool over = ((int(tx) + int(ty)) & 1) != 0;
            const float weave = std::sin(kPi * (over ? fy : fx)) * (0.65f + 0.35f * std::sin(kPi * (over ? fx : fy)));
            // Threads of uneven thickness, as linen has.
            const float slub = 0.75f + 0.5f * (vnoise2(u * 6.0f, v * 112.0f, 6, 112, 211) - 0.5f);
            return std::clamp(0.2f + 0.7f * weave * slub + (fbm(u, v, 128, 2, 0.7f, 213) - 0.5f) * 0.25f, 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("burlap")) {
        return render([](float, float, float u, float v) {
            constexpr float threads = 26.0f;
            const float tx = u * threads, ty = v * threads;
            const float fx = tx - std::floor(tx), fy = ty - std::floor(ty);
            const bool over = ((int(tx) + int(ty)) & 1) != 0;
            const float weave = std::sin(kPi * (over ? fy : fx)) * (0.55f + 0.45f * std::sin(kPi * (over ? fx : fy)));
            return std::clamp(0.08f + 0.8f * weave + (fbm(u, v, 104, 3, 0.7f, 217) - 0.5f) * 0.45f, 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("sand"))
        return renderEven([](float, float, float u, float v) { return 0.7f * bumps(u, v, 200, 221) + 0.3f * gfbm(u, v, 256, 1, 0.5f, 223); });
    if (name == QLatin1String("speckle")) {
        // Mostly open, with scattered grains standing up.
        return render([](float, float, float u, float v) { return smooth(0.62f, 0.78f, 0.5f + 0.5f * gfbm(u, v, 150, 2, 0.6f, 223)); });
    }
    if (name == QLatin1String("concrete")) {
        // A trowelled surface: slow variation, pocked with holes of every size.
        return renderEven([](float, float, float u, float v) {
            const float pits = std::min(smooth(0.0f, 0.25f, 1.0f - bumps(u, v, 40, 227) * 1.3f), smooth(0.0f, 0.2f, 1.0f - bumps(u, v, 110, 228) * 1.15f));
            return 0.45f * gfbm(u, v, 8, 4, 0.6f, 229) + 0.25f * gfbm(u, v, 128, 2, 0.6f, 231) + 0.55f * pits;
        });
    }
    if (name == QLatin1String("clouds"))
        return renderEven([](float, float, float u, float v) { return gfbm(u, v, 3, 6, 0.55f, 233); });
    if (name == QLatin1String("wood")) {
        // Plank seen along its length: fine, close growth lines that wander
        // and pinch, with the open fibres between them.
        return render([](float, float, float u, float v) {
            const float wander = 0.9f * gfbm(u, v, 2, 3, 0.5f, 237) + 0.25f * gfbm(u, v, 6, 2, 0.5f, 238, 2);
            const float ring = 26.0f * v + 2.2f * wander;
            const float f = ring - std::floor(ring);
            // Late wood: a thin dark line at the end of each year's growth.
            const float line = smooth(0.78f, 0.93f, f) * smooth(1.0f, 0.95f, f);
            const float fibres = 0.5f + 0.5f * gfbm(u, v, 160, 2, 0.6f, 241, 16);
            return std::clamp(0.12f + 0.75f * line + 0.3f * (fibres - 0.5f) + 0.15f * smooth(0.3f, 0.75f, f), 0.0f, 1.0f);
        });
    }
    if (name == QLatin1String("crackle")) {
        return render([](float, float, float u, float v) {
            float nearest = 0.0f, second = 0.0f;
            cellDistances(u, v, 9, 245, &nearest, &second);
            return smooth(0.03f, 0.11f, second - nearest);   // raised everywhere but along the cracks
        });
    }
    if (name == QLatin1String("halftone_coarse"))
        return screen(14, [](float d) { return smooth(0.37f, 0.35f, d); });
    if (name == QLatin1String("lines"))
        return render([](float, float, float, float v) { return smooth(0.5f, 0.66f, 0.5f + 0.5f * std::cos(2.0f * kPi * 44.0f * v)); });
    if (name == QLatin1String("hatch"))
        return render([](float, float, float u, float v) { return smooth(0.52f, 0.7f, 0.5f + 0.5f * std::cos(2.0f * kPi * 30.0f * (u + v))); });
    if (name == QLatin1String("crosshatch")) {
        return render([](float, float, float u, float v) {
            const float one = smooth(0.62f, 0.8f, 0.5f + 0.5f * std::cos(2.0f * kPi * 26.0f * (u + v)));
            const float other = smooth(0.62f, 0.8f, 0.5f + 0.5f * std::cos(2.0f * kPi * 26.0f * (u - v)));
            return std::max(one, other);
        });
    }
    if (name == QLatin1String("waves")) {
        return render([](float, float, float u, float v) {
            return smooth(0.5f, 0.68f, 0.5f + 0.5f * std::sin(2.0f * kPi * (22.0f * v + 0.45f * std::sin(2.0f * kPi * 4.0f * u))));
        });
    }
    if (name == QLatin1String("halftone_cone")) {
        // Not dots but the height of the screen: highest at each dot's centre.
        // Used as tooth, light pressure prints small dots and firm pressure
        // big ones that close up, as a real halftone does.
        return screen(24, [](float d) { return std::clamp(1.0f - d / 0.7071f, 0.0f, 1.0f); });
    }
    if (name == QLatin1String("pen_hatch")) {
        // Hatching ruled by hand at 45 degrees: lines of uneven weight that
        // wander a little, lift off here and there, and are not quite evenly
        // spaced. Used as tooth, light pressure leaves only the strongest.
        return render([](float, float, float u, float v) {
            constexpr int lines = 34;
            const float along = u - v;   // runs down each line
            const float s = (u + v) * float(lines) + 0.18f * gfbm(u, v, 4, 2, 0.5f, 251) + 0.05f * std::sin(2.0f * kPi * 3.0f * along);
            const int i = int(std::floor(s));
            const int k = ((i % lines) + lines) % lines;
            const float f = s - std::floor(s) - 0.5f - 0.18f * (rnd(k, 1, 253) - 0.5f);
            const float weight = 0.12f + 0.16f * rnd(k, 2, 255) + 0.06f * gfbm(u, v, 24, 2, 0.5f, 257 + uint32_t(k) % 5u);
            const float ink = smooth(weight + 0.04f, weight - 0.04f, std::abs(f));
            // Strength along the line: where the pen pressed and where it lifted.
            const float press = 0.55f + 0.45f * gfbm(u, v, 12, 2, 0.5f, 259 + uint32_t(k) * 7u);
            return std::clamp(ink * (0.35f + 0.65f * press), 0.0f, 1.0f);
        });
    }
    return render([](float, float, float, float) { return 1.0f; });
}

} // namespace texgen
