#include "ui/ModelView.h"

#include "ui/Theme.h"

#include <QMouseEvent>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {

constexpr double kDeg = M_PI / 180.0;

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uViewProj;
out vec3 vPos;
out vec3 vNormal;
out vec2 vUv;
void main()
{
    vPos = aPos;
    vNormal = aNormal;
    vUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

// Clay in one light: what is turned to the light is lit, the rest in shade
// with a little light from the sky, and an edge of light along the outline so
// that the shape reads against the background. Hard light makes it two tones,
// the light and shadow shapes a painter looks for.
const char* kFragment = R"(#version 330 core
in vec3 vPos;
in vec3 vNormal;
in vec2 vUv;
uniform vec3 uLight;
uniform vec3 uEye;
uniform vec4 uColor;
uniform sampler2D uTex;
uniform int uHasTex;
uniform int uHard;
out vec4 fragColor;
void main()
{
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uEye - vPos);
    if (dot(n, v) < 0.0)
        n = -n;
    vec3 albedo = uColor.rgb;
    if (uHasTex != 0)
        albedo *= texture(uTex, vUv).rgb;
    float d = max(dot(n, uLight), 0.0);
    vec3 c;
    if (uHard != 0) {
        c = albedo * (d > 0.0 ? 1.0 : 0.42);
    } else {
        float sky = 0.5 + 0.5 * n.y;
        c = albedo * (0.2 + 0.14 * sky + 0.74 * d);
        c += vec3(0.07) * pow(1.0 - max(dot(n, v), 0.0), 3.0);
    }
    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

const char* kLineVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }
)";

const char* kLineFragment = R"(#version 330 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; }
)";

} // namespace

ModelView::ModelView(QWidget* parent) : QOpenGLWidget(parent)
{
    setMinimumSize(200, 200);
    setMouseTracking(false);
}

ModelView::~ModelView()
{
    makeCurrent();
    m_parts.clear();
    m_textures.clear();
    m_floorVao.reset();
    m_floorVbo.destroy();
    doneCurrent();
}

void ModelView::setModel(std::unique_ptr<Model3D> model)
{
    m_model = std::move(model);
    m_selected = m_handle = -1;
    m_dirty = true;
    fit();
}

void ModelView::modelEdited()
{
    m_dirty = true;
    update();
}

void ModelView::setMode(Mode mode)
{
    m_mode = mode;
    m_selected = -1;
    update();
}

void ModelView::setHardLight(bool hard)
{
    m_hard = hard;
    update();
}

void ModelView::setFloor(bool shown)
{
    m_floor = shown;
    update();
}

void ModelView::setPicking(bool picking)
{
    if (m_picking == picking)
        return;
    m_picking = picking;
    setCursor(picking ? Qt::CrossCursor : Qt::ArrowCursor);
    emit pickingChanged(picking);
}

void ModelView::setLens(double mm)
{
    // Keep the model the same size on screen: a longer lens stands further back.
    const double before = std::tan(fovY() / 2.0);
    m_lens = std::clamp(mm, 14.0, 200.0);
    m_distance *= before / std::tan(fovY() / 2.0);
    update();
    emit edited();
}

double ModelView::fovY() const
{
    return 2.0 * std::atan(12.0 / m_lens);   // a 24 mm high frame
}

void ModelView::setZoom(double t)
{
    // Exponential between close and far, so that the slider feels even.
    const double nearD = m_size * 0.25, farD = m_size * 12.0;
    m_distance = farD * std::pow(nearD / farD, std::clamp(t, 0.0, 1.0)) / std::tan(fovY() / 2.0) * std::tan(2.0 * std::atan(12.0 / 50.0) / 2.0);
    update();
    emit edited();
}

double ModelView::zoom() const
{
    const double nearD = m_size * 0.25, farD = m_size * 12.0;
    const double d = m_distance * std::tan(fovY() / 2.0) / std::tan(2.0 * std::atan(12.0 / 50.0) / 2.0);
    return std::clamp(std::log(d / farD) / std::log(nearD / farD), 0.0, 1.0);
}

void ModelView::setFrameAspect(double aspect)
{
    m_frameAspect = aspect;
    update();
}

void ModelView::fit()
{
    if (!m_model)
        return;
    QVector3D lo, hi;
    if (!m_model->bounds(&lo, &hi))
        return;
    m_target = (lo + hi) / 2.0f;
    m_size = std::max(0.001f, (hi - lo).length());
    const double height = std::max({double(hi.y() - lo.y()), double(hi.x() - lo.x()) * 0.7, double(hi.z() - lo.z()) * 0.7});
    m_distance = height * 0.62 / std::tan(fovY() / 2.0) + (hi - lo).length() * 0.2;
    update();
}

QVariantMap ModelView::viewState() const
{
    return {{QStringLiteral("yaw"), m_yaw}, {QStringLiteral("pitch"), m_pitch}, {QStringLiteral("distance"), m_distance},
            {QStringLiteral("lens"), m_lens}, {QStringLiteral("targetX"), m_target.x()}, {QStringLiteral("targetY"), m_target.y()},
            {QStringLiteral("targetZ"), m_target.z()}, {QStringLiteral("lightAz"), m_lightAz}, {QStringLiteral("lightEl"), m_lightEl},
            {QStringLiteral("hard"), m_hard}, {QStringLiteral("floor"), m_floor}};
}

void ModelView::setViewState(const QVariantMap& s)
{
    if (s.isEmpty())
        return;
    m_yaw = s.value(QStringLiteral("yaw"), m_yaw).toDouble();
    m_pitch = s.value(QStringLiteral("pitch"), m_pitch).toDouble();
    m_distance = s.value(QStringLiteral("distance"), m_distance).toDouble();
    m_lens = s.value(QStringLiteral("lens"), m_lens).toDouble();
    m_target = QVector3D(s.value(QStringLiteral("targetX"), m_target.x()).toFloat(), s.value(QStringLiteral("targetY"), m_target.y()).toFloat(),
                         s.value(QStringLiteral("targetZ"), m_target.z()).toFloat());
    m_lightAz = s.value(QStringLiteral("lightAz"), m_lightAz).toDouble();
    m_lightEl = s.value(QStringLiteral("lightEl"), m_lightEl).toDouble();
    m_hard = s.value(QStringLiteral("hard"), m_hard).toBool();
    m_floor = s.value(QStringLiteral("floor"), m_floor).toBool();
    update();
}

QVector3D ModelView::eye() const
{
    const double cp = std::cos(m_pitch * kDeg);
    return m_target + QVector3D(float(cp * std::sin(m_yaw * kDeg)), float(std::sin(m_pitch * kDeg)), float(cp * std::cos(m_yaw * kDeg))) * float(m_distance);
}

QVector3D ModelView::lightDirection() const
{
    const double ce = std::cos(m_lightEl * kDeg);
    return QVector3D(float(ce * std::sin(m_lightAz * kDeg)), float(std::sin(m_lightEl * kDeg)), float(ce * std::cos(m_lightAz * kDeg))).normalized();
}

QMatrix4x4 ModelView::viewMatrix() const
{
    QMatrix4x4 v;
    v.lookAt(eye(), m_target, QVector3D(0, 1, 0));
    return v;
}

QMatrix4x4 ModelView::projection(double aspect) const
{
    QMatrix4x4 p;
    p.perspective(float(fovY() / kDeg), float(aspect), float(std::max(0.001, m_distance * 0.01)), float(m_distance * 20.0 + m_size * 10.0));
    return p;
}

QRectF ModelView::frameRect() const
{
    const QRectF r = rect();
    if (m_frameAspect <= 0.0)
        return r;
    // The canvas's shape, the full height of the view, narrower or wider as it is.
    const double w = r.height() * m_frameAspect;
    if (w <= r.width())
        return QRectF(r.center().x() - w / 2.0, r.top(), w, r.height());
    const double h = r.width() / m_frameAspect;
    return QRectF(r.left(), r.center().y() - h / 2.0, r.width(), h);
}

QPointF ModelView::project(const QVector3D& world) const
{
    // Through the frame's projection when there is one, as what is placed would see it.
    const QRectF f = frameRect();
    const QVector4D clip = projection(f.width() / std::max(1.0, f.height())) * viewMatrix() * QVector4D(world, 1.0f);
    if (clip.w() <= 1e-6f)
        return {-1e6, -1e6};
    const double x = clip.x() / clip.w(), y = clip.y() / clip.w();
    return {f.left() + (x * 0.5 + 0.5) * f.width(), f.top() + (0.5 - y * 0.5) * f.height()};
}

void ModelView::beginRig(const std::vector<QVector3D>& joints, bool facesAway)
{
    m_rigging = true;
    m_rig = joints;
    m_rigJoint = -1;
    m_selected = -1;
    // Face to face with it, to see where its joints should go.
    m_yaw = facesAway ? 180.0 : 0.0;
    m_pitch = 4.0;
    fit();
}

void ModelView::endRig()
{
    m_rigging = false;
    m_rigJoint = -1;
    update();
}

QVector<QPointF> ModelView::rigPositions() const
{
    QVector<QPointF> out;
    for (const QVector3D& j : m_rig)
        out.push_back(project(j));
    return out;
}

QVector<QPointF> ModelView::handlePositions() const
{
    QVector<QPointF> out;
    if (!m_model)
        return out;
    const std::vector<QMatrix4x4> world = m_model->worldTransforms();
    for (const ModelHandle& h : m_model->handles)
        out.push_back(project(m_model->handlePosition(h, world)));
    return out;
}

// ------------------------------------------------------------------- GL

void ModelView::initializeGL()
{
    initializeOpenGLFunctions();
    m_program.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex);
    m_program.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragment);
    m_program.link();
    m_lineProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVertex);
    m_lineProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFragment);
    m_lineProgram.link();
    m_ready = m_program.isLinked() && m_lineProgram.isLinked();
    m_dirty = true;
}

void ModelView::upload()
{
    m_parts.clear();
    m_floorCount = 0;
    if (!m_model)
        return;
    for (const Model3D::Part& part : m_model->posed()) {
        GpuPart g;
        std::vector<float> data;
        data.reserve(part.positions.size() * 8);
        for (size_t v = 0; v < part.positions.size(); ++v) {
            const QVector3D& p = part.positions[v];
            const QVector3D& n = part.normals[v];
            const QVector2D uv = v < part.uvs.size() ? part.uvs[v] : QVector2D();
            data.insert(data.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z(), uv.x(), uv.y()});
        }
        g.vao = std::make_unique<QOpenGLVertexArrayObject>();
        g.vao->create();
        g.vao->bind();
        g.vbo.create();
        g.vbo.bind();
        g.vbo.allocate(data.data(), int(data.size() * sizeof(float)));
        g.ibo.create();
        g.ibo.bind();
        g.ibo.allocate(part.indices->data(), int(part.indices->size() * sizeof(quint32)));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(6 * sizeof(float)));
        g.vao->release();
        g.count = int(part.indices->size());
        g.color = part.color;
        if (part.texture && !part.uvs.empty()) {
            auto& tex = m_textures[part.texture];
            if (!tex) {
                tex = std::make_shared<QOpenGLTexture>(part.texture->convertToFormat(QImage::Format_RGBA8888).mirrored());
                tex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
                tex->setMagnificationFilter(QOpenGLTexture::Linear);
                tex->setWrapMode(QOpenGLTexture::Repeat);
            }
            g.texture = tex;
        }
        m_parts.push_back(std::move(g));
    }
    // A floor of squares under it, to see where it stands and which way is level.
    const double step = std::pow(10.0, std::floor(std::log10(m_size / 4.0)));
    const double extent = step * std::ceil(m_size * 1.2 / step);
    const float y = m_model->restFloor();
    std::vector<float> lines;
    for (double t = -extent; t <= extent + 1e-6; t += step) {
        lines.insert(lines.end(), {float(t), y, float(-extent), float(t), y, float(extent)});
        lines.insert(lines.end(), {float(-extent), y, float(t), float(extent), y, float(t)});
    }
    if (!m_floorVao) {
        m_floorVao = std::make_unique<QOpenGLVertexArrayObject>();
        m_floorVao->create();
    }
    m_floorVao->bind();
    if (!m_floorVbo.isCreated())
        m_floorVbo.create();
    m_floorVbo.bind();
    m_floorVbo.allocate(lines.data(), int(lines.size() * sizeof(float)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    m_floorVao->release();
    m_floorCount = int(lines.size() / 3);
}

// The model and its floor, through a camera of the given shape, into whatever is bound.
void ModelView::drawScene(double aspect, bool transparent)
{
    if (!m_ready || !m_model)
        return;
    if (m_dirty) {
        upload();
        m_dirty = false;
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    const QMatrix4x4 viewProj = projection(aspect) * viewMatrix();
    m_program.bind();
    m_program.setUniformValue("uViewProj", viewProj);
    m_program.setUniformValue("uLight", lightDirection());
    m_program.setUniformValue("uEye", eye());
    m_program.setUniformValue("uHard", m_hard ? 1 : 0);
    m_program.setUniformValue("uTex", 0);
    for (GpuPart& g : m_parts) {
        m_program.setUniformValue("uColor", QVector4D(g.color.redF(), g.color.greenF(), g.color.blueF(), 1.0f));
        m_program.setUniformValue("uHasTex", g.texture ? 1 : 0);
        if (g.texture)
            g.texture->bind(0);
        g.vao->bind();
        glDrawElements(GL_TRIANGLES, g.count, GL_UNSIGNED_INT, nullptr);
        g.vao->release();
    }
    m_program.release();
    // Placed on the canvas, the model goes alone: the floor is only for seeing it in the panel.
    if (m_floor && !transparent && m_floorCount > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_lineProgram.bind();
        m_lineProgram.setUniformValue("uViewProj", viewProj);
        m_lineProgram.setUniformValue("uColor", QVector4D(1.0f, 1.0f, 1.0f, 0.16f));
        m_floorVao->bind();
        glDrawArrays(GL_LINES, 0, m_floorCount);
        m_floorVao->release();
        m_lineProgram.release();
        glDisable(GL_BLEND);
    }
    glDisable(GL_DEPTH_TEST);
}

void ModelView::paintGL()
{
    const qreal dpr = devicePixelRatioF();
    const QRectF f = frameRect();
    // Darker round the frame, and the scene inside it through the frame's own lens.
    glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    glClearColor(0.15f, 0.15f, 0.17f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const QRect fp(int(f.left() * dpr), int((height() - f.bottom()) * dpr), int(f.width() * dpr), int(f.height() * dpr));
    glEnable(GL_SCISSOR_TEST);
    glScissor(fp.x(), fp.y(), fp.width(), fp.height());
    glViewport(fp.x(), fp.y(), fp.width(), fp.height());
    glClearColor(0.21f, 0.21f, 0.23f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawScene(f.width() / std::max(1.0, f.height()), false);
    glDisable(GL_SCISSOR_TEST);

    // Over it: the frame's edge, the joints to take hold of, where the light comes from.
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_frameAspect > 0.0) {
        p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(f.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    if (m_rigging) {
        // The skeleton being fitted: bones as lines, joints as dots, the left side blue and the right orange.
        const QVector<QPointF> at = rigPositions();
        p.setPen(QPen(QColor(255, 255, 255, 170), 2.0));
        for (const auto& [a, b] : Model3D::rigBones()) {
            if (a < at.size() && b < at.size())
                p.drawLine(at[a], at[b]);
        }
        for (int i = 0; i < at.size(); ++i) {
            const int twin = Model3D::rigMirror(i);
            const QColor side = twin == i ? QColor(255, 255, 255) : (i < twin ? QColor(90, 200, 250) : QColor(255, 159, 10));
            p.setPen(QPen(i == m_rigJoint ? QColor(Qt::white) : QColor(0, 0, 0, 140), 1.5));
            p.setBrush(i == m_rigJoint ? theme::accent : side);
            p.drawEllipse(at[i], 6.0, 6.0);
        }
    } else if (m_mode == Mode::Pose && m_model) {
        const QVector<QPointF> at = handlePositions();
        for (int i = 0; i < at.size(); ++i) {
            const bool moves = m_model->handles[size_t(i)].moves;
            const bool chosen = i == m_selected || i == m_handle;
            p.setPen(QPen(chosen ? QColor(Qt::white) : QColor(0, 0, 0, 120), 1.5));
            p.setBrush(chosen ? theme::accent : (moves ? QColor(52, 199, 89) : QColor(255, 255, 255, 200)));
            p.drawEllipse(at[i], moves ? 7.5 : 6.0, moves ? 7.5 : 6.0);
        }
    }
    if (m_mode == Mode::Light) {
        // A small globe in the corner, and the sun on it where the light comes from.
        const QPointF c(width() - 44.0, height() - 44.0);
        p.setPen(QPen(QColor(255, 255, 255, 90), 1.2));
        p.setBrush(QColor(0, 0, 0, 90));
        p.drawEllipse(c, 30, 30);
        const QMatrix4x4 v = viewMatrix();
        const QVector3D l = v.mapVector(lightDirection());
        const QPointF sun = c + QPointF(l.x() * 26.0, -l.y() * 26.0);
        p.setPen(Qt::NoPen);
        p.setBrush(l.z() > 0 ? QColor(255, 214, 10) : QColor(255, 214, 10, 110));
        p.drawEllipse(sun, 6, 6);
    }
}

// --------------------------------------------------------------- input

int ModelView::handleAt(const QPointF& pos) const
{
    if (!m_model)
        return -1;
    const QVector<QPointF> at = handlePositions();
    int best = -1;
    double bestD = 16.0;
    for (int i = 0; i < at.size(); ++i) {
        const double d = QLineF(at[i], pos).length();
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void ModelView::dragHandle(const QPointF& from, const QPointF& to)
{
    const ModelHandle& h = m_model->handles[size_t(m_handle)];
    const std::vector<QMatrix4x4> world = m_model->worldTransforms();
    const QVector3D forward = (m_target - eye()).normalized();
    if (h.moves) {
        // The whole figure moves in the plane facing the view.
        const QVector3D at = m_model->handlePosition(h, world);
        const double depth = std::max(0.001, double(QVector3D::dotProduct(at - eye(), forward)));
        const double perPixel = 2.0 * depth * std::tan(fovY() / 2.0) / std::max(1.0, frameRect().height());
        const QMatrix4x4 v = viewMatrix().inverted();
        const QVector3D right = v.mapVector(QVector3D(1, 0, 0)), up = v.mapVector(QVector3D(0, 1, 0));
        m_model->moveRoot((right * float(to.x() - from.x()) - up * float(to.y() - from.y())) * float(perPixel));
        return;
    }
    const QVector3D pivot = m_model->jointPosition(h.turns, world);
    if (m_twist) {
        // About the bone itself.
        QVector3D axis = m_model->handlePosition(h, world) - pivot;
        if (axis.lengthSquared() < 1e-12f)
            axis = forward;
        m_model->turnNode(h.turns, QQuaternion::fromAxisAndAngle(axis.normalized(), float((to.x() - from.x()) * 0.6)));
        return;
    }
    // Round the joint, in the plane facing the view: the point follows the pen.
    const QPointF c = project(pivot);
    const double a0 = std::atan2(from.y() - c.y(), from.x() - c.x()), a1 = std::atan2(to.y() - c.y(), to.x() - c.x());
    double delta = a1 - a0;
    while (delta > M_PI)
        delta -= 2.0 * M_PI;
    while (delta < -M_PI)
        delta += 2.0 * M_PI;
    if (QLineF(c, to).length() < 4.0)
        return;   // too close to the joint to say which way
    m_model->turnNode(h.turns, QQuaternion::fromAxisAndAngle(forward, float(delta / kDeg)));
}

void ModelView::mousePressEvent(QMouseEvent* e)
{
    m_last = e->position();
    m_panning = e->button() == Qt::MiddleButton || (e->modifiers() & Qt::ShiftModifier);
    if (m_picking && e->button() == Qt::LeftButton) {
        const QImage shot = grabFramebuffer();
        const QPoint px = (e->position() * devicePixelRatioF()).toPoint();
        if (shot.rect().contains(px))
            emit colorPicked(shot.pixelColor(px));
        setPicking(false);
        return;
    }
    m_rigJoint = -1;
    if (m_rigging && !m_panning) {
        // The nearest joint under the pen, if one is.
        const QVector<QPointF> at = rigPositions();
        double best = 16.0;
        for (int i = 0; i < at.size(); ++i) {
            const double d = QLineF(at[i], e->position()).length();
            if (d < best) {
                best = d;
                m_rigJoint = i;
            }
        }
        m_handle = -1;
        update();
        return;
    }
    m_handle = (m_mode == Mode::Pose && !m_panning) ? handleAt(e->position()) : -1;
    if (m_handle >= 0) {
        m_selected = m_handle;
        update();
    }
}

void ModelView::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton)))
        return;
    const QPointF pos = e->position();
    const QPointF d = pos - m_last;
    if (m_rigJoint >= 0) {
        // In the plane facing the view, and the same joint on the other side mirrored with it.
        const QVector3D at = m_rig[size_t(m_rigJoint)];
        const QVector3D forward = (m_target - eye()).normalized();
        const double depth = std::max(0.001, double(QVector3D::dotProduct(at - eye(), forward)));
        const double perPixel = 2.0 * depth * std::tan(fovY() / 2.0) / std::max(1.0, frameRect().height());
        const QMatrix4x4 v = viewMatrix().inverted();
        const QVector3D move = (v.mapVector(QVector3D(1, 0, 0)) * float(d.x()) - v.mapVector(QVector3D(0, 1, 0)) * float(d.y())) * float(perPixel);
        m_rig[size_t(m_rigJoint)] += move;
        const int twin = Model3D::rigMirror(m_rigJoint);
        if (m_rigMirror && twin != m_rigJoint) {
            const float middle = m_rig[0].x();
            const QVector3D& p = m_rig[size_t(m_rigJoint)];
            m_rig[size_t(twin)] = QVector3D(2.0f * middle - p.x(), p.y(), p.z());
        }
    } else if (m_handle >= 0 && m_model) {
        dragHandle(m_last, pos);
        m_dirty = true;
    } else if (m_panning) {
        const QMatrix4x4 v = viewMatrix().inverted();
        const double perPixel = 2.0 * m_distance * std::tan(fovY() / 2.0) / std::max(1.0, frameRect().height());
        m_target -= (v.mapVector(QVector3D(1, 0, 0)) * float(d.x()) - v.mapVector(QVector3D(0, 1, 0)) * float(d.y())) * float(perPixel);
    } else if (m_mode == Mode::Light) {
        m_lightAz -= d.x() * 0.5;
        m_lightEl = std::clamp(m_lightEl - d.y() * 0.5, -85.0, 89.0);
    } else {
        m_yaw -= d.x() * 0.45;
        m_pitch = std::clamp(m_pitch + d.y() * 0.35, -89.0, 89.0);
    }
    m_last = pos;
    update();
}

void ModelView::mouseReleaseEvent(QMouseEvent*)
{
    if ((m_handle >= 0 || m_panning || m_mode != Mode::Pose) && !m_rigging)
        emit edited();
    m_handle = -1;
    m_rigJoint = -1;
    m_panning = false;
    update();
}

void ModelView::wheelEvent(QWheelEvent* e)
{
    m_distance = std::clamp(m_distance * std::pow(0.9, e->angleDelta().y() / 120.0), double(m_size) * 0.05, double(m_size) * 60.0);
    update();
    emit edited();
}

QImage ModelView::renderImage(const QSize& wanted, bool transparent)
{
    if (!m_model || wanted.isEmpty())
        return {};
    makeCurrent();
    GLint limit = 4096;
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &limit);
    const int cap = std::min(8192, int(limit));
    QSize size = wanted;
    if (size.width() > cap || size.height() > cap)
        size = size.scaled(cap, cap, Qt::KeepAspectRatio);
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setSamples(4);
    QOpenGLFramebufferObject fbo(size, format);
    fbo.bind();
    glViewport(0, 0, size.width(), size.height());
    if (transparent)
        glClearColor(0, 0, 0, 0);
    else
        glClearColor(0.21f, 0.21f, 0.23f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawScene(double(wanted.width()) / std::max(1, wanted.height()), transparent);
    fbo.release();
    QImage image = fbo.toImage();
    doneCurrent();
    if (image.size() != wanted)
        image = image.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
