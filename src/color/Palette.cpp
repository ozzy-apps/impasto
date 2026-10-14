#include "color/Palette.h"
#include "io/Zip.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

QString storePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/palettes.json");
}

// Whole rows, at least three, and always somewhere left to put a colour.
void fit(Palette& p)
{
    if (p.colors.size() > Palette::kMaxSlots)
        p.colors.resize(Palette::kMaxSlots);
    int last = -1;
    for (int i = 0; i < p.colors.size(); ++i) {
        if (p.colors[i].isValid())
            last = i;
    }
    int rows = std::max(Palette::kMinRows, (last + Palette::kColumns) / Palette::kColumns);
    bool full = true;
    for (int i = 0; i < rows * Palette::kColumns && full; ++i)
        full = i < p.colors.size() && p.colors[i].isValid();
    if (full && (rows + 1) * Palette::kColumns <= Palette::kMaxSlots)
        ++rows;
    p.colors.resize(rows * Palette::kColumns);
}

QVector<QColor> fromHex(std::initializer_list<const char*> hex)
{
    QVector<QColor> out;
    for (const char* h : hex)
        out.push_back(QColor(QLatin1String(h)));
    return out;
}

struct Starter {
    const char* name;
    QVector<QColor> colors;
};

const QVector<Starter>& starters()
{
    static const QVector<Starter> all = {
        {QT_TRANSLATE_NOOP("PaletteLibrary", "Impasto"),
         fromHex({"#1c1c1e", "#48484a", "#8e8e93", "#c7c7cc", "#f2f2f7", "#ffffff", "#5c3a21", "#a0673a", "#d9a066", "#f3d9b1",
                  "#7a1f1f", "#d32f2f", "#ff6b4a", "#ff9f0a", "#ffd60a", "#fff3a3", "#ff375f", "#ff7eb6", "#ffc2dd", "#bf5af2",
                  "#0b3d2e", "#1e8e5a", "#30d158", "#a8e6a1", "#0a4f6e", "#0a84ff", "#64d2ff", "#b8e8ff", "#2c2a6b", "#5e5ce6"})},
        {QT_TRANSLATE_NOOP("PaletteLibrary", "Skin and Hair"),
         fromHex({"#fde7d6", "#f9d4bb", "#f3c19f", "#e8ab85", "#d9946b", "#c47f57", "#a86a45", "#8a5335", "#6b3f28", "#4a2a1b",
                  "#ffd9cf", "#f7b6a8", "#ee8f85", "#d96a6a", "#b84a56", "#8f3345", "#c9a08d", "#9c7565", "#6e4f47", "#f6efe9",
                  "#f4e2b8", "#e2c27a", "#c99a4a", "#a8702f", "#8a4b22", "#6a3418", "#4a2616", "#2f1a12", "#1b1210", "#9b9ba3"})},
        {QT_TRANSLATE_NOOP("PaletteLibrary", "Landscape"),
         fromHex({"#eaf4fb", "#c5e1f5", "#96c8ee", "#5fa8e0", "#2f7fc4", "#1f5a96", "#173f6b", "#f9e3c2", "#f6b98a", "#e8866a",
                  "#e9f0c4", "#c7d98a", "#9dbf5a", "#6f9f3e", "#4a7f32", "#2f5f2b", "#1f4426", "#7a8c5a", "#a3a86a", "#54683a",
                  "#f1e3c6", "#dcc59a", "#c2a06c", "#a07a4a", "#7c5a34", "#5a3f26", "#3d2a1c", "#8c8a84", "#5f5e5a", "#33322f"})},
        {QT_TRANSLATE_NOOP("PaletteLibrary", "Greys"),
         fromHex({"#ffffff", "#e3e3e3", "#c6c6c6", "#aaaaaa", "#8e8e8e", "#717171", "#555555", "#393939", "#1c1c1c", "#000000",
                  "#f7f3ee", "#e3dcd3", "#cbc2b6", "#b0a698", "#958a7c", "#7a6f62", "#5f564b", "#453e36", "#2c2823", "#171411",
                  "#f1f4f7", "#d7dde4", "#bac3cd", "#9da8b4", "#818d9b", "#677381", "#4f5965", "#383f49", "#23282f", "#111418"})},
    };
    return all;
}

// ---- Oklab: distances in it agree with how different two colours look.

struct Lab {
    float L = 0, a = 0, b = 0;
    float operator[](int i) const { return i == 0 ? L : i == 1 ? a : b; }
};

float toLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float toGamma(float c)
{
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

Lab toOklab(QRgb px)
{
    const float r = toLinear(qRed(px) / 255.0f), g = toLinear(qGreen(px) / 255.0f), b = toLinear(qBlue(px) / 255.0f);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

float distance(const Lab& x, const Lab& y)
{
    return std::sqrt((x.L - y.L) * (x.L - y.L) + (x.a - y.a) * (x.a - y.a) + (x.b - y.b) * (x.b - y.b));
}

QColor fromLinear(float r, float g, float b)
{
    return QColor(qRound(toGamma(r) * 255.0f), qRound(toGamma(g) * 255.0f), qRound(toGamma(b) * 255.0f));
}

// CIE L*a*b* as swatch files store it (D50), to sRGB.
QColor fromCieLab(float L, float a, float b)
{
    const float fy = (L + 16.0f) / 116.0f, fx = fy + a / 500.0f, fz = fy - b / 200.0f;
    auto inv = [](float f) { return f * f * f > 0.008856f ? f * f * f : (116.0f * f - 16.0f) / 903.3f; };
    const float X = inv(fx) * 0.96422f, Y = inv(fy), Z = inv(fz) * 0.82521f;
    return fromLinear(3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z,
                      -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z,
                      0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z);
}

// ---- file formats

QVector<QColor> readGpl(const QByteArray& data, QString* name)
{
    QVector<QColor> out;
    static const QRegularExpression space(QStringLiteral("\\s+"));
    for (const QString& raw : QString::fromUtf8(data).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1String("GIMP Palette"))
            || line.startsWith(QLatin1String("Columns:"), Qt::CaseInsensitive))
            continue;
        if (line.startsWith(QLatin1String("Name:"), Qt::CaseInsensitive)) {
            if (name)
                *name = line.mid(5).trimmed();
            continue;
        }
        const QStringList parts = line.split(space, Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;
        bool okR = false, okG = false, okB = false;
        const int r = parts[0].toInt(&okR), g = parts[1].toInt(&okG), b = parts[2].toInt(&okB);
        if (okR && okG && okB)
            out.push_back(QColor(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)));
    }
    return out;
}

// One colour to a line, as rrggbb or aarrggbb, with or without a # in front.
QVector<QColor> readHexList(const QByteArray& data)
{
    QVector<QColor> out;
    static const QRegularExpression code(QStringLiteral("^(?:#|0x)?([0-9a-fA-F]{8}|[0-9a-fA-F]{6})\\b"));
    for (const QString& raw : QString::fromUtf8(data).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char(';')) || line.startsWith(QLatin1String("//")))
            continue;
        const QRegularExpressionMatch m = code.match(line);
        if (!m.hasMatch())
            continue;
        const QColor c(QLatin1Char('#') + m.captured(1).right(6));
        if (c.isValid())
            out.push_back(c);
    }
    return out;
}

QVector<QColor> readAse(const QByteArray& data)
{
    QVector<QColor> out;
    QDataStream in(data);
    in.setByteOrder(QDataStream::BigEndian);
    in.setFloatingPointPrecision(QDataStream::SinglePrecision);
    in.skipRawData(4);   // "ASEF"
    quint16 major = 0, minor = 0;
    quint32 blocks = 0;
    in >> major >> minor >> blocks;
    for (quint32 i = 0; i < blocks && !in.atEnd() && in.status() == QDataStream::Ok; ++i) {
        quint16 type = 0;
        quint32 length = 0;
        in >> type >> length;
        const qint64 next = in.device()->pos() + qint64(length);
        if (type == 0x0001) {   // a colour; group starts and ends are passed over
            quint16 nameLength = 0;
            in >> nameLength;
            in.skipRawData(int(nameLength) * 2);
            char model[5] = {0, 0, 0, 0, 0};
            in.readRawData(model, 4);
            float v[4] = {0, 0, 0, 0};
            const QByteArray kind(model);
            const int n = kind == "CMYK" ? 4 : kind == "Gray" ? 1 : 3;
            for (int k = 0; k < n; ++k)
                in >> v[k];
            if (in.status() != QDataStream::Ok)
                break;
            auto byte = [](float f) { return std::clamp(qRound(f * 255.0f), 0, 255); };
            if (kind == "RGB ")
                out.push_back(QColor(byte(v[0]), byte(v[1]), byte(v[2])));
            else if (kind == "Gray")
                out.push_back(QColor(byte(v[0]), byte(v[0]), byte(v[0])));
            else if (kind == "CMYK")
                out.push_back(QColor(byte((1 - v[0]) * (1 - v[3])), byte((1 - v[1]) * (1 - v[3])), byte((1 - v[2]) * (1 - v[3]))));
            else if (kind == "LAB ")
                out.push_back(fromCieLab(v[0] * 100.0f, v[1], v[2]));
        }
        if (!in.device()->seek(next))
            break;
    }
    return out;
}

} // namespace

// ----------------------------------------------------------------- Palette

int Palette::filled() const
{
    return int(std::count_if(colors.begin(), colors.end(), [](const QColor& c) { return c.isValid(); }));
}

QVector<QColor> Palette::filledColors() const
{
    QVector<QColor> out;
    for (const QColor& c : colors) {
        if (c.isValid())
            out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------- PaletteLibrary

PaletteLibrary& PaletteLibrary::instance()
{
    static PaletteLibrary library;
    return library;
}

PaletteLibrary::PaletteLibrary()
{
    resetToStarters();
}

void PaletteLibrary::resetToStarters()
{
    m_palettes.clear();
    m_nextId = 1;
    for (const Starter& s : starters()) {
        Palette p;
        p.id = newId();
        p.name = tr(s.name);
        p.colors = s.colors;
        fit(p);
        m_palettes.push_back(p);
    }
    m_current = m_palettes.first().id;
    emit changed();
}

void PaletteLibrary::addStarters()
{
    // In their usual order, above what is there.
    for (int i = int(starters().size()) - 1; i >= 0; --i)
        create(tr(starters()[i].name), starters()[i].colors);
}

QString PaletteLibrary::newId()
{
    return QStringLiteral("palette-%1").arg(m_nextId++);
}

const Palette* PaletteLibrary::find(const QString& id) const
{
    for (const Palette& p : m_palettes) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

Palette* PaletteLibrary::mutableFind(const QString& id)
{
    for (Palette& p : m_palettes) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

int PaletteLibrary::indexOf(const QString& id) const
{
    for (int i = 0; i < m_palettes.size(); ++i) {
        if (m_palettes[i].id == id)
            return i;
    }
    return -1;
}

const Palette& PaletteLibrary::current() const
{
    const Palette* p = find(m_current);
    return p ? *p : m_palettes.first();
}

void PaletteLibrary::setCurrent(const QString& id)
{
    if (id == m_current || !find(id))
        return;
    m_current = id;
    emit changed();
}

QString PaletteLibrary::uniqueName(const QString& wanted, const QString& exceptId) const
{
    auto taken = [&](const QString& name) {
        for (const Palette& p : m_palettes) {
            if (p.id != exceptId && p.name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    const QString base = wanted.trimmed().isEmpty() ? tr("Palette") : wanted.trimmed().left(60);
    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(name))
            return name;
    }
}

QString PaletteLibrary::create(const QString& name, const QVector<QColor>& colors)
{
    Palette p;
    p.id = newId();
    p.name = uniqueName(name);
    p.colors = colors;
    fit(p);
    m_palettes.prepend(p);
    emit changed();
    return p.id;
}

QString PaletteLibrary::duplicate(const QString& id)
{
    const int at = indexOf(id);
    if (at < 0)
        return {};
    Palette p = m_palettes[at];
    p.id = newId();
    p.name = uniqueName(tr("%1 copy").arg(p.name));
    m_palettes.insert(at, p);   // just above the one it came from
    emit changed();
    return p.id;
}

bool PaletteLibrary::rename(const QString& id, const QString& name)
{
    Palette* p = mutableFind(id);
    if (!p || name.trimmed().isEmpty())
        return false;
    p->name = uniqueName(name, id);
    emit changed();
    return true;
}

bool PaletteLibrary::remove(const QString& id)
{
    const int at = indexOf(id);
    if (at < 0)
        return false;
    m_palettes.removeAt(at);
    if (m_palettes.isEmpty()) {
        Palette p;
        p.id = newId();
        p.name = tr("Palette");
        fit(p);
        m_palettes.push_back(p);
    }
    // The default moves on to the palette that came up into its place.
    if (m_current == id)
        m_current = m_palettes[std::min(at, int(m_palettes.size()) - 1)].id;
    emit changed();
    return true;
}

bool PaletteLibrary::setSwatch(const QString& id, int slot, const QColor& color)
{
    Palette* p = mutableFind(id);
    if (!p || slot < 0 || slot >= p->colors.size())
        return false;
    const QColor stored = color.isValid() ? QColor(color.red(), color.green(), color.blue()) : QColor();
    if (p->colors[slot] == stored && p->colors[slot].isValid() == stored.isValid())
        return false;
    p->colors[slot] = stored;
    fit(*p);
    emit changed();
    return true;
}

int PaletteLibrary::placeSwatch(const QString& id, int slot, const QColor& color)
{
    Palette* p = mutableFind(id);
    if (!p || !color.isValid())
        return -1;
    slot = std::clamp(slot, 0, int(p->colors.size()));
    int free = -1;
    for (int k = slot; k < p->colors.size() && free < 0; ++k) {
        if (!p->colors[k].isValid())
            free = k;
    }
    for (int k = 0; k < slot && free < 0; ++k) {
        if (!p->colors[k].isValid())
            free = k;
    }
    if (free < 0) {
        if (p->colors.size() >= Palette::kMaxSlots)
            return -1;
        free = int(p->colors.size());
        p->colors.resize(p->colors.size() + Palette::kColumns);
    }
    p->colors[free] = QColor(color.red(), color.green(), color.blue());
    fit(*p);
    emit changed();
    return free;
}

bool PaletteLibrary::moveSwatch(const QString& id, int from, int to)
{
    Palette* p = mutableFind(id);
    if (!p || from == to || from < 0 || to < 0 || from >= p->colors.size() || to >= p->colors.size() || !p->colors[from].isValid())
        return false;
    std::swap(p->colors[from], p->colors[to]);
    fit(*p);
    emit changed();
    return true;
}

// -------------------------------------------------------------- interchange

QVector<QColor> PaletteLibrary::readColors(const QByteArray& data, QString* name)
{
    if (data.startsWith("ASEF"))
        return readAse(data);
    if (data.left(64).contains("GIMP Palette"))
        return readGpl(data, name);
    return readHexList(data);
}

QVector<QColor> PaletteLibrary::readSwatches(const QString& path, QString* name)
{
    zip::Reader zip;
    if (!zip.open(path) || !zip.contains(QStringLiteral("Swatches.json")))
        return {};
    const QJsonDocument json = QJsonDocument::fromJson(zip.read(QStringLiteral("Swatches.json")));
    // One palette, as an object or as the only entry of a list.
    const QJsonObject palette = json.isArray() ? json.array().first().toObject() : json.object();
    if (name)
        *name = palette.value(QStringLiteral("name")).toString();
    QVector<QColor> colors;
    for (const QJsonValue v : palette.value(QStringLiteral("swatches")).toArray()) {
        if (!v.isObject()) {
            colors.push_back(QColor());   // an empty slot keeps its place
            continue;
        }
        const QJsonObject s = v.toObject();
        colors.push_back(QColor::fromHsvF(float(std::clamp(s.value(QStringLiteral("hue")).toDouble(), 0.0, 1.0)),
                                          float(std::clamp(s.value(QStringLiteral("saturation")).toDouble(), 0.0, 1.0)),
                                          float(std::clamp(s.value(QStringLiteral("brightness")).toDouble(), 0.0, 1.0))));
    }
    while (!colors.isEmpty() && !colors.back().isValid())
        colors.pop_back();
    bool any = false;
    for (const QColor& c : colors)
        any = any || c.isValid();
    return any ? colors : QVector<QColor>();
}

bool PaletteLibrary::exportSwatches(const QString& id, const QString& path, QString* error) const
{
    const Palette* p = find(id);
    if (!p)
        return false;
    QJsonArray swatches;
    int last = -1;
    for (int i = 0; i < p->colors.size(); ++i) {
        if (p->colors[i].isValid())
            last = i;
    }
    for (int i = 0; i <= last; ++i) {
        const QColor c = p->colors[i];
        if (!c.isValid()) {
            swatches.append(QJsonValue::Null);
            continue;
        }
        float h = 0, s = 0, v = 0;
        c.getHsvF(&h, &s, &v);
        QJsonObject o;
        o[QStringLiteral("hue")] = h < 0.0f ? 0.0 : double(h);
        o[QStringLiteral("saturation")] = double(s);
        o[QStringLiteral("brightness")] = double(v);
        o[QStringLiteral("alpha")] = 1;
        o[QStringLiteral("colorSpace")] = 0;
        swatches.append(o);
    }
    QJsonObject palette;
    palette[QStringLiteral("name")] = p->name;
    palette[QStringLiteral("swatches")] = swatches;
    const QByteArray json = QJsonDocument(QJsonArray{palette}).toJson(QJsonDocument::Compact);
    const QString temporary = path + QStringLiteral(".part");
    zip::Writer zip(temporary);
    const bool ok = zip.add(QStringLiteral("Swatches.json"), json, true) && zip.finish();
    QFile::remove(path);
    if (!ok || !QFile::rename(temporary, path)) {
        QFile::remove(temporary);
        if (error)
            *error = tr("%1 could not be written.").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

QString PaletteLibrary::importFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("%1 could not be opened.").arg(QFileInfo(path).fileName());
        return {};
    }
    QString name;
    const QByteArray head = file.peek(4);
    // A zip: Procreate's palette.
    const QVector<QColor> colors = head.startsWith("PK") ? readSwatches(path, &name) : readColors(file.read(8 * 1024 * 1024), &name);
    if (colors.isEmpty()) {
        if (error)
            *error = tr("No colours were found in %1. GIMP palettes (.gpl), lists of hex codes (.hex, .txt), "
                        "Adobe swatch files (.ase) and Procreate palettes (.swatches) can be read.").arg(QFileInfo(path).fileName());
        return {};
    }
    if (name.isEmpty())
        name = QFileInfo(path).completeBaseName();
    return create(name, colors);
}

bool PaletteLibrary::exportGpl(const QString& id, const QString& path, QString* error) const
{
    const Palette* p = find(id);
    if (!p)
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("%1 could not be written.").arg(QFileInfo(path).fileName());
        return false;
    }
    QByteArray out = "GIMP Palette\nName: " + p->name.toUtf8() + "\nColumns: " + QByteArray::number(Palette::kColumns) + "\n#\n";
    for (const QColor& c : p->colors) {
        if (!c.isValid())
            continue;
        out += QStringLiteral("%1 %2 %3\t%4\n")
                   .arg(c.red(), 3).arg(c.green(), 3).arg(c.blue(), 3).arg(c.name()).toUtf8();
    }
    file.write(out);
    if (!file.commit()) {
        if (error)
            *error = tr("%1 could not be written.").arg(QFileInfo(path).fileName());
        return false;
    }
    return true;
}

// ------------------------------------------------- colours from a picture

QVector<QColor> PaletteLibrary::fromImage(const QImage& source, int count)
{
    if (source.isNull() || count <= 0)
        return {};

    // Pixels as they are, taken on an even grid: no blends of neighbours that
    // the picture itself does not contain. Differences of hue and saturation
    // are counted double, or a picture's lights and darks would take every
    // swatch and leave none for its colours.
    constexpr float kChromaWeight = 2.0f;
    struct Sample {
        Lab lab;
        QRgb rgb;
    };
    std::vector<Sample> px;
    const int step = std::max(1, int(std::ceil(std::max(source.width(), source.height()) / 160.0)));
    for (int y = step / 2; y < source.height(); y += step) {
        for (int x = step / 2; x < source.width(); x += step) {
            const QRgb c = source.pixel(x, y);
            if (qAlpha(c) < 128)
                continue;
            Lab lab = toOklab(c);
            lab.a *= kChromaWeight;
            lab.b *= kChromaWeight;
            px.push_back({lab, c | 0xff000000u});
        }
    }
    if (px.empty())
        return {};

    // Split whichever group of pixels varies most, along the direction it
    // varies in and at its mean, until there are as many groups as colours wanted.
    struct Box {
        int begin = 0, end = 0;
        Lab mean;
        int axis = 0;
        float spread = 0.0f;   // squared deviation along the axis, summed
    };
    auto measure = [&](Box& b) {
        double sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0};
        for (int i = b.begin; i < b.end; ++i) {
            for (int k = 0; k < 3; ++k) {
                sum[k] += px[size_t(i)].lab[k];
                sq[k] += double(px[size_t(i)].lab[k]) * px[size_t(i)].lab[k];
            }
        }
        const double n = b.end - b.begin;
        b.mean = {float(sum[0] / n), float(sum[1] / n), float(sum[2] / n)};
        b.spread = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float dev = float(std::max(0.0, sq[k] - sum[k] * sum[k] / n));
            if (dev > b.spread) {
                b.spread = dev;
                b.axis = k;
            }
        }
    };
    std::vector<Box> boxes(1);
    boxes[0].end = int(px.size());
    measure(boxes[0]);
    while (int(boxes.size()) < count) {
        size_t widest = 0;
        for (size_t i = 1; i < boxes.size(); ++i) {
            if (boxes[i].spread > boxes[widest].spread)
                widest = i;
        }
        Box& b = boxes[widest];
        if (b.spread < 1e-4f)
            break;   // nothing left that differs
        const int axis = b.axis;
        const float mean = b.mean[axis];
        const auto mid = std::partition(px.begin() + b.begin, px.begin() + b.end, [&](const Sample& s) { return s.lab[axis] < mean; });
        const int cut = int(mid - px.begin());
        if (cut == b.begin || cut == b.end) {
            b.spread = 0.0f;
            continue;
        }
        Box upper;
        upper.begin = cut;
        upper.end = b.end;
        b.end = cut;
        measure(b);
        measure(upper);
        boxes.push_back(upper);
    }

    // Each group is shown by the pixel in it nearest its mean, the larger
    // groups first, and a colour too close to one already taken is left out.
    std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.end - a.begin > b.end - b.begin; });
    std::vector<Sample> picked;
    for (const Box& b : boxes) {
        const Sample* best = &px[size_t(b.begin)];
        float bestD = distance(best->lab, b.mean);
        for (int i = b.begin + 1; i < b.end; ++i) {
            const float d = distance(px[size_t(i)].lab, b.mean);
            if (d < bestD) {
                bestD = d;
                best = &px[size_t(i)];
            }
        }
        const bool alike = std::any_of(picked.begin(), picked.end(), [&](const Sample& s) { return distance(s.lab, best->lab) < 0.025f; });
        if (!alike)
            picked.push_back(*best);
    }

    // Greys from dark to light, then round the hue circle starting at red,
    // each stretch of it from dark to light.
    auto place = [](const Sample& s) {
        const float chroma = std::hypot(s.lab.a, s.lab.b) / kChromaWeight;
        if (chroma < 0.035f)
            return -1;
        float hue = std::atan2(s.lab.b, s.lab.a) * 180.0f / 3.14159265f - 345.0f;
        while (hue < 0.0f)
            hue += 360.0f;
        return std::min(11, int(hue / 30.0f));
    };
    std::stable_sort(picked.begin(), picked.end(), [&](const Sample& a, const Sample& b) {
        const int pa = place(a), pb = place(b);
        return pa != pb ? pa < pb : a.lab.L < b.lab.L;
    });
    QVector<QColor> out;
    for (const Sample& s : picked)
        out.push_back(QColor(qRed(s.rgb), qGreen(s.rgb), qBlue(s.rgb)));
    return out;
}

// -------------------------------------------------------------------- store

void PaletteLibrary::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly))
        return;   // a first start: the starter palettes stay
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QVector<Palette> read;
    int highest = 0;
    for (const QJsonValue& entry : root.value(QStringLiteral("palettes")).toArray()) {
        const QJsonObject o = entry.toObject();
        Palette p;
        p.id = o.value(QStringLiteral("id")).toString();
        p.name = o.value(QStringLiteral("name")).toString().trimmed();
        const bool known = std::any_of(read.begin(), read.end(), [&](const Palette& other) { return other.id == p.id; });
        if (!p.id.startsWith(QLatin1String("palette-")) || known)
            continue;
        if (p.name.isEmpty())
            p.name = tr("Palette");
        for (const QJsonValue& c : o.value(QStringLiteral("colors")).toArray()) {
            const QString code = c.toString();
            p.colors.push_back(code.isEmpty() ? QColor() : QColor(code));
        }
        fit(p);
        highest = std::max(highest, p.id.mid(8).toInt());
        read.push_back(p);
    }
    if (read.isEmpty())
        return;
    m_palettes = read;
    m_nextId = std::max(highest + 1, root.value(QStringLiteral("next")).toInt(1));
    m_current = root.value(QStringLiteral("current")).toString();
    if (!find(m_current))
        m_current = m_palettes.first().id;
    emit changed();
}

void PaletteLibrary::save() const
{
    QJsonObject root;
    QJsonArray all;
    for (const Palette& p : m_palettes) {
        QJsonObject o;
        o[QStringLiteral("id")] = p.id;
        o[QStringLiteral("name")] = p.name;
        // Up to the last colour; the empty slots after it are made again on reading.
        int last = -1;
        for (int i = 0; i < p.colors.size(); ++i) {
            if (p.colors[i].isValid())
                last = i;
        }
        QJsonArray colors;
        for (int i = 0; i <= last; ++i)
            colors.push_back(p.colors[i].isValid() ? p.colors[i].name() : QString());
        o[QStringLiteral("colors")] = colors;
        all.push_back(o);
    }
    root[QStringLiteral("palettes")] = all;
    root[QStringLiteral("current")] = m_current;
    root[QStringLiteral("next")] = m_nextId;
    const QString path = storePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson());
    file.commit();
}
