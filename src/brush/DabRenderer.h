#pragma once

#include "brush/BrushPreset.h"
#include "brush/StrokeEngine.h"
#include "gfx/Gl.h"

#include <QRect>
#include <map>

// Where a stroke accumulates before it is merged into its layer. Paint and
// outline live in separate attachments (see tip.glsl): `color` blends dab over
// dab, `coverage` keeps the maximum antialiased edge. The merge multiplies them.
class StrokeBuffer {
public:
    StrokeBuffer() = default;
    ~StrokeBuffer() { destroy(); }
    StrokeBuffer(const StrokeBuffer&) = delete;
    StrokeBuffer& operator=(const StrokeBuffer&) = delete;

    // `sparse`: the two textures take memory only where a stroke has been (see gfx::Texture).
    void create(int w, int h, bool sparse = false);
    void destroy();
    void clear(float r = 0, float g = 0, float b = 0, float a = 0, float coverageValue = 0);
    void clearRect(const QRect& rect);   // to nothing, inside `rect` only
    void ensure(const QRect& rect);      // memory for what is about to be drawn there
    void bindAsTarget();

    gfx::Texture color;      // RGBA16F, premultiplied
    gfx::Texture coverage;   // R16F

private:
    GLuint m_fbo = 0;
};

// Rasterises dabs on the GPU as instanced quads.
class DabRenderer {
public:
    bool init();
    void destroy();

    // Accumulates dabs into the stroke buffer.
    void paint(StrokeBuffer& target, const std::vector<Dab>& dabs, const BrushPreset& preset);

    // Smears dabs straight onto `live`. `scratch` holds the pixels picked up from
    // under the previous dab; lastPos/hasLast carry state across calls in a stroke.
    // `selection` limits where it may act; `lockedLayer` (the layer before the
    // stroke) switches on alpha lock, smearing colour without touching alpha.
    // With `charge` above zero the dabs bring paint of their own (their
    // colour), mixed with what they pick up `pull` of a step behind them.
    void smudge(gfx::Texture& live, gfx::Texture& scratch, const std::vector<Dab>& dabs,
                const BrushPreset& preset, float strength, QPointF& lastPos, bool& hasLast,
                const gfx::Texture* selection = nullptr, const gfx::Texture* lockedLayer = nullptr,
                float charge = 0.0f, float pull = 1.0f);

    // Sets the uGrain* uniforms and binds the grain texture. Grain either rides
    // along with each dab (`moving`) or is fixed to the canvas, and only the
    // matching pass applies it; the other one gets it switched off.
    void bindGrain(gfx::Program& prog, const BrushPreset& preset, int unit, bool moving);

    static QRect dabBounds(const Dab& d);

private:
    void bindTip(gfx::Program& prog, const BrushPreset& preset);
    gfx::Texture* texture(std::map<QString, gfx::Texture>& set, const QString& name, bool grain);
    void upload(const std::vector<Dab>& dabs);

    gfx::Program m_paint;
    gfx::Program m_smudge;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    std::map<QString, gfx::Texture> m_tips;
    std::map<QString, gfx::Texture> m_grains;
};
