#pragma once

#include <QString>
#include <QVector3D>
#include <QVector>
#include <vector>

class Model3D;

// Figures to pose: the jointed mannequin, the people that come with Impasto,
// and the ones kept from the artist's own rigged models. Each is a GLB with,
// beside it, a .json of the same name holding its name and its joints.
namespace figures {

struct Figure {
    QString name;
    QString path;   // the model; ":/figures/..." for the ones built in
    bool builtIn = false;
};

QVector<Figure> list();   // built in first, then the artist's own, each by name
QString ownFolder();
bool isFigure(const QString& modelPath);
// The joints and name kept beside a figure's model.
bool rigFor(const QString& modelPath, std::vector<QVector3D>* joints, QString* name);
// Keeps a model rigged in Impasto among the figures. Returns where.
QString keep(const Model3D& rigged, const QString& name, QString* error);

} // namespace figures
