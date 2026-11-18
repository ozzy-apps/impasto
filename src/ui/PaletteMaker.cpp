#include "ui/PaletteMaker.h"

#include "color/Palette.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QLineF>
#include <QPainterPath>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int kHeaderHeight = 56;
constexpr int kSideWidth = 300;
const QColor kBackground(0x20, 0x20, 0x23);
const QColor kSide(0x26, 0x26, 0x29);

const char* kPrimaryStyle =
    "QPushButton { background: #0a84ff; color: white; border-radius: 9px; padding: 8px 20px; font-size: 14px; font-weight: 600; }"
    "QPushButton:hover { background: #2b95ff; }"
    "QPushButton:pressed { background: #0a6fd6; }"
    "QPushButton:disabled { background: #3a3a3e; color: #77777d; }";
const char* kSecondaryStyle =
    "QPushButton { background: #3a3a3e; color: #f2f2f7; border-radius: 9px; padding: 8px 20px; font-size: 14px; }"
    "QPushButton:hover { background: #46464b; }"
    "QPushButton:pressed { background: #505056; }"
    "QPushButton:disabled { color: #77777d; }";

QLabel* heading(const QString& text, QWidget* parent)
{
    auto* h = new QLabel(text, parent);
    h->setFont(theme::font(12, QFont::DemiBold));
    h->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    h->setContentsMargins(0, 10, 0, 0);
    return h;
}

QString pictureFilter()
{
    return PaletteMaker::tr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff *.gif);;All files (*)");
}

} // namespace

namespace palettemaker {

// The picture, as large as it fits, with a mark where each colour is.
class PictureView : public QWidget {
public:
    explicit PictureView(QWidget* parent) : QWidget(parent) { setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); }
    void set(const QImage& picture, const QVector<QColor>& colors, const QVector<QPointF>& marks, const QSet<int>& left)
    {
        if (picture.cacheKey() != m_key) {
            m_key = picture.cacheKey();
            // A copy no larger than a screen needs, so painting stays quick.
            m_shown = picture.width() > 2400 || picture.height() > 2400 ? picture.scaled(2400, 2400, Qt::KeepAspectRatio, Qt::SmoothTransformation) : picture;
        }
        m_colors = colors;
        m_marks = marks;
        m_left = left;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (m_shown.isNull()) {
            p.setPen(theme::textDim);
            p.setFont(theme::font(14));
            p.drawText(rect(), Qt::AlignCenter, PaletteMaker::tr("No picture yet"));
            return;
        }
        const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -12);
        const double s = std::min(area.width() / m_shown.width(), area.height() / m_shown.height());
        const QSizeF size(m_shown.width() * s, m_shown.height() * s);
        const QRectF r(area.center().x() - size.width() / 2, area.center().y() - size.height() / 2, size.width(), size.height());
        QPainterPath clip;
        clip.addRoundedRect(r, 8, 8);
        p.save();
        p.setClipPath(clip);
        theme::drawChecker(p, r, 10);
        p.drawImage(r, m_shown);
        p.restore();
        // A mark for each colour, where it was found.
        for (int i = 0; i < m_marks.size() && i < m_colors.size(); ++i) {
            const QPointF c(r.left() + m_marks[i].x() * r.width(), r.top() + m_marks[i].y() * r.height());
            const bool out = m_left.contains(i);
            p.setPen(QPen(QColor(0, 0, 0, 120), 4));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, 9, 9);
            p.setPen(QPen(out ? QColor(255, 255, 255, 110) : Qt::white, 2));
            p.setBrush(out ? QBrush(Qt::NoBrush) : QBrush(m_colors[i]));
            p.drawEllipse(c, 9, 9);
        }
    }

private:
    qint64 m_key = 0;
    QImage m_shown;
    QVector<QColor> m_colors;
    QVector<QPointF> m_marks;
    QSet<int> m_left;
};

// The colours found, in rows, large enough for the pen. A tap leaves one out.
class SwatchStrip : public QWidget {
public:
    static constexpr int kCell = 54;
    std::function<void(int)> tapped;

    explicit SwatchStrip(QWidget* parent) : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
    }
    void set(const QVector<QColor>& colors, const QSet<int>& left)
    {
        const bool resize = colors.size() != m_colors.size();
        m_colors = colors;
        m_left = left;
        if (resize)
            updateGeometry();
        update();
    }
    int columns() const { return std::max(1, width() / kCell); }
    QRect cell(int i) const
    {
        const int c = columns();
        return QRect((i % c) * kCell, (i / c) * kCell, kCell, kCell);
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override
    {
        const int c = std::max(1, w / kCell);
        return std::max(1, int((m_colors.size() + c - 1) / c)) * kCell;
    }
    QSize sizeHint() const override { return {kCell * 12, heightForWidth(kCell * 12)}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < m_colors.size(); ++i) {
            const QRectF r = QRectF(cell(i)).adjusted(4, 4, -4, -4);
            const bool out = m_left.contains(i);
            if (out) {
                p.setPen(QPen(QColor(0x5a, 0x5a, 0x60), 1.4, Qt::DashLine));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 9, 9);
                QColor faint = m_colors[i];
                faint.setAlpha(70);
                p.setPen(Qt::NoPen);
                p.setBrush(faint);
                p.drawRoundedRect(r.adjusted(10, 10, -10, -10), 5, 5);
                continue;
            }
            p.setPen(QPen(QColor(255, 255, 255, 40), 1));
            p.setBrush(m_colors[i]);
            p.drawRoundedRect(r, 9, 9);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        for (int i = 0; i < m_colors.size(); ++i) {
            if (cell(i).contains(e->position().toPoint())) {
                if (tapped)
                    tapped(i);
                return;
            }
        }
    }

private:
    QVector<QColor> m_colors;
    QSet<int> m_left;
};

} // namespace palettemaker

using palettemaker::PictureView;
using palettemaker::SwatchStrip;

PaletteMaker::PaletteMaker(QWidget* host) : QWidget(host)
{
    setObjectName(QStringLiteral("palette-maker"));
    setFocusPolicy(Qt::StrongFocus);
    hide();

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---- header: what this is, and the way out
    auto* header = new QWidget(this);
    header->setFixedHeight(kHeaderHeight);
    auto* top = new QHBoxLayout(header);
    top->setContentsMargins(20, 0, 16, 0);
    top->setSpacing(8);
    auto* mark = new QLabel(header);
    mark->setPixmap(theme::icon(QStringLiteral("picture"), 22, theme::text, devicePixelRatioF()));
    top->addWidget(mark);
    auto* title = new QLabel(tr("Palette from a Picture"), header);
    title->setFont(theme::font(17, QFont::DemiBold));
    top->addWidget(title);
    top->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), header);
    cancel->setObjectName(QStringLiteral("palette-maker-cancel"));
    cancel->setStyleSheet(QLatin1String(kSecondaryStyle));
    m_export = new QPushButton(tr("Save and Export…"), header);
    m_export->setObjectName(QStringLiteral("palette-maker-export"));
    m_export->setStyleSheet(QLatin1String(kSecondaryStyle));
    m_export->setToolTip(tr("Keep the palette, and also write it to a file: GIMP and Krita read .gpl, Procreate reads .swatches"));
    m_save = new QPushButton(tr("Save Palette"), header);
    m_save->setObjectName(QStringLiteral("palette-maker-save"));
    m_save->setStyleSheet(QLatin1String(kPrimaryStyle));
    m_save->setToolTip(tr("Keep it with your palettes, as the one under the pickers"));
    for (QPushButton* b : {cancel, m_export, m_save}) {
        b->setAutoDefault(false);
        b->setMinimumHeight(38);
        top->addWidget(b);
    }
    outer->addWidget(header);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    outer->addLayout(body, 1);

    // ---- down the side: the picture's source, how many colours, the name
    auto* side = new QWidget(this);
    side->setFixedWidth(kSideWidth);
    auto* column = new QVBoxLayout(side);
    column->setContentsMargins(20, 8, 20, 20);
    column->setSpacing(8);
    column->addWidget(heading(tr("Picture"), side));
    m_source = new SegmentBar({tr("Canvas"), tr("Reference"), tr("File")}, side);
    m_source->setObjectName(QStringLiteral("palette-maker-source"));
    m_source->setFixedHeight(34);
    column->addWidget(m_source);
    m_chooseFile = new IconButton(QString(), side);
    m_chooseFile->setText(tr("Choose a Picture…"));
    m_chooseFile->setObjectName(QStringLiteral("palette-maker-file"));
    m_chooseFile->setMinimumHeight(34);
    column->addWidget(m_chooseFile, 0, Qt::AlignLeft);

    column->addWidget(heading(tr("Colours"), side));
    m_count = new ValueField(tr("How many"), side);
    m_count->setObjectName(QStringLiteral("palette-maker-count"));
    m_count->setRange(2, 60);
    m_count->setDragStep(0.12, 1);
    m_count->setStep(1);
    m_count->setValue(24);
    m_count->setToolTip(tr("Drag left or right for fewer or more colours, or tap to type a number"));
    column->addWidget(m_count);
    m_summary = new QLabel(side);
    m_summary->setObjectName(QStringLiteral("palette-maker-summary"));
    m_summary->setFont(theme::font(12));
    m_summary->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    m_summary->setWordWrap(true);
    column->addWidget(m_summary);

    column->addWidget(heading(tr("Name"), side));
    m_name = new QLineEdit(side);
    m_name->setObjectName(QStringLiteral("palette-maker-name"));
    m_name->setMinimumHeight(34);
    m_name->setPlaceholderText(tr("Palette name"));
    column->addWidget(m_name);
    column->addSpacing(10);
    auto* hint = new QLabel(tr("Tap a colour under the picture to leave it out of the palette; tap it again to keep it. The marks on the picture show where each colour was found."), side);
    hint->setWordWrap(true);
    hint->setFont(theme::font(12));
    hint->setStyleSheet(QStringLiteral("color: #77777d;"));
    column->addWidget(hint);
    column->addStretch(1);
    body->addWidget(side);

    // ---- the picture, and the colours found in it
    auto* main = new QVBoxLayout;
    main->setContentsMargins(24, 12, 24, 24);
    main->setSpacing(14);
    m_view = new PictureView(this);
    m_view->setObjectName(QStringLiteral("palette-maker-picture"));
    main->addWidget(m_view, 1);
    m_strip = new SwatchStrip(this);
    m_strip->setObjectName(QStringLiteral("palette-maker-swatches"));
    main->addWidget(m_strip);
    body->addLayout(main, 1);

    connect(cancel, &QPushButton::clicked, this, &PaletteMaker::cancel);
    connect(m_save, &QPushButton::clicked, this, [this] { save(); });
    connect(m_export, &QPushButton::clicked, this, [this] {
        const QString gpl = tr("GIMP palette (*.gpl)"), swatches = tr("Procreate swatches (*.swatches)");
        QString filter = gpl;
        const QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QLatin1Char('/') + name() + QStringLiteral(".gpl");
        QString path = QFileDialog::getSaveFileName(window(), tr("Export Palette"), suggested, gpl + QStringLiteral(";;") + swatches, &filter);
        if (path.isEmpty())
            return;
        if (QFileInfo(path).suffix().isEmpty())
            path += filter == swatches ? QStringLiteral(".swatches") : QStringLiteral(".gpl");
        QString error;
        if (saveAndExport(path, &error).isEmpty() && !error.isEmpty())
            emit message(error);
    });
    connect(m_source, &SegmentBar::currentChanged, this, [this](int s) { setSource(Source(s)); });
    connect(m_chooseFile, &QAbstractButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(window(), tr("Colours from a Picture"),
                                                          QStandardPaths::writableLocation(QStandardPaths::PicturesLocation), pictureFilter());
        QString error;
        if (!path.isEmpty() && !loadFile(path, &error))
            emit message(error);
    });
    connect(m_count, &ValueField::valueChanged, this, [this] { extract(); });
    connect(m_name, &QLineEdit::textEdited, this, [this] { m_nameEdited = true; });
    connect(m_name, &QLineEdit::returnPressed, this, [this] { save(); });
    m_strip->tapped = [this](int i) { toggle(i); };
}

void PaletteMaker::open(Source source)
{
    m_nameEdited = false;
    m_name->clear();
    show();
    raise();
    setFocus();
    setSource(source);
}

void PaletteMaker::setSource(Source source)
{
    m_source->setCurrent(int(source));
    m_chooseFile->setVisible(source == File);
    if (source == Canvas)
        setPicture(canvasImage ? canvasImage() : QImage(), tr("From the Canvas"));
    else if (source == Reference)
        setPicture(referenceImage ? referenceImage() : QImage(), tr("From the Reference"));
    else if (m_pictureName.isEmpty() || m_picture.isNull())
        setPicture({}, QString());
    refresh();
}

void PaletteMaker::setPicture(const QImage& picture, const QString& name)
{
    m_picture = picture;
    m_pictureName = name;
    if (!m_nameEdited)
        m_name->setText(name);
    extract();
}

bool PaletteMaker::loadFile(const QString& path, QString* error)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QImage picture = reader.read();
    if (picture.isNull()) {
        if (error)
            *error = tr("%1 could not be read as a picture.").arg(QFileInfo(path).fileName());
        return false;
    }
    m_source->setCurrent(File);
    m_chooseFile->setVisible(true);
    setPicture(picture, QFileInfo(path).completeBaseName());
    return true;
}

void PaletteMaker::setCount(int count)
{
    m_count->setValue(count);
    extract();
}

int PaletteMaker::count() const
{
    return int(m_count->value());
}

QString PaletteMaker::name() const
{
    const QString n = m_name->text().trimmed();
    return n.isEmpty() ? tr("From a Picture") : n;
}

void PaletteMaker::setName(const QString& name)
{
    m_name->setText(name);
    m_nameEdited = true;
}

void PaletteMaker::toggle(int index)
{
    if (index < 0 || index >= m_colors.size())
        return;
    if (!m_left.remove(index))
        m_left.insert(index);
    refresh();
}

QVector<QColor> PaletteMaker::kept() const
{
    QVector<QColor> out;
    for (int i = 0; i < m_colors.size(); ++i)
        if (!m_left.contains(i))
            out.push_back(m_colors[i]);
    return out;
}

// The colours, and for each the place in the picture most like it.
void PaletteMaker::extract()
{
    m_left.clear();
    m_colors = PaletteLibrary::fromImage(m_picture, count());
    m_marks.clear();
    if (!m_colors.isEmpty()) {
        // Each sampled pixel goes to the colour nearest it; a colour's mark is
        // the pixel of its own nearest the middle of all of them, so it sits
        // on that colour and in the thick of it.
        const int step = std::max(1, int(std::ceil(std::max(m_picture.width(), m_picture.height()) / 200.0)));
        struct Sample {
            QPoint at;
            int colour;
        };
        std::vector<Sample> samples;
        QVector<QPointF> sum(m_colors.size());
        QVector<int> n(m_colors.size(), 0);
        for (int y = step / 2; y < m_picture.height(); y += step) {
            for (int x = step / 2; x < m_picture.width(); x += step) {
                const QRgb px = m_picture.pixel(x, y);
                if (qAlpha(px) < 128)
                    continue;
                int best = 0;
                double bestD = 1e18;
                for (int i = 0; i < m_colors.size(); ++i) {
                    const QColor& c = m_colors[i];
                    // Weighted the way the eye weighs red, green and blue.
                    const double dr = qRed(px) - c.red(), dg = qGreen(px) - c.green(), db = qBlue(px) - c.blue();
                    const double d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                    if (d < bestD) {
                        bestD = d;
                        best = i;
                    }
                }
                samples.push_back({QPoint(x, y), best});
                sum[best] += QPointF(x, y);
                ++n[best];
            }
        }
        m_marks.resize(m_colors.size());
        QVector<double> nearest(m_colors.size(), 1e18);
        for (const Sample& sm : samples) {
            const QPointF middle = sum[sm.colour] / std::max(1, n[sm.colour]);
            const double d = QLineF(middle, QPointF(sm.at)).length();
            if (d < nearest[sm.colour]) {
                nearest[sm.colour] = d;
                m_marks[sm.colour] = QPointF((sm.at.x() + 0.5) / m_picture.width(), (sm.at.y() + 0.5) / m_picture.height());
            }
        }
    }
    refresh();
}

void PaletteMaker::refresh()
{
    const int keptCount = int(kept().size());
    if (m_picture.isNull())
        m_summary->setText(m_source->current() == File ? tr("Choose a picture to take colours from.")
                           : m_source->current() == Reference ? tr("There is no reference picture yet.")
                                                              : tr("There is nothing on the canvas."));
    else if (m_colors.isEmpty())
        m_summary->setText(tr("There are no colours in this picture to take."));
    else if (m_left.isEmpty())
        m_summary->setText(keptCount == 1 ? tr("1 colour") : tr("%1 colours").arg(keptCount));
    else
        m_summary->setText(tr("%1 of %2 colours kept").arg(keptCount).arg(m_colors.size()));
    m_save->setEnabled(keptCount > 0);
    m_export->setEnabled(keptCount > 0);
    m_view->set(m_picture, m_colors, m_marks, m_left);
    m_strip->set(m_colors, m_left);
}

QString PaletteMaker::save()
{
    const QVector<QColor> colors = kept();
    if (colors.isEmpty())
        return {};
    PaletteLibrary& lib = PaletteLibrary::instance();
    const QString id = lib.create(name(), colors);
    if (id.isEmpty())
        return {};
    lib.setCurrent(id);
    lib.save();
    hide();
    emit message(colors.size() == 1 ? tr("Palette “%1” saved, 1 colour").arg(lib.find(id)->name)
                                    : tr("Palette “%1” saved, %2 colours").arg(lib.find(id)->name).arg(colors.size()));
    emit saved(id);
    emit closed();
    return id;
}

QString PaletteMaker::saveAndExport(const QString& path, QString* error)
{
    const QString id = save();
    if (id.isEmpty())
        return {};
    PaletteLibrary& lib = PaletteLibrary::instance();
    QString why;
    const bool ok = QFileInfo(path).suffix().toLower() == QLatin1String("swatches") ? lib.exportSwatches(id, path, &why) : lib.exportGpl(id, path, &why);
    if (!ok) {
        if (error)
            *error = why;
        return {};
    }
    emit message(tr("Palette saved, and exported to %1").arg(QFileInfo(path).fileName()));
    return id;
}

void PaletteMaker::cancel()
{
    hide();
    emit closed();
}

QRect PaletteMaker::swatchRect(int index) const
{
    if (index < 0 || index >= m_colors.size())
        return {};
    return m_strip->cell(index).translated(m_strip->mapTo(this, QPoint(0, 0)));
}

void PaletteMaker::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    p.fillRect(QRect(0, 0, width(), kHeaderHeight), kSide);
    p.fillRect(QRect(0, kHeaderHeight, kSideWidth, height()), kSide);
    p.setPen(QColor(255, 255, 255, 18));
    p.drawLine(0, kHeaderHeight, width(), kHeaderHeight);
    p.drawLine(kSideWidth, kHeaderHeight, kSideWidth, height());
}

void PaletteMaker::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        cancel();
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        save();
        return;
    }
    QWidget::keyPressEvent(e);
}
