#include "brush/BrushPreset.h"

#include "brush/BrushTextures.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <iterator>
#include <vector>

using Angle = BrushPreset::AngleSource;
using Buildup = BrushPreset::Buildup;
using Grain = BrushPreset::GrainMode;

namespace {

// Every tunable number of a preset, by the name it has on disk.
struct FloatField {
    const char* key;
    float BrushPreset::*member;
};
const FloatField kFloatFields[] = {
    {"hardness", &BrushPreset::hardness},         {"roundness", &BrushPreset::roundness},
    {"angle", &BrushPreset::angle},               {"angleJitter", &BrushPreset::angleJitter},
    {"spacing", &BrushPreset::spacing},           {"scatter", &BrushPreset::scatter},
    {"streamline", &BrushPreset::streamline},     {"taper", &BrushPreset::taper},
    {"size", &BrushPreset::size},                 {"minSize", &BrushPreset::minSize},
    {"maxSize", &BrushPreset::maxSize},           {"opacity", &BrushPreset::opacity},
    {"flow", &BrushPreset::flow},                 {"pressureSize", &BrushPreset::pressureSize},
    {"pressureFlow", &BrushPreset::pressureFlow}, {"pressureGamma", &BrushPreset::pressureGamma},
    {"sizeJitter", &BrushPreset::sizeJitter},     {"flowJitter", &BrushPreset::flowJitter},
    {"tiltSize", &BrushPreset::tiltSize},         {"tiltFlow", &BrushPreset::tiltFlow},
    {"speedSize", &BrushPreset::speedSize},       {"grainScale", &BrushPreset::grainScale},
    {"grainDepth", &BrushPreset::grainDepth},     {"hueJitter", &BrushPreset::hueJitter},
    {"satJitter", &BrushPreset::satJitter},       {"valJitter", &BrushPreset::valJitter},
    {"wetEdges", &BrushPreset::wetEdges},         {"stabilization", &BrushPreset::stabilization},
    {"stringLength", &BrushPreset::stringLength}, {"pressureSmoothing", &BrushPreset::pressureSmoothing},
    {"taperEnd", &BrushPreset::taperEnd},         {"taperTip", &BrushPreset::taperTip},
    {"speedFlow", &BrushPreset::speedFlow},       {"wetMix", &BrushPreset::wetMix},
    {"wetCharge", &BrushPreset::wetCharge},
};

const QLatin1String kCustomKey("@custom");
const QLatin1String kOwnSet("My Brushes");

QString overridesPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/brushes.json");
}

// Everything but the id. Name and set are written too; whether they are read
// back is up to the caller (a built-in brush keeps its own).
QJsonObject toJson(const BrushPreset& p)
{
    QJsonObject o;
    for (const FloatField& f : kFloatFields)
        o[QLatin1String(f.key)] = double(p.*f.member);
    o[QStringLiteral("count")] = p.count;
    o[QStringLiteral("name")] = p.name;
    o[QStringLiteral("category")] = p.category;
    o[QStringLiteral("tip")] = p.tip;
    o[QStringLiteral("grain")] = p.grain;
    o[QStringLiteral("angleSource")] = int(p.angleSource);
    o[QStringLiteral("buildup")] = int(p.buildup);
    o[QStringLiteral("grainMode")] = int(p.grainMode);
    o[QStringLiteral("grainMoving")] = p.grainMoving;
    o[QStringLiteral("blend")] = int(p.blend);
    QJsonArray curve;
    for (const QPointF& point : p.pressureCurve) {
        curve.append(point.x());
        curve.append(point.y());
    }
    o[QStringLiteral("pressureCurve")] = curve;
    return o;
}

// Keys that are missing leave the preset as it is, which is what lets files
// written by earlier versions (numbers only) still load.
void fromJson(const QJsonObject& o, BrushPreset& p, bool withIdentity)
{
    for (const FloatField& f : kFloatFields) {
        if (o.contains(QLatin1String(f.key)))
            p.*f.member = float(o.value(QLatin1String(f.key)).toDouble(double(p.*f.member)));
    }
    p.count = std::clamp(o.value(QStringLiteral("count")).toInt(p.count), 1, 16);
    if (o.contains(QStringLiteral("tip")))
        p.tip = o.value(QStringLiteral("tip")).toString();
    if (o.contains(QStringLiteral("grain")))
        p.grain = o.value(QStringLiteral("grain")).toString();
    p.angleSource = Angle(std::clamp(o.value(QStringLiteral("angleSource")).toInt(int(p.angleSource)), 0, 2));
    p.buildup = Buildup(std::clamp(o.value(QStringLiteral("buildup")).toInt(int(p.buildup)), 0, 1));
    p.grainMode = Grain(std::clamp(o.value(QStringLiteral("grainMode")).toInt(int(p.grainMode)), 0, 3));
    p.grainMoving = o.value(QStringLiteral("grainMoving")).toBool(p.grainMoving);
    p.blend = BlendMode(std::clamp(o.value(QStringLiteral("blend")).toInt(int(p.blend)), 0, int(BlendMode::Count) - 1));
    if (o.contains(QStringLiteral("pressureCurve"))) {
        const QJsonArray curve = o.value(QStringLiteral("pressureCurve")).toArray();
        p.pressureCurve.clear();
        for (int i = 0; i + 1 < curve.size() && i < 24; i += 2)
            p.pressureCurve.push_back(QPointF(std::clamp(curve[i].toDouble(), 0.0, 1.0), std::clamp(curve[i + 1].toDouble(), 0.0, 1.0)));
        if (p.pressureCurve.size() < 2)
            p.pressureCurve.clear();
    }
    p.minSize = std::clamp(p.minSize, 0.5f, 2000.0f);
    p.maxSize = std::clamp(p.maxSize, p.minSize, 2000.0f);
    p.size = std::clamp(p.size, p.minSize, p.maxSize);
    if (withIdentity) {
        p.name = o.value(QStringLiteral("name")).toString(p.name).trimmed().left(60);
        p.category = o.value(QStringLiteral("category")).toString(p.category).trimmed().left(40);
    }
}

QString newId()
{
    return QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::Id128).left(12);
}

} // namespace

BrushLibrary& BrushLibrary::instance()
{
    static BrushLibrary lib;
    return lib;
}

BrushPreset* BrushLibrary::mutableFind(const QString& id)
{
    for (BrushPreset& p : m_presets) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

void BrushLibrary::update(const BrushPreset& preset)
{
    BrushPreset* p = mutableFind(preset.id);
    if (!p)
        return;
    const bool custom = p->custom;
    const QString name = p->name, category = p->category;
    *p = preset;
    // Who a brush is does not change by editing it.
    p->custom = custom;
    p->name = name;
    p->category = category;
    if (!custom)
        m_modified.insert(p->id);
    ++m_revision;
}

void BrushLibrary::reset(const QString& id)
{
    BrushPreset* p = mutableFind(id);
    if (!p || p->custom)
        return;
    for (const BrushPreset& d : m_defaults) {
        if (d.id == id)
            *p = d;
    }
    m_modified.remove(id);
    ++m_revision;
}

QString BrushLibrary::uniqueName(const QString& wanted, const QString& exceptId) const
{
    auto taken = [&](const QString& name) {
        for (const BrushPreset& p : m_presets) {
            if (p.id != exceptId && p.name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    const QString base = wanted.trimmed().isEmpty() ? QObject::tr("Brush") : wanted.trimmed().left(60);
    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const QString name = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(name))
            return name;
    }
}

QString BrushLibrary::createBrush(const QString& category)
{
    BrushPreset b;
    b.id = newId();
    b.custom = true;
    b.name = uniqueName(QObject::tr("New Brush"));
    b.category = category.trimmed().isEmpty() ? QString(kOwnSet) : category.trimmed();
    // A plain round brush that answers to pressure: something to start from.
    b.spacing = 0.06f;
    b.hardness = 0.85f;
    b.size = 24.0f;
    b.minSize = 1.0f;
    b.maxSize = 400.0f;
    b.pressureSize = 0.7f;
    b.streamline = 0.2f;
    m_presets.push_back(b);
    rebuildCategories();
    ++m_revision;
    return b.id;
}

QString BrushLibrary::duplicate(const QString& id)
{
    const BrushPreset* source = find(id);
    if (!source)
        return {};
    BrushPreset b = *source;
    b.id = newId();
    b.custom = true;
    b.name = uniqueName(QObject::tr("%1 copy").arg(source->name));
    m_presets.push_back(b);
    rebuildCategories();
    ++m_revision;
    return b.id;
}

bool BrushLibrary::rename(const QString& id, const QString& name)
{
    BrushPreset* p = mutableFind(id);
    if (!p || !p->custom || name.trimmed().isEmpty())
        return false;
    p->name = uniqueName(name, id);
    ++m_revision;
    return true;
}

bool BrushLibrary::moveToCategory(const QString& id, const QString& category)
{
    BrushPreset* p = mutableFind(id);
    if (!p || !p->custom || category.trimmed().isEmpty())
        return false;
    p->category = category.trimmed().left(40);
    rebuildCategories();
    ++m_revision;
    return true;
}

bool BrushLibrary::remove(const QString& id)
{
    for (auto it = m_presets.begin(); it != m_presets.end(); ++it) {
        if (it->id != id)
            continue;
        if (!it->custom)
            return false;
        m_presets.erase(it);
        rebuildCategories();
        ++m_revision;
        return true;
    }
    return false;
}

bool BrushLibrary::exportBrush(const QString& id, const QString& path, QString* error) const
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    const BrushPreset* p = find(id);
    if (!p)
        return fail(QObject::tr("That brush no longer exists"));
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("impasto-brush");
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("brush")] = toJson(*p);
    // Imported shapes travel inside the file, or the brush would arrive bald.
    QJsonObject textures;
    for (const QString& name : {p->tip, p->grain}) {
        const QImage map = brushtex::load(name);
        if (map.isNull())
            continue;
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        map.save(&buffer, "PNG");
        QJsonObject t;
        t[QStringLiteral("label")] = map.text(QStringLiteral("label"));
        t[QStringLiteral("png")] = QString::fromLatin1(png.toBase64());
        textures[name] = t;
    }
    if (!textures.isEmpty())
        root[QStringLiteral("textures")] = textures;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(file.errorString());
    file.write(QJsonDocument(root).toJson());
    return file.commit() ? true : fail(file.errorString());
}

QString BrushLibrary::importBrush(const QString& path, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return QString();
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > 64 * 1024 * 1024)
        return fail(QObject::tr("That file is too large to be a brush"));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QString format = root.value(QStringLiteral("format")).toString();
    if ((format != QLatin1String("impasto-brush") && format != QLatin1String("sketchbook-brush")) || !root.value(QStringLiteral("brush")).isObject())
        return fail(QObject::tr("%1 is not a brush file").arg(QFileInfo(path).fileName()));

    BrushPreset b;
    b.name = QFileInfo(path).completeBaseName();
    b.category = kOwnSet;
    fromJson(root.value(QStringLiteral("brush")).toObject(), b, true);
    b.id = newId();
    b.custom = true;
    b.name = uniqueName(b.name);
    if (b.category.isEmpty())
        b.category = kOwnSet;

    // Unpack the shapes it brought along; they may be here already.
    const QJsonObject textures = root.value(QStringLiteral("textures")).toObject();
    auto unpack = [&](QString& name, bool grain) {
        if (!brushtex::isUser(name))
            return;
        const QJsonObject t = textures.value(name).toObject();
        QImage map;
        map.loadFromData(QByteArray::fromBase64(t.value(QStringLiteral("png")).toString().toLatin1()), "PNG");
        const QString stored = brushtex::adopt(map, t.value(QStringLiteral("label")).toString(), grain);
        if (!stored.isEmpty())
            name = stored;
        else if (brushtex::load(name).isNull())
            name.clear();   // nothing to show for it: fall back to the round tip / no grain
    };
    unpack(b.tip, false);
    unpack(b.grain, true);

    m_presets.push_back(b);
    rebuildCategories();
    ++m_revision;
    return b.id;
}

void BrushLibrary::load()
{
    m_presets.assign(m_defaults.begin(), m_defaults.end());
    m_modified.clear();
    m_order.clear();
    QFile file(overridesPath());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (BrushPreset& p : m_presets) {
            const QJsonValue entry = root.value(p.id);
            if (!entry.isObject())
                continue;
            fromJson(entry.toObject(), p, false);
            m_modified.insert(p.id);
        }
        const QJsonObject order = root.value(QStringLiteral("@order")).toObject();
        for (auto it = order.constBegin(); it != order.constEnd(); ++it) {
            QStringList ids;
            for (const QJsonValue& v : it.value().toArray())
                ids << v.toString();
            m_order.insert(it.key(), ids);
        }
        const QJsonArray own = root.value(kCustomKey).toArray();
        for (const QJsonValue& entry : own) {
            const QJsonObject o = entry.toObject();
            BrushPreset b;
            b.id = o.value(QStringLiteral("id")).toString();
            if (!b.id.startsWith(QLatin1String("custom-")) || find(b.id))
                continue;
            b.custom = true;
            b.name = QObject::tr("Brush");
            b.category = kOwnSet;
            fromJson(o, b, true);
            if (b.name.isEmpty())
                b.name = QObject::tr("Brush");
            if (b.category.isEmpty())
                b.category = kOwnSet;
            m_presets.push_back(b);
        }
    }
    rebuildCategories();
    ++m_revision;
}

void BrushLibrary::save() const
{
    QJsonObject root;
    QJsonArray own;
    for (const BrushPreset& p : m_presets) {
        if (p.custom) {
            QJsonObject o = toJson(p);
            o[QStringLiteral("id")] = p.id;
            own.push_back(o);
        } else if (m_modified.contains(p.id)) {
            root[p.id] = toJson(p);
        }
    }
    if (!own.isEmpty())
        root[kCustomKey] = own;
    QJsonObject order;
    for (auto it = m_order.constBegin(); it != m_order.constEnd(); ++it)
        order[it.key()] = QJsonArray::fromStringList(it.value());
    if (!order.isEmpty())
        root[QStringLiteral("@order")] = order;
    const QString path = overridesPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson());
    file.commit();
}

QVector<const BrushPreset*> BrushLibrary::inCategory(const QString& category) const
{
    // As shipped: the artist's own brushes first, newest on top, then the built-in ones.
    QVector<const BrushPreset*> out;
    for (auto it = m_presets.rbegin(); it != m_presets.rend(); ++it) {
        if (it->custom && it->category == category)
            out.push_back(&*it);
    }
    for (const BrushPreset& p : m_presets) {
        if (!p.custom && p.category == category)
            out.push_back(&p);
    }
    // A set that has been rearranged keeps that order; brushes that arrived
    // since then go on top, as new brushes do.
    const auto order = m_order.constFind(category);
    if (order != m_order.constEnd()) {
        std::stable_sort(out.begin(), out.end(), [&](const BrushPreset* a, const BrushPreset* b) {
            return order->indexOf(a->id) < order->indexOf(b->id);
        });
    }
    return out;
}

bool BrushLibrary::moveInCategory(const QString& id, int toIndex)
{
    const BrushPreset* p = find(id);
    if (!p)
        return false;
    QStringList ids;
    for (const BrushPreset* other : inCategory(p->category))
        ids << other->id;
    const int from = int(ids.indexOf(id));
    toIndex = std::clamp(toIndex, 0, int(ids.size()) - 1);
    if (from < 0 || from == toIndex)
        return false;
    ids.move(from, toIndex);
    m_order.insert(p->category, ids);
    ++m_revision;
    return true;
}

const BrushPreset* BrushLibrary::find(const QString& id) const
{
    for (const BrushPreset& p : m_presets) {
        if (p.id == id)
            return &p;
    }
    return nullptr;
}

void BrushLibrary::add(BrushPreset preset)
{
    m_presets.push_back(std::move(preset));
}

// The shipped sets in their shipped order, then any set the artist started.
void BrushLibrary::rebuildCategories()
{
    m_categories.clear();
    for (const BrushPreset& p : m_presets) {
        if (!m_categories.contains(p.category))
            m_categories.push_back(p.category);
    }
}

static BrushPreset make(const char* id, const char* name, const char* category)
{
    BrushPreset b;
    b.id = QString::fromLatin1(id);
    b.name = QString::fromUtf8(name);
    b.category = QString::fromLatin1(category);
    return b;
}

static void setGrain(BrushPreset& b, const char* grain, Grain mode, float depth, float scale = 1.0f)
{
    b.grain = QString::fromLatin1(grain);
    b.grainMode = mode;
    b.grainDepth = depth;
    b.grainScale = scale;
}

// The sets as the library lists them. Brushes are written down set by set
// below, but not in this order throughout: this is what sorts them.
const char* const kSetOrder[] = {"Sketching", "Inking", "Drawing", "Calligraphy", "Painting", "Watercolour", "Airbrushing",
                                 "Charcoals", "Spray Paint", "Textures", "Screentones", "Elements", "Luminance", "Abstract"};

static int setRank(const QString& category)
{
    for (size_t i = 0; i < std::size(kSetOrder); ++i) {
        if (category == QLatin1String(kSetOrder[i]))
            return int(i);
    }
    return int(std::size(kSetOrder));
}

// The second batch of shipped brushes: the sets that came later, and more for the first five.
static std::vector<BrushPreset> moreBrushes()
{
    std::vector<BrushPreset> out;
    // A new brush to fill in; good until the next one is started.
    auto brush = [&](const char* id, const char* name, const char* set) -> BrushPreset& {
        out.push_back(make(id, name, set));
        return out.back();
    };
    auto range = [](BrushPreset& b, float size, float smallest, float largest) {
        b.size = size;
        b.minSize = smallest;
        b.maxSize = largest;
    };
    // A plain tone brush for showing off a grain: paint an area and the pattern fills it.
    auto tone = [&](const char* id, const char* name, const char* set, const char* grain, Grain mode, float scale, float hardness) -> BrushPreset& {
        BrushPreset& b = brush(id, name, set);
        b.hardness = hardness;
        b.spacing = 0.06f;
        range(b, 140, 10, 1000);
        b.streamline = 0.15f;
        setGrain(b, grain, mode, 1.0f, scale);
        return b;
    };

    // ------------------------------------------------------------ Sketching
    {
        BrushPreset& b = brush("pencil-2b", "2B Pencil", "Sketching");
        b.hardness = 0.62f; b.spacing = 0.07f; range(b, 6, 1, 60);
        b.flow = 0.25f; b.pressureFlow = 0.97f; b.pressureSize = 0.25f; b.pressureGamma = 1.6f;
        b.tiltSize = 1.9f; b.tiltFlow = 0.42f; b.streamline = 0.05f;
        setGrain(b, "fine", Grain::Height, 0.8f, 0.7f);
    }
    {
        // Held flat, for laying in tone: wide, pale, all tooth.
        BrushPreset& b = brush("pencil-shading", "Shading Pencil", "Sketching");
        b.hardness = 0.35f; b.spacing = 0.06f; range(b, 42, 6, 200);
        b.flow = 0.12f; b.pressureFlow = 0.95f; b.pressureSize = 0.1f; b.pressureGamma = 1.3f;
        b.tiltSize = 0.8f; b.streamline = 0.05f;
        setGrain(b, "paper", Grain::Height, 0.9f, 0.55f);
    }
    {
        BrushPreset& b = brush("pencil-colour", "Coloured Pencil", "Sketching");
        b.hardness = 0.72f; b.spacing = 0.06f; range(b, 7, 1, 60);
        b.flow = 0.38f; b.pressureFlow = 0.9f; b.pressureSize = 0.25f; b.pressureGamma = 1.3f;
        b.tiltSize = 1.4f; b.tiltFlow = 0.35f; b.streamline = 0.08f;
        setGrain(b, "paper", Grain::Height, 0.82f, 0.42f);
    }
    {
        BrushPreset& b = brush("conte", "Conté Crayon", "Sketching");
        b.tip = QStringLiteral("chalk"); b.roundness = 0.7f; b.angleJitter = 0.03f;
        b.spacing = 0.07f; range(b, 16, 3, 160);
        b.flow = 0.55f; b.pressureFlow = 0.85f; b.pressureSize = 0.15f; b.tiltSize = 1.0f; b.streamline = 0.05f;
        setGrain(b, "cold_press", Grain::Height, 0.85f, 0.4f);
    }

    // --------------------------------------------------------------- Inking
    {
        // A manga nib: hairlines that open up under the hand, and run out to nothing.
        BrushPreset& b = brush("g-pen", "G-Pen", "Inking");
        b.hardness = 0.97f; b.spacing = 0.03f; range(b, 12, 1, 120);
        b.pressureSize = 0.97f; b.pressureGamma = 1.35f; b.streamline = 0.38f;
        b.taper = 0.6f; b.taperEnd = 0.8f; b.taperTip = 0.05f;
    }
    {
        BrushPreset& b = brush("mapping-pen", "Mapping Pen", "Inking");
        b.hardness = 0.98f; b.spacing = 0.03f; range(b, 4, 0.5f, 40);
        b.pressureSize = 0.85f; b.pressureGamma = 1.7f; b.streamline = 0.3f;
        b.taperEnd = 0.5f; b.taperTip = 0.05f;
    }
    {
        BrushPreset& b = brush("gel-pen", "Gel Pen", "Inking");
        b.hardness = 0.93f; b.spacing = 0.04f; range(b, 5, 1, 50);
        b.flow = 0.95f; b.pressureSize = 0.25f; b.speedSize = 0.12f; b.streamline = 0.28f;
    }
    {
        BrushPreset& b = brush("felt-tip", "Felt Tip", "Inking");
        b.hardness = 0.82f; b.spacing = 0.04f; range(b, 9, 1, 90);
        b.opacity = 0.92f; b.buildup = Buildup::Max;
        b.pressureSize = 0.45f; b.pressureGamma = 0.9f; b.streamline = 0.25f;
        setGrain(b, "fine", Grain::Edge, 0.5f, 0.5f);
    }
    {
        // Six points side by side: one stroke rules six lines.
        BrushPreset& b = brush("rake-pen", "Rake Pen", "Inking");
        b.tip = QStringLiteral("rake"); b.angleSource = Angle::Direction; b.angle = 90;
        b.spacing = 0.03f; range(b, 46, 8, 300);
        b.pressureSize = 0.3f; b.streamline = 0.35f;
    }
    {
        BrushPreset& b = brush("stipple-pen", "Stipple Pen", "Inking");
        b.tip = QStringLiteral("stipple"); b.angleJitter = 1.0f;
        b.spacing = 0.55f; b.scatter = 0.15f; range(b, 36, 6, 300);
        b.pressureSize = 0.3f; b.sizeJitter = 0.2f; b.streamline = 0.0f;
    }

    // -------------------------------------------------------------- Drawing
    {
        BrushPreset& b = brush("crayon", "Wax Crayon", "Drawing");
        b.tip = QStringLiteral("crayon"); b.angleJitter = 0.03f;
        b.spacing = 0.06f; range(b, 22, 4, 200);
        b.flow = 0.6f; b.pressureFlow = 0.75f; b.pressureSize = 0.12f; b.tiltSize = 0.6f; b.streamline = 0.05f;
        setGrain(b, "paper", Grain::Height, 0.9f, 0.6f);
    }
    {
        BrushPreset& b = brush("oil-pastel", "Oil Pastel", "Drawing");
        b.tip = QStringLiteral("pastel"); b.angleJitter = 0.5f;
        b.spacing = 0.07f; range(b, 34, 5, 300);
        b.flow = 0.7f; b.pressureFlow = 0.6f; b.pressureSize = 0.1f; b.valJitter = 0.02f; b.streamline = 0.05f;
        setGrain(b, "paper", Grain::Height, 0.75f, 0.7f);
    }
    {
        BrushPreset& b = brush("soft-pastel", "Soft Pastel", "Drawing");
        b.tip = QStringLiteral("pastel"); b.angleJitter = 1.0f;
        b.spacing = 0.08f; range(b, 44, 6, 400);
        b.flow = 0.32f; b.pressureFlow = 0.9f; b.sizeJitter = 0.08f; b.streamline = 0.05f;
        setGrain(b, "cold_press", Grain::Height, 0.9f, 0.5f);
    }
    {
        BrushPreset& b = brush("carbon-pencil", "Carbon Pencil", "Drawing");
        b.hardness = 0.6f; b.spacing = 0.06f; range(b, 8, 1, 80);
        b.flow = 0.5f; b.pressureFlow = 0.9f; b.pressureSize = 0.3f; b.pressureGamma = 1.2f;
        b.tiltSize = 1.5f; b.streamline = 0.06f;
        setGrain(b, "charcoal", Grain::Height, 0.75f, 0.35f);
    }
    {
        BrushPreset& b = brush("highlighter", "Highlighter", "Drawing");
        b.blend = BlendMode::Multiply;
        b.hardness = 0.92f; b.roundness = 0.34f; b.angle = 90;
        b.spacing = 0.03f; range(b, 40, 6, 240);
        b.opacity = 0.45f; b.buildup = Buildup::Max; b.streamline = 0.3f;
    }
    {
        // A band of ruled strokes that follows the hand: hatching by the yard.
        BrushPreset& b = brush("hatch-shader", "Hatch Shader", "Drawing");
        b.tip = QStringLiteral("hatch"); b.angleSource = Angle::Direction;
        b.spacing = 0.22f; range(b, 90, 12, 500);
        b.flow = 0.75f; b.pressureFlow = 0.5f; b.sizeJitter = 0.05f; b.streamline = 0.2f;
    }
    tone("crosshatch-shader", "Crosshatch Shader", "Drawing", "crosshatch", Grain::Height, 0.8f, 0.5f).pressureFlow = 0.9f;

    // ---------------------------------------------------------- Calligraphy
    {
        BrushPreset& b = brush("italic-nib", "Italic Nib", "Calligraphy");
        b.hardness = 0.98f; b.roundness = 0.13f; b.angle = 45;
        b.spacing = 0.02f; range(b, 22, 3, 200);
        b.pressureSize = 0.15f; b.streamline = 0.5f;
    }
    {
        BrushPreset& b = brush("brush-script", "Brush Script", "Calligraphy");
        b.hardness = 0.95f; b.spacing = 0.025f; range(b, 30, 3, 240);
        b.pressureSize = 1.0f; b.pressureGamma = 1.45f; b.speedSize = 0.15f; b.streamline = 0.55f;
        b.taper = 1.0f; b.taperEnd = 1.6f; b.taperTip = 0.04f;
    }
    {
        // A pointed pen: hairline upstrokes, and shades only where the hand leans in.
        BrushPreset& b = brush("copperplate", "Copperplate", "Calligraphy");
        b.hardness = 0.97f; b.roundness = 0.55f; b.angle = 55;
        b.spacing = 0.025f; range(b, 16, 2, 120);
        b.pressureSize = 1.0f; b.pressureGamma = 2.4f; b.streamline = 0.6f;
        b.taperEnd = 0.8f; b.taperTip = 0.06f;
    }
    {
        BrushPreset& b = brush("chisel-marker", "Chisel Marker", "Calligraphy");
        b.blend = BlendMode::Multiply;
        b.hardness = 0.9f; b.roundness = 0.26f; b.angle = 30;
        b.spacing = 0.03f; range(b, 34, 4, 260);
        b.opacity = 0.85f; b.buildup = Buildup::Max; b.pressureSize = 0.1f; b.streamline = 0.3f;
        setGrain(b, "fine", Grain::Multiply, 0.12f, 0.8f);
    }
    {
        // A brush running out of ink: it breaks up the faster it goes.
        BrushPreset& b = brush("dry-script", "Dry Script", "Calligraphy");
        b.tip = QStringLiteral("dry"); b.angleSource = Angle::Direction;
        b.spacing = 0.03f; range(b, 34, 4, 260);
        b.flow = 0.9f; b.pressureSize = 0.85f; b.pressureGamma = 1.2f; b.speedFlow = 0.6f; b.streamline = 0.45f;
        b.taper = 0.8f; b.taperEnd = 1.2f;
    }

    // ------------------------------------------------------------- Painting
    {
        BrushPreset& b = brush("acrylic", "Acrylic", "Painting");
        b.tip = QStringLiteral("flat_bristle"); b.angleSource = Angle::Direction;
        b.spacing = 0.03f; range(b, 54, 6, 500);
        b.flow = 0.95f; b.pressureFlow = 0.3f; b.pressureSize = 0.2f; b.valJitter = 0.02f; b.streamline = 0.1f;
        setGrain(b, "canvas", Grain::Multiply, 0.1f, 0.7f);
    }
    {
        BrushPreset& b = brush("thick-oil", "Thick Oil", "Painting");
        b.tip = QStringLiteral("bristle"); b.angleSource = Angle::Direction;
        b.spacing = 0.025f; range(b, 70, 8, 600);
        b.pressureFlow = 0.25f; b.pressureSize = 0.3f;
        b.valJitter = 0.06f; b.hueJitter = 0.012f; b.satJitter = 0.03f; b.streamline = 0.1f;
        setGrain(b, "canvas", Grain::Multiply, 0.2f, 0.8f);
    }
    {
        BrushPreset& b = brush("filbert", "Filbert", "Painting");
        b.hardness = 0.6f; b.roundness = 0.62f; b.angleSource = Angle::Direction; b.angle = 90;
        b.spacing = 0.04f; range(b, 48, 4, 500);
        b.flow = 0.85f; b.pressureFlow = 0.5f; b.pressureSize = 0.45f; b.streamline = 0.12f;
    }
    {
        BrushPreset& b = brush("fan-brush", "Fan Brush", "Painting");
        b.tip = QStringLiteral("fan"); b.angleSource = Angle::Direction; b.angle = 90;
        b.spacing = 0.03f; range(b, 90, 12, 600);
        b.flow = 0.85f; b.pressureFlow = 0.6f; b.speedFlow = 0.4f; b.streamline = 0.1f;
    }
    {
        BrushPreset& b = brush("rake-brush", "Rake Brush", "Painting");
        b.tip = QStringLiteral("rake"); b.angleSource = Angle::Direction; b.angle = 90;
        b.spacing = 0.03f; range(b, 80, 10, 500);
        b.flow = 0.7f; b.pressureFlow = 0.6f; b.valJitter = 0.03f; b.streamline = 0.12f;
    }
    {
        // The flat of a knife: one broad, hard-edged drag of paint.
        BrushPreset& b = brush("palette-knife", "Palette Knife", "Painting");
        b.tip = QStringLiteral("knife"); b.angleSource = Angle::Direction; b.angle = 90;
        b.spacing = 0.02f; range(b, 110, 12, 700);
        b.pressureFlow = 0.15f; b.valJitter = 0.035f; b.streamline = 0.12f;
    }
    {
        // Loaded with paint and worked into what is already there: colours drag into each other.
        BrushPreset& b = brush("wet-oil", "Wet Oil", "Painting");
        b.tip = QStringLiteral("bristle"); b.angleSource = Angle::Direction;
        b.spacing = 0.04f; range(b, 60, 8, 500);
        b.pressureFlow = 0.3f; b.pressureSize = 0.2f; b.streamline = 0.1f;
        b.wetMix = 0.8f; b.wetCharge = 0.45f;
    }
    {
        BrushPreset& b = brush("wet-acrylic", "Wet Acrylic", "Painting");
        b.tip = QStringLiteral("flat_bristle"); b.angleSource = Angle::Direction;
        b.spacing = 0.04f; range(b, 56, 8, 500);
        b.pressureFlow = 0.3f; b.streamline = 0.1f;
        b.wetMix = 0.5f; b.wetCharge = 0.7f;
    }
    {
        // Hardly any paint of its own: it softens and blends what is there.
        BrushPreset& b = brush("blender", "Blender", "Painting");
        b.hardness = 0.3f; b.spacing = 0.05f; range(b, 70, 8, 600);
        b.flow = 0.8f; b.pressureFlow = 0.6f; b.streamline = 0.1f;
        b.wetMix = 1.0f; b.wetCharge = 0.04f;
    }
    {
        BrushPreset& b = brush("poster-paint", "Poster Paint", "Painting");
        b.hardness = 0.9f; b.spacing = 0.04f; range(b, 38, 3, 500);
        b.buildup = Buildup::Max; b.pressureSize = 0.3f; b.streamline = 0.15f;
        setGrain(b, "paper", Grain::Edge, 0.6f, 0.3f);
    }
    {
        // A thin veil of colour: however often the stroke crosses itself, it stays one coat.
        BrushPreset& b = brush("glaze", "Glaze", "Painting");
        b.blend = BlendMode::Multiply;
        b.hardness = 0.25f; b.spacing = 0.06f; range(b, 120, 8, 900);
        b.opacity = 0.4f; b.flow = 0.5f; b.buildup = Buildup::Max; b.pressureFlow = 0.6f; b.streamline = 0.1f;
    }
    {
        BrushPreset& b = brush("stipple-brush", "Stipple Brush", "Painting");
        b.tip = QStringLiteral("stipple"); b.angleJitter = 1.0f;
        b.spacing = 0.3f; b.scatter = 0.25f; range(b, 70, 8, 500);
        b.flow = 0.6f; b.pressureFlow = 0.7f; b.sizeJitter = 0.25f; b.valJitter = 0.04f;
    }

    // ---------------------------------------------------------- Watercolour
    {
        BrushPreset& b = brush("wc-detail", "Detail Round", "Watercolour");
        b.blend = BlendMode::Multiply;
        b.hardness = 0.82f; b.spacing = 0.04f; range(b, 16, 2, 160);
        b.opacity = 0.9f; b.buildup = Buildup::Max; b.wetEdges = 0.5f;
        b.pressureSize = 0.9f; b.pressureGamma = 1.1f; b.streamline = 0.2f; b.taper = 0.5f; b.taperEnd = 0.8f;
        setGrain(b, "cold_press", Grain::Multiply, 0.18f, 1.2f);
    }
    {
        BrushPreset& b = brush("wc-glaze", "Wet Glaze", "Watercolour");
        b.blend = BlendMode::Multiply;
        b.hardness = 0.3f; b.spacing = 0.06f; range(b, 160, 12, 900);
        b.opacity = 0.32f; b.buildup = Buildup::Max; b.wetEdges = 0.4f;
        b.pressureSize = 0.3f; b.pressureFlow = 0.3f; b.streamline = 0.1f;
        setGrain(b, "cold_press", Grain::Multiply, 0.3f, 1.6f);
    }
    {
        BrushPreset& b = brush("wc-bloom", "Bloom", "Watercolour");
        b.blend = BlendMode::Multiply;
        b.tip = QStringLiteral("cloud"); b.angleJitter = 1.0f;
        b.spacing = 0.22f; b.scatter = 0.12f; range(b, 150, 16, 900);
        b.opacity = 0.6f; b.flow = 0.3f; b.wetEdges = 0.5f; b.pressureFlow = 0.7f; b.sizeJitter = 0.3f;
        setGrain(b, "blotch", Grain::Multiply, 0.35f, 1.8f);
    }
    {
        BrushPreset& b = brush("wc-dry", "Dry Watercolour", "Watercolour");
        b.blend = BlendMode::Multiply;
        b.tip = QStringLiteral("dry"); b.angleSource = Angle::Direction;
        b.spacing = 0.04f; range(b, 60, 6, 500);
        b.opacity = 0.8f; b.flow = 0.5f; b.wetEdges = 0.25f;
        b.pressureFlow = 0.6f; b.pressureSize = 0.25f; b.speedFlow = 0.5f;
        setGrain(b, "cold_press", Grain::Height, 0.5f, 0.9f);
    }
    {
        BrushPreset& b = brush("wc-splash", "Splash", "Watercolour");
        b.tip = QStringLiteral("splatter"); b.angleJitter = 1.0f;
        b.spacing = 0.7f; b.scatter = 0.9f; b.count = 2; range(b, 110, 12, 800);
        b.opacity = 0.75f; b.flow = 0.7f; b.wetEdges = 0.45f;
        b.sizeJitter = 0.7f; b.flowJitter = 0.3f; b.pressureSize = 0.4f;
    }
    {
        // A soft brush on wet paper: what it touches runs into it.
        BrushPreset& b = brush("wc-wet", "Wet in Wet", "Watercolour");
        b.hardness = 0.25f; b.spacing = 0.05f; range(b, 110, 10, 800);
        b.flow = 0.5f; b.pressureFlow = 0.5f; b.streamline = 0.1f;
        b.wetMix = 0.9f; b.wetCharge = 0.3f;
    }

    // ---------------------------------------------------------- Airbrushing
    {
        BrushPreset& b = brush("airbrush-detail", "Detail Airbrush", "Airbrushing");
        b.hardness = 0.5f; b.spacing = 0.05f; range(b, 20, 2, 300);
        b.flow = 0.2f; b.pressureFlow = 0.95f; b.pressureSize = 0.3f; b.streamline = 0.15f;
    }
    {
        BrushPreset& b = brush("airbrush-speckle", "Speckled Airbrush", "Airbrushing");
        b.tip = QStringLiteral("spray_fine"); b.angleJitter = 1.0f;
        b.spacing = 0.1f; range(b, 130, 12, 1000);
        b.flow = 0.6f; b.pressureFlow = 0.9f; b.sizeJitter = 0.15f;
    }

    // ------------------------------------------------------------ Charcoals
    {
        BrushPreset& b = brush("vine-charcoal", "Vine Charcoal", "Charcoals");
        b.tip = QStringLiteral("charcoal"); b.angleSource = Angle::Direction; b.angleJitter = 0.03f;
        b.spacing = 0.06f; range(b, 26, 4, 300);
        b.flow = 0.28f; b.pressureFlow = 0.9f; b.pressureSize = 0.2f; b.tiltSize = 1.2f; b.streamline = 0.05f;
        setGrain(b, "charcoal", Grain::Height, 0.92f, 0.5f);
    }
    {
        BrushPreset& b = brush("compressed-charcoal", "Compressed Charcoal", "Charcoals");
        b.tip = QStringLiteral("chalk"); b.angleJitter = 0.04f;
        b.spacing = 0.06f; range(b, 30, 4, 300);
        b.flow = 0.85f; b.pressureFlow = 0.6f; b.pressureSize = 0.15f; b.streamline = 0.05f;
        setGrain(b, "charcoal", Grain::Height, 0.6f, 0.45f);
    }
    {
        BrushPreset& b = brush("charcoal-pencil", "Charcoal Pencil", "Charcoals");
        b.hardness = 0.6f; b.spacing = 0.06f; range(b, 7, 1, 70);
        b.flow = 0.5f; b.pressureFlow = 0.9f; b.pressureSize = 0.3f;
        b.tiltSize = 2.0f; b.tiltFlow = 0.4f; b.streamline = 0.05f;
        setGrain(b, "charcoal", Grain::Height, 0.82f, 0.35f);
    }
    {
        // The long side of a stick, dragged flat.
        BrushPreset& b = brush("charcoal-block", "Charcoal Block", "Charcoals");
        b.tip = QStringLiteral("chalk"); b.roundness = 0.38f; b.angleSource = Angle::Direction; b.angle = 90;
        b.spacing = 0.05f; range(b, 90, 12, 600);
        b.flow = 0.5f; b.pressureFlow = 0.85f; b.streamline = 0.05f;
        setGrain(b, "paper", Grain::Height, 0.88f, 0.5f);
    }
    {
        BrushPreset& b = brush("soft-charcoal", "Soft Charcoal", "Charcoals");
        b.tip = QStringLiteral("pastel"); b.angleJitter = 1.0f;
        b.spacing = 0.07f; range(b, 70, 8, 600);
        b.flow = 0.18f; b.pressureFlow = 0.95f; b.streamline = 0.05f;
        setGrain(b, "charcoal", Grain::Height, 0.7f, 0.6f);
    }

    // ---------------------------------------------------------- Spray Paint
    {
        BrushPreset& b = brush("spray-fat", "Fat Cap", "Spray Paint");
        b.tip = QStringLiteral("spray_fine"); b.angleJitter = 1.0f;
        b.spacing = 0.08f; b.scatter = 0.05f; range(b, 260, 30, 1200);
        b.flow = 0.5f; b.pressureFlow = 0.7f; b.sizeJitter = 0.1f;
    }
    {
        BrushPreset& b = brush("spray-skinny", "Skinny Cap", "Spray Paint");
        b.tip = QStringLiteral("spray_fine"); b.angleJitter = 1.0f;
        b.spacing = 0.06f; range(b, 46, 8, 400);
        b.flow = 0.85f; b.pressureFlow = 0.6f;
    }
    {
        BrushPreset& b = brush("splatter", "Splatter", "Spray Paint");
        b.tip = QStringLiteral("splatter"); b.angleJitter = 1.0f;
        b.spacing = 0.9f; b.scatter = 1.2f; range(b, 140, 16, 900);
        b.sizeJitter = 0.8f; b.flowJitter = 0.2f; b.pressureSize = 0.3f;
    }
    {
        BrushPreset& b = brush("flicks", "Flicks", "Spray Paint");
        b.tip = QStringLiteral("stipple"); b.angleJitter = 1.0f;
        b.spacing = 1.0f; b.scatter = 1.5f; b.count = 2; range(b, 60, 8, 500);
        b.sizeJitter = 0.8f; b.flowJitter = 0.4f;
    }
    {
        BrushPreset& b = brush("drizzle", "Drizzle", "Spray Paint");
        b.hardness = 0.9f; b.spacing = 0.5f; b.scatter = 0.7f; b.count = 3; range(b, 12, 2, 120);
        b.sizeJitter = 0.9f; b.flowJitter = 0.2f; b.pressureSize = 0.4f;
    }

    // ------------------------------------------------------------- Textures
    {
        BrushPreset& b = tone("concrete", "Concrete", "Textures", "concrete", Grain::Height, 0.8f, 0.35f);
        b.flow = 0.55f; b.pressureFlow = 0.85f;
    }
    tone("wood-grain", "Wood Grain", "Textures", "wood", Grain::Multiply, 0.6f, 0.7f).buildup = Buildup::Max;
    {
        BrushPreset& b = tone("canvas-rub", "Canvas Rub", "Textures", "canvas", Grain::Height, 0.9f, 0.2f);
        b.flow = 0.4f; b.pressureFlow = 0.9f;
    }
    {
        BrushPreset& b = tone("linen", "Linen", "Textures", "linen", Grain::Height, 0.8f, 0.3f);
        b.flow = 0.4f; b.pressureFlow = 0.9f;
    }
    {
        BrushPreset& b = tone("burlap", "Burlap", "Textures", "burlap", Grain::Height, 0.5f, 0.5f);
        b.flow = 0.5f; b.pressureFlow = 0.85f;
    }
    tone("crackle", "Crackle", "Textures", "crackle", Grain::Multiply, 0.6f, 0.8f).buildup = Buildup::Max;
    {
        BrushPreset& b = tone("sand", "Sand", "Textures", "sand", Grain::Height, 0.7f, 0.3f);
        b.flow = 0.5f; b.pressureFlow = 0.9f;
    }
    {
        BrushPreset& b = tone("paper-fibre", "Paper Fibre", "Textures", "fibre", Grain::Height, 0.7f, 0.4f);
        b.flow = 0.5f; b.pressureFlow = 0.9f;
    }
    {
        BrushPreset& b = brush("scratches", "Scratches", "Textures");
        b.tip = QStringLiteral("scratch"); b.angleSource = Angle::Direction; b.angleJitter = 0.05f;
        b.spacing = 0.3f; b.scatter = 0.1f; range(b, 120, 16, 700);
        b.flow = 0.8f; b.pressureFlow = 0.5f; b.sizeJitter = 0.2f;
    }

    // ---------------------------------------------------------- Screentones
    tone("tone-coarse", "Coarse Dots", "Screentones", "halftone_coarse", Grain::Multiply, 0.7f, 0.96f).buildup = Buildup::Max;
    tone("tone-lines", "Line Tone", "Screentones", "lines", Grain::Multiply, 0.8f, 0.96f).buildup = Buildup::Max;
    tone("tone-hatch", "Hatch Tone", "Screentones", "hatch", Grain::Multiply, 0.8f, 0.96f).buildup = Buildup::Max;
    tone("tone-crosshatch", "Crosshatch Tone", "Screentones", "crosshatch", Grain::Multiply, 0.8f, 0.96f).buildup = Buildup::Max;
    tone("tone-waves", "Wave Tone", "Screentones", "waves", Grain::Multiply, 0.9f, 0.96f).buildup = Buildup::Max;

    // ------------------------------------------------------------- Elements
    {
        BrushPreset& b = brush("grass", "Grass", "Elements");
        b.tip = QStringLiteral("grass"); b.angleJitter = 0.05f;
        b.spacing = 0.2f; b.scatter = 0.2f; range(b, 70, 10, 500);
        b.sizeJitter = 0.45f; b.flowJitter = 0.1f; b.hueJitter = 0.04f; b.satJitter = 0.1f; b.valJitter = 0.16f; b.pressureSize = 0.4f;
    }
    {
        BrushPreset& b = brush("fur", "Fur", "Elements");
        b.tip = QStringLiteral("fur"); b.angleSource = Angle::Direction; b.angleJitter = 0.05f;
        b.spacing = 0.1f; b.scatter = 0.08f; range(b, 60, 8, 400);
        b.flow = 0.8f; b.sizeJitter = 0.25f; b.valJitter = 0.1f; b.pressureSize = 0.3f;
    }
    {
        BrushPreset& b = brush("clouds", "Clouds", "Elements");
        b.tip = QStringLiteral("cloud"); b.angleJitter = 1.0f;
        b.spacing = 0.25f; b.scatter = 0.25f; range(b, 180, 20, 1000);
        b.flow = 0.3f; b.pressureFlow = 0.7f; b.sizeJitter = 0.35f;
    }
    {
        BrushPreset& b = brush("smoke", "Smoke", "Elements");
        b.tip = QStringLiteral("smoke"); b.angleJitter = 1.0f;
        b.spacing = 0.18f; b.scatter = 0.1f; range(b, 200, 20, 1000);
        b.flow = 0.22f; b.pressureFlow = 0.8f; b.sizeJitter = 0.3f;
    }
    {
        BrushPreset& b = brush("snow", "Snow", "Elements");
        b.hardness = 0.4f; b.spacing = 0.7f; b.scatter = 2.0f; b.count = 2; range(b, 14, 2, 120);
        b.sizeJitter = 0.8f; b.flowJitter = 0.5f;
    }
    {
        BrushPreset& b = brush("snowflakes", "Snowflakes", "Elements");
        b.tip = QStringLiteral("snow"); b.angleJitter = 1.0f;
        b.spacing = 1.1f; b.scatter = 1.4f; range(b, 46, 8, 300);
        b.sizeJitter = 0.7f; b.flowJitter = 0.3f;
    }
    {
        BrushPreset& b = brush("rain", "Rain", "Elements");
        b.tip = QStringLiteral("drop"); b.angle = 14;
        b.spacing = 0.5f; b.scatter = 1.8f; b.count = 3; range(b, 60, 10, 400);
        b.sizeJitter = 0.5f; b.flowJitter = 0.4f;
    }
    {
        BrushPreset& b = brush("starfield", "Starfield", "Elements");
        b.hardness = 0.85f; b.spacing = 0.8f; b.scatter = 2.0f; b.count = 3; range(b, 7, 1, 60);
        b.sizeJitter = 0.9f; b.flowJitter = 0.6f;
    }
    {
        BrushPreset& b = brush("blossom", "Blossom", "Elements");
        b.tip = QStringLiteral("flower"); b.angleJitter = 1.0f;
        b.spacing = 0.6f; b.scatter = 0.9f; range(b, 44, 6, 300);
        b.sizeJitter = 0.5f; b.flowJitter = 0.15f; b.hueJitter = 0.04f; b.satJitter = 0.12f; b.valJitter = 0.1f;
    }

    // ------------------------------------------------------------ Luminance
    {
        // A line with a soft halo, for light drawn onto a dark ground.
        BrushPreset& b = brush("light-pen", "Light Pen", "Luminance");
        b.blend = BlendMode::Screen;
        b.hardness = 0.3f; b.spacing = 0.03f; range(b, 14, 2, 160);
        b.flow = 0.9f; b.pressureSize = 0.85f; b.pressureGamma = 1.2f; b.streamline = 0.4f;
        b.taper = 0.5f; b.taperEnd = 0.8f;
    }
    {
        BrushPreset& b = brush("glow", "Glow", "Luminance");
        b.blend = BlendMode::Add;
        b.tip = QStringLiteral("glow");
        b.spacing = 0.05f; range(b, 120, 10, 900);
        b.flow = 0.12f; b.pressureFlow = 0.9f; b.streamline = 0.1f;
    }
    {
        BrushPreset& b = brush("sparkle", "Sparkle", "Luminance");
        b.blend = BlendMode::Add;
        b.tip = QStringLiteral("star"); b.angleJitter = 0.06f;
        b.spacing = 0.9f; b.scatter = 1.2f; range(b, 60, 8, 400);
        b.sizeJitter = 0.8f; b.flowJitter = 0.3f;
    }
    {
        BrushPreset& b = brush("bokeh", "Bokeh", "Luminance");
        b.blend = BlendMode::Screen;
        b.tip = QStringLiteral("bokeh");
        b.spacing = 0.9f; b.scatter = 1.5f; range(b, 90, 12, 600);
        b.flow = 0.45f; b.sizeJitter = 0.6f; b.flowJitter = 0.6f; b.hueJitter = 0.05f;
    }
    {
        BrushPreset& b = brush("glimmer", "Glimmer", "Luminance");
        b.blend = BlendMode::Add;
        b.hardness = 0.6f; b.spacing = 0.3f; b.scatter = 1.2f; b.count = 4; range(b, 6, 1, 60);
        b.sizeJitter = 0.8f; b.flowJitter = 0.7f;
    }
    {
        BrushPreset& b = brush("nebula", "Nebula", "Luminance");
        b.blend = BlendMode::Screen;
        b.tip = QStringLiteral("smoke"); b.angleJitter = 1.0f;
        b.spacing = 0.15f; b.scatter = 0.2f; range(b, 260, 30, 1200);
        b.flow = 0.16f; b.pressureFlow = 0.8f; b.sizeJitter = 0.3f; b.hueJitter = 0.12f; b.satJitter = 0.15f;
    }

    // ------------------------------------------------------------- Abstract
    {
        BrushPreset& b = brush("bubbles", "Bubbles", "Abstract");
        b.tip = QStringLiteral("bubble");
        b.spacing = 0.7f; b.scatter = 1.3f; range(b, 60, 8, 400);
        b.sizeJitter = 0.7f; b.flowJitter = 0.3f;
    }
    {
        BrushPreset& b = brush("squares", "Squares", "Abstract");
        b.tip = QStringLiteral("square");
        b.spacing = 0.6f; b.scatter = 1.2f; b.count = 2; range(b, 30, 4, 300);
        b.sizeJitter = 0.6f; b.flowJitter = 0.3f; b.hueJitter = 0.08f; b.valJitter = 0.1f;
    }
    {
        BrushPreset& b = brush("pixel-pen", "Pixel Pen", "Abstract");
        b.tip = QStringLiteral("square");
        b.spacing = 0.25f; range(b, 8, 1, 100);
        b.streamline = 0.0f;
    }
    {
        BrushPreset& b = brush("blots", "Blots", "Abstract");
        b.tip = QStringLiteral("ink_blot"); b.angleJitter = 1.0f;
        b.spacing = 0.5f; b.scatter = 0.6f; range(b, 70, 8, 500);
        b.sizeJitter = 0.6f;
    }
    {
        BrushPreset& b = brush("rainbow-beads", "Rainbow Beads", "Abstract");
        b.hardness = 0.9f; b.spacing = 0.55f; range(b, 24, 3, 200);
        b.hueJitter = 0.5f; b.pressureSize = 0.5f;
    }
    {
        BrushPreset& b = brush("hatch-scatter", "Hatch Scatter", "Abstract");
        b.tip = QStringLiteral("hatch"); b.angleJitter = 1.0f;
        b.spacing = 0.5f; b.scatter = 0.5f; range(b, 80, 10, 500);
        b.flow = 0.8f; b.sizeJitter = 0.3f;
    }
    return out;
}

BrushLibrary::BrushLibrary()
{
    // ------------------------------------------------------------ Sketching
    {
        BrushPreset b = make("pencil-6b", "6B Pencil", "Sketching");
        b.hardness = 0.55f;
        b.spacing = 0.07f;
        b.size = 9; b.minSize = 1; b.maxSize = 80;
        b.flow = 0.3f;
        b.pressureFlow = 0.97f; b.pressureSize = 0.3f; b.pressureGamma = 1.6f;
        b.tiltSize = 2.2f; b.tiltFlow = 0.45f;
        b.streamline = 0.05f;
        setGrain(b, "fine", Grain::Height, 0.75f, 0.8f);
        add(b);
    }
    {
        BrushPreset b = make("pencil-hb", "HB Pencil", "Sketching");
        b.hardness = 0.7f;
        b.spacing = 0.07f;
        b.size = 4.5f; b.minSize = 1; b.maxSize = 40;
        b.flow = 0.2f;
        b.pressureFlow = 0.97f; b.pressureSize = 0.2f; b.pressureGamma = 1.6f;
        b.tiltSize = 1.6f; b.tiltFlow = 0.4f;
        b.streamline = 0.05f;
        setGrain(b, "fine", Grain::Height, 0.85f, 0.6f);
        add(b);
    }
    {
        BrushPreset b = make("pencil-technical", "Technical Pencil", "Sketching");
        b.hardness = 0.85f;
        b.spacing = 0.06f;
        b.size = 2.5f; b.minSize = 0.5f; b.maxSize = 20;
        b.flow = 0.5f;
        b.pressureFlow = 0.6f; b.pressureSize = 0.25f;
        b.streamline = 0.2f;
        setGrain(b, "fine", Grain::Height, 0.3f, 0.5f);
        add(b);
    }
    {
        // A biro: line weight barely changes, the tone does. Hatching builds up.
        BrushPreset b = make("ballpoint", "Ballpoint", "Sketching");
        b.hardness = 0.88f;
        b.spacing = 0.06f;
        b.size = 3.5f; b.minSize = 0.5f; b.maxSize = 24;
        b.flow = 0.5f;
        b.pressureFlow = 0.92f; b.pressureSize = 0.22f; b.pressureGamma = 1.35f;
        b.streamline = 0.12f;
        setGrain(b, "fine", Grain::Height, 0.3f, 0.45f);
        add(b);
    }
    {
        BrushPreset b = make("charcoal-stick", "Charcoal Stick", "Sketching");
        b.tip = QStringLiteral("charcoal");
        b.angleSource = Angle::Direction;
        b.angleJitter = 0.02f;
        b.spacing = 0.06f;
        b.size = 34; b.minSize = 4; b.maxSize = 300;
        b.flow = 0.45f;
        b.pressureFlow = 0.85f; b.pressureSize = 0.15f;
        b.sizeJitter = 0.08f;
        b.tiltSize = 0.8f;
        b.streamline = 0.05f;
        setGrain(b, "charcoal", Grain::Height, 0.85f, 0.42f);
        add(b);
    }
    {
        BrushPreset b = make("chalk", "Chalk", "Sketching");
        b.tip = QStringLiteral("chalk");
        b.angleJitter = 0.04f;
        b.spacing = 0.08f;
        b.size = 30; b.minSize = 4; b.maxSize = 300;
        b.flow = 0.5f;
        b.pressureFlow = 0.8f; b.pressureSize = 0.1f;
        b.streamline = 0.05f;
        setGrain(b, "paper", Grain::Height, 0.7f, 0.45f);
        add(b);
    }

    // --------------------------------------------------------------- Inking
    {
        BrushPreset b = make("studio-pen", "Studio Pen", "Inking");
        b.hardness = 0.97f;
        b.spacing = 0.04f;
        b.size = 10; b.minSize = 1; b.maxSize = 120;
        b.pressureSize = 0.9f; b.pressureGamma = 0.8f;
        b.streamline = 0.45f;
        b.taper = 1.5f;
        add(b);
    }
    {
        // The smooth, heavily steadied liner: swells generously with pressure
        // and trails off to a hair.
        BrushPreset b = make("syrup", "Syrup", "Inking");
        b.hardness = 0.9f;
        b.spacing = 0.03f;
        b.size = 16; b.minSize = 1; b.maxSize = 160;
        b.pressureSize = 1.0f; b.pressureGamma = 1.15f;
        b.speedSize = 0.12f;
        b.streamline = 0.62f;
        b.taper = 2.5f;
        add(b);
    }
    {
        BrushPreset b = make("technical-pen", "Technical Pen", "Inking");
        b.hardness = 0.98f;
        b.spacing = 0.04f;
        b.size = 5; b.minSize = 0.5f; b.maxSize = 60;
        b.pressureSize = 0.12f;
        b.streamline = 0.35f;
        add(b);
    }
    {
        BrushPreset b = make("brush-pen", "Brush Pen", "Inking");
        b.hardness = 0.94f;
        b.spacing = 0.035f;
        b.size = 26; b.minSize = 2; b.maxSize = 200;
        b.pressureSize = 1.0f; b.pressureGamma = 1.5f;
        b.speedSize = 0.25f;
        b.streamline = 0.3f;
        add(b);
    }
    {
        BrushPreset b = make("dry-ink", "Dry Ink", "Inking");
        b.tip = QStringLiteral("bristle");
        b.angleSource = Angle::Direction;
        b.spacing = 0.05f;
        b.size = 18; b.minSize = 2; b.maxSize = 160;
        b.flow = 0.8f;
        b.pressureSize = 0.7f; b.pressureFlow = 0.45f;
        b.streamline = 0.3f;
        setGrain(b, "paper", Grain::Height, 0.6f, 0.5f);
        add(b);
    }
    {
        // Ink on rough paper: a solid core with a ragged, feathered edge.
        BrushPreset b = make("ink-bleed", "Ink Bleed", "Inking");
        b.hardness = 0.62f;
        b.spacing = 0.04f;
        b.size = 22; b.minSize = 2; b.maxSize = 200;
        b.buildup = Buildup::Max;   // keeps the soft rim soft, so the grain has something to bite
        b.pressureSize = 0.85f; b.pressureGamma = 0.9f;
        b.streamline = 0.3f;
        b.taper = 1.0f;
        setGrain(b, "paper", Grain::Edge, 1.0f, 0.22f);
        add(b);
    }
    {
        // A loaded ink brush: bristle streaks, wide swings in width, dry at speed.
        BrushPreset b = make("sumi", "Sumi Brush", "Inking");
        b.tip = QStringLiteral("bristle");
        b.angleSource = Angle::Direction;
        b.spacing = 0.03f;
        b.size = 44; b.minSize = 4; b.maxSize = 400;
        b.flow = 0.9f;
        b.pressureSize = 1.0f; b.pressureGamma = 1.25f; b.pressureFlow = 0.25f;
        b.speedSize = 0.3f;
        b.streamline = 0.25f;
        b.taper = 0.8f;
        setGrain(b, "paper", Grain::Edge, 0.85f, 0.6f);
        add(b);
    }
    {
        BrushPreset b = make("marker", "Marker", "Inking");
        b.hardness = 0.9f;
        b.roundness = 0.4f;
        b.angle = 50;
        b.spacing = 0.04f;
        b.size = 32; b.minSize = 4; b.maxSize = 240;
        b.opacity = 0.65f;
        b.buildup = Buildup::Max;
        b.pressureSize = 0.15f;
        b.streamline = 0.2f;
        add(b);
    }
    {
        BrushPreset b = make("calligraphy", "Calligraphy", "Calligraphy");
        b.hardness = 0.98f;
        b.roundness = 0.2f;
        b.angle = 130;
        b.spacing = 0.025f;
        b.size = 28; b.minSize = 3; b.maxSize = 200;
        b.pressureSize = 0.35f;
        b.streamline = 0.55f;
        add(b);
    }
    {
        BrushPreset b = make("monoline", "Monoline", "Calligraphy");
        b.hardness = 0.97f;
        b.spacing = 0.04f;
        b.size = 8; b.minSize = 1; b.maxSize = 150;
        b.streamline = 0.8f;
        add(b);
    }

    // ------------------------------------------------------------- Painting
    {
        BrushPreset b = make("round-brush", "Round Brush", "Painting");
        b.hardness = 0.82f;
        b.spacing = 0.05f;
        b.size = 40; b.minSize = 2; b.maxSize = 500;
        b.flow = 0.9f;
        b.pressureFlow = 0.75f; b.pressureSize = 0.35f;
        b.streamline = 0.1f;
        add(b);
    }
    {
        BrushPreset b = make("flat-brush", "Flat Brush", "Painting");
        b.tip = QStringLiteral("flat_bristle");
        b.angleSource = Angle::Direction;
        b.spacing = 0.035f;
        b.size = 60; b.minSize = 6; b.maxSize = 500;
        b.flow = 0.7f;
        b.pressureFlow = 0.7f; b.pressureSize = 0.15f;
        b.valJitter = 0.02f;
        b.streamline = 0.1f;
        add(b);
    }
    {
        BrushPreset b = make("oil-paint", "Oil Paint", "Painting");
        b.tip = QStringLiteral("bristle");
        b.angleSource = Angle::Direction;
        b.spacing = 0.03f;
        b.size = 56; b.minSize = 6; b.maxSize = 500;
        b.flow = 0.8f;
        b.pressureFlow = 0.6f; b.pressureSize = 0.25f;
        b.valJitter = 0.035f; b.hueJitter = 0.006f;
        b.streamline = 0.1f;
        setGrain(b, "canvas", Grain::Multiply, 0.14f, 0.7f);
        add(b);
    }
    {
        BrushPreset b = make("gouache", "Gouache", "Painting");
        b.hardness = 0.7f;
        b.spacing = 0.05f;
        b.size = 46; b.minSize = 3; b.maxSize = 500;
        b.flow = 0.95f;
        b.pressureSize = 0.45f; b.pressureFlow = 0.35f;
        b.streamline = 0.1f;
        setGrain(b, "fine", Grain::Multiply, 0.14f, 0.8f);
        add(b);
    }
    {
        BrushPreset b = make("dry-brush", "Dry Brush", "Painting");
        b.tip = QStringLiteral("dry");
        b.angleSource = Angle::Direction;
        b.spacing = 0.04f;
        b.size = 64; b.minSize = 6; b.maxSize = 500;
        b.flow = 0.55f;
        b.pressureFlow = 0.8f; b.pressureSize = 0.2f;
        b.streamline = 0.1f;
        setGrain(b, "fine", Grain::Height, 0.45f, 1.0f);
        add(b);
    }
    {
        // A round watercolour brush: an even wash within one stroke, darker
        // where pigment pools at the rim, and deeper where strokes overlap.
        BrushPreset b = make("watercolor-round", "Watercolour Round", "Watercolour");
        b.hardness = 0.78f;
        b.spacing = 0.05f;
        b.size = 70; b.minSize = 4; b.maxSize = 600;
        b.opacity = 0.85f;
        b.buildup = Buildup::Max;
        b.wetEdges = 0.65f;
        b.pressureSize = 0.65f; b.pressureFlow = 0.2f; b.pressureGamma = 0.9f;
        b.streamline = 0.15f;
        b.taper = 0.6f;
        setGrain(b, "fine", Grain::Multiply, 0.22f, 1.5f);
        add(b);
    }
    {
        BrushPreset b = make("watercolor", "Watercolour Wash", "Watercolour");
        b.tip = QStringLiteral("sponge");
        b.angleJitter = 1.0f;
        b.spacing = 0.12f;
        b.scatter = 0.06f;
        b.size = 110; b.minSize = 10; b.maxSize = 800;
        b.opacity = 0.55f;
        b.flow = 0.2f;
        b.pressureFlow = 0.7f; b.pressureSize = 0.25f;
        b.sizeJitter = 0.2f;
        b.streamline = 0.1f;
        setGrain(b, "blotch", Grain::Multiply, 0.45f, 2.0f);
        add(b);
    }

    // ---------------------------------------------------------- Airbrushing
    {
        BrushPreset b = make("airbrush-soft", "Soft Airbrush", "Airbrushing");
        b.hardness = 0.0f;
        b.spacing = 0.06f;
        b.size = 140; b.minSize = 4; b.maxSize = 1200;
        b.flow = 0.07f;
        b.pressureFlow = 0.95f;
        b.streamline = 0.1f;
        add(b);
    }
    {
        BrushPreset b = make("airbrush-medium", "Medium Airbrush", "Airbrushing");
        b.hardness = 0.4f;
        b.spacing = 0.06f;
        b.size = 90; b.minSize = 4; b.maxSize = 1000;
        b.flow = 0.14f;
        b.pressureFlow = 0.95f;
        b.streamline = 0.1f;
        add(b);
    }
    {
        BrushPreset b = make("airbrush-hard", "Hard Airbrush", "Airbrushing");
        b.hardness = 0.8f;
        b.spacing = 0.05f;
        b.size = 60; b.minSize = 2; b.maxSize = 800;
        b.flow = 0.3f;
        b.pressureFlow = 0.9f; b.pressureSize = 0.2f;
        b.streamline = 0.1f;
        add(b);
    }

    // ------------------------------------------------------------- Textures
    {
        BrushPreset b = make("spray-paint", "Spray Paint", "Spray Paint");
        b.tip = QStringLiteral("splatter");
        b.angleJitter = 1.0f;
        b.spacing = 0.18f;
        b.scatter = 0.35f;
        b.size = 120; b.minSize = 10; b.maxSize = 900;
        b.flow = 0.45f;
        b.pressureFlow = 0.7f;
        b.sizeJitter = 0.45f;
        add(b);
    }
    {
        BrushPreset b = make("sponge", "Sponge", "Textures");
        b.tip = QStringLiteral("sponge");
        b.angleJitter = 1.0f;
        b.spacing = 0.22f;
        b.scatter = 0.2f;
        b.size = 130; b.minSize = 10; b.maxSize = 900;
        b.flow = 0.5f;
        b.pressureFlow = 0.8f;
        b.sizeJitter = 0.25f;
        setGrain(b, "charcoal", Grain::Multiply, 0.5f, 1.0f);
        add(b);
    }
    {
        BrushPreset b = make("grit", "Grit", "Textures");
        b.hardness = 0.3f;
        b.spacing = 0.1f;
        b.size = 150; b.minSize = 10; b.maxSize = 1000;
        b.flow = 0.5f;
        b.pressureFlow = 0.9f;
        setGrain(b, "charcoal", Grain::Height, 0.9f, 0.6f);
        add(b);
    }
    {
        // Leaves tumbling off the line, each a slightly different green.
        BrushPreset b = make("foliage", "Foliage", "Elements");
        b.tip = QStringLiteral("leaf");
        b.angleJitter = 1.0f;
        b.spacing = 0.42f;
        b.scatter = 1.1f;
        b.count = 2;
        b.size = 46; b.minSize = 6; b.maxSize = 400;
        b.sizeJitter = 0.55f;
        b.flowJitter = 0.15f;
        b.hueJitter = 0.07f; b.satJitter = 0.14f; b.valJitter = 0.2f;
        b.pressureSize = 0.5f;
        add(b);
    }
    {
        // Comic screentone: paint an area and it fills with an even dot screen.
        BrushPreset b = make("screentone", "Screentone", "Screentones");
        b.hardness = 0.96f;
        b.spacing = 0.05f;
        b.size = 110; b.minSize = 8; b.maxSize = 900;
        b.buildup = Buildup::Max;
        b.streamline = 0.15f;
        setGrain(b, "halftone", Grain::Multiply, 1.0f, 0.7f);
        add(b);
    }
    {
        BrushPreset b = make("confetti", "Confetti", "Abstract");
        b.hardness = 0.95f;
        b.spacing = 0.35f;
        b.scatter = 1.6f;
        b.count = 2;
        b.size = 22; b.minSize = 3; b.maxSize = 200;
        b.sizeJitter = 0.75f;
        b.flowJitter = 0.3f;
        b.hueJitter = 0.18f; b.satJitter = 0.1f; b.valJitter = 0.12f;
        b.pressureSize = 0.4f;
        add(b);
    }

    for (BrushPreset& b : moreBrushes())
        add(std::move(b));
    // Set by set, each in the order its brushes were written down.
    m_presets.sort([](const BrushPreset& a, const BrushPreset& b) { return setRank(a.category) < setRank(b.category); });

    m_defaults.assign(m_presets.cbegin(), m_presets.cend());
    rebuildCategories();
}
