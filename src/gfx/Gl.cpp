#include "gfx/Gl.h"

#include <QDebug>
#include <QFile>
#include <QOpenGLContext>
#include <QOpenGLVersionFunctionsFactory>
#include <atomic>
#include <cmath>

#ifndef GL_TEXTURE_SPARSE_ARB
#define GL_TEXTURE_SPARSE_ARB 0x91A6
#endif
#ifndef GL_VIRTUAL_PAGE_SIZE_INDEX_ARB
#define GL_VIRTUAL_PAGE_SIZE_INDEX_ARB 0x91A7
#endif
#ifndef GL_NUM_VIRTUAL_PAGE_SIZES_ARB
#define GL_NUM_VIRTUAL_PAGE_SIZES_ARB 0x91A8
#endif
#ifndef GL_VIRTUAL_PAGE_SIZE_X_ARB
#define GL_VIRTUAL_PAGE_SIZE_X_ARB 0x9195
#define GL_VIRTUAL_PAGE_SIZE_Y_ARB 0x9196
#endif
#ifndef GL_MAX_SPARSE_TEXTURE_SIZE_ARB
#define GL_MAX_SPARSE_TEXTURE_SIZE_ARB 0x9198
#endif

namespace gfx {

namespace {

// Entry points from beyond OpenGL 4.3 core, looked up for each context.
using TexPageCommitment = void (APIENTRY *)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLboolean);
using TexturePageCommitment = void (APIENTRY *)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLboolean);
using ClearTexSubImage = void (APIENTRY *)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*);
struct Extras {
    bool sparse = false;
    int largestSparse = 0;
    TexPageCommitment commit = nullptr;
    TexturePageCommitment commitNamed = nullptr;   // needs no binding, so it disturbs nothing
    ClearTexSubImage clearSub = nullptr;
};

thread_local QOpenGLContext* t_ctx = nullptr;
thread_local GLF* t_fn = nullptr;
thread_local Extras t_extras;
thread_local Texture* t_target = nullptr;   // the texture bindAsTarget() last made the target
bool s_sparseAllowed = !qEnvironmentVariableIsSet("IMPASTO_NO_SPARSE");
std::atomic<qint64> s_textureBytes{0};

} // namespace

GLF* gl()
{
    QOpenGLContext* ctx = QOpenGLContext::currentContext();
    if (ctx != t_ctx || !t_fn) {
        t_fn = ctx ? QOpenGLVersionFunctionsFactory::get<GLF>(ctx) : nullptr;
        t_ctx = ctx;
        t_target = nullptr;
        t_extras = {};
        if (ctx && t_fn) {
            t_extras.clearSub = ClearTexSubImage(ctx->getProcAddress("glClearTexSubImage"));
            t_extras.commit = TexPageCommitment(ctx->getProcAddress("glTexPageCommitmentARB"));
            t_extras.commitNamed = TexturePageCommitment(ctx->getProcAddress("glTexturePageCommitmentEXT"));
            // Sparse textures alone leave it open what an empty block reads
            // as; the second extension settles it as zero, which is what an
            // empty stretch of a layer has to be.
            if (ctx->hasExtension(QByteArrayLiteral("GL_ARB_sparse_texture")) && ctx->hasExtension(QByteArrayLiteral("GL_ARB_sparse_texture2"))
                && ctx->hasExtension(QByteArrayLiteral("GL_ARB_clear_texture")) && t_extras.clearSub && t_extras.commit) {
                GLint largest = 0;
                t_fn->glGetIntegerv(GL_MAX_SPARSE_TEXTURE_SIZE_ARB, &largest);
                t_extras.largestSparse = largest;
                t_extras.sparse = largest > 0;
            }
        }
    }
    return t_fn;
}

void invalidateGlCache()
{
    t_ctx = nullptr;
    t_fn = nullptr;
    t_target = nullptr;
    t_extras = {};
}

bool sparseSupported()
{
    return gl() && t_extras.sparse;
}

void setSparseAllowed(bool allowed)
{
    s_sparseAllowed = allowed;
}

bool sparseAllowed()
{
    return s_sparseAllowed;
}

qint64 textureBytes()
{
    return s_textureBytes.load();
}

void beforeDraw()
{
    Texture* t = t_target;
    if (!t || !t->m_sparse || !t->m_autoCommit || !t->m_fbo)
        return;
    GLF* f = gl();
    GLint bound = 0;
    f->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
    if (GLuint(bound) != t->m_fbo)
        return;   // something else has become the target since
    QRect r = t->rect();
    if (f->glIsEnabled(GL_SCISSOR_TEST)) {
        GLint box[4] = {0, 0, 0, 0};
        f->glGetIntegerv(GL_SCISSOR_BOX, box);
        r &= QRect(box[0], box[1], box[2], box[3]);
    }
    t->ensure(r);
}

// ---------------------------------------------------------------- Texture

static int bytesPerPixel(GLenum fmt)
{
    switch (fmt) {
    case GL_RGBA16F: return 8;
    case GL_RGBA8: return 4;
    case GL_RG16F: return 4;
    case GL_RG8: return 2;
    case GL_R16F: return 2;
    case GL_R8: return 1;
    default: return 4;
    }
}

// The channels a format has, for calls that want them spelled out.
static GLenum channelsOf(GLenum fmt)
{
    switch (fmt) {
    case GL_R16F:
    case GL_R8: return GL_RED;
    case GL_RG16F:
    case GL_RG8: return GL_RG;
    default: return GL_RGBA;
    }
}

// Whether blocks of kPage pixels can be given and taken for this format: the
// card's own page has to fit into them a whole number of times. `index`
// receives which of the page sizes the card offers does.
static bool pageFits(GLenum fmt, GLint* index)
{
    GLF* f = gl();
    GLint count = 0;
    f->glGetInternalformativ(GL_TEXTURE_2D, fmt, GL_NUM_VIRTUAL_PAGE_SIZES_ARB, 1, &count);
    count = std::min(count, 16);
    GLint xs[16] = {}, ys[16] = {};
    if (count > 0) {
        f->glGetInternalformativ(GL_TEXTURE_2D, fmt, GL_VIRTUAL_PAGE_SIZE_X_ARB, count, xs);
        f->glGetInternalformativ(GL_TEXTURE_2D, fmt, GL_VIRTUAL_PAGE_SIZE_Y_ARB, count, ys);
    }
    for (GLint i = 0; i < count; ++i) {
        if (xs[i] > 0 && ys[i] > 0 && kPage % xs[i] == 0 && kPage % ys[i] == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

Texture::~Texture()
{
    destroy();
}

Texture::Texture(Texture&& o) noexcept
{
    swap(o);
}

Texture& Texture::operator=(Texture&& o) noexcept
{
    if (this != &o) {
        destroy();
        swap(o);
    }
    return *this;
}

void Texture::swap(Texture& o) noexcept
{
    if (t_target == this || t_target == &o)
        t_target = nullptr;
    std::swap(m_id, o.m_id);
    std::swap(m_fbo, o.m_fbo);
    std::swap(m_w, o.m_w);
    std::swap(m_h, o.m_h);
    std::swap(m_levels, o.m_levels);
    std::swap(m_format, o.m_format);
    std::swap(m_sparse, o.m_sparse);
    std::swap(m_autoCommit, o.m_autoCommit);
    std::swap(m_pagesX, o.m_pagesX);
    std::swap(m_pagesY, o.m_pagesY);
    std::swap(m_held, o.m_held);
    std::swap(m_bytes, o.m_bytes);
    m_pages.swap(o.m_pages);
    ++m_written;
    ++o.m_written;
}

void Texture::create(int w, int h, GLenum internalFormat, bool mipmaps, bool sparse)
{
    destroy();
    GLF* f = gl();
    m_w = w;
    m_h = h;
    m_format = internalFormat;
    m_levels = mipmaps ? 1 + int(std::floor(std::log2(double(std::max(w, h))))) : 1;
    m_pagesX = (w + kPage - 1) / kPage;
    m_pagesY = (h + kPage - 1) / kPage;
    GLint pageIndex = 0;
    m_sparse = sparse && !mipmaps && s_sparseAllowed && t_extras.sparse && std::max(w, h) <= t_extras.largestSparse
               && pageFits(internalFormat, &pageIndex);
    f->glGenTextures(1, &m_id);
    f->glBindTexture(GL_TEXTURE_2D, m_id);
    if (m_sparse) {
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SPARSE_ARB, GL_TRUE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_VIRTUAL_PAGE_SIZE_INDEX_ARB, pageIndex);
        m_pages.assign(size_t(m_pagesX) * size_t(m_pagesY), 0);
    }
    f->glTexStorage2D(GL_TEXTURE_2D, m_levels, internalFormat, w, h);
    m_held = 0;
    m_bytes = m_sparse ? 0 : qint64(w) * h * bytesPerPixel(internalFormat) * (m_levels > 1 ? 4 : 3) / 3;
    s_textureBytes += m_bytes;
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Texture::destroy()
{
    if (!m_id && !m_fbo)
        return;
    GLF* f = gl();
    if (!f) {
        qWarning("gfx::Texture leaked: destroyed without a current GL context");
    } else {
        if (m_fbo)
            f->glDeleteFramebuffers(1, &m_fbo);
        if (m_id)
            f->glDeleteTextures(1, &m_id);
    }
    if (t_target == this)
        t_target = nullptr;
    s_textureBytes -= m_bytes;
    m_id = 0;
    m_fbo = 0;
    m_w = m_h = 0;
    m_sparse = false;
    m_autoCommit = true;
    m_pagesX = m_pagesY = m_held = 0;
    m_bytes = 0;
    m_pages.clear();
}

qint64 Texture::bytes() const
{
    return m_bytes;
}

// ---- sparse

QRect Texture::pageRect(int pageX, int pageY) const
{
    const int x = pageX * kPage, y = pageY * kPage;
    return QRect(x, y, std::min(kPage, m_w - x), std::min(kPage, m_h - y));
}

qint64 Texture::pageBytes(int pageX, int pageY) const
{
    const QRect r = pageRect(pageX, pageY);
    return qint64(r.width()) * r.height() * bytesPerPixel(m_format);
}

// Gives memory to, or takes it from, a run of blocks side by side.
void Texture::commitRun(int pageX0, int pageX1, int pageY, bool commit)
{
    const QRect first = pageRect(pageX0, pageY), last = pageRect(pageX1, pageY);
    const int x = first.x(), y = first.y(), w = last.right() + 1 - x, h = first.height();
    const Extras& e = t_extras;
    if (e.commitNamed) {
        e.commitNamed(m_id, 0, x, y, 0, w, h, 1, commit ? GL_TRUE : GL_FALSE);
    } else {
        // This form acts on whatever is bound: put back what a draw being set up had there.
        GLF* f = gl();
        GLint was = 0;
        f->glGetIntegerv(GL_TEXTURE_BINDING_2D, &was);
        f->glBindTexture(GL_TEXTURE_2D, m_id);
        e.commit(GL_TEXTURE_2D, 0, x, y, 0, w, h, 1, commit ? GL_TRUE : GL_FALSE);
        f->glBindTexture(GL_TEXTURE_2D, GLuint(was));
    }
    // Fresh memory holds whatever was last in it.
    if (commit)
        e.clearSub(m_id, 0, x, y, 0, w, h, 1, channelsOf(m_format), GL_FLOAT, nullptr);
    qint64 bytes = 0;
    for (int px = pageX0; px <= pageX1; ++px) {
        m_pages[size_t(pageY) * size_t(m_pagesX) + size_t(px)] = commit ? 1 : 0;
        bytes += pageBytes(px, pageY);
    }
    const int count = pageX1 - pageX0 + 1;
    ++m_written;
    m_held += commit ? count : -count;
    m_bytes += commit ? bytes : -bytes;
    s_textureBytes += commit ? bytes : -bytes;
}

void Texture::ensure(const QRect& rect)
{
    if (!m_sparse)
        return;
    const QRect r = rect & this->rect();
    if (r.isEmpty())
        return;
    const int x0 = r.left() / kPage, x1 = r.right() / kPage, y0 = r.top() / kPage, y1 = r.bottom() / kPage;
    for (int py = y0; py <= y1; ++py) {
        const uint8_t* row = m_pages.data() + size_t(py) * size_t(m_pagesX);
        for (int px = x0; px <= x1; ++px) {
            if (row[px])
                continue;
            int end = px;
            while (end < x1 && !row[end + 1])
                ++end;
            commitRun(px, end, py, true);
            px = end;
        }
    }
}

void Texture::ensureLike(const Texture& like, const QRect& rect)
{
    if (!m_sparse)
        return;
    if (!like.m_sparse || like.m_pagesX != m_pagesX || like.m_pagesY != m_pagesY) {
        ensure(rect);
        return;
    }
    const QRect r = rect & this->rect();
    if (r.isEmpty() || like.m_held == 0)
        return;
    const int x0 = r.left() / kPage, x1 = r.right() / kPage, y0 = r.top() / kPage, y1 = r.bottom() / kPage;
    for (int py = y0; py <= y1; ++py) {
        const uint8_t* row = m_pages.data() + size_t(py) * size_t(m_pagesX);
        const uint8_t* wanted = like.m_pages.data() + size_t(py) * size_t(m_pagesX);
        for (int px = x0; px <= x1; ++px) {
            if (row[px] || !wanted[px])
                continue;
            int end = px;
            while (end < x1 && !row[end + 1] && wanted[end + 1])
                ++end;
            commitRun(px, end, py, true);
            px = end;
        }
    }
}

void Texture::release(const QRect& rect)
{
    if (!m_sparse || m_held == 0)
        return;
    const QRect r = rect & this->rect();
    if (r.isEmpty())
        return;
    // Only blocks that lie in it whole; a block cut short by the texture's
    // edge is whole when the rectangle reaches that edge.
    const int x0 = (r.left() + kPage - 1) / kPage, y0 = (r.top() + kPage - 1) / kPage;
    const int x1 = r.right() + 1 >= m_w ? m_pagesX - 1 : (r.right() + 1) / kPage - 1;
    const int y1 = r.bottom() + 1 >= m_h ? m_pagesY - 1 : (r.bottom() + 1) / kPage - 1;
    for (int py = y0; py <= y1; ++py) {
        const uint8_t* row = m_pages.data() + size_t(py) * size_t(m_pagesX);
        for (int px = x0; px <= x1; ++px) {
            if (!row[px])
                continue;
            int end = px;
            while (end < x1 && row[end + 1])
                ++end;
            commitRun(px, end, py, false);
            px = end;
        }
    }
}

void Texture::releaseAll()
{
    release(rect());
}

QRect Texture::heldBounds() const
{
    if (!m_sparse)
        return rect();
    QRect bounds;
    for (int py = 0; py < m_pagesY; ++py) {
        const uint8_t* row = m_pages.data() + size_t(py) * size_t(m_pagesX);
        int first = -1, last = -1;
        for (int px = 0; px < m_pagesX; ++px) {
            if (row[px]) {
                if (first < 0)
                    first = px;
                last = px;
            }
        }
        if (first >= 0)
            bounds |= QRect(first * kPage, py * kPage, (last - first + 1) * kPage, kPage);
    }
    return bounds & rect();
}

QRect Texture::keepOnly(const QRect& rect)
{
    if (!m_sparse)
        return this->rect();
    const QRect r = rect & this->rect();
    if (r.isEmpty()) {
        releaseAll();
        return {};
    }
    const int x0 = r.left() / kPage, x1 = r.right() / kPage, y0 = r.top() / kPage, y1 = r.bottom() / kPage;
    for (int py = 0; py < m_pagesY; ++py) {
        const uint8_t* row = m_pages.data() + size_t(py) * size_t(m_pagesX);
        const bool inRows = py >= y0 && py <= y1;
        for (int px = 0; px < m_pagesX; ++px) {
            if (!row[px] || (inRows && px >= x0 && px <= x1))
                continue;
            int end = px;
            while (end + 1 < m_pagesX && row[end + 1] && !(inRows && end + 1 >= x0 && end + 1 <= x1))
                ++end;
            commitRun(px, end, py, false);
            px = end;
        }
    }
    return QRect(QPoint(x0 * kPage, y0 * kPage), pageRect(x1, y1).bottomRight());
}

QRect Texture::holdOnly(const QRect& rect)
{
    const QRect area = keepOnly(rect);
    if (m_sparse)
        ensure(area);
    return area;
}

void Texture::matchPages(const Texture& like, int margin)
{
    if (!m_sparse)
        return;
    std::vector<uint8_t> want(m_pages.size(), 0);
    for (int py = 0; py < like.m_pagesY && py < m_pagesY; ++py) {
        for (int px = 0; px < like.m_pagesX && px < m_pagesX; ++px) {
            if (!like.held(px, py))
                continue;
            const QRect reach = like.pageRect(px, py).adjusted(-margin, -margin, margin, margin) & rect();
            for (int qy = reach.top() / kPage; qy <= reach.bottom() / kPage; ++qy) {
                for (int qx = reach.left() / kPage; qx <= reach.right() / kPage; ++qx)
                    want[size_t(qy) * size_t(m_pagesX) + size_t(qx)] = 1;
            }
        }
    }
    for (int py = 0; py < m_pagesY; ++py) {
        const size_t at = size_t(py) * size_t(m_pagesX);
        for (int px = 0; px < m_pagesX; ++px) {
            const bool have = m_pages[at + size_t(px)] != 0, wanted = want[at + size_t(px)] != 0;
            if (have == wanted)
                continue;
            int end = px;
            while (end + 1 < m_pagesX && (m_pages[at + size_t(end) + 1] != 0) == have && (want[at + size_t(end) + 1] != 0) == wanted)
                ++end;
            commitRun(px, end, py, wanted);
            px = end;
        }
    }
}

void Texture::clearRect(const QRect& rect, bool keepPages)
{
    const QRect r = rect & this->rect();
    if (r.isEmpty())
        return;
    const Extras& e = t_extras;
    ++m_written;
    if (!m_sparse) {
        if (e.clearSub) {
            e.clearSub(m_id, 0, r.x(), r.y(), 0, r.width(), r.height(), 1, channelsOf(m_format), GL_FLOAT, nullptr);
            return;
        }
        // The long way round, leaving the drawing state as it was found.
        GLF* f = gl();
        GLint target = 0, box[4] = {0, 0, 0, 0};
        GLfloat colour[4] = {0, 0, 0, 0};
        f->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
        f->glGetIntegerv(GL_SCISSOR_BOX, box);
        f->glGetFloatv(GL_COLOR_CLEAR_VALUE, colour);
        const bool scissored = f->glIsEnabled(GL_SCISSOR_TEST);
        const GLuint own = fbo();
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, own);
        f->glEnable(GL_SCISSOR_TEST);
        f->glScissor(r.x(), r.y(), r.width(), r.height());
        f->glClearColor(0, 0, 0, 0);
        f->glClear(GL_COLOR_BUFFER_BIT);
        f->glClearColor(colour[0], colour[1], colour[2], colour[3]);
        f->glScissor(box[0], box[1], box[2], box[3]);
        if (!scissored)
            f->glDisable(GL_SCISSOR_TEST);
        f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(target));
        return;
    }
    if (!keepPages)
        release(r);
    for (int py = r.top() / kPage; py <= r.bottom() / kPage; ++py) {
        const uint8_t* row = m_pages.data() + size_t(py) * size_t(m_pagesX);
        for (int px = r.left() / kPage; px <= r.right() / kPage; ++px) {
            if (!row[px])
                continue;
            int end = px;
            while (end < r.right() / kPage && row[end + 1])
                ++end;
            const QRect part = (pageRect(px, py) | pageRect(end, py)) & r;
            e.clearSub(m_id, 0, part.x(), part.y(), 0, part.width(), part.height(), 1, channelsOf(m_format), GL_FLOAT, nullptr);
            px = end;
        }
    }
}

void Texture::setFilter(GLenum minFilter, GLenum magFilter)
{
    GLF* f = gl();
    f->glBindTexture(GL_TEXTURE_2D, m_id);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(minFilter));
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(magFilter));
}

void Texture::setWrap(GLenum wrap)
{
    GLF* f = gl();
    f->glBindTexture(GL_TEXTURE_2D, m_id);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GLint(wrap));
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GLint(wrap));
    if (wrap == GL_CLAMP_TO_BORDER) {
        const float zero[4] = {0, 0, 0, 0};
        f->glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, zero);
    }
}

void Texture::bind(int unit) const
{
    GLF* f = gl();
    f->glActiveTexture(GLenum(GL_TEXTURE0 + unit));
    f->glBindTexture(GL_TEXTURE_2D, m_id);
}

void Texture::generateMipmaps()
{
    if (m_levels <= 1)
        return;
    GLF* f = gl();
    f->glBindTexture(GL_TEXTURE_2D, m_id);
    f->glGenerateMipmap(GL_TEXTURE_2D);
}

GLuint Texture::fbo()
{
    if (!m_fbo) {
        GLF* f = gl();
        f->glGenFramebuffers(1, &m_fbo);
        f->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_id, 0);
        if (f->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            qCritical("gfx::Texture: framebuffer incomplete (%dx%d, format 0x%x)", m_w, m_h, m_format);
    }
    return m_fbo;
}

void Texture::bindAsTarget()
{
    GLF* f = gl();
    f->glBindFramebuffer(GL_FRAMEBUFFER, fbo());
    f->glViewport(0, 0, m_w, m_h);
    t_target = this;
    ++m_written;
}

void Texture::clear(float r, float g, float b, float a)
{
    if (m_sparse) {
        // Nothing everywhere is what a sparse texture without memory already is.
        if (r == 0.0f && g == 0.0f && b == 0.0f && a == 0.0f) {
            releaseAll();
            return;
        }
        ensure(rect());
    }
    GLF* f = gl();
    bindAsTarget();
    f->glDisable(GL_SCISSOR_TEST);
    f->glClearColor(r, g, b, a);
    f->glClear(GL_COLOR_BUFFER_BIT);
}

void Texture::upload(const QImage& source, QPoint at)
{
    GLF* f = gl();
    const bool gray = (m_format == GL_R8 || m_format == GL_R16F);
    const QImage img = source.convertToFormat(gray ? QImage::Format_Grayscale8
                                                   : QImage::Format_RGBA8888_Premultiplied);
    const int bpp = gray ? 1 : 4;
    ++m_written;
    ensure(QRect(at, img.size()));
    f->glBindTexture(GL_TEXTURE_2D, m_id);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, int(img.bytesPerLine()) / bpp);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, at.x(), at.y(), img.width(), img.height(),
                       gray ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

QImage Texture::readback(const QRect& r, QImage::Format fmt)
{
    GLF* f = gl();
    QImage img(r.size(), QImage::Format_RGBA8888_Premultiplied);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_PACK_ROW_LENGTH, int(img.bytesPerLine()) / 4);
    f->glReadPixels(r.x(), r.y(), r.width(), r.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    f->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    return fmt == img.format() ? img : img.convertToFormat(fmt);
}

QImage Texture::readbackGrey(const QRect& r)
{
    GLF* f = gl();
    QImage img(r.size(), QImage::Format_Grayscale8);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_PACK_ROW_LENGTH, int(img.bytesPerLine()));
    f->glReadPixels(r.x(), r.y(), r.width(), r.height(), GL_RED, GL_UNSIGNED_BYTE, img.bits());
    f->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    return img;
}

QByteArray Texture::readbackRG(const QRect& r)
{
    GLF* f = gl();
    QByteArray data(qsizetype(r.width()) * r.height() * 2, Qt::Uninitialized);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    f->glReadPixels(r.x(), r.y(), r.width(), r.height(), GL_RG, GL_UNSIGNED_BYTE, data.data());
    return data;
}

QByteArray Texture::readbackHalf(const QRect& r)
{
    GLF* f = gl();
    QByteArray data(qsizetype(r.width()) * r.height() * 8, Qt::Uninitialized);
    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    f->glReadPixels(r.x(), r.y(), r.width(), r.height(), GL_RGBA, GL_HALF_FLOAT, data.data());
    return data;
}

void Texture::uploadHalf(const QRect& r, const QByteArray& data)
{
    if (data.size() < qsizetype(r.width()) * r.height() * 8)
        return;
    GLF* f = gl();
    ++m_written;
    ensure(r);
    f->glBindTexture(GL_TEXTURE_2D, m_id);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    f->glTexSubImage2D(GL_TEXTURE_2D, 0, r.x(), r.y(), r.width(), r.height(), GL_RGBA, GL_HALF_FLOAT,
                       data.constData());
}

void copyTexture(const Texture& src, Texture& dst, const QRect& srcRect, QPoint dstPos, bool keepPages)
{
    if (srcRect.isEmpty())
        return;
    GLF* f = gl();
    dst.markWritten();
    const QPoint shift = dstPos - srcRect.topLeft();
    auto copy = [&](const QRect& part) {
        const QRect to = part.translated(shift);
        dst.ensure(to);
        f->glCopyImageSubData(src.id(), GL_TEXTURE_2D, 0, part.x(), part.y(), 0, dst.id(), GL_TEXTURE_2D, 0, to.x(), to.y(), 0,
                              part.width(), part.height(), 1);
    };
    if (!src.sparse()) {
        copy(srcRect);
        return;
    }
    // Block by block along each row, in runs that are all there or all not.
    for (int py = srcRect.top() / kPage; py <= srcRect.bottom() / kPage; ++py) {
        for (int px = srcRect.left() / kPage; px <= srcRect.right() / kPage; ++px) {
            const bool there = src.held(px, py);
            int end = px;
            while (end < srcRect.right() / kPage && src.held(end + 1, py) == there)
                ++end;
            const QRect part = (src.pageRect(px, py) | src.pageRect(end, py)) & srcRect;
            if (there)
                copy(part);
            else
                dst.clearRect(part.translated(shift), keepPages);
            px = end;
        }
    }
}

void copyToLayer(const Texture& src, const QRect& r, GLuint array, int layer)
{
    if (r.isEmpty())
        return;
    GLF* f = gl();
    for (int py = r.top() / kPage; py <= r.bottom() / kPage; ++py) {
        for (int px = r.left() / kPage; px <= r.right() / kPage; ++px) {
            const QRect part = src.pageRect(px, py) & r;
            const QPoint at = part.topLeft() - r.topLeft();
            if (src.held(px, py)) {
                f->glCopyImageSubData(src.id(), GL_TEXTURE_2D, 0, part.x(), part.y(), 0, array, GL_TEXTURE_2D_ARRAY, 0, at.x(), at.y(), layer,
                                      part.width(), part.height(), 1);
            } else if (t_extras.clearSub) {
                t_extras.clearSub(array, 0, at.x(), at.y(), layer, part.width(), part.height(), 1, GL_RGBA, GL_FLOAT, nullptr);
            }
        }
    }
}

void copyFromLayer(GLuint array, int layer, Texture& dst, const QRect& r)
{
    if (r.isEmpty())
        return;
    dst.markWritten();
    dst.ensure(r);
    gl()->glCopyImageSubData(array, GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, dst.id(), GL_TEXTURE_2D, 0, r.x(), r.y(), 0, r.width(), r.height(), 1);
}

void copyTexture(const Texture& src, Texture& dst)
{
    copyTexture(src, dst, src.rect(), {0, 0});
}

// ---------------------------------------------------------------- Program

static QByteArray readSources(const QStringList& files)
{
    QByteArray out = "#version 430 core\n";
    for (const QString& path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qCritical() << "shader source missing:" << path;
            continue;
        }
        out += file.readAll();
        out += '\n';
    }
    return out;
}

static GLuint compileStage(GLenum type, const QByteArray& src, const QString& label)
{
    GLF* f = gl();
    GLuint s = f->glCreateShader(type);
    const char* ptr = src.constData();
    GLint len = GLint(src.size());
    f->glShaderSource(s, 1, &ptr, &len);
    f->glCompileShader(s);
    GLint ok = 0;
    f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        f->glGetShaderInfoLog(s, sizeof(log), &n, log);
        qCritical().noquote() << "shader compile failed:" << label << "\n" << QByteArray(log, n);
        f->glDeleteShader(s);
        return 0;
    }
    return s;
}

Program::~Program()
{
    destroy();
}

void Program::destroy()
{
    if (m_id && gl())
        gl()->glDeleteProgram(m_id);
    m_id = 0;
    m_locs.clear();
}

bool Program::build(const QStringList& vertFiles, const QStringList& fragFiles)
{
    destroy();
    GLF* f = gl();
    GLuint vs = compileStage(GL_VERTEX_SHADER, readSources(vertFiles), vertFiles.join(QLatin1Char('+')));
    GLuint fs = compileStage(GL_FRAGMENT_SHADER, readSources(fragFiles), fragFiles.join(QLatin1Char('+')));
    if (!vs || !fs)
        return false;
    m_id = f->glCreateProgram();
    f->glAttachShader(m_id, vs);
    f->glAttachShader(m_id, fs);
    f->glLinkProgram(m_id);
    f->glDeleteShader(vs);
    f->glDeleteShader(fs);
    GLint ok = 0;
    f->glGetProgramiv(m_id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        GLsizei n = 0;
        f->glGetProgramInfoLog(m_id, sizeof(log), &n, log);
        qCritical().noquote() << "shader link failed:" << fragFiles.join(QLatin1Char('+')) << "\n"
                              << QByteArray(log, n);
        f->glDeleteProgram(m_id);
        m_id = 0;
        return false;
    }
    return true;
}

void Program::use()
{
    gl()->glUseProgram(m_id);
}

int Program::loc(const char* name)
{
    const QByteArray key(name);
    auto it = m_locs.constFind(key);
    if (it != m_locs.constEnd())
        return it.value();
    int l = gl()->glGetUniformLocation(m_id, name);
    m_locs.insert(key, l);
    return l;
}

void Program::set(const char* name, int v) { gl()->glUniform1i(loc(name), v); }
void Program::set(const char* name, float v) { gl()->glUniform1f(loc(name), v); }
void Program::set(const char* name, float x, float y) { gl()->glUniform2f(loc(name), x, y); }
void Program::set(const char* name, float x, float y, float z) { gl()->glUniform3f(loc(name), x, y, z); }
void Program::set(const char* name, float x, float y, float z, float w) { gl()->glUniform4f(loc(name), x, y, z, w); }
void Program::setInts(const char* name, int x, int y) { gl()->glUniform2i(loc(name), x, y); }
void Program::setMat3(const char* name, const float* m9) { gl()->glUniformMatrix3fv(loc(name), 1, GL_FALSE, m9); }
void Program::setFloats(const char* name, const float* values, int count) { gl()->glUniform1fv(loc(name), count, values); }
void Program::setVec4s(const char* name, const float* values, int count) { gl()->glUniform4fv(loc(name), count, values); }

// ---------------------------------------------------------------- Passes

void toMat3(const QTransform& t, float out[9])
{
    out[0] = float(t.m11()); out[1] = float(t.m12()); out[2] = float(t.m13());
    out[3] = float(t.m21()); out[4] = float(t.m22()); out[5] = float(t.m23());
    out[6] = float(t.m31()); out[7] = float(t.m32()); out[8] = float(t.m33());
}

void FullscreenPass::init()
{
    if (!m_vao)
        gl()->glGenVertexArrays(1, &m_vao);
}

void FullscreenPass::destroy()
{
    if (m_vao && gl())
        gl()->glDeleteVertexArrays(1, &m_vao);
    m_vao = 0;
}

void FullscreenPass::draw()
{
    beforeDraw();
    GLF* f = gl();
    f->glBindVertexArray(m_vao);
    f->glDrawArrays(GL_TRIANGLES, 0, 3);
}

ScissorScope::ScissorScope(const QRect& r)
{
    GLF* f = gl();
    f->glEnable(GL_SCISSOR_TEST);
    f->glScissor(r.x(), r.y(), r.width(), r.height());
}

ScissorScope::~ScissorScope()
{
    gl()->glDisable(GL_SCISSOR_TEST);
}

} // namespace gfx
