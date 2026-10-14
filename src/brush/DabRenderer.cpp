#include "brush/DabRenderer.h"

#include "brush/BrushTextures.h"

#include "brush/TextureGen.h"

#include <QDebug>
#include <cmath>

using gfx::gl;

// Dual-source blend factors (GL 3.3 core); not always present in Qt's headers.
#ifndef GL_ONE_MINUS_SRC1_COLOR
#define GL_ONE_MINUS_SRC1_COLOR 0x88FA
#endif
#ifndef GL_ONE_MINUS_SRC1_ALPHA
#define GL_ONE_MINUS_SRC1_ALPHA 0x88FB
#endif

void StrokeBuffer::create(int w, int h, bool sparse)
{
    destroy();
    color.create(w, h, GL_RGBA16F, false, sparse);
    coverage.create(w, h, GL_R16F, false, sparse);
    // Dabs are drawn where the stroke says, not where a scissor rectangle is:
    // the memory for them is asked for by name (see ensure).
    color.setAutoCommit(false);
    coverage.setAutoCommit(false);
    gfx::GLF* f = gl();
    f->glGenFramebuffers(1, &m_fbo);
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id(), 0);
    f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, coverage.id(), 0);
    const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    f->glDrawBuffers(2, buffers);
    if (f->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        qCritical("StrokeBuffer: framebuffer incomplete (%dx%d)", w, h);
    clear();
}

void StrokeBuffer::destroy()
{
    if (m_fbo && gl())
        gl()->glDeleteFramebuffers(1, &m_fbo);
    m_fbo = 0;
    color.destroy();
    coverage.destroy();
}

void StrokeBuffer::clear(float r, float g, float b, float a, float coverageValue)
{
    if (color.sparse() && r == 0.0f && g == 0.0f && b == 0.0f && a == 0.0f && coverageValue == 0.0f) {
        // The next stroke is most likely near the last one: wipe the memory
        // and keep it, unless a great deal has piled up.
        constexpr qint64 kKeep = qint64(256) << 20;
        if (color.bytes() + coverage.bytes() > kKeep) {
            color.releaseAll();
            coverage.releaseAll();
        } else {
            clearRect(color.heldBounds() | coverage.heldBounds());
        }
        return;
    }
    if (color.sparse())
        ensure(color.rect());
    color.clear(r, g, b, a);
    coverage.clear(coverageValue, 0, 0, 0);
}

void StrokeBuffer::clearRect(const QRect& rect)
{
    const QRect r = rect & color.rect();
    if (r.isEmpty())
        return;
    // One clear of both attachments. Where a sparse buffer has no memory
    // there is nothing to wipe, and the clear leaves it that way.
    gfx::GLF* f = gl();
    bindAsTarget();
    f->glEnable(GL_SCISSOR_TEST);
    f->glScissor(r.x(), r.y(), r.width(), r.height());
    const float nothing[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    f->glClearBufferfv(GL_COLOR, 0, nothing);
    f->glClearBufferfv(GL_COLOR, 1, nothing);
    f->glDisable(GL_SCISSOR_TEST);
    color.markWritten();
    coverage.markWritten();
}

void StrokeBuffer::ensure(const QRect& rect)
{
    color.ensure(rect);
    coverage.ensure(rect);
}

void StrokeBuffer::bindAsTarget()
{
    gfx::GLF* f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    f->glViewport(0, 0, color.width(), color.height());
}

bool DabRenderer::init()
{
    const QStringList vert{QStringLiteral(":/shaders/dab.vert")};
    const QString tipChunk = QStringLiteral(":/shaders/tip.glsl");
    if (!m_paint.build(vert, {tipChunk, QStringLiteral(":/shaders/dab.frag")}))
        return false;
    if (!m_smudge.build(vert, {tipChunk, QStringLiteral(":/shaders/smudge.frag")}))
        return false;

    gfx::GLF* f = gl();
    f->glGenVertexArrays(1, &m_vao);
    f->glGenBuffers(1, &m_vbo);
    f->glBindVertexArray(m_vao);
    f->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    for (GLuint i = 0; i < 3; ++i) {
        f->glEnableVertexAttribArray(i);
        f->glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(Dab),
                                 reinterpret_cast<const void*>(quintptr(i * 16)));
        f->glVertexAttribDivisor(i, 1);
    }
    f->glBindVertexArray(0);

    for (const QString& name : texgen::tipNames()) {
        gfx::Texture& t = m_tips[name];
        t.create(texgen::kSize, texgen::kSize, GL_R8, true);
        t.upload(texgen::tip(name));
        t.setWrap(GL_CLAMP_TO_BORDER);
        t.generateMipmaps();
    }
    for (const QString& name : texgen::grainNames()) {
        gfx::Texture& t = m_grains[name];
        t.create(texgen::kSize, texgen::kSize, GL_R8, true);
        t.upload(texgen::grain(name));
        t.setWrap(GL_REPEAT);
        t.generateMipmaps();
    }
    return true;
}

void DabRenderer::destroy()
{
    m_tips.clear();
    m_grains.clear();
    m_paint.destroy();
    m_smudge.destroy();
    if (gfx::GLF* f = gl()) {
        if (m_vbo)
            f->glDeleteBuffers(1, &m_vbo);
        if (m_vao)
            f->glDeleteVertexArrays(1, &m_vao);
    }
    m_vbo = m_vao = 0;
}

QRect DabRenderer::dabBounds(const Dab& d)
{
    const float r = std::max(d.size, 1.0f) * 0.5f + 2.5f;
    const int x0 = int(std::floor(d.x - r)), y0 = int(std::floor(d.y - r));
    const int x1 = int(std::ceil(d.x + r)), y1 = int(std::ceil(d.y + r));
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

// A built-in texture, or an imported one, which goes up to the GPU the first
// time a brush asks for it.
gfx::Texture* DabRenderer::texture(std::map<QString, gfx::Texture>& set, const QString& name, bool grain)
{
    if (name.isEmpty())
        return nullptr;
    auto it = set.find(name);
    if (it != set.end())
        return &it->second;
    if (!brushtex::isUser(name))
        return nullptr;
    const QImage map = brushtex::load(name);
    if (map.isNull())
        return nullptr;   // its file is gone: the brush falls back to a round tip, or no grain
    gfx::Texture& t = set[name];
    t.create(texgen::kSize, texgen::kSize, GL_R8, true);
    t.upload(map);
    t.setWrap(grain ? GL_REPEAT : GL_CLAMP_TO_BORDER);
    t.generateMipmaps();
    return &t;
}

void DabRenderer::bindTip(gfx::Program& prog, const BrushPreset& preset)
{
    if (gfx::Texture* tip = texture(m_tips, preset.tip, false)) {
        tip->bind(0);
        prog.set("uTipMode", 1);
    } else {
        prog.set("uTipMode", 0);
    }
    prog.set("uTip", 0);
    prog.set("uHardness", std::clamp(preset.hardness, 0.0f, 1.0f));
}

void DabRenderer::bindGrain(gfx::Program& prog, const BrushPreset& preset, int unit, bool moving)
{
    gfx::Texture* grain = texture(m_grains, preset.grain, true);
    const bool active = grain && preset.grainMode != BrushPreset::GrainMode::Off
                        && preset.grainMoving == moving;
    if (active)
        grain->bind(unit);
    prog.set("uGrain", unit);
    prog.set("uGrainMode", active ? int(preset.grainMode) : 0);
    prog.set("uGrainScale", std::max(preset.grainScale, 0.05f));
    prog.set("uGrainDepth", std::clamp(preset.grainDepth, 0.0f, 1.0f));
}

void DabRenderer::upload(const std::vector<Dab>& dabs)
{
    gfx::GLF* f = gl();
    f->glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    f->glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(dabs.size() * sizeof(Dab)), dabs.data(), GL_STREAM_DRAW);
}

void DabRenderer::paint(StrokeBuffer& target, const std::vector<Dab>& dabs, const BrushPreset& preset)
{
    if (dabs.empty())
        return;
    gfx::GLF* f = gl();
    // Memory for where each dab lands, not for the rectangle round all of
    // them: the outline of a large circle touches a ring of blocks, and the
    // middle it goes round should cost nothing.
    for (const Dab& d : dabs)
        target.ensure(dabBounds(d));
    target.bindAsTarget();
    f->glDisable(GL_SCISSOR_TEST);
    f->glEnable(GL_BLEND);
    // Attachment 0 is paint: dab over dab, or MAX to keep a stroke uniform where
    // it crosses itself (markers). Attachment 1 is the outline, always MAX.
    f->glBlendFunci(0, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    f->glBlendEquationi(0, preset.buildup == BrushPreset::Buildup::Max ? GL_MAX : GL_FUNC_ADD);
    f->glBlendFunci(1, GL_ONE, GL_ONE);
    f->glBlendEquationi(1, GL_MAX);

    m_paint.use();
    m_paint.set("uCanvasSize", float(target.color.width()), float(target.color.height()));
    bindTip(m_paint, preset);

    bindGrain(m_paint, preset, 1, true);

    upload(dabs);
    f->glBindVertexArray(m_vao);
    f->glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(dabs.size()));
    f->glBindVertexArray(0);
    f->glBlendEquation(GL_FUNC_ADD);
    f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    f->glDisable(GL_BLEND);
}

void DabRenderer::smudge(gfx::Texture& live, gfx::Texture& scratch, const std::vector<Dab>& dabs,
                         const BrushPreset& preset, float strength, QPointF& lastPos, bool& hasLast,
                         const gfx::Texture* selection, const gfx::Texture* lockedLayer, float charge, float pull)
{
    if (dabs.empty())
        return;
    gfx::GLF* f = gl();
    const QRect canvas = live.rect();

    live.bindAsTarget();
    f->glDisable(GL_SCISSOR_TEST);
    f->glEnable(GL_BLEND);
    f->glBlendEquation(GL_FUNC_ADD);
    f->glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC1_COLOR, GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);

    m_smudge.use();
    m_smudge.set("uCanvasSize", float(live.width()), float(live.height()));
    m_smudge.set("uStrength", std::clamp(strength, 0.0f, 1.0f));
    m_smudge.set("uPull", pull);
    bindTip(m_smudge, preset);
    scratch.bind(1);
    m_smudge.set("uPickup", 1);
    (selection ? *selection : scratch).bind(2);
    m_smudge.set("uSelection", 2);
    m_smudge.set("uHasSelection", selection ? 1 : 0);
    (lockedLayer ? *lockedLayer : scratch).bind(3);
    m_smudge.set("uLayer", 3);
    m_smudge.set("uAlphaLock", lockedLayer ? 1 : 0);

    upload(dabs);
    f->glBindVertexArray(m_vao);
    for (size_t i = 0; i < dabs.size(); ++i) {
        const Dab& d = dabs[i];
        const QPointF cur(d.x, d.y);
        if (!hasLast) {
            lastPos = cur;
            hasLast = true;
            // Nothing to drag yet; but a brush with paint of its own leaves its first mark.
            if (charge <= 0.0f)
                continue;
        }
        const QPointF delta = cur - lastPos;
        lastPos = cur;
        // Charge is what one pass of the brush leaves of its own paint. A
        // point is under the brush for about one diameter of travel, so each
        // dab lays the share that this step is of that.
        const float travelled = float(std::hypot(delta.x(), delta.y())) / std::max(d.size, 1.0f);
        const float perDab = charge >= 0.999f ? 1.0f
                             : travelled <= 0.0f ? charge
                                                 : 1.0f - std::pow(1.0f - charge, std::min(1.0f, travelled * 1.5f));
        m_smudge.set("uCharge", charge > 0.0f ? perDab : 0.0f);
        const QRect dst = dabBounds(d) & canvas;
        if (dst.isEmpty())
            continue;
        // The pixels this dab will deposit sit `delta` behind it.
        const QRect src = dst.translated(-int(std::floor(delta.x())), -int(std::floor(delta.y())))
                              .adjusted(-2, -2, 2, 2) & canvas;
        if (src.isEmpty())
            continue;
        // Each dab reads what the previous one wrote, so they cannot be batched.
        live.ensure(dst);
        gfx::copyTexture(live, scratch, src, src.topLeft());
        m_smudge.set("uDelta", float(delta.x()), float(delta.y()));
        m_smudge.set("uSrcRect", float(src.left()), float(src.top()), float(src.left() + src.width()),
                     float(src.top() + src.height()));
        f->glDrawArraysInstancedBaseInstance(GL_TRIANGLE_STRIP, 0, 4, 1, GLuint(i));
    }
    f->glBindVertexArray(0);
    f->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    f->glDisable(GL_BLEND);
}
