#pragma once

#include <QColor>
#include <QImage>
#include <QMatrix4x4>
#include <QQuaternion>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector2D>
#include <QVector3D>
#include <array>
#include <vector>

// A 3D model to draw from: its meshes, the joints they hang from, and how the
// artist has posed it. Loaded from OBJ, STL or glTF (.gltf / .glb, rigged ones
// included), or the built-in figure, a jointed mannequin. No graphics here:
// the view puts it on screen.
struct ModelMesh {
    std::vector<QVector3D> positions, normals;
    std::vector<QVector2D> uvs;                 // empty when it has none
    std::vector<quint32> indices;               // triangles
    std::vector<std::array<int, 4>> joints;     // skinned meshes: into the skin's joint list
    std::vector<std::array<float, 4>> weights;
    QColor color{214, 204, 190};
    QImage texture;                             // base colour, or none
};

struct ModelNode {
    QString name;
    int parent = -1;
    // At rest.
    QVector3D translation;
    QQuaternion rotation;
    QVector3D scale{1.0f, 1.0f, 1.0f};
    // What the artist has done to it: a turn on top of the rest rotation,
    // and (for a root) a move.
    QQuaternion pose;
    QVector3D offset;
};

struct ModelSkin {
    std::vector<int> joints;                // nodes
    std::vector<QMatrix4x4> inverseBind;
};

// A mesh where it stands: hung from a node, or bent by a skin.
struct ModelInstance {
    int mesh = -1;
    int node = -1;
    int skin = -1;
};

// Something to take hold of when posing: a point on the model that, dragged,
// turns a joint (or, for the figure's hips, moves the whole of it).
struct ModelHandle {
    int at = -1;          // the node the point is fixed to
    QVector3D tip;        // where on it, in its own space
    int turns = -1;       // the node that turns when it is dragged
    bool moves = false;   // drags the whole model instead
};

class Model3D {
public:
    QString name;
    QString path;          // empty for the figure
    bool figure = false;
    // Jointed like a person, with the figure's joint names: the figure itself,
    // or a model rigged in Impasto. The figure's poses work on it.
    bool humanoid = false;
    std::vector<QVector3D> rig;   // the joints it was rigged with, where it stood (see rigJointNames)
    std::vector<ModelNode> nodes;
    std::vector<ModelMesh> meshes;
    std::vector<ModelSkin> skins;
    std::vector<ModelInstance> instances;
    std::vector<ModelHandle> handles;

    // OBJ, STL, glTF or GLB, by what the file holds.
    static bool load(const QString& path, Model3D* out, QString* error);
    static QStringList fileFilters();
    static Model3D makeFigure();

    std::vector<QMatrix4x4> worldTransforms() const;
    // Every mesh as triangles where they are now, posed.
    struct Part {
        std::vector<QVector3D> positions, normals;
        std::vector<QVector2D> uvs;
        const std::vector<quint32>* indices = nullptr;
        QColor color;
        const QImage* texture = nullptr;
    };
    std::vector<Part> posed() const;
    bool bounds(QVector3D* lo, QVector3D* hi) const;   // of the model as posed
    float restFloor() const { return m_floor; }          // the lowest point at rest

    QVector3D handlePosition(const ModelHandle& h, const std::vector<QMatrix4x4>& world) const;
    QVector3D jointPosition(int node, const std::vector<QMatrix4x4>& world) const;
    // Turns a node about its own joint by a rotation given in world space.
    void turnNode(int node, const QQuaternion& worldTurn);
    void moveRoot(const QVector3D& worldDelta);
    void resetPose();
    bool posed_() const;   // anything turned or moved

    static QStringList figurePoses();
    void applyFigurePose(const QString& pose);
    // The pose, to keep between sessions.
    QVariantList poseState() const;
    void setPoseState(const QVariantList& state);

    void finish();   // after loading: normals, the handles, the floor

    // ---- rigging a model that came without joints
    // A person's joints, as points in the model where it stands as loaded:
    // the figure's 17 and the ends of the head, hands and feet.
    static QStringList rigJointNames();
    static int rigMirror(int joint);   // the same joint on the other side, or itself
    static std::vector<std::pair<int, int>> rigBones();   // pairs of joints, to draw the skeleton by
    bool canRig() const { return skins.empty() && !figure && !instances.empty(); }
    // A first guess from the shape: a person standing upright, arms out or
    // down at the sides. False, with the reason, when it does not look like one.
    bool fitRig(std::vector<QVector3D>* joints, QString* why) const;
    // Gives it those joints and binds every point of its surface to the bones
    // nearest it, blended where two meet. It keeps the pose it was in.
    bool bindRig(const std::vector<QVector3D>& joints);
    bool facesAway(const std::vector<QVector3D>& joints) const;   // its front towards -Z

    // Written out as a GLB of plain meshes, as it stands at rest (a rigged one
    // as it was bound), without joints. Where no picture is on it, points that
    // coincide are welded and the normals left for loading to work out again.
    bool saveGlb(const QString& path, QString* error) const;

private:
    float m_floor = 0.0f;
    float m_poseScale = 1.0f;   // its size against the figure's, for the poses that lower the hips
};
