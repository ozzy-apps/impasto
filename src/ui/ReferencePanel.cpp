#include "ui/ReferencePanel.h"

#include "io/DocumentIO.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QClipboard>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

namespace {
constexpr double kZoomOut = 0.5, kZoomIn = 12.0;   // relative to the size at which the picture just fits
}

class ReferencePanel::View : public QWidget {
public:
    explicit View(ReferencePanel* panel) : QWidget(panel), m_panel(panel)
    {
        setAcceptDrops(true);
        setMouseTracking(true);
    }

    QPointF toImage(const QPointF& viewPos) const
    {
        const QPointF middle(width() / 2.0, height() / 2.0);
        QPointF d = (viewPos - middle) / m_panel->m_scale;
        if (m_panel->m_mirrored)
            d.setX(-d.x());
        return m_panel->m_centre + d;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform, m_panel->m_scale < 3.0);
        QPainterPath shape;
        shape.addRoundedRect(QRectF(rect()), 9, 9);
        p.setClipPath(shape);
        p.fillRect(rect(), QColor(0x1c, 0x1c, 0x1f));
        if (m_panel->m_image.isNull()) {
            p.setPen(theme::textDim);
            p.setFont(theme::font(13));
            p.drawText(rect().adjusted(24, 0, -24, 0), Qt::AlignCenter | Qt::TextWordWrap,
                       tr("A picture to draw from.\n\nOpen one, paste one, or drop a file here."));
            return;
        }
        p.translate(width() / 2.0, height() / 2.0);
        p.scale(m_panel->m_mirrored ? -m_panel->m_scale : m_panel->m_scale, m_panel->m_scale);
        p.translate(-m_panel->m_centre);
        p.drawImage(QPointF(0, 0), m_panel->m_image);
        p.resetTransform();
        if (m_panel->m_picking && m_hover) {
            // The colour under the pen, before it is taken.
            const QColor c = m_panel->colorAt(mapTo(m_panel, m_at.toPoint()));
            if (c.isValid()) {
                p.setPen(QPen(Qt::white, 2));
                p.setBrush(c);
                p.drawEllipse(m_at + QPointF(18, -18), 13, 13);
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        m_at = e->position();
        if (m_panel->m_image.isNull())
            return;
        const bool pick = m_panel->m_picking || e->button() == Qt::RightButton || (e->modifiers() & Qt::AltModifier);
        if (pick) {
            m_dragPick = true;
            take(e->position());
        } else if (e->button() == Qt::LeftButton) {
            m_dragPan = true;
            m_grab = toImage(e->position());
            setCursor(Qt::ClosedHandCursor);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        m_at = e->position();
        m_hover = true;
        if (m_dragPick) {
            take(e->position());
        } else if (m_dragPan) {
            // Keep the point of the picture that was grabbed under the pen.
            m_panel->m_centre += m_grab - toImage(e->position());
            m_panel->syncZoom();
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (m_dragPick)
            m_panel->m_tools->noteColorUsed();
        m_dragPan = m_dragPick = false;
        setCursor(m_panel->m_picking ? Qt::CrossCursor : Qt::OpenHandCursor);
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = false;
        update();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        if (!m_panel->m_image.isNull())
            m_panel->setScale(m_panel->m_scale * std::pow(1.0015, e->angleDelta().y()), e->position());
        e->accept();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasUrls() || e->mimeData()->hasImage())
            e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override
    {
        if (e->mimeData()->hasImage()) {
            m_panel->setImage(qvariant_cast<QImage>(e->mimeData()->imageData()), tr("Dropped picture"));
        } else if (!e->mimeData()->urls().isEmpty()) {
            m_panel->loadImage(e->mimeData()->urls().first().toLocalFile());
        }
        e->acceptProposedAction();
    }

private:
    void take(const QPointF& at)
    {
        const QColor c = m_panel->colorAt(mapTo(m_panel, at.toPoint()));
        if (c.isValid())
            m_panel->m_tools->setColor(c);
    }

    ReferencePanel* m_panel;
    QPointF m_at, m_grab;
    bool m_hover = false, m_dragPan = false, m_dragPick = false;
};

ReferencePanel::ReferencePanel(ToolState* tools, QWidget* parent) : QWidget(parent), m_tools(tools)
{
    setFixedSize(420, 470);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 12, 12);
    outer->setSpacing(8);

    auto* header = new QHBoxLayout;
    header->setSpacing(2);
    m_title = new QLabel(tr("Reference"), this);
    m_title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(m_title, 1);
    auto textButton = [&](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), this);
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        header->addWidget(b);
        return b;
    };
    IconButton* open = textButton(tr("Open"), tr("Choose a picture to draw from"), "reference-open");
    IconButton* paste = textButton(tr("Paste"), tr("Use the picture on the clipboard"), "reference-paste");
    IconButton* flip = textButton(tr("Flip"), tr("Mirror the picture"), "reference-flip");
    IconButton* fitBtn = textButton(tr("Fit"), tr("Show the whole picture"), "reference-fit");
    m_pick = new IconButton(QStringLiteral("eyedropper"), this, 20);
    m_pick->setObjectName(QStringLiteral("reference-pick"));
    m_pick->setToolTip(tr("Pick colours from the picture (or hold Alt, or right-click)"));
    header->addWidget(m_pick);
    outer->addLayout(header);

    m_view = new View(this);
    m_view->setObjectName(QStringLiteral("reference-view"));
    m_view->setCursor(Qt::OpenHandCursor);
    outer->addWidget(m_view, 1);

    auto* footer = new QHBoxLayout;
    auto* zoomLabel = new QLabel(tr("Zoom"), this);
    zoomLabel->setFont(theme::font(12, QFont::Medium));
    zoomLabel->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    m_zoom = new Slider(this);
    m_zoom->setObjectName(QStringLiteral("reference-zoom"));
    footer->addWidget(zoomLabel);
    footer->addWidget(m_zoom, 1);
    m_remove = new IconButton(QString(), this);
    m_remove->setText(tr("Remove"));
    m_remove->setToolTip(tr("Take the picture away"));
    m_remove->setObjectName(QStringLiteral("reference-remove"));
    footer->addWidget(m_remove);
    outer->addLayout(footer);
    m_flip = flip;
    m_fit = fitBtn;

    connect(open, &QAbstractButton::clicked, this, &ReferencePanel::openImage);
    connect(paste, &QAbstractButton::clicked, this, &ReferencePanel::pasteImage);
    connect(fitBtn, &QAbstractButton::clicked, this, &ReferencePanel::fit);
    connect(m_remove, &QAbstractButton::clicked, this, &ReferencePanel::clearImage);
    connect(flip, &QAbstractButton::clicked, this, [this] {
        m_mirrored = !m_mirrored;
        m_view->update();
    });
    connect(m_pick, &QAbstractButton::clicked, this, [this] {
        m_picking = !m_picking;
        m_pick->setActive(m_picking);
        m_view->setCursor(m_picking ? Qt::CrossCursor : Qt::OpenHandCursor);
    });
    connect(m_zoom, &Slider::valueChanged, this, [this](float v) {
        // The slider runs from half the fitted size to twelve times it, evenly by ratio.
        setScale(m_fitScale * kZoomOut * std::pow(kZoomIn / kZoomOut, double(v)), QPointF(m_view->width() / 2.0, m_view->height() / 2.0));
    });
    syncButtons();
}

// What needs a picture to act on is only there to be pressed while there is one.
void ReferencePanel::syncButtons()
{
    const bool has = !m_image.isNull();
    for (QWidget* w : {static_cast<QWidget*>(m_remove), static_cast<QWidget*>(m_flip), static_cast<QWidget*>(m_fit),
                       static_cast<QWidget*>(m_pick), static_cast<QWidget*>(m_zoom)})
        w->setEnabled(has);
}

void ReferencePanel::clearImage()
{
    if (m_image.isNull())
        return;
    m_image = QImage();
    m_title->setText(tr("Reference"));
    m_mirrored = false;
    m_picking = false;
    m_pick->setActive(false);
    m_view->setCursor(Qt::OpenHandCursor);
    m_scale = m_fitScale = 1.0;
    m_centre = QPointF();
    m_zoom->setValue(0.0f);
    m_restored = true;   // nothing is to be brought back, now or at the next start
    QSettings().remove(QStringLiteral("reference/path"));
    syncButtons();
    m_view->update();
}

QRect ReferencePanel::viewRect() const
{
    return m_view->geometry();
}

QColor ReferencePanel::colorAt(const QPointF& panelPos) const
{
    if (m_image.isNull())
        return {};
    const QPointF at = m_view->toImage(panelPos - m_view->pos());
    const QPoint px(int(std::floor(at.x())), int(std::floor(at.y())));
    if (!m_image.rect().contains(px))
        return {};
    QColor c = m_image.pixelColor(px);
    c.setAlpha(255);
    return c;
}

void ReferencePanel::showEvent(QShowEvent*)
{
    // The picture used last comes back by itself, once.
    if (!m_restored) {
        m_restored = true;
        const QString last = QSettings().value(QStringLiteral("reference/path")).toString();
        if (m_image.isNull() && !last.isEmpty() && QFileInfo::exists(last))
            loadImage(last);
    }
}

bool ReferencePanel::loadImage(const QString& path)
{
    QString error;
    const QImage image = docio::readImage(path, &error);
    if (image.isNull())
        return false;
    setImage(image, QFileInfo(path).completeBaseName());
    QSettings().setValue(QStringLiteral("reference/path"), QFileInfo(path).absoluteFilePath());
    return true;
}

void ReferencePanel::setImage(const QImage& image, const QString& label)
{
    if (image.isNull())
        return;
    // Larger than anyone can look at in a panel only costs memory.
    m_image = image.width() > 4096 || image.height() > 4096 ? image.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
    m_image = m_image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    m_title->setText(label.isEmpty() ? tr("Reference") : m_title->fontMetrics().elidedText(label, Qt::ElideRight, 150));
    m_mirrored = false;
    syncButtons();
    fit();
}

void ReferencePanel::fit()
{
    if (m_image.isNull())
        return;
    const QSize area = m_view->size().isEmpty() ? QSize(394, 360) : m_view->size();
    m_fitScale = std::min(double(area.width()) / m_image.width(), double(area.height()) / m_image.height());
    m_scale = m_fitScale;
    m_centre = QPointF(m_image.width() / 2.0, m_image.height() / 2.0);
    syncZoom();
}

void ReferencePanel::setScale(double scale, const QPointF& about)
{
    if (m_image.isNull())
        return;
    // What is under `about` stays under it.
    const QPointF fixed = m_view->toImage(about);
    m_scale = std::clamp(scale, m_fitScale * kZoomOut, m_fitScale * kZoomIn);
    m_centre += fixed - m_view->toImage(about);
    syncZoom();
}

void ReferencePanel::syncZoom()
{
    // The picture never slides wholly out of view.
    m_centre.setX(std::clamp(m_centre.x(), 0.0, double(m_image.width())));
    m_centre.setY(std::clamp(m_centre.y(), 0.0, double(m_image.height())));
    m_zoom->setValue(float(std::log(m_scale / (m_fitScale * kZoomOut)) / std::log(kZoomIn / kZoomOut)));
    m_view->update();
}

void ReferencePanel::openImage()
{
    const QString dir = QSettings().value(QStringLiteral("reference/folder"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString path = QFileDialog::getOpenFileName(window(), tr("Reference Picture"), dir,
                                                      tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif)"));
    if (path.isEmpty())
        return;
    if (loadImage(path))
        QSettings().setValue(QStringLiteral("reference/folder"), QFileInfo(path).absolutePath());
}

void ReferencePanel::pasteImage()
{
    const QImage image = QGuiApplication::clipboard()->image();
    if (!image.isNull())
        setImage(image, tr("Pasted picture"));
}
