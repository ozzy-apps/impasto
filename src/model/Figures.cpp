#include "model/Figures.h"

#include "model/Model3D.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace figures {

namespace {

QString sidecar(const QString& modelPath)
{
    const QFileInfo info(modelPath);
    return info.path() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".json");
}

QVector<Figure> inFolder(const QString& folder, bool builtIn)
{
    QVector<Figure> out;
    for (const QFileInfo& info : QDir(folder).entryInfoList({QStringLiteral("*.glb")}, QDir::Files, QDir::Name)) {
        std::vector<QVector3D> joints;
        QString name;
        if (rigFor(info.filePath(), &joints, &name))
            out.push_back({name, info.filePath(), builtIn});
    }
    std::sort(out.begin(), out.end(), [](const Figure& a, const Figure& b) { return a.name.localeAwareCompare(b.name) < 0; });
    return out;
}

} // namespace

QString ownFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/figures");
}

QVector<Figure> list()
{
    return inFolder(QStringLiteral(":/figures"), true) + inFolder(ownFolder(), false);
}

bool isFigure(const QString& modelPath)
{
    return modelPath.startsWith(QLatin1String(":/figures/")) || QFileInfo(modelPath).absolutePath() == QDir(ownFolder()).absolutePath();
}

bool rigFor(const QString& modelPath, std::vector<QVector3D>* joints, QString* name)
{
    QFile file(sidecar(modelPath));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonArray rig = o[QStringLiteral("rig")].toArray();
    if (rig.size() != Model3D::rigJointNames().size() * 3)
        return false;
    if (joints) {
        joints->clear();
        for (int i = 0; i + 2 < rig.size(); i += 3)
            joints->emplace_back(float(rig[i].toDouble()), float(rig[i + 1].toDouble()), float(rig[i + 2].toDouble()));
    }
    if (name)
        *name = o[QStringLiteral("name")].toString(QFileInfo(modelPath).completeBaseName());
    return true;
}

QString keep(const Model3D& rigged, const QString& name, QString* error)
{
    if (rigged.rig.empty()) {
        if (error)
            *error = QObject::tr("Only a model rigged in Impasto can be kept as a figure");
        return {};
    }
    QDir().mkpath(ownFolder());
    // A file name from the name, one not yet taken.
    QString base = name.trimmed();
    for (QChar& c : base) {
        if (!c.isLetterOrNumber() && c != QLatin1Char(' ') && c != QLatin1Char('-'))
            c = QLatin1Char('_');
    }
    if (base.isEmpty())
        base = QStringLiteral("Figure");
    QString path = QDir(ownFolder()).filePath(base + QStringLiteral(".glb"));
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = QDir(ownFolder()).filePath(QStringLiteral("%1 %2.glb").arg(base).arg(n));
    if (!rigged.saveGlb(path, error))
        return {};
    QJsonArray joints;
    for (const QVector3D& j : rigged.rig)
        joints << j.x() << j.y() << j.z();
    QSaveFile json(sidecar(path));
    if (!json.open(QIODevice::WriteOnly)) {
        if (error)
            *error = json.errorString();
        QFile::remove(path);
        return {};
    }
    json.write(QJsonDocument(QJsonObject{{"name", name.trimmed().isEmpty() ? base : name.trimmed()}, {"rig", joints}}).toJson());
    if (!json.commit()) {
        if (error)
            *error = json.errorString();
        QFile::remove(path);
        return {};
    }
    return path;
}

} // namespace figures
