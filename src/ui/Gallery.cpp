#include "ui/Gallery.h"

#include "io/DocumentIO.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace {

constexpr int kCardW = 264, kThumbH = 198, kCaptionH = 54, kGap = 26, kMargin = 40;
constexpr int kMaxRecent = 40;

QString suffix()
{
    return QLatin1Char('.') + QLatin1String(docio::kExtension);
}

QString whenText(const QDateTime& t)
{
    const QDate today = QDate::currentDate();
    const QLocale locale;
    if (t.date() == today)
        return QObject::tr("today %1").arg(locale.toString(t.time(), QLocale::ShortFormat));
    if (t.date() == today.addDays(-1))
        return QObject::tr("yesterday %1").arg(locale.toString(t.time(), QLocale::ShortFormat));
    return locale.toString(t.date(), QLocale::ShortFormat);
}

// A name people may type, made safe to be a file name.
QString cleanName(const QString& name)
{
    QString out = name.trimmed();
    if (out.endsWith(suffix(), Qt::CaseInsensitive))
        out.chop(suffix().size());
    for (QChar& c : out) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c.unicode() < 32)
            c = QLatin1Char('-');
    }
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    return out.left(120).trimmed();
}

// `base` in `dir`, or "base 2", "base 3"... whichever is free first.
QString unusedPath(const QString& dir, const QString& base)
{
    const QDir d(dir);
    QString path = d.filePath(base + suffix());
    for (int n = 2; QFileInfo::exists(path); ++n)
        path = d.filePath(QStringLiteral("%1 %2").arg(base).arg(n) + suffix());
    return path;
}

QStringList recentPaths()
{
    return QSettings().value(QStringLiteral("gallery/recent")).toStringList();
}

} // namespace

// The wall itself: cards laid out in as many columns as fit.
class Gallery::Grid : public QWidget {
public:
    explicit Grid(Gallery* gallery) : QWidget(gallery), m_gallery(gallery)
    {
        setMouseTracking(true);
        m_loader.setSingleShot(true);
        m_loader.setInterval(0);
        connect(&m_loader, &QTimer::timeout, this, [this] { loadNext(); });
    }

    struct Item {
        QString path, name;
        QDateTime modified;
        qint64 bytes = 0;
        bool elsewhere = false;   // not in the gallery folder
        bool loaded = false;
        QSize size;
        QPixmap thumb;
    };

    void setItems(QVector<Item> items)
    {
        m_items = std::move(items);
        m_hover = m_pressed = -1;
        // Pictures read before are still good unless the file has changed since.
        for (Item& item : m_items) {
            const auto it = cache().constFind(key(item));
            if (it != cache().constEnd()) {
                item.thumb = it->thumb;
                item.size = it->size;
                item.loaded = true;
            }
        }
        relayout();
        m_loader.start();
    }
    const QVector<Item>& items() const { return m_items; }

    QRect cardRect(int index) const
    {
        const int col = index % m_columns, row = index / m_columns;
        return QRect(m_left + col * (kCardW + kGap), 12 + row * (kThumbH + kCaptionH + kGap), kCardW, kThumbH + kCaptionH);
    }
    QRect menuRect(int index) const
    {
        const QRect c = cardRect(index);
        return QRect(c.right() - 30, c.top() + kThumbH + 8, 30, 30);
    }
    int indexAt(const QPoint& pos) const
    {
        for (int i = 0; i < m_items.size(); ++i) {
            if (cardRect(i).contains(pos))
                return i;
        }
        return -1;
    }
    void relayout()
    {
        const int w = std::max(width(), kCardW + 2 * kMargin);
        m_columns = std::max(1, (w - 2 * kMargin + kGap) / (kCardW + kGap));
        m_left = kMargin;   // in line with the title above
        const int rows = (int(m_items.size()) + m_columns - 1) / m_columns;
        setMinimumHeight(24 + rows * (kThumbH + kCaptionH + kGap));
        update();
    }

protected:
    void resizeEvent(QResizeEvent*) override { relayout(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (m_items.isEmpty()) {
            p.setPen(theme::textDim);
            p.setFont(theme::font(15));
            p.drawText(QRect(0, 60, width(), 80), Qt::AlignHCenter | Qt::AlignTop,
                       tr("Nothing here yet.\nStart a canvas with “New canvas”; what you save lands on this wall."));
            return;
        }
        const qreal dpr = devicePixelRatioF();
        for (int i = 0; i < m_items.size(); ++i) {
            const Item& item = m_items[i];
            const QRect card = cardRect(i);
            if (!card.intersects(rect()))
                continue;
            const QRectF frame(card.left(), card.top(), kCardW, kThumbH);
            const bool open = item.path == m_gallery->m_open;

            // The picture sits on a mat, at its own proportions.
            p.setPen(Qt::NoPen);
            for (int s = 6; s > 0; s -= 2) {
                p.setBrush(QColor(0, 0, 0, 14));
                p.drawRoundedRect(frame.adjusted(-s, -s + 3, s, s + 3), 10 + s, 10 + s);
            }
            QPainterPath mat;
            mat.addRoundedRect(frame, 10, 10);
            p.fillPath(mat, i == m_hover ? QColor(0x3c, 0x3c, 0x41) : QColor(0x30, 0x30, 0x34));
            if (!item.thumb.isNull()) {
                const QSizeF fitted = QSizeF(item.thumb.size()).scaled(frame.size() - QSizeF(20, 20), Qt::KeepAspectRatio);
                const QRectF at(frame.center().x() - fitted.width() / 2, frame.center().y() - fitted.height() / 2, fitted.width(), fitted.height());
                p.fillRect(at.adjusted(1.5, 2.5, 1.5, 2.5), QColor(0, 0, 0, 70));
                p.drawPixmap(at, item.thumb, QRectF(item.thumb.rect()));
            } else if (item.loaded) {
                p.setPen(theme::textDim);
                p.setFont(theme::font(12));
                p.drawText(frame, Qt::AlignCenter, tr("No preview"));
            }
            if (open) {
                p.setPen(QPen(theme::accent, 2.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(frame.adjusted(1, 1, -1, -1), 10, 10);
                const QRectF tag(frame.left() + 10, frame.top() + 10, 86, 22);
                p.setPen(Qt::NoPen);
                p.setBrush(theme::accent);
                p.drawRoundedRect(tag, 11, 11);
                p.setPen(Qt::white);
                p.setFont(theme::font(11, QFont::DemiBold));
                p.drawText(tag, Qt::AlignCenter, tr("On the canvas"));
            }

            const QRect more = menuRect(i);
            p.setFont(theme::font(13, QFont::Medium));
            p.setPen(theme::text);
            const QRectF nameRect(card.left() + 2, frame.bottom() + 9, kCardW - 38, 20);
            p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(item.name, Qt::ElideMiddle, int(nameRect.width())));
            p.setFont(theme::font(11));
            p.setPen(theme::textDim);
            QString detail = whenText(item.modified);
            if (!item.size.isEmpty())
                detail = tr("%1 × %2 · %3").arg(item.size.width()).arg(item.size.height()).arg(detail);
            if (item.elsewhere)
                detail += tr(" · in %1").arg(QFileInfo(item.path).dir().dirName());
            p.drawText(QRectF(card.left() + 2, frame.bottom() + 29, kCardW - 38, 18), Qt::AlignLeft | Qt::AlignVCenter,
                       p.fontMetrics().elidedText(detail, Qt::ElideRight, kCardW - 38));
            p.drawPixmap(more.topLeft() + QPoint(5, 5), theme::icon(QStringLiteral("more"), 20, i == m_hover ? theme::text : theme::textDim, dpr));
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int at = indexAt(e->position().toPoint());
        if (at != m_hover) {
            m_hover = at;
            setCursor(at >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = -1;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        m_pressed = e->button() == Qt::LeftButton ? indexAt(e->position().toPoint()) : -1;
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        // A tap opens: the pen has to come up on the card it went down on.
        const QPoint pos = e->position().toPoint();
        const int at = indexAt(pos);
        const int pressed = m_pressed;
        m_pressed = -1;
        if (e->button() != Qt::LeftButton || at < 0 || at != pressed)
            return;
        const QString path = m_items[at].path;
        if (menuRect(at).contains(pos))
            m_gallery->showArtworkMenu(path, mapToGlobal(menuRect(at).bottomLeft()));
        else
            emit m_gallery->openRequested(path);
    }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        const int at = indexAt(e->pos());
        if (at >= 0)
            m_gallery->showArtworkMenu(m_items[at].path, e->globalPos());
    }

private:
    struct Cached {
        QPixmap thumb;
        QSize size;
    };
    static QHash<QString, Cached>& cache()
    {
        static QHash<QString, Cached> pictures;
        return pictures;
    }
    static QString key(const Item& item)
    {
        return QStringLiteral("%1|%2|%3").arg(item.path).arg(item.modified.toMSecsSinceEpoch()).arg(item.bytes);
    }

    // One picture per turn of the event loop, so a full wall never freezes the window.
    void loadNext()
    {
        for (Item& item : m_items) {
            if (item.loaded)
                continue;
            const docio::Summary summary = docio::peek(item.path);
            item.loaded = true;
            item.size = summary.size;
            if (!summary.preview.isNull())
                item.thumb = QPixmap::fromImage(summary.preview);
            if (cache().size() > 400)
                cache().clear();
            cache().insert(key(item), {item.thumb, item.size});
            update();
            m_loader.start();
            return;
        }
    }

    Gallery* m_gallery;
    QVector<Item> m_items;
    QTimer m_loader;
    int m_columns = 1, m_left = kMargin;
    int m_hover = -1, m_pressed = -1;
};

// ------------------------------------------------------------------ folder

QString Gallery::folder()
{
    const QString fallback = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + QStringLiteral("/Impasto");
    return QSettings().value(QStringLiteral("gallery/folder"), fallback).toString();
}

void Gallery::setFolder(const QString& dir)
{
    QSettings().setValue(QStringLiteral("gallery/folder"), dir);
}

QString Gallery::freshPath(const QString& baseName)
{
    const QString dir = folder();
    QDir().mkpath(dir);
    const QString base = cleanName(baseName);
    return unusedPath(dir, base.isEmpty() ? tr("Untitled Artwork") : base);
}

void Gallery::noteRecent(const QString& path)
{
    const QFileInfo info(path);
    if (!docio::isDocumentPath(info.fileName()) || info.absoluteDir() == QDir(folder()))
        return;
    QStringList recent = recentPaths();
    recent.removeAll(info.absoluteFilePath());
    recent.prepend(info.absoluteFilePath());
    while (recent.size() > kMaxRecent)
        recent.removeLast();
    QSettings().setValue(QStringLiteral("gallery/recent"), recent);
}

// ------------------------------------------------------------------ widget

Gallery::Gallery(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(this);
    header->setFixedHeight(92);
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(kMargin, 18, kMargin - 8, 10);
    h->setSpacing(6);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto* title = new QLabel(tr("Gallery"), header);
    title->setFont(theme::font(28, QFont::Bold));
    m_where = new QLabel(header);
    m_where->setFont(theme::font(12));
    m_where->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    titles->addWidget(title);
    titles->addWidget(m_where);
    h->addLayout(titles);
    h->addStretch();

    auto textButton = [&](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), header);
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        h->addWidget(b, 0, Qt::AlignVCenter);
        return b;
    };
    m_back = textButton(tr("Back to canvas"), tr("Return to the canvas that is open (Esc)"), "gallery-back");
    IconButton* import = textButton(tr("Import"), tr("Open a document or an image from anywhere (Ctrl+O)"), "gallery-import");
    IconButton* create = textButton(tr("New canvas"), tr("Start a new canvas (Ctrl+N)"), "gallery-new");
    create->setActive(true);
    m_more = new IconButton(QStringLiteral("more"), header, 20);
    m_more->setObjectName(QStringLiteral("gallery-menu"));
    m_more->setToolTip(tr("Gallery folder"));
    h->addWidget(m_more, 0, Qt::AlignVCenter);
    outer->addWidget(header);

    m_grid = new Grid(this);
    m_grid->setObjectName(QStringLiteral("gallery-grid"));
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_grid);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_grid->setAutoFillBackground(false);
    outer->addWidget(m_scroll, 1);
    new DragScroller(m_scroll, m_grid);

    connect(m_back, &QAbstractButton::clicked, this, &Gallery::backRequested);
    connect(import, &QAbstractButton::clicked, this, &Gallery::importRequested);
    connect(create, &QAbstractButton::clicked, this, &Gallery::newRequested);
    connect(m_more, &QAbstractButton::clicked, this, &Gallery::showFolderMenu);
}

void Gallery::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), theme::workspace);
}

void Gallery::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape)
        emit backRequested();
    else
        QWidget::keyPressEvent(e);
}

void Gallery::refresh(const QString& openPath)
{
    m_open = openPath.isEmpty() ? QString() : QFileInfo(openPath).absoluteFilePath();
    const QString dir = folder();
    m_where->setText(QDir::toNativeSeparators(dir));

    QVector<Grid::Item> items;
    QStringList seen;
    auto add = [&](const QFileInfo& info, bool elsewhere) {
        const QString path = info.absoluteFilePath();
        if (!info.isFile() || seen.contains(path))
            return;
        seen << path;
        Grid::Item item;
        item.path = path;
        item.name = info.completeBaseName();
        item.modified = info.lastModified();
        item.bytes = info.size();
        item.elsewhere = elsewhere;
        items.push_back(item);
    };
    for (const QFileInfo& info : QDir(dir).entryInfoList({QLatin1Char('*') + suffix(), QStringLiteral("*.") + QLatin1String(docio::kOldExtension)}, QDir::Files))
        add(info, false);
    // Recent documents from elsewhere; the ones that are gone drop off the list.
    QStringList recent = recentPaths(), still;
    for (const QString& path : recent) {
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        still << path;
        add(info, info.absoluteDir() != QDir(dir));
    }
    if (still != recent)
        QSettings().setValue(QStringLiteral("gallery/recent"), still);
    std::stable_sort(items.begin(), items.end(), [](const Grid::Item& a, const Grid::Item& b) { return a.modified > b.modified; });
    m_grid->setItems(items);
}

QStringList Gallery::paths() const
{
    QStringList out;
    for (const Grid::Item& item : m_grid->items())
        out << item.path;
    return out;
}

QRect Gallery::cardRect(const QString& path) const
{
    const QVector<Grid::Item>& items = m_grid->items();
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].path == path)
            return QRect(m_grid->mapTo(this, m_grid->cardRect(i).topLeft()), m_grid->cardRect(i).size());
    }
    return {};
}

QString Gallery::renameArtwork(const QString& path, const QString& name, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return QString();
    };
    const QFileInfo info(path);
    const QString wanted = cleanName(name);
    if (wanted.isEmpty())
        return fail(tr("An artwork needs a name"));
    if (wanted == info.completeBaseName())
        return path;
    const QString target = info.dir().filePath(wanted + suffix());
    // An artwork from before the app was Impasto holds its name as much as a new one does.
    const QString oldTarget = info.dir().filePath(wanted + QLatin1Char('.') + QLatin1String(docio::kOldExtension));
    if (QFileInfo::exists(target) || (QFileInfo::exists(oldTarget) && QFileInfo(oldTarget) != info))
        return fail(tr("There is already an artwork called “%1”").arg(wanted));
    if (!QFile::rename(path, target))
        return fail(tr("Could not rename “%1”").arg(info.completeBaseName()));
    QStringList recent = recentPaths();
    const int at = int(recent.indexOf(info.absoluteFilePath()));
    if (at >= 0) {
        recent[at] = target;
        QSettings().setValue(QStringLiteral("gallery/recent"), recent);
    }
    const bool wasOpen = info.absoluteFilePath() == m_open;
    emit artworkRenamed(info.absoluteFilePath(), target);
    refresh(wasOpen ? target : m_open);
    return target;
}

QString Gallery::duplicateArtwork(const QString& path, QString* error)
{
    const QFileInfo info(path);
    const QString target = unusedPath(info.absolutePath(), tr("%1 copy").arg(info.completeBaseName()));
    if (!QFile::copy(path, target)) {
        if (error)
            *error = tr("Could not copy “%1”").arg(info.completeBaseName());
        return {};
    }
    noteRecent(target);
    refresh(m_open);
    return target;
}

bool Gallery::trashArtwork(const QString& path, QString* error)
{
    const QFileInfo info(path);
    if (info.absoluteFilePath() == m_open) {
        if (error)
            *error = tr("“%1” is on the canvas. Open another artwork first.").arg(info.completeBaseName());
        return false;
    }
    // To the system's trash, where it can be brought back from.
    if (!QFile::moveToTrash(path)) {
        if (error)
            *error = tr("Could not move “%1” to the trash").arg(info.completeBaseName());
        return false;
    }
    refresh(m_open);
    return true;
}

void Gallery::showArtworkMenu(const QString& path, const QPoint& globalPos)
{
    const QFileInfo info(path);
    const QString name = info.completeBaseName();
    QMenu menu(window());
    menu.addAction(tr("Open"), this, [=, this] { emit openRequested(path); });
    menu.addSeparator();
    menu.addAction(tr("Rename…"), this, [=, this] {
        bool ok = false;
        const QString wanted = QInputDialog::getText(window(), tr("Rename Artwork"), tr("Name"), QLineEdit::Normal, name, &ok);
        QString error;
        if (ok && renameArtwork(path, wanted, &error).isEmpty())
            QMessageBox::warning(window(), tr("Rename Artwork"), error);
    });
    menu.addAction(tr("Duplicate"), this, [=, this] {
        QString error;
        if (duplicateArtwork(path, &error).isEmpty())
            QMessageBox::warning(window(), tr("Duplicate"), error);
    });
    menu.addAction(tr("Show in Files"), this, [=] { QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath())); });
    menu.addSeparator();
    QAction* trash = menu.addAction(tr("Move to Trash"), this, [=, this] {
        QString error;
        if (trashArtwork(path, &error))
            emit message(tr("“%1” is in the trash").arg(name));
        else
            QMessageBox::warning(window(), tr("Move to Trash"), error);
    });
    trash->setEnabled(info.absoluteFilePath() != m_open);
    menu.exec(globalPos);
}

void Gallery::showFolderMenu()
{
    QMenu menu(window());
    menu.addAction(tr("Choose Gallery Folder…"), this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(window(), tr("Gallery Folder"), folder());
        if (dir.isEmpty())
            return;
        setFolder(dir);
        refresh(m_open);
    });
    menu.addAction(tr("Show Folder in Files"), this, [] {
        QDir().mkpath(folder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder()));
    });
    menu.addAction(tr("Refresh"), this, [this] { refresh(m_open); });
    menu.exec(m_more->mapToGlobal(QPoint(m_more->width() - 220, m_more->height() + 6)));
}
