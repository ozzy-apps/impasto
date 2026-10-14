#pragma once

#include <QHash>
#include <QImage>
#include <QOpenGLFunctions_4_3_Core>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QTransform>
#include <vector>

namespace gfx {

using GLF = QOpenGLFunctions_4_3_Core;

// OpenGL 4.3 core entry points for whichever context is current on this thread.
GLF* gl();
void invalidateGlCache();

// ---- Textures that only take memory where something is in them.
//
// A sparse texture is as large as any other to everything that reads it or
// draws into it, but the graphics card only sets memory aside for the parts
// that have been asked for, in blocks of kPage pixels square. A block nobody
// asked for reads as nothing (zero) and swallows what is drawn into it. The
// canvas keeps its layers this way, which is what lets a canvas be larger
// than the memory a full layer of it would take.
constexpr int kPage = 256;
// Whether the context current on this thread can do it: the driver has to
// promise that blocks without memory read as zero, and offer a way to wipe a
// fresh block without disturbing any drawing in progress.
bool sparseSupported();
// Off: every texture is allocated whole, as on a card that cannot do better.
// (Also switched off by IMPASTO_NO_SPARSE=1 in the environment.)
void setSparseAllowed(bool allowed);
bool sparseAllowed();
// Graphics memory held by all textures alive, in bytes: for a sparse one,
// what its blocks take.
qint64 textureBytes();
// Call before a draw: when the target is a sparse texture, gives it memory
// for the area the draw can reach (the scissor rectangle, or all of it).
void beforeDraw();

// Canvas convention used everywhere: texel (x, y) is canvas pixel (x, y) with y
// pointing down, so rows come back from glReadPixels already in image order.
class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(Texture&& o) noexcept;
    Texture& operator=(Texture&& o) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // `sparse` is a wish: it is granted where the card can (see above) and
    // the texture has no mipmaps.
    void create(int w, int h, GLenum internalFormat, bool mipmaps = false, bool sparse = false);
    void destroy();
    void swap(Texture& o) noexcept;

    // ---- sparse textures (for any other, these do nothing and everything is "held")
    bool sparse() const { return m_sparse; }
    // Goes up whenever something may have changed what the texture holds.
    quint64 written() const { return m_written; }
    void markWritten() { ++m_written; }
    // Gives memory to every block `r` touches. New blocks start out as nothing.
    void ensure(const QRect& r);
    // Memory for the blocks that `r` touches and that `like` has memory for:
    // what a pass that can only leave something where `like` has something needs.
    void ensureLike(const Texture& like, const QRect& r);
    // Takes the memory back from the blocks that lie wholly inside `r`.
    void release(const QRect& r);
    void releaseAll();
    bool held(int pageX, int pageY) const { return !m_sparse || m_pages[size_t(pageY) * size_t(m_pagesX) + size_t(pageX)] != 0; }
    bool heldAt(const QPoint& texel) const { return held(texel.x() / kPage, texel.y() / kPage); }
    int pagesX() const { return m_pagesX; }
    int pagesY() const { return m_pagesY; }
    QRect pageRect(int pageX, int pageY) const;
    int heldPages() const { return m_sparse ? m_held : m_pagesX * m_pagesY; }
    const std::vector<uint8_t>& pages() const { return m_pages; }   // sparse only: one byte per block, row by row
    // The smallest rectangle around every block that has memory.
    QRect heldBounds() const;
    // Memory for the blocks `r` touches and for no others. Returns the area
    // those blocks cover, which is what the caller must then draw anew: what
    // the kept blocks hold stays in them. A texture that is not sparse has
    // no blocks to give up, and the answer is all of it.
    QRect holdOnly(const QRect& r);
    // The first half of that alone: gives up the blocks `r` does not touch,
    // and asks for none. Returns the same area.
    QRect keepOnly(const QRect& r);
    // Gives this texture memory for exactly the blocks of `like` that have it
    // (grown by `margin` pixels), and for no others.
    void matchPages(const Texture& like, int margin = 0);
    // Draws into a sparse target normally bring memory for what they can
    // reach. Switched off, they only land where memory already is: a way to
    // keep a pass over the whole canvas to the parts that matter.
    void setAutoCommit(bool on) { m_autoCommit = on; }
    bool autoCommit() const { return m_autoCommit; }
    // To nothing, inside `r`. A sparse texture gives up the blocks that end
    // up empty, unless `keepPages` says they will be wanted again at once.
    void clearRect(const QRect& r, bool keepPages = false);

    bool valid() const { return m_id != 0; }
    GLuint id() const { return m_id; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    QSize size() const { return {m_w, m_h}; }
    QRect rect() const { return {0, 0, m_w, m_h}; }
    GLenum format() const { return m_format; }
    qint64 bytes() const;

    void setFilter(GLenum minFilter, GLenum magFilter);
    void setWrap(GLenum wrap);
    void bind(int unit) const;
    void generateMipmaps();

    GLuint fbo();
    void bindAsTarget();   // binds the FBO and sets a full-size viewport
    void clear(float r = 0, float g = 0, float b = 0, float a = 0);

    void upload(const QImage& img, QPoint at = {0, 0});
    QImage readback(const QRect& r, QImage::Format fmt = QImage::Format_RGBA8888_Premultiplied);
    QImage readbackGrey(const QRect& r);                     // the red channel, 8 bits per pixel
    QByteArray readbackRG(const QRect& r);                   // red and green, 2 bytes per pixel
    QByteArray readbackHalf(const QRect& r);                 // RGBA16F, 8 bytes per pixel
    void uploadHalf(const QRect& r, const QByteArray& data); // RGBA16F, 8 bytes per pixel

private:
    friend void beforeDraw();
    void commitRun(int pageX0, int pageX1, int pageY, bool commit);
    qint64 pageBytes(int pageX, int pageY) const;

    GLuint m_id = 0;
    GLuint m_fbo = 0;
    int m_w = 0, m_h = 0;
    int m_levels = 1;
    GLenum m_format = 0;
    bool m_sparse = false;
    bool m_autoCommit = true;
    int m_pagesX = 0, m_pagesY = 0;
    int m_held = 0;
    quint64 m_written = 0;
    qint64 m_bytes = 0;               // what this texture holds on the card right now
    std::vector<uint8_t> m_pages;     // sparse: which blocks have memory
};

// Copies between textures of any kind. Where a sparse source has no memory
// there is nothing to copy, and the destination ends up with nothing there
// either (giving the memory up, if it is sparse too and a whole block is so).
// `keepPages`: the destination keeps the memory of blocks that end up empty,
// wiped: for a working copy that will soon be a copy of something else.
void copyTexture(const Texture& src, Texture& dst, const QRect& srcRect, QPoint dstPos, bool keepPages = false);
void copyTexture(const Texture& src, Texture& dst);
// The same to and from one layer of a texture array, which is where undo
// keeps the tiles a stroke changed.
void copyToLayer(const Texture& src, const QRect& r, GLuint array, int layer);
void copyFromLayer(GLuint array, int layer, Texture& dst, const QRect& r);

class Program {
public:
    Program() = default;
    ~Program();
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    // Paths are qrc paths; the files of each stage are concatenated after the
    // #version line, which lets shaders share helper chunks.
    bool build(const QStringList& vertFiles, const QStringList& fragFiles);
    void destroy();
    void use();
    GLuint id() const { return m_id; }

    void set(const char* name, int v);
    void set(const char* name, float v);
    void set(const char* name, float x, float y);
    void set(const char* name, float x, float y, float z);
    void set(const char* name, float x, float y, float z, float w);
    void setInts(const char* name, int x, int y);   // an ivec2
    void setMat3(const char* name, const float* m9);
    void setFloats(const char* name, const float* values, int count);   // a float array
    void setVec4s(const char* name, const float* values, int count);    // a vec4 array, four floats each

private:
    int loc(const char* name);
    GLuint m_id = 0;
    QHash<QByteArray, int> m_locs;
};

// Packs a QTransform as a column-major mat3 for GLSL (`M * vec3(x, y, 1)`).
void toMat3(const QTransform& t, float out[9]);

// A vertex-buffer-less triangle covering the whole viewport (see fullscreen.vert).
class FullscreenPass {
public:
    void init();
    void destroy();
    void draw();

private:
    GLuint m_vao = 0;
};

// Scissor helper for canvas-space passes that only touch a dirty rectangle.
struct ScissorScope {
    explicit ScissorScope(const QRect& r);
    ~ScissorScope();
};

} // namespace gfx
