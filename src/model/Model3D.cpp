#include "model/Model3D.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

bool fail(QString* error, const QString& text)
{
    if (error)
        *error = text;
    return false;
}

// glTF keeps matrices a column at a time.
QMatrix4x4 fromColumnMajor(const float* a)
{
    return QMatrix4x4(a[0], a[4], a[8], a[12], a[1], a[5], a[9], a[13], a[2], a[6], a[10], a[14], a[3], a[7], a[11], a[15]);
}

QVector3D unitNormal(const QVector3D& a, const QVector3D& b, const QVector3D& c)
{
    return QVector3D::crossProduct(b - a, c - a);   // area-weighted, normalised later
}

// Smooth normals from the triangles, where a file brings none.
void computeNormals(ModelMesh& m)
{
    m.normals.assign(m.positions.size(), QVector3D());
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const quint32 a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
        if (a >= m.positions.size() || b >= m.positions.size() || c >= m.positions.size())
            continue;
        const QVector3D n = unitNormal(m.positions[a], m.positions[b], m.positions[c]);
        m.normals[a] += n;
        m.normals[b] += n;
        m.normals[c] += n;
    }
    for (QVector3D& n : m.normals)
        n = n.lengthSquared() > 0 ? n.normalized() : QVector3D(0, 1, 0);
}

// ------------------------------------------------------------------- OBJ

bool loadObj(const QByteArray& data, Model3D* out, QString* error)
{
    std::vector<QVector3D> v, vn;
    std::vector<QVector2D> vt;
    ModelMesh mesh;
    QHash<QByteArray, quint32> seen;   // "v/vt/vn" -> vertex
    bool anyNormals = false;
    const QList<QByteArray> lines = data.split('\n');
    for (QByteArray line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QList<QByteArray> t = line.simplified().split(' ');
        if (t[0] == "v" && t.size() >= 4) {
            v.emplace_back(t[1].toFloat(), t[2].toFloat(), t[3].toFloat());
        } else if (t[0] == "vn" && t.size() >= 4) {
            vn.emplace_back(t[1].toFloat(), t[2].toFloat(), t[3].toFloat());
        } else if (t[0] == "vt" && t.size() >= 3) {
            vt.emplace_back(t[1].toFloat(), 1.0f - t[2].toFloat());
        } else if (t[0] == "f" && t.size() >= 4) {
            std::vector<quint32> corners;
            for (int k = 1; k < t.size(); ++k) {
                const QByteArray key = t[k];
                auto found = seen.constFind(key);
                if (found != seen.constEnd()) {
                    corners.push_back(*found);
                    continue;
                }
                const QList<QByteArray> parts = key.split('/');
                auto index = [](const QByteArray& s, size_t count) -> long long {
                    if (s.isEmpty())
                        return -1;
                    const long long i = s.toLongLong();
                    return i < 0 ? (long long)count + i : i - 1;
                };
                const long long pi = index(parts.value(0), v.size());
                if (pi < 0 || pi >= (long long)v.size())
                    return fail(error, QObject::tr("The OBJ file refers to a point it does not have"));
                const long long ti = parts.size() > 1 ? index(parts[1], vt.size()) : -1;
                const long long ni = parts.size() > 2 ? index(parts[2], vn.size()) : -1;
                mesh.positions.push_back(v[size_t(pi)]);
                mesh.uvs.push_back(ti >= 0 && ti < (long long)vt.size() ? vt[size_t(ti)] : QVector2D());
                if (ni >= 0 && ni < (long long)vn.size()) {
                    mesh.normals.push_back(vn[size_t(ni)].normalized());
                    anyNormals = true;
                } else {
                    mesh.normals.push_back(QVector3D());
                }
                const quint32 at = quint32(mesh.positions.size() - 1);
                seen.insert(key, at);
                corners.push_back(at);
            }
            for (size_t k = 1; k + 1 < corners.size(); ++k) {
                mesh.indices.push_back(corners[0]);
                mesh.indices.push_back(corners[k]);
                mesh.indices.push_back(corners[k + 1]);
            }
        }
    }
    if (mesh.indices.empty())
        return fail(error, QObject::tr("The OBJ file has no faces"));
    if (!anyNormals)
        computeNormals(mesh);
    if (vt.empty())
        mesh.uvs.clear();
    out->nodes.push_back({});
    out->meshes.push_back(std::move(mesh));
    out->instances.push_back({0, 0, -1});
    return true;
}

// ------------------------------------------------------------------- STL

bool loadStl(const QByteArray& data, Model3D* out, QString* error)
{
    ModelMesh mesh;
    const bool binary = data.size() >= 84 && qsizetype(84) + qsizetype(qFromLittleEndian<quint32>(data.constData() + 80)) * 50 == data.size();
    auto triangle = [&](const QVector3D& a, const QVector3D& b, const QVector3D& c) {
        const QVector3D n = QVector3D::crossProduct(b - a, c - a).normalized();
        for (const QVector3D& p : {a, b, c}) {
            mesh.positions.push_back(p);
            mesh.normals.push_back(n);
            mesh.indices.push_back(quint32(mesh.positions.size() - 1));
        }
    };
    if (binary) {
        const quint32 count = qFromLittleEndian<quint32>(data.constData() + 80);
        for (quint32 i = 0; i < count; ++i) {
            const char* f = data.constData() + 84 + qsizetype(i) * 50 + 12;
            auto vertex = [&](int k) {
                float xyz[3];
                for (int c = 0; c < 3; ++c) {
                    const quint32 bits = qFromLittleEndian<quint32>(f + k * 12 + c * 4);
                    memcpy(&xyz[c], &bits, 4);
                }
                return QVector3D(xyz[0], xyz[1], xyz[2]);
            };
            triangle(vertex(0), vertex(1), vertex(2));
        }
    } else {
        std::vector<QVector3D> corners;
        for (const QByteArray& raw : data.split('\n')) {
            const QList<QByteArray> t = raw.simplified().split(' ');
            if (t.size() >= 4 && t[0] == "vertex") {
                corners.emplace_back(t[1].toFloat(), t[2].toFloat(), t[3].toFloat());
                if (corners.size() == 3) {
                    triangle(corners[0], corners[1], corners[2]);
                    corners.clear();
                }
            }
        }
    }
    if (mesh.indices.empty())
        return fail(error, QObject::tr("The STL file has no triangles"));
    // STL files are mostly made Z-up: stand it upright.
    for (QVector3D& p : mesh.positions)
        p = QVector3D(p.x(), p.z(), -p.y());
    for (QVector3D& n : mesh.normals)
        n = QVector3D(n.x(), n.z(), -n.y());
    out->nodes.push_back({});
    out->meshes.push_back(std::move(mesh));
    out->instances.push_back({0, 0, -1});
    return true;
}

// ------------------------------------------------------------------- glTF

struct Gltf {
    QJsonObject json;
    std::vector<QByteArray> buffers;
    QString dir;

    QByteArray bufferViewData(int view) const
    {
        const QJsonObject bv = json[QStringLiteral("bufferViews")].toArray()[view].toObject();
        const int buffer = bv[QStringLiteral("buffer")].toInt();
        if (buffer < 0 || buffer >= int(buffers.size()))
            return {};
        return buffers[size_t(buffer)].mid(bv[QStringLiteral("byteOffset")].toInt(), bv[QStringLiteral("byteLength")].toInt());
    }

    // An accessor read as floats (or ints), `comps` to an element.
    bool read(int accessor, std::vector<float>* floats, std::vector<quint32>* ints, int* compsOut, int* countOut) const
    {
        const QJsonArray accessors = json[QStringLiteral("accessors")].toArray();
        if (accessor < 0 || accessor >= accessors.size())
            return false;
        const QJsonObject a = accessors[accessor].toObject();
        const int type = a[QStringLiteral("componentType")].toInt();
        const int count = a[QStringLiteral("count")].toInt();
        const bool normalized = a[QStringLiteral("normalized")].toBool();
        const QString t = a[QStringLiteral("type")].toString();
        const int comps = t == QLatin1String("SCALAR") ? 1 : t == QLatin1String("VEC2") ? 2 : t == QLatin1String("VEC3") ? 3
                          : t == QLatin1String("VEC4") ? 4 : t == QLatin1String("MAT4") ? 16 : 0;
        const int size = (type == 5120 || type == 5121) ? 1 : (type == 5122 || type == 5123) ? 2 : 4;
        if (comps == 0 || count < 0)
            return false;
        *compsOut = comps;
        *countOut = count;
        if (floats)
            floats->assign(size_t(count) * comps, 0.0f);
        if (ints)
            ints->assign(size_t(count) * comps, 0);
        if (!a.contains(QStringLiteral("bufferView")))
            return true;   // all zeros
        const int viewIndex = a[QStringLiteral("bufferView")].toInt();
        const QJsonObject bv = json[QStringLiteral("bufferViews")].toArray()[viewIndex].toObject();
        const QByteArray view = bufferViewData(viewIndex);
        const int stride = bv[QStringLiteral("byteStride")].toInt(comps * size);
        const int offset = a[QStringLiteral("byteOffset")].toInt();
        for (int i = 0; i < count; ++i) {
            for (int c = 0; c < comps; ++c) {
                const qsizetype at = offset + qsizetype(i) * stride + c * size;
                if (at + size > view.size())
                    return false;
                const char* p = view.constData() + at;
                double value = 0.0;
                quint32 whole = 0;
                switch (type) {
                case 5120: value = qint8(*p); whole = quint32(qint8(*p)); if (normalized) value = std::max(value / 127.0, -1.0); break;
                case 5121: whole = uchar(*p); value = normalized ? whole / 255.0 : whole; break;
                case 5122: value = qFromLittleEndian<qint16>(p); whole = quint32(qint32(value)); if (normalized) value = std::max(value / 32767.0, -1.0); break;
                case 5123: whole = qFromLittleEndian<quint16>(p); value = normalized ? whole / 65535.0 : whole; break;
                case 5125: whole = qFromLittleEndian<quint32>(p); value = whole; break;
                case 5126: {
                    const quint32 bits = qFromLittleEndian<quint32>(p);
                    float f;
                    memcpy(&f, &bits, 4);
                    value = f;
                    whole = quint32(std::max(0.0f, f));
                    break;
                }
                default: return false;
                }
                if (floats)
                    (*floats)[size_t(i) * comps + size_t(c)] = float(value);
                if (ints)
                    (*ints)[size_t(i) * comps + size_t(c)] = whole;
            }
        }
        return true;
    }

    QImage image(int index) const
    {
        const QJsonObject img = json[QStringLiteral("images")].toArray()[index].toObject();
        if (img.contains(QStringLiteral("bufferView")))
            return QImage::fromData(bufferViewData(img[QStringLiteral("bufferView")].toInt()));
        const QString uri = img[QStringLiteral("uri")].toString();
        if (uri.startsWith(QLatin1String("data:"))) {
            const qsizetype comma = uri.indexOf(QLatin1Char(','));
            return QImage::fromData(QByteArray::fromBase64(uri.mid(comma + 1).toLatin1()));
        }
        return uri.isEmpty() ? QImage() : QImage(QDir(dir).filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
    }
};

bool loadGltf(const QByteArray& data, const QString& path, Model3D* out, QString* error)
{
    Gltf g;
    g.dir = QFileInfo(path).absolutePath();
    QByteArray jsonBytes;
    QByteArray bin;
    if (data.startsWith("glTF")) {
        // GLB: a header, the JSON chunk, then the binary chunk.
        if (data.size() < 20)
            return fail(error, QObject::tr("The GLB file is cut short"));
        qsizetype at = 12;
        while (at + 8 <= data.size()) {
            const quint32 length = qFromLittleEndian<quint32>(data.constData() + at);
            const quint32 type = qFromLittleEndian<quint32>(data.constData() + at + 4);
            if (type == 0x4E4F534A)
                jsonBytes = data.mid(at + 8, length);
            else if (type == 0x004E4942)
                bin = data.mid(at + 8, length);
            at += 8 + qsizetype(length);
        }
    } else {
        jsonBytes = data;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (!doc.isObject())
        return fail(error, QObject::tr("The glTF file could not be read: %1").arg(parseError.errorString()));
    g.json = doc.object();
    for (const QJsonValue& b : g.json[QStringLiteral("buffers")].toArray()) {
        const QString uri = b.toObject()[QStringLiteral("uri")].toString();
        if (uri.isEmpty()) {
            g.buffers.push_back(bin);
        } else if (uri.startsWith(QLatin1String("data:"))) {
            g.buffers.push_back(QByteArray::fromBase64(uri.mid(uri.indexOf(QLatin1Char(',')) + 1).toLatin1()));
        } else {
            QFile f(QDir(g.dir).filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
            if (!f.open(QIODevice::ReadOnly))
                return fail(error, QObject::tr("The glTF file needs %1, which is not beside it").arg(uri));
            g.buffers.push_back(f.readAll());
        }
    }

    // The nodes and who holds whom.
    const QJsonArray nodes = g.json[QStringLiteral("nodes")].toArray();
    out->nodes.resize(size_t(nodes.size()));
    for (int i = 0; i < nodes.size(); ++i) {
        const QJsonObject n = nodes[i].toObject();
        ModelNode& node = out->nodes[size_t(i)];
        node.name = n[QStringLiteral("name")].toString(QStringLiteral("Node %1").arg(i));
        if (n.contains(QStringLiteral("matrix"))) {
            const QJsonArray m = n[QStringLiteral("matrix")].toArray();
            float values[16];
            for (int k = 0; k < 16; ++k)
                values[k] = float(m[k].toDouble());
            const QMatrix4x4 t = fromColumnMajor(values);
            node.translation = t.column(3).toVector3D();
            const QVector3D c0 = t.column(0).toVector3D(), c1 = t.column(1).toVector3D(), c2 = t.column(2).toVector3D();
            node.scale = QVector3D(c0.length(), c1.length(), c2.length());
            QMatrix3x3 r;
            for (int row = 0; row < 3; ++row) {
                r(row, 0) = c0[row] / std::max(1e-9f, node.scale.x());
                r(row, 1) = c1[row] / std::max(1e-9f, node.scale.y());
                r(row, 2) = c2[row] / std::max(1e-9f, node.scale.z());
            }
            node.rotation = QQuaternion::fromRotationMatrix(r);
        } else {
            const QJsonArray t = n[QStringLiteral("translation")].toArray();
            const QJsonArray r = n[QStringLiteral("rotation")].toArray();
            const QJsonArray s = n[QStringLiteral("scale")].toArray();
            if (t.size() == 3)
                node.translation = QVector3D(float(t[0].toDouble()), float(t[1].toDouble()), float(t[2].toDouble()));
            if (r.size() == 4)
                node.rotation = QQuaternion(float(r[3].toDouble()), float(r[0].toDouble()), float(r[1].toDouble()), float(r[2].toDouble())).normalized();
            if (s.size() == 3)
                node.scale = QVector3D(float(s[0].toDouble()), float(s[1].toDouble()), float(s[2].toDouble()));
        }
        for (const QJsonValue& c : n[QStringLiteral("children")].toArray()) {
            if (c.toInt() >= 0 && c.toInt() < nodes.size())
                out->nodes[size_t(c.toInt())].parent = i;
        }
    }

    // The skins.
    for (const QJsonValue& sv : g.json[QStringLiteral("skins")].toArray()) {
        const QJsonObject s = sv.toObject();
        ModelSkin skin;
        for (const QJsonValue& j : s[QStringLiteral("joints")].toArray())
            skin.joints.push_back(j.toInt());
        std::vector<float> m;
        int comps = 0, count = 0;
        if (s.contains(QStringLiteral("inverseBindMatrices")) && g.read(s[QStringLiteral("inverseBindMatrices")].toInt(), &m, nullptr, &comps, &count) && comps == 16) {
            for (int k = 0; k < count; ++k)
                skin.inverseBind.push_back(fromColumnMajor(m.data() + size_t(k) * 16));
        }
        skin.inverseBind.resize(skin.joints.size());
        out->skins.push_back(std::move(skin));
    }

    // The meshes, a mesh of ours for every primitive, and where each one hangs.
    const QJsonArray meshes = g.json[QStringLiteral("meshes")].toArray();
    const QJsonArray materials = g.json[QStringLiteral("materials")].toArray();
    const QJsonArray textures = g.json[QStringLiteral("textures")].toArray();
    QHash<int, std::vector<int>> made;   // glTF mesh -> ours
    QHash<int, QImage> images;
    for (int ni = 0; ni < nodes.size(); ++ni) {
        const QJsonObject n = nodes[ni].toObject();
        if (!n.contains(QStringLiteral("mesh")))
            continue;
        const int meshIndex = n[QStringLiteral("mesh")].toInt();
        if (meshIndex < 0 || meshIndex >= meshes.size())
            continue;
        if (!made.contains(meshIndex)) {
            std::vector<int> ours;
            for (const QJsonValue& pv : meshes[meshIndex].toObject()[QStringLiteral("primitives")].toArray()) {
                const QJsonObject prim = pv.toObject();
                const int mode = prim[QStringLiteral("mode")].toInt(4);
                if (mode != 4 && mode != 5 && mode != 6)
                    continue;   // lines and points are no surface
                const QJsonObject attrs = prim[QStringLiteral("attributes")].toObject();
                ModelMesh mesh;
                std::vector<float> f;
                std::vector<quint32> ix;
                int comps = 0, count = 0;
                if (!g.read(attrs[QStringLiteral("POSITION")].toInt(-1), &f, nullptr, &comps, &count) || comps != 3)
                    continue;
                for (int k = 0; k < count; ++k)
                    mesh.positions.emplace_back(f[size_t(k) * 3], f[size_t(k) * 3 + 1], f[size_t(k) * 3 + 2]);
                if (attrs.contains(QStringLiteral("NORMAL")) && g.read(attrs[QStringLiteral("NORMAL")].toInt(), &f, nullptr, &comps, &count) && comps == 3 && count == int(mesh.positions.size())) {
                    for (int k = 0; k < count; ++k)
                        mesh.normals.emplace_back(f[size_t(k) * 3], f[size_t(k) * 3 + 1], f[size_t(k) * 3 + 2]);
                }
                if (attrs.contains(QStringLiteral("TEXCOORD_0")) && g.read(attrs[QStringLiteral("TEXCOORD_0")].toInt(), &f, nullptr, &comps, &count) && comps == 2) {
                    for (int k = 0; k < count; ++k)
                        mesh.uvs.emplace_back(f[size_t(k) * 2], f[size_t(k) * 2 + 1]);
                }
                if (attrs.contains(QStringLiteral("JOINTS_0")) && attrs.contains(QStringLiteral("WEIGHTS_0"))) {
                    std::vector<float> w;
                    int wc = 0, wn = 0;
                    if (g.read(attrs[QStringLiteral("JOINTS_0")].toInt(), nullptr, &ix, &comps, &count) && comps == 4
                        && g.read(attrs[QStringLiteral("WEIGHTS_0")].toInt(), &w, nullptr, &wc, &wn) && wc == 4 && wn == count) {
                        for (int k = 0; k < count; ++k) {
                            mesh.joints.push_back({int(ix[size_t(k) * 4]), int(ix[size_t(k) * 4 + 1]), int(ix[size_t(k) * 4 + 2]), int(ix[size_t(k) * 4 + 3])});
                            mesh.weights.push_back({w[size_t(k) * 4], w[size_t(k) * 4 + 1], w[size_t(k) * 4 + 2], w[size_t(k) * 4 + 3]});
                        }
                    }
                }
                std::vector<quint32> order;
                if (prim.contains(QStringLiteral("indices")) && g.read(prim[QStringLiteral("indices")].toInt(), nullptr, &ix, &comps, &count))
                    order = ix;
                else
                    for (quint32 k = 0; k < mesh.positions.size(); ++k)
                        order.push_back(k);
                if (mode == 4) {
                    mesh.indices = order;
                } else {
                    for (size_t k = 2; k < order.size(); ++k) {
                        if (mode == 5) {
                            const bool even = k % 2 == 0;
                            mesh.indices.insert(mesh.indices.end(), {order[k - 2], even ? order[k - 1] : order[k], even ? order[k] : order[k - 1]});
                        } else {
                            mesh.indices.insert(mesh.indices.end(), {order[0], order[k - 1], order[k]});
                        }
                    }
                }
                if (mesh.normals.size() != mesh.positions.size())
                    computeNormals(mesh);
                // Its colour, and a picture for it if there is one.
                const int material = prim[QStringLiteral("material")].toInt(-1);
                if (material >= 0 && material < materials.size()) {
                    const QJsonObject pbr = materials[material].toObject()[QStringLiteral("pbrMetallicRoughness")].toObject();
                    const QJsonArray factor = pbr[QStringLiteral("baseColorFactor")].toArray();
                    if (factor.size() >= 3) {
                        auto srgb = [](double linear) { return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055; };
                        mesh.color = QColor::fromRgbF(float(std::clamp(srgb(factor[0].toDouble()), 0.0, 1.0)), float(std::clamp(srgb(factor[1].toDouble()), 0.0, 1.0)),
                                                      float(std::clamp(srgb(factor[2].toDouble()), 0.0, 1.0)));
                    }
                    const int texture = pbr[QStringLiteral("baseColorTexture")].toObject()[QStringLiteral("index")].toInt(-1);
                    if (texture >= 0 && texture < textures.size()) {
                        const int source = textures[texture].toObject()[QStringLiteral("source")].toInt(-1);
                        if (source >= 0) {
                            if (!images.contains(source))
                                images.insert(source, g.image(source));
                            mesh.texture = images.value(source);
                        }
                    }
                }
                out->meshes.push_back(std::move(mesh));
                ours.push_back(int(out->meshes.size()) - 1);
            }
            made.insert(meshIndex, ours);
        }
        const int skin = n.contains(QStringLiteral("skin")) ? n[QStringLiteral("skin")].toInt() : -1;
        for (int m : made.value(meshIndex)) {
            const bool skinned = skin >= 0 && skin < int(out->skins.size()) && !out->meshes[size_t(m)].joints.empty();
            out->instances.push_back({m, ni, skinned ? skin : -1});
        }
    }
    if (out->instances.empty())
        return fail(error, QObject::tr("The glTF file has no surfaces to show"));
    return true;
}

// ------------------------------------------------------------------ figure

void addEllipsoid(ModelMesh& m, const QVector3D& centre, const QVector3D& radii, int rings = 14, int segments = 20)
{
    const quint32 base = quint32(m.positions.size());
    for (int r = 0; r <= rings; ++r) {
        const double phi = M_PI * double(r) / rings;
        for (int s = 0; s <= segments; ++s) {
            const double theta = 2.0 * M_PI * double(s) / segments;
            const QVector3D unit(float(std::sin(phi) * std::cos(theta)), float(std::cos(phi)), float(std::sin(phi) * std::sin(theta)));
            m.positions.push_back(centre + unit * radii);
            m.normals.push_back(QVector3D(unit.x() / radii.x(), unit.y() / radii.y(), unit.z() / radii.z()).normalized());
        }
    }
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            const quint32 a = base + quint32(r * (segments + 1) + s), b = a + quint32(segments + 1);
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
}

// A rod with rounded ends from `a` to `b`, thicker at `a` when ra > rb.
void addLimb(ModelMesh& m, const QVector3D& a, const QVector3D& b, float ra, float rb, int segments = 18)
{
    const QVector3D axis = (b - a).normalized();
    QVector3D side = QVector3D::crossProduct(axis, QVector3D(0, 0, 1));
    if (side.lengthSquared() < 1e-4f)
        side = QVector3D::crossProduct(axis, QVector3D(1, 0, 0));
    side.normalize();
    const QVector3D up = QVector3D::crossProduct(side, axis);
    const int rings = 8;
    const quint32 base = quint32(m.positions.size());
    // Down the length, with a half-sphere at each end.
    std::vector<std::pair<float, float>> profile;   // (along, radius) with along in -ra..len+rb
    const float len = (b - a).length();
    for (int i = 0; i <= rings; ++i) {
        const double t = M_PI / 2.0 * (1.0 - double(i) / rings);
        profile.emplace_back(float(-ra * std::sin(t)), float(ra * std::cos(t)));
    }
    // Rings along the length too, so that it bends smoothly and has points all the way down.
    const int along = std::max(1, int(std::ceil(len / 0.03f)));
    for (int i = 1; i < along; ++i) {
        const float t = float(i) / float(along);
        profile.emplace_back(len * t, ra + (rb - ra) * t);
    }
    for (int i = 1; i <= rings; ++i) {
        const double t = M_PI / 2.0 * double(i) / rings;
        profile.emplace_back(float(len + rb * std::sin(t)), float(rb * std::cos(t)));
    }
    for (size_t p = 0; p < profile.size(); ++p) {
        const float along = profile[p].first, radius = profile[p].second;
        const QVector3D centre = a + axis * std::clamp(along, 0.0f, len);
        const float cap = along < 0 ? along : (along > len ? along - len : 0.0f);
        for (int s = 0; s <= segments; ++s) {
            const double theta = 2.0 * M_PI * double(s) / segments;
            const QVector3D around = side * float(std::cos(theta)) + up * float(std::sin(theta));
            m.positions.push_back(centre + axis * cap + around * radius);
            m.normals.push_back((around * radius + axis * cap).normalized());
        }
    }
    const int rows = int(profile.size());
    for (int r = 0; r + 1 < rows; ++r) {
        for (int s = 0; s < segments; ++s) {
            const quint32 i0 = base + quint32(r * (segments + 1) + s), i1 = i0 + quint32(segments + 1);
            m.indices.insert(m.indices.end(), {i0, i1, i0 + 1, i0 + 1, i1, i1 + 1});
        }
    }
}

const char* const kFigureNodes[] = {"hips", "spine", "chest", "neck", "head",
                                    "upperarm.L", "forearm.L", "hand.L", "upperarm.R", "forearm.R", "hand.R",
                                    "thigh.L", "shin.L", "foot.L", "thigh.R", "shin.R", "foot.R"};

int figureNode(const Model3D& m, const char* name)
{
    for (size_t i = 0; i < m.nodes.size(); ++i) {
        if (m.nodes[i].name == QLatin1String(name))
            return int(i);
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------- loading

QStringList Model3D::fileFilters()
{
    return {QObject::tr("3D models (*.gltf *.glb *.obj *.stl)"), QObject::tr("All files (*)")};
}

bool Model3D::load(const QString& path, Model3D* out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, file.errorString());
    if (file.size() > qint64(1) << 30)
        return fail(error, QObject::tr("That model is too large to open"));
    const QByteArray data = file.readAll();
    Model3D m;
    m.path = QFileInfo(path).absoluteFilePath();
    m.name = QFileInfo(path).completeBaseName();
    const QString suffix = QFileInfo(path).suffix().toLower();
    bool ok = false;
    if (data.startsWith("glTF") || suffix == QLatin1String("gltf") || suffix == QLatin1String("glb"))
        ok = loadGltf(data, path, &m, error);
    else if (suffix == QLatin1String("stl"))
        ok = loadStl(data, &m, error);
    else if (suffix == QLatin1String("obj"))
        ok = loadObj(data, &m, error);
    else
        return fail(error, QObject::tr("Impasto opens 3D models as glTF, GLB, OBJ or STL"));
    if (!ok)
        return false;
    m.finish();
    *out = std::move(m);
    return true;
}

void Model3D::finish()
{
    // Something to take hold of: every joint of a skin that has a joint above
    // it, unless the bone between them is too short to aim at.
    handles.clear();
    if (!figure && !skins.empty()) {
        QVector3D lo, hi;
        bounds(&lo, &hi);
        const float size = (hi - lo).length();
        const std::vector<QMatrix4x4> world = worldTransforms();
        std::vector<bool> isJoint(nodes.size(), false);
        for (const ModelSkin& s : skins) {
            for (int j : s.joints) {
                if (j >= 0 && j < int(nodes.size()))
                    isJoint[size_t(j)] = true;
            }
        }
        for (size_t n = 0; n < nodes.size(); ++n) {
            const int p = nodes[n].parent;
            if (!isJoint[n] || p < 0 || !isJoint[size_t(p)])
                continue;
            if ((jointPosition(int(n), world) - jointPosition(p, world)).length() < size * 0.03f)
                continue;
            handles.push_back({int(n), QVector3D(), p, false});
        }
    }
    QVector3D lo, hi;
    if (bounds(&lo, &hi))
        m_floor = lo.y();
}

// ---------------------------------------------------------------- posing

std::vector<QMatrix4x4> Model3D::worldTransforms() const
{
    std::vector<QMatrix4x4> world(nodes.size());
    std::vector<int> state(nodes.size(), 0);   // 0 to do, 1 doing, 2 done
    std::function<void(int)> resolve = [&](int i) {
        if (state[size_t(i)] == 2)
            return;
        if (state[size_t(i)] == 1) {
            world[size_t(i)] = QMatrix4x4();   // a loop in the file: cut it here
            state[size_t(i)] = 2;
            return;
        }
        state[size_t(i)] = 1;
        const ModelNode& n = nodes[size_t(i)];
        QMatrix4x4 local;
        local.translate(n.translation + (n.parent < 0 ? n.offset : QVector3D()));
        local.rotate(n.rotation * n.pose);
        local.scale(n.scale);
        if (n.parent >= 0 && n.parent < int(nodes.size())) {
            resolve(n.parent);
            world[size_t(i)] = world[size_t(n.parent)] * local;
        } else {
            world[size_t(i)] = local;
        }
        state[size_t(i)] = 2;
    };
    for (int i = 0; i < int(nodes.size()); ++i)
        resolve(i);
    return world;
}

std::vector<Model3D::Part> Model3D::posed() const
{
    const std::vector<QMatrix4x4> world = worldTransforms();
    std::vector<Part> parts;
    for (const ModelInstance& inst : instances) {
        if (inst.mesh < 0 || inst.mesh >= int(meshes.size()))
            continue;
        const ModelMesh& mesh = meshes[size_t(inst.mesh)];
        Part part;
        part.indices = &mesh.indices;
        part.color = mesh.color;
        part.texture = mesh.texture.isNull() ? nullptr : &mesh.texture;
        part.uvs = mesh.uvs;
        part.positions.resize(mesh.positions.size());
        part.normals.resize(mesh.positions.size());
        if (inst.skin >= 0) {
            const ModelSkin& skin = skins[size_t(inst.skin)];
            std::vector<QMatrix4x4> joint(skin.joints.size());
            for (size_t j = 0; j < skin.joints.size(); ++j) {
                const int node = skin.joints[j];
                joint[j] = (node >= 0 && node < int(world.size()) ? world[size_t(node)] : QMatrix4x4()) * skin.inverseBind[j];
            }
            for (size_t v = 0; v < mesh.positions.size(); ++v) {
                QVector3D p, n;
                float total = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    const float w = mesh.weights[v][size_t(k)];
                    const int j = mesh.joints[v][size_t(k)];
                    if (w <= 0.0f || j < 0 || j >= int(joint.size()))
                        continue;
                    p += joint[size_t(j)].map(mesh.positions[v]) * w;
                    n += joint[size_t(j)].mapVector(mesh.normals[v]) * w;
                    total += w;
                }
                part.positions[v] = total > 0.0f ? p / total : mesh.positions[v];
                part.normals[v] = n.lengthSquared() > 0.0f ? n.normalized() : mesh.normals[v];
            }
        } else {
            const QMatrix4x4 m = inst.node >= 0 && inst.node < int(world.size()) ? world[size_t(inst.node)] : QMatrix4x4();
            const QMatrix3x3 nm = m.normalMatrix();
            for (size_t v = 0; v < mesh.positions.size(); ++v) {
                part.positions[v] = m.map(mesh.positions[v]);
                const QVector3D& n = mesh.normals[v];
                part.normals[v] = QVector3D(nm(0, 0) * n.x() + nm(0, 1) * n.y() + nm(0, 2) * n.z(),
                                            nm(1, 0) * n.x() + nm(1, 1) * n.y() + nm(1, 2) * n.z(),
                                            nm(2, 0) * n.x() + nm(2, 1) * n.y() + nm(2, 2) * n.z()).normalized();
            }
        }
        parts.push_back(std::move(part));
    }
    return parts;
}

bool Model3D::bounds(QVector3D* lo, QVector3D* hi) const
{
    const float big = std::numeric_limits<float>::max();
    QVector3D a(big, big, big), b(-big, -big, -big);
    bool any = false;
    for (const Part& p : posed()) {
        for (const QVector3D& v : p.positions) {
            a = QVector3D(std::min(a.x(), v.x()), std::min(a.y(), v.y()), std::min(a.z(), v.z()));
            b = QVector3D(std::max(b.x(), v.x()), std::max(b.y(), v.y()), std::max(b.z(), v.z()));
            any = true;
        }
    }
    *lo = any ? a : QVector3D();
    *hi = any ? b : QVector3D();
    return any;
}

QVector3D Model3D::jointPosition(int node, const std::vector<QMatrix4x4>& world) const
{
    return node >= 0 && node < int(world.size()) ? world[size_t(node)].map(QVector3D()) : QVector3D();
}

QVector3D Model3D::handlePosition(const ModelHandle& h, const std::vector<QMatrix4x4>& world) const
{
    return h.at >= 0 && h.at < int(world.size()) ? world[size_t(h.at)].map(h.tip) : QVector3D();
}

void Model3D::turnNode(int node, const QQuaternion& worldTurn)
{
    if (node < 0 || node >= int(nodes.size()))
        return;
    ModelNode& n = nodes[size_t(node)];
    // The parent's turn in the world, so that a turn given in the world can be put in the node's own terms.
    QQuaternion parentWorld;
    if (n.parent >= 0) {
        const QMatrix4x4 w = worldTransforms()[size_t(n.parent)];
        QMatrix3x3 r;
        for (int c = 0; c < 3; ++c) {
            const QVector3D col = w.column(c).toVector3D().normalized();
            for (int row = 0; row < 3; ++row)
                r(row, c) = col[row];
        }
        parentWorld = QQuaternion::fromRotationMatrix(r).normalized();
    }
    n.pose = (n.rotation.inverted() * parentWorld.inverted() * worldTurn * parentWorld * n.rotation * n.pose).normalized();
}

void Model3D::moveRoot(const QVector3D& worldDelta)
{
    for (ModelNode& n : nodes) {
        if (n.parent < 0)
            n.offset += worldDelta;
    }
}

void Model3D::resetPose()
{
    for (ModelNode& n : nodes) {
        n.pose = QQuaternion();
        n.offset = QVector3D();
    }
}

bool Model3D::posed_() const
{
    for (const ModelNode& n : nodes) {
        if (!qFuzzyIsNull(1.0f - std::abs(n.pose.scalar())) || !n.offset.isNull())
            return true;
    }
    return false;
}

QVariantList Model3D::poseState() const
{
    QVariantList state;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const ModelNode& n = nodes[i];
        if (n.pose == QQuaternion() && n.offset.isNull())
            continue;
        // One entry for each node, kept whole: << with a list would splice it in.
        state.append(QVariant(QVariantList{int(i), n.pose.scalar(), n.pose.x(), n.pose.y(), n.pose.z(), n.offset.x(), n.offset.y(), n.offset.z()}));
    }
    return state;
}

void Model3D::setPoseState(const QVariantList& state)
{
    resetPose();
    for (const QVariant& v : state) {
        const QVariantList e = v.toList();
        const int i = e.value(0).toInt();
        if (e.size() < 8 || i < 0 || i >= int(nodes.size()))
            continue;
        nodes[size_t(i)].pose = QQuaternion(e[1].toFloat(), e[2].toFloat(), e[3].toFloat(), e[4].toFloat()).normalized();
        nodes[size_t(i)].offset = QVector3D(e[5].toFloat(), e[6].toFloat(), e[7].toFloat());
    }
}

// ------------------------------------------------------------------ figure

Model3D Model3D::makeFigure()
{
    Model3D m;
    m.name = QObject::tr("Figure");
    m.figure = true;
    m.humanoid = true;
    struct Bone {
        const char* name;
        int parent;
        QVector3D at;
        float restZ = 0.0f;   // arms hang a little out from the body
    };
    const Bone bones[] = {
        {"hips", -1, {0.0f, 0.95f, 0.0f}},
        {"spine", 0, {0.0f, 0.10f, 0.0f}},
        {"chest", 1, {0.0f, 0.16f, 0.0f}},
        {"neck", 2, {0.0f, 0.22f, 0.0f}},
        {"head", 3, {0.0f, 0.08f, 0.0f}},
        {"upperarm.L", 2, {0.19f, 0.185f, 0.0f}, 9.0f},
        {"forearm.L", 5, {0.0f, -0.28f, 0.0f}},
        {"hand.L", 6, {0.0f, -0.26f, 0.0f}},
        {"upperarm.R", 2, {-0.19f, 0.185f, 0.0f}, -9.0f},
        {"forearm.R", 8, {0.0f, -0.28f, 0.0f}},
        {"hand.R", 9, {0.0f, -0.26f, 0.0f}},
        {"thigh.L", 0, {0.095f, -0.05f, 0.0f}},
        {"shin.L", 11, {0.0f, -0.44f, 0.0f}},
        {"foot.L", 12, {0.0f, -0.42f, 0.0f}},
        {"thigh.R", 0, {-0.095f, -0.05f, 0.0f}},
        {"shin.R", 14, {0.0f, -0.44f, 0.0f}},
        {"foot.R", 15, {0.0f, -0.42f, 0.0f}},
    };
    for (const Bone& b : bones) {
        ModelNode n;
        n.name = QLatin1String(b.name);
        n.parent = b.parent;
        n.translation = b.at;
        n.rotation = QQuaternion::fromAxisAndAngle(0, 0, 1, b.restZ);
        m.nodes.push_back(n);
    }
    // The body in rounded pieces, each fixed to its bone, like a wooden mannequin.
    auto part = [&](int node, const std::function<void(ModelMesh&)>& build) {
        ModelMesh mesh;
        build(mesh);
        m.meshes.push_back(std::move(mesh));
        m.instances.push_back({int(m.meshes.size()) - 1, node, -1});
    };
    const QVector3D o;
    part(0, [](ModelMesh& x) { addEllipsoid(x, {0.0f, 0.01f, 0.0f}, {0.165f, 0.11f, 0.105f}); });
    part(1, [](ModelMesh& x) { addEllipsoid(x, {0.0f, 0.08f, 0.0f}, {0.135f, 0.11f, 0.095f}); });
    part(2, [](ModelMesh& x) { addEllipsoid(x, {0.0f, 0.11f, 0.0f}, {0.185f, 0.15f, 0.115f}); });
    part(3, [&](ModelMesh& x) { addLimb(x, o, {0.0f, 0.08f, 0.0f}, 0.05f, 0.045f); });
    part(4, [](ModelMesh& x) {
        addEllipsoid(x, {0.0f, 0.11f, 0.012f}, {0.092f, 0.118f, 0.105f});
        addEllipsoid(x, {0.0f, 0.075f, 0.085f}, {0.025f, 0.03f, 0.03f}, 8, 12);   // a nose, to say which way it faces
    });
    for (int side : {0, 1}) {
        const int arm = side == 0 ? 5 : 8, leg = side == 0 ? 11 : 14;
        part(arm, [&](ModelMesh& x) {
            addEllipsoid(x, o, {0.058f, 0.058f, 0.058f}, 10, 14);
            addLimb(x, o, {0.0f, -0.28f, 0.0f}, 0.05f, 0.043f);
        });
        part(arm + 1, [&](ModelMesh& x) { addLimb(x, o, {0.0f, -0.26f, 0.0f}, 0.043f, 0.034f); });
        part(arm + 2, [](ModelMesh& x) { addEllipsoid(x, {0.0f, -0.08f, 0.012f}, {0.03f, 0.088f, 0.048f}, 10, 14); });
        part(leg, [&](ModelMesh& x) {
            addEllipsoid(x, o, {0.08f, 0.08f, 0.08f}, 10, 14);
            addLimb(x, o, {0.0f, -0.44f, 0.0f}, 0.075f, 0.055f);
        });
        part(leg + 1, [&](ModelMesh& x) { addLimb(x, o, {0.0f, -0.42f, 0.0f}, 0.055f, 0.042f); });
        part(leg + 2, [](ModelMesh& x) { addEllipsoid(x, {0.0f, -0.03f, 0.06f}, {0.048f, 0.035f, 0.115f}, 10, 14); });
    }
    for (ModelMesh& mesh : m.meshes)
        mesh.color = QColor(214, 196, 168);
    // Where to take hold of it.
    auto id = [&](const char* name) { return figureNode(m, name); };
    m.handles = {
        {id("hips"), {}, -1, true},
        {id("chest"), {}, id("spine"), false},
        {id("neck"), {}, id("chest"), false},
        {id("head"), {}, id("neck"), false},
        {id("head"), {0.0f, 0.24f, 0.0f}, id("head"), false},
    };
    for (const char* s : {"L", "R"}) {
        auto n = [&](const char* part) { return id(QStringLiteral("%1.%2").arg(QLatin1String(part), QLatin1String(s)).toLatin1().constData()); };
        m.handles.push_back({n("forearm"), {}, n("upperarm"), false});
        m.handles.push_back({n("hand"), {}, n("forearm"), false});
        m.handles.push_back({n("hand"), {0.0f, -0.17f, 0.0f}, n("hand"), false});
        m.handles.push_back({n("shin"), {}, n("thigh"), false});
        m.handles.push_back({n("foot"), {}, n("shin"), false});
        m.handles.push_back({n("foot"), {0.0f, -0.05f, 0.17f}, n("foot"), false});
    }
    m.m_floor = 0.0f;
    return m;
}

QStringList Model3D::figurePoses()
{
    return {QObject::tr("Stand"), QObject::tr("Walk"), QObject::tr("Run"), QObject::tr("Sit"), QObject::tr("Reach"), QObject::tr("Crouch"), QObject::tr("T-pose")};
}

void Model3D::applyFigurePose(const QString& pose)
{
    resetPose();
    const int which = figurePoses().indexOf(pose);
    // Turns about each joint in degrees: x swings forward and back (a leg forward is
    // negative, a back bent forward positive), z out to the side.
    struct Turn {
        const char* node;
        float x, y, z;
    };
    std::vector<Turn> turns;
    float drop = 0.0f;
    switch (which) {
    case 1:   // walk
        turns = {{"thigh.L", -24, 0, 0}, {"shin.L", 8, 0, 0}, {"foot.L", -6, 0, 0}, {"thigh.R", 18, 0, 0}, {"shin.R", 32, 0, 0}, {"foot.R", -14, 0, 0},
                 {"upperarm.L", 20, 0, 0}, {"forearm.L", -12, 0, 0}, {"upperarm.R", -22, 0, 0}, {"forearm.R", -28, 0, 0}, {"chest", 0, -7, 0}, {"hips", 0, 6, 0}};
        drop = -0.02f;
        break;
    case 2:   // run
        turns = {{"thigh.L", -60, 0, 0}, {"shin.L", 45, 0, 0}, {"thigh.R", 28, 0, 0}, {"shin.R", 88, 0, 0}, {"foot.R", 20, 0, 0},
                 {"upperarm.L", 48, 0, 0}, {"forearm.L", -85, 0, 0}, {"upperarm.R", -52, 0, 0}, {"forearm.R", -92, 0, 0}, {"spine", 14, 0, 0}, {"neck", -10, 0, 0}};
        drop = -0.08f;
        break;
    case 3:   // sit
        turns = {{"thigh.L", -88, 0, 4}, {"thigh.R", -88, 0, -4}, {"shin.L", 88, 0, 0}, {"shin.R", 88, 0, 0},
                 {"upperarm.L", -18, 0, 0}, {"forearm.L", -55, 0, 0}, {"upperarm.R", -18, 0, 0}, {"forearm.R", -55, 0, 0}, {"spine", 4, 0, 0}};
        drop = -0.45f;
        break;
    case 4:   // reach
        turns = {{"upperarm.R", 0, 0, -150}, {"forearm.R", -10, 0, 0}, {"upperarm.L", 0, 0, 22}, {"chest", 0, 0, 8}, {"spine", 0, 0, 5}, {"neck", 0, 0, -12},
                 {"thigh.R", 0, 0, -6}};
        break;
    case 5:   // crouch
        turns = {{"thigh.L", -112, 0, 6}, {"thigh.R", -112, 0, -6}, {"shin.L", 135, 0, 0}, {"shin.R", 135, 0, 0}, {"foot.L", -22, 0, 0}, {"foot.R", -22, 0, 0},
                 {"spine", 28, 0, 0}, {"chest", 10, 0, 0}, {"neck", -25, 0, 0}, {"upperarm.L", -45, 0, 0}, {"forearm.L", -60, 0, 0}, {"upperarm.R", -45, 0, 0}, {"forearm.R", -60, 0, 0}};
        drop = -0.48f;
        break;
    case 6:   // T-pose
        turns = {{"upperarm.L", 0, 0, 81}, {"upperarm.R", 0, 0, -81}};
        break;
    default:
        break;
    }
    for (const Turn& t : turns) {
        const int n = figureNode(*this, t.node);
        if (n >= 0)
            nodes[size_t(n)].pose = QQuaternion::fromEulerAngles(t.x, t.y, t.z);
    }
    if (drop != 0.0f && !nodes.empty()) {
        // The figure's hips: on a rigged model the root may be turned to face the other way.
        int hips = figureNode(*this, "hips");
        if (hips >= 0)
            nodes[size_t(hips)].offset = QVector3D(0.0f, drop * m_poseScale, 0.0f);
    }
}
