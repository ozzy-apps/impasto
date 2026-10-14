#include "io/Layered.h"

#include <QFloat16>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace layered {

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// One row of half floats to floats.
void rowToFloats(const QByteArray& half, qsizetype at, int count, std::vector<float>& out)
{
    out.resize(size_t(count));
    qFloatFromFloat16(out.data(), reinterpret_cast<const qfloat16*>(half.constData() + at), count);
}

} // namespace

// ------------------------------------------------------------------ source

Source::Source(CanvasEngine& engine) : m_engine(engine), m_size(engine.canvasSize()), m_layers(engine.layers()) {}

int Source::parentIndex(int i) const
{
    const int parent = m_layers[i].parent;
    for (int k = 0; k < m_layers.size(); ++k) {
        if (m_layers[k].id == parent)
            return k;
    }
    return -1;
}

int Source::clipBase(int i) const
{
    if (!m_layers[i].clipping || m_layers[i].isGroup)
        return -1;
    // The nearest layer below it in the same group that is not clipped itself.
    for (int k = i - 1; k >= 0; --k) {
        if (m_layers[k].parent != m_layers[i].parent)
            continue;
        if (!m_layers[k].clipping || m_layers[k].isGroup)
            return k;
    }
    return -1;
}

LayerData Source::read(int i)
{
    LayerData d;
    d.info = m_layers[i];
    d.parentIndex = parentIndex(i);
    if (d.info.hasMask)
        d.mask = m_engine.layerMaskGrey(d.info.id);
    if (d.info.isGroup)
        return d;   // a group's mask, but no pixels of its own
    const QRect held = m_engine.layerHeldBounds(d.info.id);
    if (!held.isEmpty()) {
        const QByteArray pixels = m_engine.layerPixels(d.info.id, held);
        const QRect inside = contentBounds(pixels, held.size());
        if (!inside.isEmpty()) {
            d.rect = inside.translated(held.topLeft());
            d.pixels = inside == QRect(QPoint(0, 0), held.size()) ? pixels : crop(pixels, held.size(), inside);
        }
    }
    if (d.info.isText) {
        d.hasText = true;
        d.text = m_engine.layerText(d.info.id);
    }
    return d;
}

QByteArray Source::groupPixels(int i, QRect* rect)
{
    return m_engine.groupPixels(m_layers[i].id, rect);
}

QByteArray Source::composite()
{
    return m_engine.compositePixels();
}

// ------------------------------------------------------------------ pixels

QImage straight8(const QByteArray& half, const QSize& size)
{
    QImage img(size, QImage::Format_RGBA8888);
    if (img.isNull())
        return img;
    std::vector<float> row;
    for (int y = 0; y < size.height(); ++y) {
        rowToFloats(half, qsizetype(y) * size.width() * 8, size.width() * 4, row);
        uchar* out = img.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            const float* p = row.data() + x * 4;
            const float a = clamp01(p[3]);
            if (a <= 0.0f) {
                std::memset(out + x * 4, 0, 4);
                continue;
            }
            for (int c = 0; c < 3; ++c)
                out[x * 4 + c] = uchar(std::lround(clamp01(p[c] / a) * 255.0f));
            out[x * 4 + 3] = uchar(std::lround(a * 255.0f));
        }
    }
    return img;
}

QImage straight16(const QByteArray& half, const QSize& size)
{
    QImage img(size, QImage::Format_RGBA64);
    if (img.isNull())
        return img;
    std::vector<float> row;
    for (int y = 0; y < size.height(); ++y) {
        rowToFloats(half, qsizetype(y) * size.width() * 8, size.width() * 4, row);
        auto* out = reinterpret_cast<quint16*>(img.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const float* p = row.data() + x * 4;
            const float a = clamp01(p[3]);
            if (a <= 0.0f) {
                std::memset(out + x * 4, 0, 8);
                continue;
            }
            for (int c = 0; c < 3; ++c)
                out[x * 4 + c] = quint16(std::lround(clamp01(p[c] / a) * 65535.0f));
            out[x * 4 + 3] = quint16(std::lround(a * 65535.0f));
        }
    }
    return img;
}

QByteArray premultipliedHalf(const QImage& image)
{
    const QImage img = image.format() == QImage::Format_RGBA64 ? image : image.convertToFormat(QImage::Format_RGBA64);
    const int w = img.width(), h = img.height();
    QByteArray out(qsizetype(w) * h * 8, Qt::Uninitialized);
    std::vector<float> row(size_t(w) * 4);
    for (int y = 0; y < h; ++y) {
        const auto* in = reinterpret_cast<const quint16*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const float a = in[x * 4 + 3] / 65535.0f;
            for (int c = 0; c < 3; ++c)
                row[size_t(x) * 4 + size_t(c)] = in[x * 4 + c] / 65535.0f * a;
            row[size_t(x) * 4 + 3] = a;
        }
        qFloatToFloat16(reinterpret_cast<qfloat16*>(out.data() + qsizetype(y) * w * 8), row.data(), qsizetype(w) * 4);
    }
    return out;
}

QRect contentBounds(const QByteArray& half, const QSize& size)
{
    const auto* data = reinterpret_cast<const quint16*>(half.constData());
    int x0 = size.width(), y0 = size.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < size.height(); ++y) {
        const quint16* row = data + qsizetype(y) * size.width() * 4;
        for (int x = 0; x < size.width(); ++x) {
            if (row[x * 4 + 3] & 0x7fff) {   // any alpha other than +/-0
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

QByteArray crop(const QByteArray& half, const QSize& size, const QRect& r)
{
    QByteArray out(qsizetype(r.width()) * r.height() * 8, Qt::Uninitialized);
    for (int y = 0; y < r.height(); ++y)
        std::memcpy(out.data() + qsizetype(y) * r.width() * 8, half.constData() + (qsizetype(r.y() + y) * size.width() + r.x()) * 8,
                    size_t(r.width()) * 8);
    return out;
}

void multiplyBy(QByteArray& half, const QRect& rect, const QByteArray& grey, int canvasWidth)
{
    std::vector<float> row;
    for (int y = 0; y < rect.height(); ++y) {
        const qsizetype at = qsizetype(y) * rect.width() * 8;
        rowToFloats(half, at, rect.width() * 4, row);
        const uchar* cover = reinterpret_cast<const uchar*>(grey.constData()) + qsizetype(rect.y() + y) * canvasWidth + rect.x();
        for (int x = 0; x < rect.width(); ++x) {
            const float k = cover[x] / 255.0f;
            for (int c = 0; c < 4; ++c)
                row[size_t(x) * 4 + size_t(c)] *= k;
        }
        qFloatToFloat16(reinterpret_cast<qfloat16*>(half.data() + at), row.data(), qsizetype(rect.width()) * 4);
    }
}

void multiplyByAlpha(QByteArray& half, const QRect& rect, const QByteArray& other, const QRect& otherRect)
{
    std::vector<float> row, base;
    for (int y = 0; y < rect.height(); ++y) {
        const qsizetype at = qsizetype(y) * rect.width() * 8;
        rowToFloats(half, at, rect.width() * 4, row);
        const int oy = rect.y() + y - otherRect.y();
        const bool rowInside = oy >= 0 && oy < otherRect.height();
        if (rowInside)
            rowToFloats(other, qsizetype(oy) * otherRect.width() * 8, otherRect.width() * 4, base);
        for (int x = 0; x < rect.width(); ++x) {
            const int ox = rect.x() + x - otherRect.x();
            const float k = rowInside && ox >= 0 && ox < otherRect.width() ? base[size_t(ox) * 4 + 3] : 0.0f;
            for (int c = 0; c < 4; ++c)
                row[size_t(x) * 4 + size_t(c)] *= k;
        }
        qFloatToFloat16(reinterpret_cast<qfloat16*>(half.data() + at), row.data(), qsizetype(rect.width()) * 4);
    }
}

QByteArray flat(const QColor& colour, const QSize& size)
{
    const float rgba[4] = {float(colour.redF()), float(colour.greenF()), float(colour.blueF()), 1.0f};
    qfloat16 px[4];
    qFloatToFloat16(px, rgba, 4);
    QByteArray out(qsizetype(size.width()) * size.height() * 8, Qt::Uninitialized);
    for (qsizetype i = 0; i < qsizetype(size.width()) * size.height(); ++i)
        std::memcpy(out.data() + i * 8, px, 8);
    return out;
}

bool uniform(const QByteArray& half, QColor* colour)
{
    if (half.size() < 8)
        return false;
    const char* first = half.constData();
    for (qsizetype i = 8; i < half.size(); i += 8) {
        if (std::memcmp(first, half.constData() + i, 8) != 0)
            return false;
    }
    float px[4];
    qFloatFromFloat16(px, reinterpret_cast<const qfloat16*>(first), 4);
    if (px[3] < 0.999f)
        return false;
    if (colour)
        *colour = QColor::fromRgbF(clamp01(px[0]), clamp01(px[1]), clamp01(px[2]));
    return true;
}

// ------------------------------------------------------------------ load

bool load(CanvasEngine& engine, Document& doc, QString* error)
{
    const int limit = CanvasEngine::maxCanvasDimension();
    if (doc.size.width() < 1 || doc.size.height() < 1 || doc.size.width() > limit || doc.size.height() > limit
        || qint64(doc.size.width()) * doc.size.height() > CanvasEngine::maxCanvasPixels()) {
        if (error)
            *error = QObject::tr("The picture is %1 × %2 pixels, more than this graphics card can hold (%3 megapixels).")
                         .arg(doc.size.width()).arg(doc.size.height()).arg(CanvasEngine::maxCanvasPixels() / 1'000'000);
        return false;
    }
    // Both formats keep the paper as a layer at the bottom; it becomes paper again.
    if (!doc.layers.empty()) {
        const LayerData& bottom = doc.layers.front();
        QColor paper;
        if (!bottom.info.isGroup && bottom.parentIndex < 0 && bottom.info.visible && bottom.info.opacity >= 0.999f
            && bottom.info.blend == BlendMode::Normal && bottom.mask.isEmpty() && bottom.rect == QRect(QPoint(0, 0), doc.size)
            && bottom.info.name.compare(QStringLiteral("Background"), Qt::CaseInsensitive) == 0 && uniform(bottom.pixels, &paper)) {
            doc.background = paper;
            doc.backgroundVisible = true;
            doc.layers.erase(doc.layers.begin());
            for (LayerData& d : doc.layers) {
                if (d.parentIndex >= 0)
                    --d.parentIndex;
            }
        }
    }
    // Nothing may hang off the canvas: the part that does is cut away.
    const QRect canvas(QPoint(0, 0), doc.size);
    for (LayerData& d : doc.layers) {
        if (d.info.isGroup || d.rect.isEmpty())
            continue;
        const QRect inside = d.rect & canvas;
        if (inside.isEmpty()) {
            d.rect = QRect();
            d.pixels.clear();
        } else if (inside != d.rect) {
            d.pixels = crop(d.pixels, d.rect.size(), inside.translated(-d.rect.topLeft()));
            d.rect = inside;
        }
    }
    engine.loadDocument(doc.size, doc.background, doc.backgroundVisible, doc.layers, int(doc.layers.size()) - 1);
    engine.setResolution(doc.dpi);
    return true;
}

// ------------------------------------------------------------------ names

namespace {

struct Name {
    BlendMode mode;
    const char* psd;
    const char* ora;
};
// OpenRaster names the modes SVG has; for the rest, the names Krita gives them.
const Name kNames[] = {
    {BlendMode::Normal, "norm", "svg:src-over"},
    {BlendMode::Multiply, "mul ", "svg:multiply"},
    {BlendMode::Darken, "dark", "svg:darken"},
    {BlendMode::ColorBurn, "idiv", "svg:color-burn"},
    {BlendMode::LinearBurn, "lbrn", "krita:linear_burn"},
    {BlendMode::DarkerColor, "dkCl", "krita:darker color"},
    {BlendMode::Screen, "scrn", "svg:screen"},
    {BlendMode::Lighten, "lite", "svg:lighten"},
    {BlendMode::ColorDodge, "div ", "svg:color-dodge"},
    {BlendMode::Add, "lddg", "svg:plus"},
    {BlendMode::LighterColor, "lgCl", "krita:lighter color"},
    {BlendMode::Overlay, "over", "svg:overlay"},
    {BlendMode::SoftLight, "sLit", "svg:soft-light"},
    {BlendMode::HardLight, "hLit", "svg:hard-light"},
    {BlendMode::VividLight, "vLit", "krita:vivid_light"},
    {BlendMode::LinearLight, "lLit", "krita:linear light"},
    {BlendMode::PinLight, "pLit", "krita:pin_light"},
    {BlendMode::HardMix, "hMix", "krita:hard_mix_photoshop"},
    {BlendMode::Difference, "diff", "svg:difference"},
    {BlendMode::Exclusion, "smud", "krita:exclusion"},
    {BlendMode::Subtract, "fsub", "krita:subtract"},
    {BlendMode::Divide, "fdiv", "krita:divide"},
    {BlendMode::Hue, "hue ", "svg:hue"},
    {BlendMode::Saturation, "sat ", "svg:saturation"},
    {BlendMode::Color, "colr", "svg:color"},
    {BlendMode::Luminosity, "lum ", "svg:luminosity"},
};

} // namespace

QByteArray psdKey(BlendMode mode)
{
    for (const Name& n : kNames) {
        if (n.mode == mode)
            return QByteArray(n.psd, 4);
    }
    return QByteArrayLiteral("norm");
}

BlendMode fromPsdKey(const QByteArray& key, bool* known)
{
    for (const Name& n : kNames) {
        if (key == QByteArray(n.psd, 4)) {
            if (known)
                *known = true;
            return n.mode;
        }
    }
    if (known)
        *known = key == "pass";   // a group that is not isolated: as near as it gets is an isolated one
    return BlendMode::Normal;
}

QString oraOp(BlendMode mode)
{
    for (const Name& n : kNames) {
        if (n.mode == mode)
            return QString::fromLatin1(n.ora);
    }
    return QStringLiteral("svg:src-over");
}

BlendMode fromOraOp(const QString& op, bool* known)
{
    for (const Name& n : kNames) {
        if (op == QLatin1String(n.ora)) {
            if (known)
                *known = true;
            return n.mode;
        }
    }
    // Other names for the same, as Krita and others write them.
    static const std::pair<const char*, BlendMode> aliases[] = {
        {"krita:linear_dodge", BlendMode::Add},       {"krita:add", BlendMode::Add},
        {"krita:soft_light", BlendMode::SoftLight},    {"krita:soft_light_svg", BlendMode::SoftLight},
        {"krita:soft_light_ifs_illusions", BlendMode::SoftLight}, {"krita:soft_light_pegtop_delphi", BlendMode::SoftLight},
        {"krita:hard_mix", BlendMode::HardMix},       {"krita:hard mix", BlendMode::HardMix},
        {"krita:exclusion", BlendMode::Exclusion},    {"svg:exclusion", BlendMode::Exclusion},
        {"krita:color_burn", BlendMode::ColorBurn},   {"krita:color_dodge", BlendMode::ColorDodge},
        {"krita:linear light", BlendMode::LinearLight}, {"krita:linear_light", BlendMode::LinearLight},
        {"krita:darker color", BlendMode::DarkerColor}, {"krita:lighter color", BlendMode::LighterColor},
        {"krita:overlay", BlendMode::Overlay},        {"krita:hard_light", BlendMode::HardLight},
    };
    for (const auto& [name, mode] : aliases) {
        if (op == QLatin1String(name)) {
            if (known)
                *known = true;
            return mode;
        }
    }
    if (known)
        *known = op.isEmpty() || op == QLatin1String("svg:src-over");
    return BlendMode::Normal;
}

} // namespace layered
