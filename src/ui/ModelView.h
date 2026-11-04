#pragma once

#include "model/Model3D.h"

#include <QHash>
#include <QOpenGLBuffer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <memory>

// A 3D model on screen, to draw from: turned with the pen, lit from wherever
// the light is put, and posed by its joints. Nothing in it is part of the
// artwork until it is placed on the canvas.
class ModelView : public QOpenGLWidget, protected QOpenGLExtraFunctions {
    Q_OBJECT
public:
    enum class Mode { Turn, Pose, Light };

    explicit ModelView(QWidget* parent = nullptr);
    ~ModelView() override;

    void setModel(std::unique_ptr<Model3D> model);
    Model3D* model() const { return m_model.get(); }
    void modelEdited();   // the pose changed from outside: draw it again, and say so

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    void setTwist(bool twist) { m_twist = twist; }
    bool twist() const { return m_twist; }
    void setHardLight(bool hard);
    bool hardLight() const { return m_hard; }
    void setFloor(bool shown);
    bool floor() const { return m_floor; }
    void setPicking(bool picking);
    bool picking() const { return m_picking; }
    // Focal length in millimetres, as on a 35 mm camera: wide angles distort, long ones flatten.
    void setLens(double mm);
    double lens() const { return m_lens; }
    void setZoom(double t);   // 0 far .. 1 near
    double zoom() const;
    // The canvas's shape, outlined in the view: what lands on the canvas when placed.
    void setFrameAspect(double aspect);
    void fit();

    // Rigging: the joints of a person shown over the model, to be dragged into place.
    void beginRig(const std::vector<QVector3D>& joints, bool facesAway);
    void endRig();
    bool rigging() const { return m_rigging; }
    const std::vector<QVector3D>& rigJoints() const { return m_rig; }
    void setRigMirror(bool mirror) { m_rigMirror = mirror; }
    bool rigMirror() const { return m_rigMirror; }
    QVector<QPointF> rigPositions() const;

    // The camera and light, to keep between sessions.
    QVariantMap viewState() const;
    void setViewState(const QVariantMap& state);

    // The model as it is shown, the size of the canvas, ready to be a layer.
    QImage renderImage(const QSize& size, bool transparent);

    // The picture of the model somewhere, in that place's pixels: the frame
    // it shows through, and the centre and height of the camera's whole view
    // (what the lens sees, top to bottom). In the panel the frame is the whole
    // of the view; on the canvas it is the frame the model is placed in, which
    // may show only part of the view, or more than it.
    struct Window {
        QRectF rect;
        QPointF centre;
        double height = 1.0;
        double hitRadius = 16.0;   // how near a handle a press must land
    };
    Window panelWindow() const;
    // The frame of that window drawn at the given number of pixels, see-through
    // round the model. Empty until the view has been shown once.
    QImage render(const Window& w, const QSize& pixels, bool transparent);
    QPointF project(const QVector3D& world, const Window& w) const;
    QVector<QPointF> handlePositions(const Window& w) const;
    // A press, drag and release in some window's pixels, doing what the mode
    // says: turning, posing (by the handle under the press), lighting, or with
    // the middle button or Shift moving the model about.
    void press(const QPointF& pos, Qt::MouseButton button, Qt::KeyboardModifiers mods, const Window& w);
    void move(const QPointF& pos, const Window& w);
    void release();
    int selectedHandle() const { return m_handle >= 0 ? m_handle : m_selected; }

    // For checks: where a point of the model and each handle are on screen.
    QPointF project(const QVector3D& world) const;
    QVector<QPointF> handlePositions() const;
    double yaw() const { return m_yaw; }
    QVector3D lightDirection() const;

signals:
    void colorPicked(const QColor& color);
    void edited();        // posed, turned or lit: worth remembering
    void pickingChanged(bool on);
    void modeChanged(Mode mode);

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    struct GpuPart {
        std::unique_ptr<QOpenGLVertexArrayObject> vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int count = 0;
        QColor color;
        std::shared_ptr<QOpenGLTexture> texture;
    };
    void upload();
    void drawScene(const QMatrix4x4& projection, bool transparent);
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projection(const Window& w) const;
    QVector3D eye() const;
    double fovY() const;
    QRectF frameRect() const;   // the canvas's shape inside the view
    int handleAt(const QPointF& pos, const Window& w) const;
    void dragHandle(const QPointF& from, const QPointF& to, const Window& w);
    // How far a point at that depth moves for one pixel of the window.
    double perPixel(const QVector3D& at, const Window& w) const;

    std::unique_ptr<Model3D> m_model;
    std::vector<GpuPart> m_parts;
    QOpenGLShaderProgram m_program, m_lineProgram;
    std::unique_ptr<QOpenGLVertexArrayObject> m_floorVao;
    QOpenGLBuffer m_floorVbo{QOpenGLBuffer::VertexBuffer};
    int m_floorCount = 0;
    QHash<const QImage*, std::shared_ptr<QOpenGLTexture>> m_textures;
    bool m_dirty = true;
    bool m_ready = false;

    Mode m_mode = Mode::Turn;
    bool m_twist = false, m_hard = false, m_floor = true, m_picking = false;
    double m_yaw = 28.0, m_pitch = 8.0, m_distance = 4.0, m_lens = 50.0;
    QVector3D m_target{0.0f, 0.9f, 0.0f};
    double m_lightAz = -35.0, m_lightEl = 40.0;
    double m_frameAspect = 0.0;
    float m_size = 1.8f;   // of the model, for zoom limits and the floor

    QPointF m_last;
    int m_handle = -1;      // being dragged
    int m_selected = -1;    // shown as chosen
    bool m_panning = false;
    bool m_rigging = false, m_rigMirror = true;
    std::vector<QVector3D> m_rig;
    int m_rigJoint = -1;   // being dragged
};
