#include "ui/Timeline.h"

#include "engine/CanvasEngine.h"
#include "io/AnimationExport.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kThumbH = 64, kLabelH = 18, kGap = 8, kPad = 6;
constexpr int kRestMs = 450;   // the same rest that picks up a swatch or a layer

IconButton* textButton(const QString& label, const QString& tip, const char* name, QWidget* parent)
{
    auto* b = new IconButton(QString(), parent);
    b->setText(label);
    b->setToolTip(tip);
    b->setObjectName(QLatin1String(name));
    return b;
}

QLabel* heading(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(theme::font(17, QFont::DemiBold));
    return label;
}

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(theme::font(13));
    return label;
}

} // namespace

// ------------------------------------------------------------------- strip

FrameStrip::FrameStrip(CanvasEngine* engine, QWidget* parent) : QWidget(parent), m_engine(engine)
{
    setObjectName(QStringLiteral("frame-strip"));
    setMinimumHeight(kThumbH + kLabelH + kPad * 2);
    m_rest.setSingleShot(true);
    connect(&m_rest, &QTimer::timeout, this, [this] {
        if (m_pressed >= 0 && !m_moved) {
            m_lifted = true;
            update();
        }
    });
    connect(m_engine, &CanvasEngine::layersChanged, this, qOverload<>(&QWidget::update));
    connect(m_engine, &CanvasEngine::animationChanged, this, qOverload<>(&QWidget::update));
    connect(m_engine, &CanvasEngine::damaged, this, [this] {
        if (m_engine->playbackFrame() >= 0)
            update();   // the frame playing is marked
    });
}

int FrameStrip::cellWidth() const
{
    const QSize canvas = m_engine->canvasSize();
    const double aspect = canvas.height() > 0 ? double(canvas.width()) / canvas.height() : 1.0;
    return std::clamp(int(std::lround(kThumbH * aspect)), 44, 128) + kPad * 2;
}

QRectF FrameStrip::cellRect(int index) const
{
    return QRectF(kGap + index * (cellWidth() + kGap) - m_scroll, kPad, cellWidth(), kThumbH + kLabelH + kPad);
}

int FrameStrip::indexAt(const QPointF& pos) const
{
    const int n = int(m_engine->frames().size());
    for (int i = 0; i < n; ++i) {
        if (cellRect(i).contains(pos))
            return i;
    }
    return -1;
}

int FrameStrip::dropSlot(qreal x) const
{
    const int n = int(m_engine->frames().size());
    const qreal step = cellWidth() + kGap;
    return std::clamp(int(std::lround((x + m_scroll - kGap / 2.0) / step)), 0, n);
}

void FrameStrip::clampScroll()
{
    const int n = int(m_engine->frames().size());
    const qreal content = kGap + n * (cellWidth() + kGap);
    m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, content - width()));
}

void FrameStrip::scrollToFrame(int index)
{
    const QRectF r = cellRect(index);
    if (r.left() < 0)
        m_scroll += r.left() - kGap;
    else if (r.right() > width())
        m_scroll += r.right() - width() + kGap;
    clampScroll();
    update();
}

void FrameStrip::resizeEvent(QResizeEvent*)
{
    clampScroll();
}

const QImage& FrameStrip::thumbnail(int index, int id, quint64 key)
{
    Thumb& t = m_thumbs[id];
    if (t.image.isNull() || t.key != key) {
        t.image = m_engine->frameThumbnail(index, std::max(kThumbH, cellWidth()) * 2);
        t.key = key;
    }
    return t.image;
}

void FrameStrip::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const QVector<int> frames = m_engine->frames();
    const QVector<LayerInfo> layers = m_engine->layers();
    // What each frame's thumbnail depends on: the revisions of everything in it, and where it stands.
    QHash<int, int> parentOf;
    for (const LayerInfo& l : layers)
        parentOf.insert(l.id, l.parent);
    QHash<int, quint64> keyOf;
    int currentBase = 0;
    for (const LayerInfo& l : layers) {
        int top = l.id;
        while (parentOf.value(top))
            top = parentOf.value(top);
        if (l.parent == 0 && !(l.clipping && !l.isGroup && currentBase))
            currentBase = l.id;
        const int frame = (l.parent == 0) ? currentBase : top;
        keyOf[frame] = keyOf.value(frame) * 1000003u + l.revision + quint64(l.visible) * 7u + quint64(l.opacity * 1000.0f);
    }
    const int current = m_engine->currentFrame();
    const int playing = m_engine->playbackFrame();
    const Animation& anim = m_engine->animation();
    const QColor paper = m_engine->backgroundVisible() ? m_engine->backgroundColor() : QColor(70, 70, 76);
    for (int i = 0; i < frames.size(); ++i) {
        const QRectF cell = cellRect(i);
        if (cell.right() < 0 || cell.left() > width())
            continue;
        const LayerInfo info = m_engine->layerInfo(frames[i]);
        const QRectF thumbBox(cell.left() + kPad, cell.top(), cell.width() - kPad * 2, kThumbH);
        const bool lifted = m_lifted && i == m_pressed;
        p.setOpacity(lifted ? 0.4 : (info.visible ? 1.0 : 0.45));
        p.setPen(Qt::NoPen);
        p.setBrush(paper);
        p.drawRoundedRect(thumbBox, 4, 4);
        const QImage& img = thumbnail(i, frames[i], keyOf.value(frames[i]));
        if (!img.isNull()) {
            const QSizeF fit = QSizeF(img.size()).scaled(thumbBox.size(), Qt::KeepAspectRatio);
            p.drawImage(QRectF(thumbBox.center() - QPointF(fit.width() / 2, fit.height() / 2), fit), img);
        }
        const bool isCurrent = (playing >= 0 ? playing : current) == i;
        p.setBrush(Qt::NoBrush);
        p.setPen(isCurrent ? QPen(theme::accent, 2.5) : QPen(QColor(255, 255, 255, 40), 1));
        p.drawRoundedRect(thumbBox.adjusted(-1, -1, 1, 1), 5, 5);
        // Its number, how long it is held, and whether it stays under or over the rest.
        QString label = QString::number(i + 1);
        if (info.hold > 1)
            label += QStringLiteral("  ×%1").arg(info.hold);
        if (anim.background && i == 0)
            label += tr("  bg");
        if (anim.foreground && i == frames.size() - 1 && frames.size() > 1)
            label += tr("  fg");
        p.setOpacity(1.0);
        p.setFont(theme::font(11, isCurrent ? QFont::DemiBold : QFont::Normal));
        p.setPen(isCurrent ? theme::text : theme::textDim);
        p.drawText(QRectF(cell.left(), thumbBox.bottom() + 2, cell.width(), kLabelH), Qt::AlignCenter, label);
    }
    if (m_lifted && m_pressed >= 0) {
        // Where it would go, and the frame itself under the pen.
        const int slot = dropSlot(m_lastPos.x());
        const qreal x = kGap / 2.0 + slot * (cellWidth() + kGap) - m_scroll;
        p.setPen(QPen(theme::accent, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(x, kPad), QPointF(x, kPad + kThumbH));
        const QImage& img = m_thumbs.value(frames.value(m_pressed)).image;
        if (!img.isNull()) {
            const QSizeF fit = QSizeF(img.size()).scaled(QSizeF(cellWidth() - kPad * 2, kThumbH), Qt::KeepAspectRatio);
            p.setOpacity(0.85);
            p.drawImage(QRectF(m_lastPos - QPointF(fit.width() / 2, fit.height() / 2), fit), img);
        }
    }
}

void FrameStrip::mousePressEvent(QMouseEvent* e)
{
    m_pressed = indexAt(e->position());
    m_pressPos = m_lastPos = e->position();
    m_moved = m_lifted = false;
    if (m_pressed >= 0)
        m_rest.start(kRestMs);
}

void FrameStrip::mouseMoveEvent(QMouseEvent* e)
{
    if (!(e->buttons() & Qt::LeftButton))
        return;
    const QPointF delta = e->position() - m_lastPos;
    m_lastPos = e->position();
    if (m_lifted) {
        // Carried to an end, the row scrolls along.
        if (m_lastPos.x() < 30)
            m_scroll -= 12;
        else if (m_lastPos.x() > width() - 30)
            m_scroll += 12;
        clampScroll();
        update();
        return;
    }
    if (!m_moved && QLineF(e->position(), m_pressPos).length() > 6.0) {
        m_moved = true;
        m_rest.stop();
    }
    if (m_moved) {
        m_scroll -= delta.x();
        clampScroll();
        update();
    }
}

void FrameStrip::mouseReleaseEvent(QMouseEvent* e)
{
    m_rest.stop();
    const QVector<int> frames = m_engine->frames();
    if (m_lifted && m_pressed >= 0 && m_pressed < frames.size()) {
        const int slot = dropSlot(e->position().x());
        if (slot != m_pressed && slot != m_pressed + 1) {
            // Directly below the frame now in that place, or on top of everything.
            const int above = slot < frames.size() ? frames[slot] : 0;
            m_engine->moveLayer(frames[m_pressed], 0, above);
            m_engine->showFrame(slot > m_pressed ? slot - 1 : slot);
        }
    } else if (!m_moved && m_pressed >= 0 && indexAt(e->position()) == m_pressed) {
        if (m_pressed == m_engine->currentFrame() && m_engine->playbackFrame() < 0)
            emit optionsRequested(m_pressed, cellRect(m_pressed).toRect().translated(pos()));
        else
            m_engine->showFrame(m_pressed);
    }
    m_pressed = -1;
    m_moved = m_lifted = false;
    update();
}

void FrameStrip::wheelEvent(QWheelEvent* e)
{
    const QPoint d = e->angleDelta();
    m_scroll -= (std::abs(d.x()) > std::abs(d.y()) ? d.x() : d.y()) / 2.0;
    clampScroll();
    update();
}

// -------------------------------------------------------------------- bar

TimelineBar::TimelineBar(CanvasEngine* engine, QWidget* host) : Panel(host, 16), m_engine(engine), m_host(host)
{
    setObjectName(QStringLiteral("timeline"));
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(12, 8, 12, 8);
    l->setSpacing(6);

    auto* controls = new QVBoxLayout;
    controls->setSpacing(4);
    m_playBtn = new IconButton(QStringLiteral("play"), this, 22);
    m_playBtn->setObjectName(QStringLiteral("timeline-play"));
    controls->addWidget(m_playBtn, 0, Qt::AlignHCenter);
    m_settingsBtn = textButton(tr("Settings"), tr("Frame rate, playback and onion skins"), "timeline-settings", this);
    controls->addWidget(m_settingsBtn);
    m_exportBtn = textButton(tr("Export"), tr("Animated GIF, animated PNG, video or a PNG for every frame"), "timeline-export", this);
    controls->addWidget(m_exportBtn);
    l->addLayout(controls);

    m_strip = new FrameStrip(engine, this);
    l->addWidget(m_strip, 1);

    m_addBtn = new IconButton(QStringLiteral("plus"), this, 22);
    m_addBtn->setObjectName(QStringLiteral("timeline-add"));
    m_addBtn->setToolTip(tr("Add a frame after this one"));
    l->addWidget(m_addBtn);

    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &TimelineBar::tick);
    connect(m_playBtn, &QAbstractButton::clicked, this, [this] { play(!playing()); });
    connect(m_addBtn, &QAbstractButton::clicked, this, [this] {
        play(false);
        if (m_engine->addFrame())
            m_strip->scrollToFrame(m_engine->currentFrame());
    });
    connect(m_settingsBtn, &QAbstractButton::clicked, this, &TimelineBar::openSettings);
    connect(m_exportBtn, &QAbstractButton::clicked, this, &TimelineBar::openExport);
    connect(m_strip, &FrameStrip::optionsRequested, this, [this](int index) { openFrameOptions(index); });
    // Drawing, choosing a layer or changing the stack stops playback: the work is on one frame again.
    connect(m_engine, &CanvasEngine::layersChanged, this, [this] {
        if (playing())
            play(false);
    });
    connect(m_engine, &CanvasEngine::animationChanged, this, [this] {
        if (!m_engine->animation().enabled)
            play(false);
    });
    syncPlayButton();
}

QVector<int> TimelineBar::playOrder() const
{
    const QVector<int> frames = m_engine->frames();
    const int n = int(frames.size());
    QVector<int> order;
    for (int i = 0; i < n; ++i)
        order.push_back(i);
    if (m_engine->animation().playback == Animation::Playback::PingPong) {
        for (int i = n - 2; i >= 1; --i)
            order.push_back(i);
    }
    QVector<int> out;
    for (int i : order) {
        for (int k = 0; k < m_engine->layerInfo(frames[i]).hold; ++k)
            out.push_back(i);
    }
    return out;
}

void TimelineBar::play(bool on)
{
    if (on == playing())
        return;
    if (on) {
        m_sequence = playOrder();
        if (m_sequence.isEmpty())
            return;
        // From the frame being worked on, unless a one-shot would have nothing left to play.
        const int current = m_engine->currentFrame();
        m_position = std::max(0, int(m_sequence.indexOf(current)));
        if (m_engine->animation().playback == Animation::Playback::OneShot && m_position >= m_sequence.size() - 1)
            m_position = 0;
        m_engine->setPlaybackFrame(m_sequence[m_position]);
        m_timer.start(std::max(1, int(std::lround(1000.0 / std::max(1, m_engine->animation().fps)))));
    } else {
        m_timer.stop();
        m_engine->setPlaybackFrame(-1);
    }
    syncPlayButton();
    m_strip->update();
}

void TimelineBar::tick()
{
    if (++m_position >= m_sequence.size()) {
        if (m_engine->animation().playback == Animation::Playback::OneShot) {
            play(false);
            return;
        }
        m_position = 0;
    }
    m_engine->setPlaybackFrame(m_sequence[m_position]);
}

void TimelineBar::syncPlayButton()
{
    m_playBtn->setIconName(playing() ? QStringLiteral("pause") : QStringLiteral("play"));
    m_playBtn->setActive(playing());
    m_playBtn->setToolTip(playing() ? tr("Stop") : tr("Play"));
}

// Panels stand on the top edge of the bar, over the button that opened them,
// so that none of them covers the frames.
QWidget* TimelineBar::edgeAnchor(QWidget* button)
{
    auto* anchor = findChild<QWidget*>(QStringLiteral("edge-anchor-") + button->objectName(), Qt::FindDirectChildrenOnly);
    if (!anchor) {
        anchor = new QWidget(this);
        anchor->setObjectName(QStringLiteral("edge-anchor-") + button->objectName());
        anchor->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    anchor->setGeometry(button->mapTo(this, QPoint(0, 0)).x(), 0, button->width(), 1);
    return anchor;
}

// ---------------------------------------------------------------- settings

void TimelineBar::openSettings()
{
    if (!m_settingsPop) {
        m_settingsPop = new Popover(m_host);
        m_settingsPop->setObjectName(QStringLiteral("timeline-settings-popover"));
        m_settingsPop->setOpensUpward(true);
        auto* panel = new QWidget;
        panel->setFixedWidth(430);
        auto* v = new QVBoxLayout(panel);
        v->setContentsMargins(16, 6, 14, 14);
        v->setSpacing(8);
        v->addWidget(heading(tr("Animation"), panel));
        auto* playback = new SegmentBar({tr("Loop"), tr("Ping-Pong"), tr("One Shot")}, panel);
        playback->setObjectName(QStringLiteral("animation-playback"));
        v->addWidget(playback);
        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(10);
        grid->setColumnStretch(1, 1);
        struct Row {
            Slider* slider;
            QLabel* value;
        };
        auto row = [&](int r, const QString& label, const char* name) {
            grid->addWidget(caption(label, panel), r, 0);
            auto* s = new Slider(panel);
            s->setObjectName(QLatin1String(name));
            grid->addWidget(s, r, 1);
            auto* value = new QLabel(panel);
            value->setFont(theme::font(12));
            value->setMinimumWidth(40);
            value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(value, r, 2);
            return Row{s, value};
        };
        const Row fps = row(0, tr("Frames per second"), "animation-fps");
        const Row onion = row(1, tr("Onion skin frames"), "animation-onion-frames");
        const Row opacity = row(2, tr("Onion skin opacity"), "animation-onion-opacity");
        v->addLayout(grid);
        auto* toggles = new QHBoxLayout;
        IconButton* tint = textButton(tr("Colour onion skins"), tr("Earlier frames red, later frames green"), "animation-tint", panel);
        toggles->addWidget(tint);
        toggles->addStretch();
        v->addLayout(toggles);
        auto* layers = new QHBoxLayout;
        IconButton* background = textButton(tr("First frame as background"), tr("The first frame stays under every other"), "animation-background", panel);
        IconButton* foreground = textButton(tr("Last frame as foreground"), tr("The last frame stays over every other"), "animation-foreground", panel);
        layers->addWidget(background);
        layers->addWidget(foreground);
        layers->addStretch();
        v->addLayout(layers);
        m_settingsPop->setContent(panel);

        auto sync = [=, this] {
            const Animation& a = m_engine->animation();
            playback->setCurrent(int(a.playback));
            fps.slider->setValue(float(a.fps - 1) / 59.0f);
            fps.value->setText(QString::number(a.fps));
            onion.slider->setValue(float(a.onionFrames) / 12.0f);
            onion.value->setText(QString::number(a.onionFrames));
            opacity.slider->setValue(a.onionOpacity);
            opacity.value->setText(QStringLiteral("%1%").arg(qRound(a.onionOpacity * 100)));
            tint->setActive(a.tintOnion);
            background->setActive(a.background);
            foreground->setActive(a.foreground);
        };
        auto change = [this](const std::function<void(Animation&)>& edit) {
            Animation a = m_engine->animation();
            edit(a);
            m_engine->setAnimation(a);
            if (playing()) {
                // The new rate or order from here on.
                play(false);
                play(true);
            }
        };
        connect(playback, &SegmentBar::currentChanged, panel, [=](int i) { change([&](Animation& a) { a.playback = Animation::Playback(i); }); });
        connect(fps.slider, &Slider::valueChanged, panel, [=](float v) { change([&](Animation& a) { a.fps = 1 + int(std::lround(v * 59.0f)); }); });
        connect(onion.slider, &Slider::valueChanged, panel, [=](float v) { change([&](Animation& a) { a.onionFrames = int(std::lround(v * 12.0f)); }); });
        connect(opacity.slider, &Slider::valueChanged, panel, [=](float v) { change([&](Animation& a) { a.onionOpacity = v; }); });
        connect(tint, &QAbstractButton::clicked, panel, [=] { change([](Animation& a) { a.tintOnion = !a.tintOnion; }); });
        connect(background, &QAbstractButton::clicked, panel, [=] { change([](Animation& a) { a.background = !a.background; }); });
        connect(foreground, &QAbstractButton::clicked, panel, [=] { change([](Animation& a) { a.foreground = !a.foreground; }); });
        connect(m_engine, &CanvasEngine::animationChanged, panel, sync);
        sync();
    }
    m_settingsPop->popup(edgeAnchor(m_settingsBtn));
}

// ------------------------------------------------------------ frame options

void TimelineBar::openFrameOptions(int index)
{
    const QVector<int> frames = m_engine->frames();
    if (index < 0 || index >= frames.size())
        return;
    if (m_framePop)
        m_framePop->deleteLater();
    m_framePop = new Popover(m_host);
    m_framePop->setObjectName(QStringLiteral("frame-options-popover"));
    m_framePop->setOpensUpward(true);
    auto* panel = new QWidget;
    panel->setFixedWidth(300);
    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(16, 6, 14, 14);
    v->setSpacing(10);
    v->addWidget(heading(tr("Frame %1").arg(index + 1), panel));
    auto* holdRow = new QHBoxLayout;
    holdRow->addWidget(caption(tr("Hold"), panel));
    holdRow->addStretch();
    auto* less = textButton(QStringLiteral("−"), tr("Shorter"), "frame-hold-less", panel);
    auto* value = new QLabel(panel);
    value->setObjectName(QStringLiteral("frame-hold-value"));
    value->setFont(theme::font(13, QFont::Medium));
    value->setMinimumWidth(70);
    value->setAlignment(Qt::AlignCenter);
    auto* more = textButton(QStringLiteral("+"), tr("Longer"), "frame-hold-more", panel);
    holdRow->addWidget(less);
    holdRow->addWidget(value);
    holdRow->addWidget(more);
    v->addLayout(holdRow);
    auto* actions = new QHBoxLayout;
    IconButton* duplicate = textButton(tr("Duplicate"), tr("A copy of this frame, straight after it"), "frame-duplicate", panel);
    IconButton* remove = textButton(tr("Delete"), tr("Take this frame out"), "frame-delete", panel);
    actions->addWidget(duplicate);
    actions->addWidget(remove);
    actions->addStretch();
    v->addLayout(actions);
    m_framePop->setContent(panel);

    const int id = frames[index];
    auto syncHold = [=, this] {
        const int hold = m_engine->layerInfo(id).hold;
        value->setText(hold == 1 ? tr("1 frame") : tr("%1 frames").arg(hold));
    };
    connect(less, &QAbstractButton::clicked, panel, [=, this] {
        m_engine->setFrameHold(id, m_engine->layerInfo(id).hold - 1);
        syncHold();
    });
    connect(more, &QAbstractButton::clicked, panel, [=, this] {
        m_engine->setFrameHold(id, m_engine->layerInfo(id).hold + 1);
        syncHold();
    });
    connect(duplicate, &QAbstractButton::clicked, panel, [=, this] {
        m_framePop->dismiss();
        m_engine->duplicateLayer(id);
    });
    connect(remove, &QAbstractButton::clicked, panel, [=, this] {
        m_framePop->dismiss();
        if (m_engine->frames().size() <= 1) {
            emit message(tr("An animation needs one frame at least"));
            return;
        }
        m_engine->removeLayer(id);
        m_engine->showFrame(std::max(0, index - 1));
    });
    syncHold();
    // An anchor at the frame's own cell, so the panel points at it.
    auto* anchor = new QWidget(this);
    anchor->setAttribute(Qt::WA_TransparentForMouseEvents);
    anchor->setGeometry(m_strip->cellRect(index).toRect().translated(m_strip->pos()));
    connect(m_framePop, &QObject::destroyed, anchor, &QObject::deleteLater);
    m_framePop->popup(anchor);
}

// ------------------------------------------------------------------ export

void TimelineBar::openExport()
{
    if (!m_exportPop) {
        m_exportPop = new Popover(m_host);
        m_exportPop->setObjectName(QStringLiteral("timeline-export-popover"));
        m_exportPop->setOpensUpward(true);
        auto* panel = new QWidget;
        panel->setFixedWidth(340);
        auto* v = new QVBoxLayout(panel);
        v->setContentsMargins(16, 6, 14, 14);
        v->setSpacing(10);
        v->addWidget(heading(tr("Export Animation"), panel));
        auto* size = new SegmentBar({tr("Full size"), tr("Half"), tr("Web, 1080 px")}, panel);
        size->setObjectName(QStringLiteral("export-size"));
        size->setCurrent(QSettings().value(QStringLiteral("animation/exportSize"), 0).toInt());
        v->addWidget(size);
        auto* formats = new QGridLayout;
        formats->setSpacing(6);
        IconButton* gif = textButton(tr("Animated GIF"), tr("Plays anywhere; 256 colours"), "export-gif", panel);
        IconButton* png = textButton(tr("Animated PNG"), tr("Every colour and see-through parts; plays in web browsers"), "export-apng", panel);
        IconButton* video = textButton(tr("MP4 video"), tr("H.264 video, through ffmpeg"), "export-video", panel);
        IconButton* frames = textButton(tr("PNG frames"), tr("A numbered picture for every frame"), "export-frames", panel);
        formats->addWidget(gif, 0, 0);
        formats->addWidget(png, 0, 1);
        formats->addWidget(video, 1, 0);
        formats->addWidget(frames, 1, 1);
        v->addLayout(formats);
        if (animio::ffmpegPath().isEmpty()) {
            video->setEnabled(false);
            video->setToolTip(tr("Video needs ffmpeg: install it and start Impasto again"));
        }
        m_exportPop->setContent(panel);
        connect(size, &SegmentBar::currentChanged, panel, [](int i) { QSettings().setValue(QStringLiteral("animation/exportSize"), i); });
        auto go = [=, this](Format format) {
            m_exportPop->dismiss();
            const QString ext = format == Format::Gif ? QStringLiteral("gif") : format == Format::Video ? QStringLiteral("mp4") : QStringLiteral("png");
            const QString filter = format == Format::Gif ? tr("Animated GIF (*.gif)") : format == Format::Video ? tr("MP4 video (*.mp4)")
                                   : format == Format::Png ? tr("Animated PNG (*.png *.apng)") : tr("PNG frames (*.png)");
            QSettings st;
            const QString dir = st.value(QStringLiteral("paths/export"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
            QString path = QFileDialog::getSaveFileName(window(), tr("Export Animation"), dir + QStringLiteral("/Animation.") + ext, filter);
            if (path.isEmpty())
                return;
            if (QFileInfo(path).suffix().isEmpty())
                path += QLatin1Char('.') + ext;
            st.setValue(QStringLiteral("paths/export"), QFileInfo(path).absolutePath());
            const int sizeChoice = size->current();
            const QSize canvas = m_engine->canvasSize();
            const double scale = sizeChoice == 1 ? 0.5 : sizeChoice == 2 ? std::min(1.0, 1080.0 / std::max(canvas.width(), canvas.height())) : 1.0;
            QString error;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            const bool ok = exportAnimation(format, path, scale, &error);
            QApplication::restoreOverrideCursor();
            emit message(ok ? tr("Exported %1").arg(QFileInfo(path).fileName()) : error);
        };
        connect(gif, &QAbstractButton::clicked, panel, [=] { go(Format::Gif); });
        connect(png, &QAbstractButton::clicked, panel, [=] { go(Format::Png); });
        connect(video, &QAbstractButton::clicked, panel, [=] { go(Format::Video); });
        connect(frames, &QAbstractButton::clicked, panel, [=] { go(Format::Sequence); });
    }
    m_exportPop->popup(edgeAnchor(m_exportBtn));
}

bool TimelineBar::exportAnimation(Format format, const QString& path, double scale, QString* error)
{
    play(false);
    const QVector<int> frames = m_engine->frames();
    if (frames.isEmpty()) {
        if (error)
            *error = tr("There are no frames to export");
        return false;
    }
    // Rendered once each, in the order they play.
    QVector<QImage> rendered;
    for (int i = 0; i < frames.size(); ++i) {
        QImage img = m_engine->renderFrame(i);
        if (scale < 0.999)
            img = img.scaled(QSize(std::max(1, int(std::lround(img.width() * scale))), std::max(1, int(std::lround(img.height() * scale)))),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        rendered.push_back(img);
    }
    QVector<int> order;
    for (int i = 0; i < frames.size(); ++i)
        order.push_back(i);
    const Animation& a = m_engine->animation();
    if (a.playback == Animation::Playback::PingPong) {
        for (int i = int(frames.size()) - 2; i >= 1; --i)
            order.push_back(i);
    }
    QVector<animio::Frame> out;
    for (int i : order)
        out.push_back({rendered[i], m_engine->layerInfo(frames[i]).hold});
    const bool loop = a.playback != Animation::Playback::OneShot;
    switch (format) {
    case Format::Gif:
        return animio::writeGif(path, out, a.fps, loop, error);
    case Format::Png:
        return animio::writeApng(path, out, a.fps, loop, error);
    case Format::Video:
        return animio::writeVideo(path, out, a.fps, error);
    case Format::Sequence: {
        QVector<animio::Frame> once;
        for (int i = 0; i < frames.size(); ++i)
            once.push_back({rendered[i], m_engine->layerInfo(frames[i]).hold});
        return animio::writeSequence(path, once, error) > 0;
    }
    }
    return false;
}
