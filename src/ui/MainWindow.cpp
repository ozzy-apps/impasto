#include "ui/MainWindow.h"

#include "io/DocumentIO.h"
#include "io/Interchange.h"
#include "shape/Perspective.h"
#include "ui/BrushLab.h"
#include "ui/BrushPanel.h"
#include "ui/CanvasWidget.h"
#include "ui/ColorPanel.h"
#include "ui/Gallery.h"
#include "ui/LayerPanel.h"
#include "ui/NewCanvasDialog.h"
#include "ui/ReferencePanel.h"
#include "ui/StabilisePanel.h"
#include "ui/Timeline.h"
#include "ui/ModelPanel.h"
#include "ui/ShortcutSheet.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDialog>
#include <QFontMetricsF>
#include <QButtonGroup>
#include <QPlainTextEdit>
#include <QFontComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QRandomGenerator>
#include <array>
#include <memory>
#include <cmath>

namespace {

constexpr int kTopBarHeight = 48;

// The current colour, shown as a disc at the right end of the top bar.
class ColorChip : public QAbstractButton {
public:
    explicit ColorChip(QWidget* parent) : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
    }
    void setColor(const QColor& c)
    {
        m_color = c;
        update();
    }
    QSize sizeHint() const override { return {42, 34}; }

    // Dragging the chip away is ColorDrop: these report where it is and where it lands.
    std::function<void(const QPoint& global)> onDragMove;
    std::function<void(const QPoint& global)> onDrop;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor(255, 255, 255, 215), 2));
        p.setBrush(m_color);
        p.drawEllipse(QPointF(width() / 2.0, height() / 2.0), 12.5, 12.5);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        m_pressAt = e->globalPosition().toPoint();
        m_dragging = false;
        QAbstractButton::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton))
            return;
        const QPoint global = e->globalPosition().toPoint();
        if (!m_dragging && (global - m_pressAt).manhattanLength() > 12) {
            m_dragging = true;
            setDown(false);   // no longer a click
        }
        if (m_dragging && onDragMove)
            onDragMove(global);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (!m_dragging) {
            QAbstractButton::mouseReleaseEvent(e);
            return;
        }
        m_dragging = false;
        if (onDrop)
            onDrop(e->globalPosition().toPoint());
    }

private:
    QColor m_color = Qt::black;
    QPoint m_pressAt;
    bool m_dragging = false;
};

// The colours of a gradient laid out along a strip, each with a handle under
// it. A press on the strip puts another colour there; a handle is dragged
// along, or pulled well away from the strip to take its colour out.
class GradientStrip : public QWidget {
public:
    explicit GradientStrip(QWidget* parent) : QWidget(parent)
    {
        setFixedSize(250, 38);
        setCursor(Qt::PointingHandCursor);
    }
    static constexpr int kPad = 10;
    QVector<GradientStop> stops;
    int selected = 0;
    std::function<QColor()> newColour;   // what a stop put on the strip starts as
    std::function<void()> onChanged;

    qreal xOf(float at) const { return kPad + at * (width() - 2 * kPad); }
    float atOf(qreal x) const { return float(std::clamp((x - kPad) / (width() - 2.0 * kPad), 0.0, 1.0)); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF strip(kPad, 2, width() - 2 * kPad, 18);
        QPainterPath shape;
        shape.addRoundedRect(strip, 5, 5);
        p.save();
        p.setClipPath(shape);
        theme::drawChecker(p, strip, 5);
        QVector<GradientStop> sorted = stops;
        std::stable_sort(sorted.begin(), sorted.end(), [](const GradientStop& a, const GradientStop& b) { return a.at < b.at; });
        QLinearGradient g(strip.left(), 0, strip.right(), 0);
        for (const GradientStop& stop : sorted)
            g.setColorAt(std::clamp(double(stop.at), 0.0, 1.0), stop.color);
        p.fillRect(strip, g);
        p.restore();
        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(strip, 5, 5);
        for (int i = 0; i < stops.size(); ++i) {
            const QRectF handle(xOf(stops[i].at) - 6, 23, 12, 12);
            QColor fill = stops[i].color;
            if (fill.alpha() < 255) {
                p.save();
                p.setClipRect(handle);
                theme::drawChecker(p, handle, 4);
                p.restore();
            }
            p.setPen(QPen(i == selected ? theme::accent : QColor(255, 255, 255, 190), i == selected ? 2.2 : 1.2));
            p.setBrush(fill);
            p.drawRoundedRect(handle, 3, 3);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        int hit = -1;
        double nearest = 9.0;
        for (int i = 0; i < stops.size(); ++i) {
            const double d = std::abs(xOf(stops[i].at) - e->position().x());
            if (d < nearest) {
                nearest = d;
                hit = i;
            }
        }
        if (hit < 0 && stops.size() < kMaxGradientStops) {
            stops.push_back({atOf(e->position().x()), newColour ? newColour() : QColor(Qt::white)});
            hit = int(stops.size()) - 1;
        }
        if (hit < 0)
            return;
        selected = hit;
        m_dragging = true;
        changed();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging || !(e->buttons() & Qt::LeftButton) || selected < 0 || selected >= stops.size())
            return;
        // Pulled well clear of the strip, a colour comes out; two always stay.
        if (stops.size() > 2 && (e->position().y() < -28 || e->position().y() > height() + 28)) {
            stops.removeAt(selected);
            selected = std::min(selected, int(stops.size()) - 1);
            m_dragging = false;
        } else {
            stops[selected].at = atOf(e->position().x());
        }
        changed();
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_dragging = false; }

private:
    void changed()
    {
        update();
        if (onChanged)
            onChanged();
    }
    bool m_dragging = false;
};

struct GradientPreset {
    const char* name;
    QVector<GradientStop> stops;
};

QVector<GradientStop> gradientOf(std::initializer_list<std::pair<double, const char*>> colours)
{
    QVector<GradientStop> out;
    for (const auto& c : colours)
        out.push_back({float(c.first), QColor(QLatin1String(c.second))});
    return out;
}

const QVector<GradientPreset>& gradientPresets()
{
    static const QVector<GradientPreset> all = {
        {QT_TRANSLATE_NOOP("MainWindow", "Sunset"), gradientOf({{0, "#1b1464"}, {0.4, "#c2185b"}, {0.72, "#ff7043"}, {1, "#ffe082"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Sky"), gradientOf({{0, "#0d47a1"}, {0.55, "#42a5f5"}, {1, "#e3f2fd"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Dusk"), gradientOf({{0, "#0f2027"}, {0.5, "#2c5364"}, {0.8, "#c06c84"}, {1, "#f8b195"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Fire"), gradientOf({{0, "#000000"}, {0.3, "#b71c1c"}, {0.6, "#ff6f00"}, {0.86, "#ffee58"}, {1, "#ffffff"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Forest"), gradientOf({{0, "#0b3d2e"}, {0.4, "#2e7d32"}, {0.75, "#9ccc65"}, {1, "#f1f8e9"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Sea"), gradientOf({{0, "#001f3f"}, {0.45, "#006d77"}, {0.8, "#83c5be"}, {1, "#edf6f9"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Chrome"), gradientOf({{0, "#f5f5f5"}, {0.44, "#9e9e9e"}, {0.5, "#424242"}, {0.56, "#bdbdbd"}, {1, "#fafafa"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Pastel"), gradientOf({{0, "#ffc2dd"}, {0.5, "#b8e8ff"}, {1, "#a8e6a1"}})},
        {QT_TRANSLATE_NOOP("MainWindow", "Spectrum"),
         gradientOf({{0, "#ff0000"}, {0.17, "#ffff00"}, {0.33, "#00ff00"}, {0.5, "#00ffff"}, {0.67, "#0000ff"}, {0.83, "#ff00ff"}, {1, "#ff0000"}})},
    };
    return all;
}

// Stops as text, for the settings: "0.25:#ff336699", position and colour with its alpha.
QStringList stopsToText(const QVector<GradientStop>& stops)
{
    QStringList out;
    for (const GradientStop& stop : stops)
        out << QStringLiteral("%1:%2").arg(double(stop.at), 0, 'f', 4).arg(stop.color.name(QColor::HexArgb));
    return out;
}

QVector<GradientStop> stopsFromText(const QStringList& text)
{
    QVector<GradientStop> out;
    for (const QString& entry : text) {
        const int colon = entry.indexOf(QLatin1Char(':'));
        const QColor c(entry.mid(colon + 1));
        if (colon > 0 && c.isValid() && out.size() < kMaxGradientStops)
            out.push_back({std::clamp(entry.left(colon).toFloat(), 0.0f, 1.0f), c});
    }
    return out.size() >= 2 ? out : QVector<GradientStop>();
}

QString imageFilter()
{
    return QObject::tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff)");
}

QString sizeText(float px)
{
    return QObject::tr("%1 px").arg(QString::number(px, 'f', px < 10.0f ? 1 : 0));
}

} // namespace

MainWindow::MainWindow(CanvasEngine* engine, QWidget* parent) : QMainWindow(parent), m_engine(engine)
{
    m_tools = new ToolState(this);
    m_tools->load();

    m_host = new QWidget(this);
    setCentralWidget(m_host);
    m_canvas = new CanvasWidget(m_engine, m_tools, m_host);
    m_host->installEventFilter(this);

    m_bubble = new QLabel(m_host);
    m_bubble->setStyleSheet(QStringLiteral("background: rgba(58,58,62,240); color: #f2f2f7; border-radius: 9px; padding: 7px 12px; font-size: 13px; font-weight: 500;"));
    m_bubble->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_bubble->hide();
    buildTopBar();
    buildSideBar();
    buildPopovers();
    m_timeline = new TimelineBar(m_engine, m_host);
    m_timeline->hide();
    connect(m_timeline, &TimelineBar::message, this, [this](const QString& text) { m_toast->showMessage(text); });
    m_toast = new Toast(m_host);
    m_shapePill = new Panel(m_host, 17);
    {
        auto* l = new QHBoxLayout(m_shapePill);
        l->setContentsMargins(6, 0, 6, 0);
        m_shapePillBtn = new IconButton(QString(), m_shapePill);
        m_shapePillBtn->setText(tr("Edit Shape"));
        m_shapePillBtn->setActive(true);
        m_shapePillBtn->setToolTip(tr("Adjust the shape you just drew"));
        l->addWidget(m_shapePillBtn);
        m_shapePill->hide();
    }
    buildShortcuts();

    // Recovery copies: looked at often, written seldom, and only while the pen rests.
    m_recoveryId = QStringLiteral("%1-%2").arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch());
    m_sinceRecovery.start();
    auto* recoveryTick = new QTimer(this);
    recoveryTick->setInterval(4000);
    connect(recoveryTick, &QTimer::timeout, this, [this] {
        const int minutes = QSettings().value(QStringLiteral("prefs/recoveryMinutes"), 2).toInt();
        if (minutes <= 0 || m_sinceRecovery.elapsed() < qint64(minutes) * 60'000)
            return;
        if (editing() || m_engine->strokeActive() || m_engine->floodActive() || m_canvas->idleMs() < 1500)
            return;   // not in the middle of something: the next tick will do
        saveRecoveryNow();
    });
    recoveryTick->start();

    DragScroller::trackPen();
    m_gallery = new Gallery(m_host);
    m_gallery->hide();
    connect(m_gallery, &Gallery::backRequested, this, &MainWindow::hideGallery);
    connect(m_gallery, &Gallery::message, m_toast, &Toast::showMessage);
    connect(m_gallery, &Gallery::openRequested, this, [this](const QString& path) {
        if (QFileInfo(path).absoluteFilePath() == QFileInfo(m_path).absoluteFilePath() && !m_path.isEmpty()) {
            hideGallery();   // it is what the canvas already shows
            return;
        }
        if (maybeSave() && openPath(path))
            hideGallery();
        else
            m_gallery->refresh(m_path);
    });
    connect(m_gallery, &Gallery::newRequested, this, [this] {
        if (newCanvas())
            hideGallery();
    });
    connect(m_gallery, &Gallery::importRequested, this, [this] {
        if (openDocument())
            hideGallery();
    });
    connect(m_gallery, &Gallery::artworkRenamed, this, [this](const QString& from, const QString& to) {
        if (!m_path.isEmpty() && QFileInfo(m_path).absoluteFilePath() == from) {
            m_path = to;
            updateTitle();
        }
    });

    connect(m_engine, &CanvasEngine::message, m_toast, &Toast::showMessage);
    connect(m_canvas, &CanvasWidget::toast, m_toast, &Toast::showMessage);
    connect(m_engine, &CanvasEngine::modifiedChanged, this, &MainWindow::updateTitle);
    connect(m_engine, &CanvasEngine::documentChanged, this, &MainWindow::updateTitle);
    connect(m_engine, &CanvasEngine::documentChanged, this, &MainWindow::syncTimeline);
    connect(m_engine, &CanvasEngine::animationChanged, this, &MainWindow::syncTimeline);
    connect(m_engine, &CanvasEngine::guidesChanged, this, [this] {
        const Guides& g = m_engine->guides();
        m_guidesBtn->setActive(m_guideBar || g.symmetry != Guides::Symmetry::Off || g.perspective != Guides::Perspective::Off);
    });
    // A document opened with a perspective set up keeps its points when that kind is picked again.
    connect(m_engine, &CanvasEngine::documentChanged, this, [this] { m_lastPerspective = m_engine->guides().perspective; });
    connect(&m_engine->history(), &History::changed, this, [this] {
        m_undoBtn->setEnabled(m_engine->history().canUndo() && !editing());
        m_redoBtn->setEnabled(m_engine->history().canRedo() && !editing());
    });
    connect(m_tools, &ToolState::toolChanged, this, &MainWindow::syncToolButtons);
    connect(m_tools, &ToolState::sizeChanged, this, &MainWindow::syncSliders);
    connect(m_tools, &ToolState::opacityChanged, this, &MainWindow::syncSliders);
    connect(m_tools, &ToolState::colorChanged, this, [this] { static_cast<ColorChip*>(m_colorBtn)->setColor(m_tools->color()); });
    connect(m_canvas, &CanvasWidget::eyedropperChanged, this, [this](bool armed) { m_pickBtn->setActive(armed); });
    connect(m_canvas, &CanvasWidget::transformFinished, this, &MainWindow::endEditBar);
    connect(m_canvas, &CanvasWidget::cropFinished, this, &MainWindow::endEditBar);
    connect(m_canvas, &CanvasWidget::liquifyFinished, this, &MainWindow::endEditBar);
    connect(m_shapePillBtn, &QAbstractButton::clicked, this, [this] { m_canvas->startShapeEdit(); });
    connect(m_canvas, &CanvasWidget::shapeEditOfferChanged, this, [this](bool offered) {
        if (offered) {
            m_shapePillBtn->setText(tr("Edit %1").arg(m_canvas->shapeName()));
            m_toast->hide();
        }
        m_toast->setTop(offered ? 110 : 64);
        m_shapePill->setVisible(offered && m_interfaceVisible);
        layoutOverlays();
    });
    connect(m_canvas, &CanvasWidget::selectModeChanged, this, &MainWindow::showSelectBar);
    connect(m_canvas, &CanvasWidget::gradientModeChanged, this, &MainWindow::showGradientBar);
    connect(m_canvas, &CanvasWidget::textModeChanged, this, &MainWindow::showTextBar);
    // Finished text becomes a layer where it was typed, in the transform box in case it should move or turn.
    // It stays text: tapping it with the text tool opens it again.
    connect(m_canvas, &CanvasWidget::textCommitted, this, [this](const TextInfo& info, const QString& name) {
        if (m_engine->addTextLayer(info, name))
            startTransform();
    });
    auto* chip = static_cast<ColorChip*>(m_colorBtn);
    // Letting go over the top bar is a change of mind, not a fill.
    auto dropAllowed = [this](const QPoint& global) {
        return !editing() && m_canvas->rect().contains(m_canvas->mapFromGlobal(global))
               && !m_topBar->geometry().contains(m_host->mapFromGlobal(global));
    };
    chip->onDragMove = [=, this](const QPoint& global) {
        closePopovers();
        m_canvas->dropMove(m_canvas->mapFromGlobal(global), dropAllowed(global));
    };
    chip->onDrop = [=, this](const QPoint& global) { m_canvas->dropRelease(m_canvas->mapFromGlobal(global), dropAllowed(global)); };
    connect(m_canvas, &CanvasWidget::fillArmedChanged, this, [this](bool armed) {
        if (armed)
            m_toast->showMessage(tr("Tap an area to fill it · slide sideways to adjust the threshold"));
    });
    connect(m_canvas, &CanvasWidget::thresholdShown, this, [this] { m_toast->hide(); });
    // A threshold settled on by sliding is where the next fill or selection starts.
    connect(m_canvas, &CanvasWidget::fillToleranceChanged, this,
            [](float t) { QSettings().setValue(QStringLiteral("prefs/fillTolerance"), double(t)); });
    connect(m_canvas, &CanvasWidget::selectToleranceChanged, this,
            [](float t) { QSettings().setValue(QStringLiteral("prefs/selectTolerance"), double(t)); });
    connect(m_canvas, &CanvasWidget::shapeEditStarted, this, &MainWindow::showShapeBar);
    connect(m_canvas, &CanvasWidget::shapeEditFinished, this, &MainWindow::endEditBar);

    static_cast<ColorChip*>(m_colorBtn)->setColor(m_tools->color());
    m_undoBtn->setEnabled(false);
    m_redoBtn->setEnabled(false);
    syncToolButtons();
    syncSliders();
    updateTitle();
    applyPreferences();

    resize(1500, 950);
    QSettings st;
    if (st.contains(QStringLiteral("window/geometry")))
        restoreGeometry(st.value(QStringLiteral("window/geometry")).toByteArray());
    m_canvas->setFocus();
    // Once the window has its size, so pinned panels land where they were left.
    QTimer::singleShot(0, this, &MainWindow::restorePanels);
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------- building

void MainWindow::buildTopBar()
{
    m_topBar = new Panel(m_host, 0);
    m_topBar->setFlatEdges(Qt::TopEdge | Qt::LeftEdge | Qt::RightEdge);
    auto* l = new QHBoxLayout(m_topBar);
    l->setContentsMargins(10, 6, 10, 6);
    l->setSpacing(4);

    m_fileBtn = new IconButton(QString(), m_topBar);
    m_fileBtn->setText(tr("Gallery"));
    m_fileBtn->setObjectName(QStringLiteral("gallery-button"));
    m_fileBtn->setToolTip(tr("Your artworks: open, rename, duplicate, start a new one"));
    l->addWidget(m_fileBtn);
    l->addSpacing(6);
    m_actionsBtn = new IconButton(QStringLiteral("actions"), m_topBar);
    m_actionsBtn->setToolTip(tr("Actions"));
    l->addWidget(m_actionsBtn);
    m_adjustBtn = new IconButton(QStringLiteral("adjust"), m_topBar);
    m_adjustBtn->setToolTip(tr("Adjustments"));
    l->addWidget(m_adjustBtn);
    m_selectBtn = new IconButton(QStringLiteral("select"), m_topBar);
    m_selectBtn->setToolTip(tr("Select an area (Q)"));
    m_selectBtn->setObjectName(QStringLiteral("select-button"));
    l->addWidget(m_selectBtn);
    m_transformBtn = new IconButton(QStringLiteral("transform"), m_topBar);
    m_transformBtn->setToolTip(tr("Transform layer (Ctrl+T)"));
    l->addWidget(m_transformBtn);
    m_guidesBtn = new IconButton(QStringLiteral("symmetry"), m_topBar);
    m_guidesBtn->setObjectName(QStringLiteral("guides-button"));
    m_guidesBtn->setToolTip(tr("Symmetry, perspective and grid"));
    l->addWidget(m_guidesBtn);
    m_steadyBtn = new IconButton(QStringLiteral("stabilise"), m_topBar);
    m_steadyBtn->setObjectName(QStringLiteral("steady-button"));
    m_steadyBtn->setToolTip(tr("Stabilisation of the brush in hand (Y)"));
    l->addWidget(m_steadyBtn);
    l->addStretch();

    const char* icons[3] = {"brush", "smudge", "eraser"};
    const QString tips[3] = {tr("Paint (B) — click again for the brush library"),
                             tr("Smudge (S) — click again for the brush library"),
                             tr("Erase (E) — click again for the brush library")};
    for (int i = 0; i < 3; ++i) {
        m_toolBtn[i] = new IconButton(QString::fromLatin1(icons[i]), m_topBar);
        m_toolBtn[i]->setToolTip(tips[i]);
        m_toolBtn[i]->setObjectName(QStringLiteral("tool-%1").arg(QLatin1String(icons[i])));
        l->addWidget(m_toolBtn[i]);
        connect(m_toolBtn[i], &QAbstractButton::clicked, this, [this, i] { toolButtonClicked(StrokeTool(i), m_toolBtn[i]); });
    }
    m_refBtn = new IconButton(QStringLiteral("picture"), m_topBar);
    m_refBtn->setToolTip(tr("Reference picture (R)"));
    m_refBtn->setObjectName(QStringLiteral("reference-button"));
    l->addWidget(m_refBtn);
    m_modelBtn = new IconButton(QStringLiteral("cube"), m_topBar);
    m_modelBtn->setToolTip(tr("3D reference: a figure to pose, or a model of your own (Shift+R)"));
    m_modelBtn->setObjectName(QStringLiteral("model-button"));
    l->addWidget(m_modelBtn);
    m_layersBtn = new IconButton(QStringLiteral("layers"), m_topBar);
    m_layersBtn->setToolTip(tr("Layers (L)"));
    m_layersBtn->setObjectName(QStringLiteral("layers-button"));
    l->addWidget(m_layersBtn);
    m_colorBtn = new ColorChip(m_topBar);
    m_colorBtn->setToolTip(tr("Colours (C)"));
    m_colorBtn->setObjectName(QStringLiteral("color-chip"));
    l->addWidget(m_colorBtn);

    connect(m_fileBtn, &QAbstractButton::clicked, this, &MainWindow::showGallery);
    connect(m_actionsBtn, &QAbstractButton::clicked, this, &MainWindow::showActionsMenu);
    connect(m_adjustBtn, &QAbstractButton::clicked, this, &MainWindow::showAdjustMenu);
    connect(m_transformBtn, &QAbstractButton::clicked, this, &MainWindow::startTransform);
    connect(m_guidesBtn, &QAbstractButton::clicked, this, [this] { showGuideBar(!m_guideBar); });
    connect(m_selectBtn, &QAbstractButton::clicked, this, [this] {
        closePopovers();
        m_canvas->setSelectMode(!m_canvas->selectMode());
    });
    connect(m_layersBtn, &QAbstractButton::clicked, this, [this] { togglePopover(m_layerPop, m_layersBtn); });
    connect(m_refBtn, &QAbstractButton::clicked, this, [this] { togglePopover(m_refPop, m_refBtn); });
    connect(m_modelBtn, &QAbstractButton::clicked, this, [this] { togglePopover(m_modelPop, m_modelBtn); });
    connect(m_steadyBtn, &QAbstractButton::clicked, this, [this] { togglePopover(m_steadyPop, m_steadyBtn); });
    connect(m_colorBtn, &QAbstractButton::clicked, this, [this] { togglePopover(m_colorPop, m_colorBtn); });
}

void MainWindow::buildSideBar()
{
    m_sideBar = new Panel(m_host, 14);
    m_sideBar->setFlatEdges(Qt::LeftEdge);
    auto* l = new QVBoxLayout(m_sideBar);
    l->setContentsMargins(2, 10, 5, 10);
    l->setSpacing(4);

    m_sizeSlider = new VSlider(m_sideBar);
    m_sizeSlider->setToolTip(tr("Brush size  [ and ]"));
    l->addWidget(m_sizeSlider, 0, Qt::AlignHCenter);
    m_pickBtn = new IconButton(QStringLiteral("eyedropper"), m_sideBar, 18);
    m_pickBtn->setToolTip(tr("Eyedropper (I) — or hold Alt, or right-click"));
    l->addWidget(m_pickBtn, 0, Qt::AlignHCenter);
    m_opacitySlider = new VSlider(m_sideBar);
    m_opacitySlider->setToolTip(tr("Opacity"));
    l->addWidget(m_opacitySlider, 0, Qt::AlignHCenter);
    l->addSpacing(4);
    m_undoBtn = new IconButton(QStringLiteral("undo"), m_sideBar, 20);
    m_undoBtn->setToolTip(tr("Undo (Ctrl+Z)"));
    l->addWidget(m_undoBtn, 0, Qt::AlignHCenter);
    m_redoBtn = new IconButton(QStringLiteral("redo"), m_sideBar, 20);
    m_redoBtn->setToolTip(tr("Redo (Ctrl+Shift+Z)"));
    l->addWidget(m_redoBtn, 0, Qt::AlignHCenter);

    connect(m_sizeSlider, &VSlider::valueChanged, this, [this](float v) {
        m_tools->setSizeFraction(v);
        showBubble(m_sizeSlider, sizeText(m_tools->size()));
    });
    connect(m_sizeSlider, &VSlider::pressed, this, [this] { showBubble(m_sizeSlider, sizeText(m_tools->size())); });
    connect(m_sizeSlider, &VSlider::released, m_bubble, &QWidget::hide);
    connect(m_opacitySlider, &VSlider::valueChanged, this, [this](float v) {
        m_tools->setOpacity(v);
        showBubble(m_opacitySlider, tr("%1%").arg(qRound(m_tools->opacity() * 100)));
    });
    connect(m_opacitySlider, &VSlider::pressed, this, [this] {
        showBubble(m_opacitySlider, tr("%1%").arg(qRound(m_tools->opacity() * 100)));
    });
    connect(m_opacitySlider, &VSlider::released, m_bubble, &QWidget::hide);
    connect(m_pickBtn, &QAbstractButton::clicked, this, [this] { m_canvas->armEyedropper(!m_canvas->eyedropperArmed()); });
    connect(m_undoBtn, &QAbstractButton::clicked, this, [this] { m_engine->undo(); });
    connect(m_redoBtn, &QAbstractButton::clicked, this, [this] { m_engine->redo(); });
}

void MainWindow::buildPopovers()
{
    m_brushPop = new Popover(m_host);
    m_brushPanel = new BrushPanel(m_engine, m_tools);
    m_brushPop->setContent(m_brushPanel);
    connect(m_brushPanel, &BrushPanel::labRequested, this, &MainWindow::showBrushLab);
    m_layerPop = new Popover(m_host);
    auto* layerPanel = new LayerPanel(m_engine, m_tools);
    m_layerPop->setContent(layerPanel);
    connect(layerPanel, &LayerPanel::editTextRequested, this, [this](int layerId) {
        if (editing())
            return;
        closePopovers();
        m_canvas->editTextLayer(layerId);
    });
    m_colorPop = new Popover(m_host);
    m_colorPanel = new ColorPanel(m_tools);
    m_colorPop->setContent(m_colorPanel);
    m_refPop = new Popover(m_host);
    m_refPanel = new ReferencePanel(m_tools);
    m_refPop->setContent(m_refPanel);
    m_modelPop = new Popover(m_host);
    m_modelPanel = new ModelPanel(m_tools);
    m_modelPop->setContent(m_modelPanel);
    m_modelPanel->setCanvasSize(m_engine->canvasSize());
    connect(m_engine, &CanvasEngine::documentChanged, m_modelPanel, [this] { m_modelPanel->setCanvasSize(m_engine->canvasSize()); });
    connect(m_modelPanel, &ModelPanel::message, this, [this](const QString& text) { m_toast->showMessage(text); });
    connect(m_modelPanel, &ModelPanel::placeRequested, this, [this](const QImage& image) {
        if (editing())
            return;
        if (m_engine->addImageLayer(image, tr("3D reference"), QPoint(0, 0), tr("Place 3D Reference")))
            m_toast->showMessage(tr("Placed as a layer of its own: lower its opacity to draw over it"));
    });
    m_steadyPop = new Popover(m_host);
    m_steadyPanel = new StabilisePanel(m_tools);
    m_steadyPop->setContent(m_steadyPanel);
    connect(m_steadyPanel, &StabilisePanel::everyBrushChanged, this, [this](float amount) {
        m_engine->setExtraStabilization(amount);
        QSettings().setValue(QStringLiteral("prefs/extraStabilization"), double(amount));
    });
    connect(m_steadyPanel, &StabilisePanel::labRequested, this, [this] {
        showBrushLab();
        if (m_brushLab && m_brushLab->isVisible())
            m_brushLab->setSection(BrushLab::Stabilisation);
    });
    // Palettes can be made from what is on the canvas and from the reference picture.
    m_colorPanel->canvasImage = [this] { return m_engine->renderImage(); };
    m_colorPanel->referenceImage = [this] { return m_refPanel->image(); };
    connect(m_colorPanel, &ColorPanel::message, this, [this](const QString& text) { m_toast->showMessage(text); });
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop}) {
        p->setTopInset(kTopBarHeight);
        connect(p, &Popover::dismissed, this, [this] { m_canvas->setFocus(); });
        connect(p, &Popover::pinnedChanged, this, [this](bool pinned) {
            if (pinned)
                m_toast->showMessage(tr("Pinned: it stays open while you paint"));
        });
    }
    m_brushPop->setObjectName(QStringLiteral("brush-popover"));
    m_layerPop->setObjectName(QStringLiteral("layer-popover"));
    m_colorPop->setObjectName(QStringLiteral("color-popover"));
    m_refPop->setObjectName(QStringLiteral("reference-popover"));
    m_steadyPop->setObjectName(QStringLiteral("steady-popover"));
    m_modelPop->setObjectName(QStringLiteral("model-popover"));
}

QWidget* MainWindow::anchorOf(Popover* popover) const
{
    if (popover == m_brushPop)
        return m_toolBtn[int(m_tools->tool())];
    if (popover == m_layerPop)
        return m_layersBtn;
    if (popover == m_refPop)
        return m_refBtn;
    if (popover == m_steadyPop)
        return m_steadyBtn;
    if (popover == m_modelPop)
        return m_modelBtn;
    return m_colorBtn;
}

// Which panels were pinned, where, and whether they were up: kept between sessions.
void MainWindow::savePanels() const
{
    QSettings st;
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop}) {
        const QString key = QStringLiteral("panels/") + p->objectName();
        st.setValue(key + QStringLiteral("/pinned"), p->isPinned());
        st.setValue(key + QStringLiteral("/pos"), p->pinnedPos());
        st.setValue(key + QStringLiteral("/open"), p->isPinned() && (p->isVisible() || m_suppressed.contains(p)));
    }
}

void MainWindow::restorePanels()
{
    QSettings st;
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop}) {
        const QString key = QStringLiteral("panels/") + p->objectName();
        if (!st.value(key + QStringLiteral("/pinned"), false).toBool())
            continue;
        p->setPinnedPos(st.value(key + QStringLiteral("/pos")).toPoint());
        QSignalBlocker quiet(p);   // no "Pinned" message for what was pinned all along
        p->setPinned(true);
        p->setPinnedPos(st.value(key + QStringLiteral("/pos")).toPoint());
        if (st.value(key + QStringLiteral("/open"), false).toBool()) {
            QWidget* anchor = anchorOf(p);
            if (m_gallery && m_gallery->isVisible())
                m_suppressed.push_back(p);
            else
                p->popup(anchor);
        }
    }
}

void MainWindow::setPanelsSuppressed(bool suppressed)
{
    if (suppressed) {
        for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop}) {
            if (p->isVisible() && p->isPinned() && !m_suppressed.contains(p)) {
                m_suppressed.push_back(p);
                p->hidePanel();
            }
        }
        return;
    }
    if ((m_gallery && m_gallery->isVisible()) || !m_interfaceVisible)
        return;   // still something in the way
    const QVector<Popover*> back = m_suppressed;
    m_suppressed.clear();
    for (Popover* p : back) {
        p->popup(anchorOf(p));
    }
}

void MainWindow::buildShortcuts()
{
    // Most shortcuts act on the canvas, and have no business doing so while
    // the gallery hides it.
    auto add = [this](const QKeySequence& keys, std::function<void()> fn, bool allowWhileEditing = false, bool allowInGallery = false) {
        auto* s = new QShortcut(keys, this);
        s->setContext(Qt::WindowShortcut);
        connect(s, &QShortcut::activated, this, [this, keys, fn = std::move(fn), allowWhileEditing, allowInGallery] {
            // The brush lab covers everything: only the keys that leave it, or the app, still act.
            if (m_brushLab && m_brushLab->isVisible() && keys != QKeySequence(Qt::Key_Escape) && keys != QKeySequence(Qt::CTRL | Qt::Key_Q))
                return;
            if (m_gallery && m_gallery->isVisible() && !allowInGallery)
                return;
            // While text is being typed, only the keys that end it mean anything else.
            if (m_canvas->textMode() && !allowWhileEditing)
                return;
            if (allowWhileEditing || !editing())
                fn();
        });
    };
    add(QKeySequence::New, [this] {
        if (newCanvas())
            hideGallery();
    }, false, true);
    add(QKeySequence::Open, [this] {
        if (openDocument())
            hideGallery();
    }, false, true);
    add(QKeySequence::Save, [this] { saveDocument(); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] { saveDocumentAs(); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { exportImage(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_I), [this] { insertImage(); });
    add(QKeySequence(Qt::Key_T), [this] { addText(); });
    // Enter starts a new line while typing; Ctrl+Enter finishes the text.
    add(QKeySequence(Qt::CTRL | Qt::Key_Return), [this] {
        if (m_canvas->textMode())
            m_canvas->commitText();
    }, true);
    add(QKeySequence(Qt::CTRL | Qt::Key_Q), [this] { close(); }, true, true);

    add(QKeySequence::Undo, [this] { m_engine->undo(); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), [this] { m_engine->redo(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { m_engine->redo(); });

    auto pickTool = [this](StrokeTool tool) {
        m_canvas->setSelectMode(false);
        m_canvas->armFill(false);
        m_tools->setTool(tool);
    };
    add(QKeySequence(Qt::Key_B), [pickTool] { pickTool(StrokeTool::Brush); });
    add(QKeySequence(Qt::Key_S), [pickTool] { pickTool(StrokeTool::Smudge); });
    add(QKeySequence(Qt::Key_E), [pickTool] { pickTool(StrokeTool::Eraser); });
    add(QKeySequence(Qt::Key_Q), [this] { m_canvas->setSelectMode(!m_canvas->selectMode()); });
    add(QKeySequence(Qt::Key_G), [this] { m_canvas->armFill(!m_canvas->fillArmed()); });
    add(QKeySequence(Qt::SHIFT | Qt::Key_G), [this] { m_canvas->setGradientMode(!m_canvas->gradientMode()); });
    add(QKeySequence::SelectAll, [this] { m_engine->selectAll(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_D), [this] { m_engine->clearSelection(); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), [this] { m_engine->invertSelection(); });
    add(QKeySequence::Copy, [this] { copySelection(false); });
    add(QKeySequence::Cut, [this] { copySelection(true); });
    add(QKeySequence::Paste, [this] { paste(); });
    add(QKeySequence(Qt::Key_I), [this] { m_canvas->armEyedropper(!m_canvas->eyedropperArmed()); });
    add(QKeySequence(Qt::Key_BracketLeft), [this] { m_tools->setSizeFraction(m_tools->sizeFraction() - 0.025f); });
    add(QKeySequence(Qt::Key_BracketRight), [this] { m_tools->setSizeFraction(m_tools->sizeFraction() + 0.025f); });
    add(QKeySequence(Qt::Key_BraceLeft), [this] { m_tools->setOpacity(m_tools->opacity() - 0.05f); });
    add(QKeySequence(Qt::Key_BraceRight), [this] { m_tools->setOpacity(m_tools->opacity() + 0.05f); });
    add(QKeySequence(Qt::Key_L), [this] { togglePopover(m_layerPop, m_layersBtn); });
    add(QKeySequence(Qt::Key_R), [this] { togglePopover(m_refPop, m_refBtn); });
    add(QKeySequence(Qt::SHIFT | Qt::Key_R), [this] { togglePopover(m_modelPop, m_modelBtn); });
    add(QKeySequence(Qt::Key_Y), [this] { togglePopover(m_steadyPop, m_steadyBtn); });
    add(QKeySequence(Qt::Key_C), [this] { togglePopover(m_colorPop, m_colorBtn); });
    add(QKeySequence(Qt::Key_X), [this] { m_tools->swapColor(); });

    add(QKeySequence(Qt::CTRL | Qt::Key_0), [this] { m_canvas->fitToView(); }, true);
    add(QKeySequence(Qt::CTRL | Qt::Key_1), [this] { m_canvas->zoomTo(1.0); }, true);
    add(QKeySequence(Qt::CTRL | Qt::Key_Equal), [this] { m_canvas->zoomStep(1); }, true);
    add(QKeySequence(Qt::CTRL | Qt::Key_Plus), [this] { m_canvas->zoomStep(1); }, true);
    add(QKeySequence(Qt::CTRL | Qt::Key_Minus), [this] { m_canvas->zoomStep(-1); }, true);
    add(QKeySequence(Qt::Key_4), [this] { m_canvas->rotateStep(-15.0); }, true);
    add(QKeySequence(Qt::Key_6), [this] { m_canvas->rotateStep(15.0); }, true);
    add(QKeySequence(Qt::Key_5), [this] { m_canvas->resetRotation(); }, true);
    add(QKeySequence(Qt::Key_M), [this] { m_canvas->flipView(); }, true);
    add(QKeySequence(Qt::Key_F11), [this] { isFullScreen() ? showNormal() : showFullScreen(); }, true);
    add(QKeySequence(Qt::Key_Tab), [this] { setInterfaceVisible(!m_interfaceVisible); }, true);

    add(QKeySequence(Qt::CTRL | Qt::Key_T), [this] { startTransform(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_U), [this] { startAdjustment(Adjustment::HueSaturation); });
    add(QKeySequence(Qt::CTRL | Qt::Key_L), [this] { startAdjustment(Adjustment::Levels); });
    add(QKeySequence(Qt::Key_W), [this] { startLiquify(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_M), [this] { startAdjustment(Adjustment::Curves); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] { m_engine->addLayer(); });
    add(QKeySequence(Qt::CTRL | Qt::Key_J), [this] { m_engine->duplicateLayer(m_engine->activeLayerId()); });
    add(QKeySequence(Qt::CTRL | Qt::Key_G), [this] { m_engine->groupLayer(m_engine->activeLayerId()); });
    add(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), [this] { m_engine->ungroupLayer(m_engine->activeLayerId()); });
    add(QKeySequence(Qt::CTRL | Qt::Key_E), [this] { m_engine->mergeDown(m_engine->activeLayerId()); });
    add(QKeySequence(Qt::Key_Delete), [this] {
        // In a shape that is open for editing, Delete takes out the chosen point.
        if (m_canvas->shapeEditing())
            m_canvas->removeShapePoint();
        else if (!editing() && !m_canvas->textMode())
            m_engine->clearLayer(m_engine->activeLayerId());
    }, true);

    add(QKeySequence(Qt::Key_Return), [this] {
        if (m_canvas->transforming())
            m_canvas->finishTransform(true);
        else if (m_canvas->cropping())
            m_canvas->finishCrop(true);
        else if (m_canvas->liquifying())
            m_canvas->finishLiquify(true);
        else if (m_canvas->textMode())
            m_canvas->commitText();
        else if (m_canvas->shapeEditing())
            m_canvas->finishShapeEdit(true);
        else if (m_adjusting)
            finishAdjustment(true);
        else if (m_canvas->selectMode())
            m_canvas->setSelectMode(false);
    }, true);
    add(QKeySequence(Qt::Key_Escape), [this] {
        if (m_brushLab && m_brushLab->isVisible())
            m_brushLab->done();
        else if (m_gallery->isVisible())
            hideGallery();
        else if (m_canvas->floodAdjusting())
            m_canvas->endFloodDrag(false);
        else if (m_canvas->transforming())
            m_canvas->finishTransform(false);
        else if (m_canvas->cropping())
            m_canvas->finishCrop(false);
        else if (m_canvas->liquifying())
            m_canvas->finishLiquify(false);
        else if (m_canvas->textMode())
            m_canvas->setTextMode(false);
        else if (m_canvas->shapeEditing())
            m_canvas->finishShapeEdit(false);
        else if (m_adjusting)
            finishAdjustment(false);
        else if (m_canvas->eyedropperArmed())
            m_canvas->armEyedropper(false);
        else if (m_canvas->fillArmed())
            m_canvas->armFill(false);
        else if (m_canvas->gradientMode())
            m_canvas->setGradientMode(false);
        else if (m_canvas->selectMode())
            m_canvas->setSelectMode(false);
        else
            closePopovers();
    }, true, true);
}

void MainWindow::showForScreenshot(const QString& what)
{
    if (m_canvas->transforming())
        m_canvas->finishTransform(false);
    if (m_canvas->cropping())
        m_canvas->finishCrop(false);
    if (m_canvas->liquifying())
        m_canvas->finishLiquify(false);
    if (m_canvas->shapeEditing())
        m_canvas->finishShapeEdit(true);
    if (m_adjusting)
        finishAdjustment(false);
    if (m_brushLab && m_brushLab->isVisible())
        m_brushLab->done();
    closePopovers();
    hideGallery();
    m_canvas->setSelectMode(false);
    m_canvas->setGradientMode(false);
    m_canvas->setTextMode(false);
    if (what == QLatin1String("gallery"))
        showGallery();
    else if (what == QLatin1String("select"))
        m_canvas->setSelectMode(true);
    else if (what == QLatin1String("brush-settings"))
        showBrushLab();
    else if (what == QLatin1String("shape"))
        m_canvas->startShapeEdit();
    else if (what == QLatin1String("brushes"))
        m_brushPop->popup(m_toolBtn[0]);
    else if (what == QLatin1String("layers"))
        m_layerPop->popup(m_layersBtn);
    else if (what.startsWith(QLatin1String("colors"))) {
        m_colorPanel->setTab(what.endsWith(QLatin1String("-classic"))    ? ColorPanel::Classic
                             : what.endsWith(QLatin1String("-harmony"))  ? ColorPanel::Harmonies
                             : what.endsWith(QLatin1String("-value"))    ? ColorPanel::Value
                             : what.endsWith(QLatin1String("-palettes")) ? ColorPanel::Palettes
                                                                        : ColorPanel::Disc);
        m_colorPop->popup(m_colorBtn);
    }
    else if (what == QLatin1String("reference"))
        m_refPop->popup(m_refBtn);
    else if (what == QLatin1String("transform"))
        startTransform();
    else if (what == QLatin1String("adjust"))
        startAdjustment(Adjustment::HueSaturation);
    else if (what == QLatin1String("levels"))
        startAdjustment(Adjustment::Levels);
    else if (what == QLatin1String("crop"))
        startCrop();
    else if (what == QLatin1String("gradient"))
        m_canvas->setGradientMode(true);
    else if (what == QLatin1String("liquify"))
        startLiquify();
    else if (what == QLatin1String("text"))
        addText();
    else if (what == QLatin1String("curves"))
        startAdjustment(Adjustment::Curves);
}

// ------------------------------------------------------------------ layout

bool MainWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_host && e->type() == QEvent::Resize)
        layoutOverlays();
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::setAnimationAssist(bool on)
{
    Animation a = m_engine->animation();
    a.enabled = on;
    m_engine->setAnimation(a);
    if (on && m_engine->frames().size() <= 1)
        m_toast->showMessage(tr("Every layer is a frame: + in the timeline adds the next one"));
}

void MainWindow::syncTimeline()
{
    const bool on = m_engine->animation().enabled;
    if (!on && m_timeline->playing())
        m_timeline->play(false);
    if (m_timeline->isVisible() != on) {
        m_timeline->setVisible(on);
        if (on)
            m_timeline->raise();
        layoutOverlays();
        // The whole canvas above the timeline, and all of it again without.
        m_canvas->setBottomInset(on ? m_timeline->preferredHeight() + 12 : 0);
        m_canvas->fitToView();
    }
}

void MainWindow::layoutOverlays()
{
    const QSize s = m_host->size();
    m_canvas->setGeometry(0, 0, s.width(), s.height());
    m_topBar->setGeometry(0, 0, s.width(), kTopBarHeight);
    const QSize sb = m_sideBar->sizeHint();
    m_sideBar->setGeometry(0, std::max(kTopBarHeight + 12, (s.height() - sb.height()) / 2 + 12), sb.width(), sb.height());
    // The bars along the bottom stand on the timeline while it is there.
    const int lift = (m_timeline && m_timeline->isVisible()) ? m_timeline->preferredHeight() + 12 : 0;
    if (m_shapePill) {
        const QSize ps = m_shapePill->sizeHint();
        m_shapePill->setGeometry((s.width() - ps.width()) / 2, kTopBarHeight + 12, ps.width(), 34);
    }
    if (m_selectBar) {
        const QSize sb2 = m_selectBar->sizeHint();
        m_selectBar->setGeometry((s.width() - sb2.width()) / 2, s.height() - sb2.height() - 28 - lift, sb2.width(), sb2.height());
    }
    if (m_textBar) {
        const QSize tb = m_textBar->sizeHint();
        m_textBar->setGeometry((s.width() - tb.width()) / 2, s.height() - tb.height() - 28 - lift, tb.width(), tb.height());
    }
    if (m_gradientBar) {
        const QSize gb = m_gradientBar->sizeHint();
        m_gradientBar->setGeometry((s.width() - gb.width()) / 2, s.height() - gb.height() - 28 - lift, gb.width(), gb.height());
    }
    if (m_guideBar) {
        const QSize gb = m_guideBar->sizeHint();
        m_guideBar->setGeometry((s.width() - gb.width()) / 2, s.height() - gb.height() - 28 - lift, gb.width(), gb.height());
    }
    if (m_editBar) {
        const QSize eb = m_editBar->sizeHint();
        m_editBar->setGeometry((s.width() - eb.width()) / 2, s.height() - eb.height() - 28 - lift, eb.width(), eb.height());
    }
    if (m_timeline && m_timeline->isVisible()) {
        const int left = sb.width() + 10;
        m_timeline->setGeometry(left, s.height() - m_timeline->preferredHeight() - 12, s.width() - left - 12, m_timeline->preferredHeight());
    }
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop}) {
        if (p && p->isVisible())
            p->reposition();
    }
    if (m_gallery)
        m_gallery->setGeometry(0, 0, s.width(), s.height());
    if (m_brushLab)
        m_brushLab->setGeometry(0, 0, s.width(), s.height());
}

void MainWindow::showBrushLab()
{
    if (editing() || (m_gallery && m_gallery->isVisible()))
        return;
    if (!m_brushLab) {
        m_brushLab = new BrushLab(m_engine, m_tools, m_host);
        connect(m_brushLab, &BrushLab::duplicateRequested, m_brushPanel, &BrushPanel::duplicateBrush);
        connect(m_brushLab, &BrushLab::deleteRequested, m_brushPanel, &BrushPanel::deleteBrush);
        connect(m_brushLab, &BrushLab::closed, this, [this] {
            setPanelsSuppressed(false);
            m_canvas->setFocus();
        });
    }
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop}) {
        if (!p->isPinned())
            p->hidePanel();
    }
    setPanelsSuppressed(true);   // pinned panels step aside and come back afterwards
    m_brushLab->setGeometry(m_host->rect());
    m_brushLab->setCanvasPaper(m_engine->backgroundColor());
    m_brushLab->open(m_canvas->pressureCurve());
}

// ---------------------------------------------------------------- recovery

namespace {
QString recoveryFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/recovery");
}
QString noteFor(const QString& recoveryFile)
{
    return recoveryFile.left(recoveryFile.size() - 4) + QStringLiteral(".json");
}
QJsonObject readNote(const QString& recoveryFile)
{
    QFile note(noteFor(recoveryFile));
    return note.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(note.readAll()).object() : QJsonObject();
}
} // namespace

QString MainWindow::recoveryFile() const
{
    return recoveryFolder() + QLatin1Char('/') + m_recoveryId + QLatin1Char('.') + QLatin1String(docio::kExtension);
}

bool MainWindow::saveRecoveryNow()
{
    m_sinceRecovery.restart();
    if (!m_engine->isModified() || m_engine->history().serial() == m_recoverySerial)
        return false;   // nothing new to lose
    QDir().mkpath(recoveryFolder());
    QString error;
    if (!docio::save(*m_engine, recoveryFile(), &error))
        return false;
    QJsonObject note;
    note[QStringLiteral("pid")] = qint64(QCoreApplication::applicationPid());
    note[QStringLiteral("path")] = m_path;
    note[QStringLiteral("name")] = m_path.isEmpty() ? (m_nameHint.isEmpty() ? tr("Untitled Artwork") : m_nameHint) : QFileInfo(m_path).completeBaseName();
    note[QStringLiteral("saved")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QFile out(noteFor(recoveryFile()));
    if (out.open(QIODevice::WriteOnly))
        out.write(QJsonDocument(note).toJson());
    m_recoverySerial = m_engine->history().serial();
    return true;
}

// The work is safe in a file of its own, or was given up on purpose.
void MainWindow::dropRecovery()
{
    QFile::remove(recoveryFile());
    QFile::remove(noteFor(recoveryFile()));
    m_recoverySerial = m_engine->history().serial();
    m_sinceRecovery.restart();
}

QStringList MainWindow::leftoverRecoveries(bool includeOwn)
{
    QStringList out;
    const QFileInfoList files = QDir(recoveryFolder()).entryInfoList({QStringLiteral("*.imp"), QStringLiteral("*.sbk")}, QDir::Files, QDir::Time);
    for (const QFileInfo& file : files) {
        const qint64 pid = readNote(file.absoluteFilePath()).value(QStringLiteral("pid")).toInteger(0);
        const bool own = pid == QCoreApplication::applicationPid();
        // A copy whose owner is still running belongs to that window, not to us.
        const bool ownerAlive = pid > 0 && !own && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));
        if (ownerAlive || (own && !includeOwn))
            continue;
        out << file.absoluteFilePath();
    }
    return out;
}

bool MainWindow::recover(const QString& file)
{
    const QJsonObject note = readNote(file);
    QString error;
    if (!docio::load(*m_engine, file, &error)) {
        QMessageBox::warning(this, tr("Recover"), tr("Could not read the recovery copy.\n%1").arg(error));
        return false;
    }
    // It goes back to being the document it was, with changes not yet saved.
    const QString original = note.value(QStringLiteral("path")).toString();
    m_path = QFileInfo::exists(original) ? original : QString();
    m_nameHint = note.value(QStringLiteral("name")).toString();
    m_engine->setModified(true);
    updateTitle();
    QFile::remove(file);
    QFile::remove(noteFor(file));
    m_recoverySerial = 0;
    saveRecoveryNow();   // under this session's name, in case lightning strikes twice
    m_toast->showMessage(tr("Recovered “%1”").arg(m_nameHint));
    return true;
}

bool MainWindow::offerRecovery()
{
    bool recovered = false;
    for (const QString& file : leftoverRecoveries()) {
        const QJsonObject note = readNote(file);
        const QString name = note.value(QStringLiteral("name")).toString(tr("Untitled Artwork"));
        const QDateTime when = QDateTime::fromString(note.value(QStringLiteral("saved")).toString(), Qt::ISODate);
        QMessageBox box(this);
        box.setWindowTitle(tr("Recover Unsaved Work"));
        box.setText(tr("Impasto did not close properly last time."));
        box.setInformativeText(tr("“%1” had changes that were not saved. A copy was kept on %2.\n\nBring it back?")
                                   .arg(name, QLocale().toString(when.isValid() ? when : QFileInfo(file).lastModified(), QLocale::ShortFormat)));
        QPushButton* yes = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
        QPushButton* no = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("Decide Later"), QMessageBox::RejectRole);
        box.setDefaultButton(yes);
        box.exec();
        if (box.clickedButton() == yes) {
            if (!recovered && recover(file))
                recovered = true;
        } else if (box.clickedButton() == no) {
            QFile::remove(file);
            QFile::remove(noteFor(file));
        }
        if (recovered)
            break;   // one canvas at a time; any others are offered next start
    }
    return recovered;
}

// ----------------------------------------------------------------- gallery

void MainWindow::showGallery()
{
    if (m_gallery->isVisible())
        return;
    if (editing()) {
        m_toast->showMessage(tr("Finish the edit first"));
        return;
    }
    closePopovers();
    showGuideBar(false);
    setPanelsSuppressed(true);
    m_canvas->endFloodDrag(true);
    m_canvas->setSelectMode(false);
    m_canvas->setGradientMode(false);
    m_canvas->setTextMode(false);
    m_canvas->armFill(false);
    m_gallery->refresh(m_path);
    m_gallery->setGeometry(m_host->rect());
    m_gallery->show();
    m_gallery->raise();
    m_gallery->setFocus();
}

void MainWindow::hideGallery()
{
    if (!m_gallery->isVisible())
        return;
    m_gallery->hide();
    setPanelsSuppressed(false);
    m_canvas->setFocus();
}

void MainWindow::showGalleryAtStart()
{
    if (!QSettings().value(QStringLiteral("prefs/startInGallery"), true).toBool())
        return;
    m_gallery->refresh(m_path);
    if (!m_gallery->paths().isEmpty())
        showGallery();
}

void MainWindow::setInterfaceVisible(bool visible)
{
    m_interfaceVisible = visible;
    closePopovers();
    setPanelsSuppressed(!visible);
    if (m_guideBar)
        m_guideBar->setVisible(visible);
    m_topBar->setVisible(visible);
    m_sideBar->setVisible(visible);
    m_shapePill->setVisible(visible && m_canvas->shapeEditOffered());
    if (!visible)
        m_toast->showMessage(tr("Interface hidden — press Tab to bring it back"));
}

void MainWindow::syncToolButtons()
{
    for (int i = 0; i < 3; ++i)
        m_toolBtn[i]->setActive(int(m_tools->tool()) == i);
}

void MainWindow::syncSliders()
{
    m_sizeSlider->setValue(m_tools->sizeFraction());
    m_opacitySlider->setValue(m_tools->opacity());
}

void MainWindow::updateTitle()
{
    const QString name = m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).completeBaseName();
    setWindowTitle(QStringLiteral("%1[*] — Impasto").arg(name));
    setWindowModified(m_engine->isModified());
}

void MainWindow::showBubble(QWidget* beside, const QString& text)
{
    m_bubble->setText(text);
    m_bubble->adjustSize();
    const QPoint at = beside->mapTo(m_host, QPoint(beside->width() + 14, beside->height() / 2 - m_bubble->height() / 2));
    m_bubble->move(at);
    m_bubble->show();
    m_bubble->raise();
}

// ---------------------------------------------------------------- popovers

void MainWindow::toolButtonClicked(StrokeTool tool, IconButton* button)
{
    if (editing())
        return;
    const bool wasSelecting = m_canvas->selectMode();
    m_canvas->setSelectMode(false);
    m_canvas->setGradientMode(false);
    m_canvas->setTextMode(false);
    m_canvas->armFill(false);
    if (m_tools->tool() != tool || wasSelecting) {
        closePopovers();
        m_tools->setTool(tool);
        return;
    }
    togglePopover(m_brushPop, button);
}

void MainWindow::togglePopover(Popover* popover, QWidget* anchor)
{
    if (editing())
        return;
    // Its button shows and hides a panel whether pinned or not; the other
    // panels only step aside if they are not pinned.
    const bool wasVisible = popover->isVisible();
    closePopovers();
    if (wasVisible)
        popover->hidePanel();
    else
        popover->popup(anchor);
}

void MainWindow::closePopovers()
{
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop})
        p->dismiss();
}

// ------------------------------------------------------------------- menus

void MainWindow::showActionsMenu()
{
    QMenu menu(this);
    menu.addAction(tr("Save\tCtrl+S"), this, [this] { saveDocument(); });
    menu.addAction(tr("Save As…\tCtrl+Shift+S"), this, [this] { saveDocumentAs(); });
    menu.addAction(tr("Export (PNG, JPEG, PSD, OpenRaster)…\tCtrl+Shift+E"), this, &MainWindow::exportImage);
    menu.addAction(tr("New Canvas…\tCtrl+N"), this, [this] { newCanvas(); });
    menu.addAction(tr("Open…\tCtrl+O"), this, [this] { openDocument(); });
    menu.addSeparator();
    menu.addAction(tr("Insert Image…\tCtrl+I"), this, &MainWindow::insertImage);
    menu.addAction(tr("Add Text…\tT"), this, &MainWindow::addText);
    menu.addSeparator();
    menu.addAction(tr("Copy\tCtrl+C"), this, [this] { copySelection(false); });
    menu.addAction(tr("Cut\tCtrl+X"), this, [this] { copySelection(true); });
    menu.addAction(tr("Paste\tCtrl+V"), this, &MainWindow::paste);
    menu.addSeparator();
    menu.addAction(tr("Select All\tCtrl+A"), this, [this] { m_engine->selectAll(); });
    menu.addAction(tr("Deselect\tCtrl+D"), this, [this] { m_engine->clearSelection(); })->setEnabled(m_engine->hasSelection());
    menu.addAction(tr("Invert Selection\tCtrl+Shift+I"), this, [this] { m_engine->invertSelection(); });
    menu.addAction(tr("Fill with a Click\tG"), this, [this] { m_canvas->armFill(true); });
    menu.addAction(tr("Gradient\tShift+G"), this, [this] { m_canvas->setGradientMode(true); });
    menu.addSeparator();
    QAction* animation = menu.addAction(tr("Animation Assist"), this, [this] { setAnimationAssist(!m_engine->animation().enabled); });
    animation->setCheckable(true);
    animation->setChecked(m_engine->animation().enabled);
    menu.addSeparator();
    menu.addAction(tr("Crop or Extend Canvas…"), this, &MainWindow::startCrop);
    menu.addAction(tr("Resize Image…"), this, &MainWindow::resizeImage);
    menu.addAction(tr("Flip Canvas Horizontally"), this, [this] { m_engine->flipCanvas(true); });
    menu.addAction(tr("Flip Canvas Vertically"), this, [this] { m_engine->flipCanvas(false); });
    menu.addSeparator();
    menu.addAction(tr("Fit to Screen\tCtrl+0"), m_canvas, &CanvasWidget::fitToView);
    menu.addAction(tr("Actual Size\tCtrl+1"), this, [this] { m_canvas->zoomTo(1.0); });
    menu.addAction(tr("Mirror View\tM"), m_canvas, &CanvasWidget::flipView);
    menu.addAction(tr("Reset Rotation\t5"), m_canvas, &CanvasWidget::resetRotation);
    menu.addAction(tr("Fullscreen\tF11"), this, [this] { isFullScreen() ? showNormal() : showFullScreen(); });
    menu.addAction(tr("Hide Interface\tTab"), this, [this] { setInterfaceVisible(false); });
    menu.addSeparator();
    menu.addAction(tr("Preferences…"), this, &MainWindow::showPreferences);
    menu.addAction(tr("Keyboard Shortcuts"), this, &MainWindow::showShortcuts);
    menu.addAction(tr("About Impasto"), this, &MainWindow::showAbout);
    menu.addSeparator();
    menu.addAction(tr("Quit\tCtrl+Q"), this, &QWidget::close);
    menu.exec(m_actionsBtn->mapToGlobal(QPoint(0, m_actionsBtn->height() + 8)));
}

void MainWindow::showAdjustMenu()
{
    QMenu menu(this);
    auto add = [&](Adjustment type, const QString& shortcut = QString()) {
        const QString name = CanvasEngine::adjustmentName(type);
        menu.addAction(shortcut.isEmpty() ? name : name + QLatin1Char('\t') + shortcut, this, [this, type] { startAdjustment(type); });
    };
    add(Adjustment::HueSaturation, QStringLiteral("Ctrl+U"));
    add(Adjustment::BrightnessContrast);
    add(Adjustment::ColorBalance);
    add(Adjustment::Levels, QStringLiteral("Ctrl+L"));
    add(Adjustment::Curves, QStringLiteral("Ctrl+M"));
    menu.addSeparator();
    add(Adjustment::BlackAndWhite);
    add(Adjustment::Invert);
    add(Adjustment::Posterize);
    add(Adjustment::Threshold);
    menu.addSeparator();
    menu.addAction(tr("Liquify\tW"), this, &MainWindow::startLiquify);
    add(Adjustment::GaussianBlur);
    add(Adjustment::Sharpen);
    add(Adjustment::Noise);
    add(Adjustment::Mosaic);
    menu.exec(m_adjustBtn->mapToGlobal(QPoint(0, m_adjustBtn->height() + 8)));
}

void MainWindow::showShortcuts()
{
    // A window of its own that is not modal: it can sit anywhere while the painting goes on.
    if (!m_shortcuts)
        m_shortcuts = new ShortcutSheet(this);
    m_shortcuts->show();
    m_shortcuts->raise();
    m_shortcuts->activateWindow();
}

void MainWindow::showAbout()
{
    const QSize cs = m_engine->canvasSize();
    QMessageBox::about(this, tr("About Impasto"),
                       tr("<h3>Impasto %1</h3>"
                          "<p>A GPU painting app for Linux.</p>"
                          "<p style='color:#a0a0a8'>Renderer: %2<br>Canvas: %3 × %4 px, %5 layers<br>Undo memory: %6 MB</p>")
                           .arg(QStringLiteral(IMPASTO_VERSION), m_engine->rendererName())
                           .arg(cs.width())
                           .arg(cs.height())
                           .arg(m_engine->layerCount())
                           .arg(m_engine->history().bytes() / (1024 * 1024)));
}

// ------------------------------------------------------------------- files

void MainWindow::newDefaultCanvas()
{
    const QScreen* scr = screen() ? screen() : QGuiApplication::primaryScreen();
    const QSize px = scr ? scr->size() * scr->devicePixelRatio() : QSize(2560, 1440);
    m_engine->newDocument(px, Qt::white);
    m_path.clear();
    m_nameHint.clear();
    dropRecovery();
    updateTitle();
}

bool MainWindow::newCanvas()
{
    if (!maybeSave())
        return false;
    const QScreen* scr = screen() ? screen() : QGuiApplication::primaryScreen();
    const QSize screenPx = scr ? scr->size() * scr->devicePixelRatio() : QSize(2560, 1440);
    NewCanvasDialog dlg(screenPx, this);
    if (dlg.exec() != QDialog::Accepted)
        return false;
    const QSize size = dlg.pixelSize();
    const QColor color = dlg.background() == NewCanvasDialog::CurrentColour ? m_tools->color() : QColor(Qt::white);
    m_engine->newDocument(size, color, dlg.background() != NewCanvasDialog::Transparent);
    m_engine->setResolution(dlg.resolution());
    m_path.clear();
    m_nameHint.clear();
    dropRecovery();
    updateTitle();
    return true;
}

bool MainWindow::openDocument()
{
    if (!maybeSave())
        return false;
    const QString dir = QSettings().value(QStringLiteral("paths/open"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open"), dir,
        tr("Impasto, Photoshop, OpenRaster and images (*.%1 *.%2 *.psd *.psb *.ora *.png *.jpg *.jpeg *.webp *.bmp *.tif *.tiff);;All files (*)")
            .arg(QLatin1String(docio::kExtension), QLatin1String(docio::kOldExtension)));
    return !path.isEmpty() && openPath(path);
}

bool MainWindow::openPath(const QString& path)
{
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = false;
    QStringList notes;
    if (docio::isDocumentPath(path)) {
        ok = docio::load(*m_engine, path, &error);
        if (ok) {
            m_path = path;
            m_nameHint.clear();
        }
    } else if (interchange::canOpen(path)) {
        // Layers from another program open as a new untitled canvas, like a picture does.
        ok = interchange::open(*m_engine, path, &error, &notes);
        if (ok) {
            m_path.clear();
            m_nameHint = QFileInfo(path).completeBaseName();
        }
    } else {
        const QImage img = docio::readImage(path, &error);
        if (!img.isNull() && qint64(img.width()) * img.height() > CanvasEngine::maxCanvasPixels()) {
            error = tr("The image is too large to open as a canvas.");
        } else if (!img.isNull()) {
            // Images open as a new untitled canvas so saving never overwrites the original.
            m_engine->newDocument(img.size(), Qt::white, true, img);
            m_path.clear();
            m_nameHint = QFileInfo(path).completeBaseName();   // saved, it keeps the picture's name
            ok = true;
        }
    }
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Open"), tr("Could not open %1.\n%2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    QSettings().setValue(QStringLiteral("paths/open"), QFileInfo(path).absolutePath());
    Gallery::noteRecent(path);
    dropRecovery();
    updateTitle();
    // What another program's file had that could not come across.
    if (!notes.isEmpty())
        m_toast->showMessage(notes.join(QLatin1Char(' ')));
    return true;
}

bool MainWindow::saveDocument()
{
    // An untitled canvas gets a name in the gallery without a dialog in the
    // way; it can be renamed there, or put elsewhere with Save As.
    const bool fresh = m_path.isEmpty();
    if (fresh)
        m_path = Gallery::freshPath(m_nameHint);
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = docio::save(*m_engine, m_path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Save"), tr("Could not save %1.\n%2").arg(QFileInfo(m_path).fileName(), error));
        if (fresh)
            m_path.clear();
        return false;
    }
    m_engine->setModified(false);
    dropRecovery();
    Gallery::noteRecent(m_path);
    updateTitle();
    m_toast->showMessage(fresh ? tr("Saved to the gallery as “%1”").arg(QFileInfo(m_path).completeBaseName()) : tr("Saved"));
    if (m_gallery->isVisible())
        m_gallery->refresh(m_path);
    return true;
}

bool MainWindow::saveDocumentAs()
{
    const QString ext = QLatin1String(docio::kExtension);
    const QString dir = QSettings().value(QStringLiteral("paths/save"), Gallery::folder()).toString();
    QDir().mkpath(Gallery::folder());
    const QString suggested = m_path.isEmpty() ? dir + QStringLiteral("/Untitled.") + ext : m_path;
    QString path = QFileDialog::getSaveFileName(this, tr("Save As"), suggested, tr("Impasto documents (*.%1)").arg(ext));
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().toLower() != ext)
        path += QLatin1Char('.') + ext;
    const QString previous = m_path;
    m_path = path;
    if (!saveDocument()) {
        m_path = previous;
        return false;
    }
    QSettings().setValue(QStringLiteral("paths/save"), QFileInfo(path).absolutePath());
    return true;
}

void MainWindow::exportImage()
{
    QSettings st;
    const QString dir = st.value(QStringLiteral("paths/export"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString base = m_path.isEmpty() ? (m_nameHint.isEmpty() ? tr("Untitled") : m_nameHint) : QFileInfo(m_path).completeBaseName();
    // Flat pictures, and files that keep the layers for other programs.
    const QString png = tr("PNG (*.png)"), jpeg = tr("JPEG (*.jpg)"), webp = tr("WebP (*.webp)");
    const QString psd8 = tr("Photoshop, with layers (*.psd)"), psd16 = tr("Photoshop, with layers, 16 bits a channel (*.psd)");
    const QString ora = tr("OpenRaster, with layers: Krita, GIMP, MyPaint (*.ora)");
    const QStringList filters{png, jpeg, webp, psd8, psd16, ora};
    QString filter = st.value(QStringLiteral("paths/exportFilter"), png).toString();
    if (!filters.contains(filter))
        filter = png;
    const QString suffix = filter == jpeg ? QStringLiteral(".jpg") : filter == webp ? QStringLiteral(".webp")
                           : filter == ora                        ? QStringLiteral(".ora")
                           : filter == psd8 || filter == psd16    ? QStringLiteral(".psd")
                                                                  : QStringLiteral(".png");
    QString path = QFileDialog::getSaveFileName(this, tr("Export"), dir + QLatin1Char('/') + base + suffix, filters.join(QStringLiteral(";;")), &filter);
    if (path.isEmpty())
        return;
    if (QFileInfo(path).suffix().isEmpty())
        path += filter == jpeg ? QStringLiteral(".jpg") : filter == webp ? QStringLiteral(".webp") : filter == ora ? QStringLiteral(".ora")
                : filter == psd8 || filter == psd16 ? QStringLiteral(".psd") : QStringLiteral(".png");
    QString ext = QFileInfo(path).suffix().toLower();
    // Photoshop wants a picture with a side past 30,000 pixels in a file of its own kind.
    const QSize size = m_engine->canvasSize();
    if (ext == QLatin1String("psd") && std::max(size.width(), size.height()) > 30000) {
        path = path.left(path.size() - 3) + QStringLiteral("psb");
        ext = QStringLiteral("psb");
    }
    QString error;
    QStringList notes;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool ok = false;
    if (ext == QLatin1String("psd") || ext == QLatin1String("psb"))
        ok = interchange::exportPsd(*m_engine, path, filter == psd16 ? 16 : 8, &error, &notes);
    else if (ext == QLatin1String("ora"))
        ok = interchange::exportOra(*m_engine, path, &error, &notes);
    else
        ok = docio::exportImage(*m_engine, path, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, tr("Export"), error);
        return;
    }
    st.setValue(QStringLiteral("paths/export"), QFileInfo(path).absolutePath());
    st.setValue(QStringLiteral("paths/exportFilter"), filter);
    notes.prepend(tr("Exported %1.").arg(QFileInfo(path).fileName()));
    m_toast->showMessage(notes.join(QLatin1Char(' ')));
}

void MainWindow::insertImage()
{
    const QString dir = QSettings().value(QStringLiteral("paths/open"), QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    const QString path = QFileDialog::getOpenFileName(this, tr("Insert Image"), dir, imageFilter());
    if (path.isEmpty())
        return;
    QString error;
    const QImage img = docio::readImage(path, &error);
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Insert Image"), tr("Could not read %1.\n%2").arg(QFileInfo(path).fileName(), error));
        return;
    }
    if (m_engine->addImageLayer(img, QFileInfo(path).completeBaseName()))
        startTransform();   // let the artist place it straight away
}

bool MainWindow::maybeSave()
{
    if (!m_engine->isModified())
        return true;
    if (QSettings().value(QStringLiteral("prefs/autoSave"), false).toBool())
        return saveDocument();
    QMessageBox box(this);
    box.setWindowTitle(tr("Unsaved Changes"));
    box.setText(tr("Save changes to this canvas before closing it?"));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);
    const int choice = box.exec();
    if (choice == QMessageBox::Save)
        return saveDocument();
    return choice == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_canvas->transforming())
        m_canvas->finishTransform(false);
    if (m_canvas->cropping())
        m_canvas->finishCrop(false);
    if (m_canvas->liquifying())
        m_canvas->finishLiquify(false);
    if (m_canvas->shapeEditing())
        m_canvas->finishShapeEdit(true);
    if (m_adjusting)
        finishAdjustment(false);
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    QSettings st;
    st.setValue(QStringLiteral("window/geometry"), saveGeometry());
    savePanels();
    dropRecovery();   // closing properly: either saved, or let go on purpose
    m_tools->save();
    BrushLibrary::instance().save();
    e->accept();
}

// ------------------------------------------------------- transform / adjust

bool MainWindow::editing() const
{
    return m_adjusting || m_canvas->transforming() || m_canvas->shapeEditing() || m_canvas->cropping() || m_canvas->liquifying();
}

void MainWindow::setEditingUi(bool on)
{
    for (QWidget* w : std::initializer_list<QWidget*>{m_fileBtn, m_actionsBtn, m_adjustBtn, m_transformBtn, m_guidesBtn, m_steadyBtn, m_selectBtn, m_toolBtn[0],
                                                      m_toolBtn[1], m_toolBtn[2], m_refBtn, m_modelBtn, m_layersBtn, m_colorBtn, m_pickBtn,
                                                      m_sizeSlider, m_opacitySlider})
        w->setEnabled(!on);
    for (Popover* p : {m_brushPop, m_layerPop, m_colorPop, m_refPop, m_steadyPop, m_modelPop})
        p->setEnabled(!on);   // a pinned panel waits while an edit is in progress
    if (m_timeline) {
        if (on)
            m_timeline->play(false);
        m_timeline->setEnabled(!on);
    }
    m_undoBtn->setEnabled(!on && m_engine->history().canUndo());
    m_redoBtn->setEnabled(!on && m_engine->history().canRedo());
}

void MainWindow::startTransform()
{
    if (editing())
        return;
    closePopovers();
    showGuideBar(false);
    m_canvas->setSelectMode(false);
    m_canvas->setGradientMode(false);
    m_canvas->setTextMode(false);
    m_canvas->armFill(false);
    if (!m_canvas->startTransform())
        return;

    m_editBar = new Panel(m_host, 16);
    auto* l = new QHBoxLayout(m_editBar);
    l->setContentsMargins(10, 6, 10, 6);
    l->setSpacing(2);
    auto add = [&](const char* icon, const QString& tip) {
        auto* b = new IconButton(QString::fromLatin1(icon), m_editBar, 20);
        b->setToolTip(tip);
        l->addWidget(b);
        return b;
    };
    IconButton* uniform = add("link", tr("Keep proportions (hold Shift to toggle while dragging)"));
    uniform->setActive(m_canvas->transformUniform());
    connect(uniform, &QAbstractButton::clicked, this, [this, uniform] {
        m_canvas->setTransformUniform(!m_canvas->transformUniform());
        uniform->setActive(m_canvas->transformUniform());
    });
    auto* distort = new IconButton(QString(), m_editBar);
    distort->setText(tr("Distort"));
    distort->setObjectName(QStringLiteral("transform-distort"));
    distort->setToolTip(tr("Move each corner on its own, to put the layer into perspective.\nSwitching it off goes back to the plain box."));
    l->addWidget(distort);
    auto* warp = new IconButton(QString(), m_editBar);
    warp->setText(tr("Warp"));
    warp->setObjectName(QStringLiteral("transform-warp"));
    warp->setToolTip(tr("Bend the layer: a sheet of points lies over it, and moving one pulls the paint around it along.\n"
                        "Switching it off goes back to the box as it was."));
    l->addWidget(warp);
    // Distort and warp replace the plain box in turn; flips and turns belong to the box.
    auto syncModes = [this, distort, warp, uniform] {
        const bool bending = m_canvas->transformWarp();
        warp->setActive(bending);
        distort->setActive(m_canvas->transformDistort() && !bending);
        distort->setEnabled(!bending);
        uniform->setEnabled(!bending && !m_canvas->transformDistort());
    };
    connect(distort, &QAbstractButton::clicked, this, [this, syncModes] {
        m_canvas->setTransformDistort(!m_canvas->transformDistort());
        syncModes();
    });
    connect(warp, &QAbstractButton::clicked, this, [this, syncModes] {
        m_canvas->setTransformWarp(!m_canvas->transformWarp());
        syncModes();
    });
    connect(add("flip-h", tr("Flip horizontally")), &QAbstractButton::clicked, this, [this] { m_canvas->transformFlip(true); });
    connect(add("flip-v", tr("Flip vertically")), &QAbstractButton::clicked, this, [this] { m_canvas->transformFlip(false); });
    connect(add("rotate", tr("Rotate 45°")), &QAbstractButton::clicked, this, [this] { m_canvas->transformRotate(45.0); });
    auto leaveDistort = [syncModes] { syncModes(); };
    connect(add("fit", tr("Fit to canvas")), &QAbstractButton::clicked, this, [this, leaveDistort] {
        m_canvas->transformFit();
        leaveDistort();
    });
    connect(add("reset", tr("Reset")), &QAbstractButton::clicked, this, [this, leaveDistort] {
        m_canvas->transformReset();
        leaveDistort();
    });
    l->addSpacing(14);
    connect(add("close", tr("Cancel (Esc)")), &QAbstractButton::clicked, this, [this] { m_canvas->finishTransform(false); });
    IconButton* apply = add("check", tr("Apply (Enter)"));
    apply->setObjectName(QStringLiteral("transform-apply"));
    apply->setActive(true);
    connect(apply, &QAbstractButton::clicked, this, [this] { m_canvas->finishTransform(true); });

    m_editBar->show();
    setEditingUi(true);
    layoutOverlays();
    m_canvas->setFocus();
}

void MainWindow::showShapeBar()
{
    closePopovers();
    m_editBar = new Panel(m_host, 16);
    m_editBar->setObjectName(QStringLiteral("shape-bar"));
    auto* l = new QHBoxLayout(m_editBar);
    l->setContentsMargins(12, 6, 10, 6);
    l->setSpacing(4);

    auto* name = new QLabel(m_editBar);
    name->setObjectName(QStringLiteral("shape-name"));
    name->setFont(theme::font(14, QFont::Medium));
    name->setContentsMargins(6, 0, 6, 0);
    l->addWidget(name);

    // Which precise versions make sense depends on what the shape is, and
    // that can change while it is open: a circle with a point taken out of
    // it is no longer anything that could be squared up.
    struct Variant {
        CanvasWidget::ShapeFamily family;
        const char* id;
        QString label;
        QString tip;
    };
    const QVector<Variant> variants = {
        {CanvasWidget::ShapeFamily::Ellipse, "ellipse", tr("Ellipse"), tr("Free proportions")},
        {CanvasWidget::ShapeFamily::Ellipse, "circle", tr("Circle"), tr("Make it a perfect circle. Handles also snap to it; Shift forces it.")},
        {CanvasWidget::ShapeFamily::Quad, "quad", tr("Quadrilateral"), tr("Move each corner on its own")},
        {CanvasWidget::ShapeFamily::Quad, "rectangle", tr("Rectangle"), tr("Square up the corners")},
        {CanvasWidget::ShapeFamily::Quad, "square", tr("Square"), tr("Make it a perfect square. Handles also snap to it; Shift forces it.")}};
    QVector<IconButton*> buttons;
    for (const Variant& v : variants) {
        auto* b = new IconButton(QString(), m_editBar);
        b->setText(v.label);
        b->setToolTip(v.tip);
        const QString id = QString::fromLatin1(v.id);
        b->setProperty("variant", id);
        b->setProperty("family", int(v.family));
        connect(b, &QAbstractButton::clicked, this, [this, id] { m_canvas->setShapeVariant(id); });
        l->addWidget(b);
        buttons.push_back(b);
    }

    // The points the shape is drawn through. The keys do the same with the
    // pointer on the shape itself; these are for a pen with no keyboard by it.
    l->addSpacing(10);
    auto* addPoint = new IconButton(QStringLiteral("point-add"), m_editBar, 20);
    addPoint->setObjectName(QStringLiteral("shape-point-add"));
    addPoint->setToolTip(tr("Add a point: press this, then the line where it should go (or Ctrl + click the line)"));
    l->addWidget(addPoint);
    auto* removePoint = new IconButton(QStringLiteral("point-remove"), m_editBar, 20);
    removePoint->setObjectName(QStringLiteral("shape-point-remove"));
    removePoint->setToolTip(tr("Remove the chosen point (Delete, or Ctrl + click a point)"));
    l->addWidget(removePoint);
    auto* smoothPoint = new IconButton(QStringLiteral("point-smooth"), m_editBar, 20);
    smoothPoint->setObjectName(QStringLiteral("shape-point-smooth"));
    l->addWidget(smoothPoint);
    auto* closeShape = new IconButton(QStringLiteral("shape-close"), m_editBar, 20);
    closeShape->setObjectName(QStringLiteral("shape-close"));
    l->addWidget(closeShape);
    connect(closeShape, &QAbstractButton::clicked, this, [this] { m_canvas->setShapeClosed(!m_canvas->shapeClosed()); });
    connect(addPoint, &QAbstractButton::clicked, this, [this] { m_canvas->armShapeAdd(!m_canvas->shapeAddArmed()); });
    connect(removePoint, &QAbstractButton::clicked, this, [this] { m_canvas->removeShapePoint(); });
    connect(smoothPoint, &QAbstractButton::clicked, this, [this] { m_canvas->setShapePointSmooth(!m_canvas->shapePointSmooth()); });

    auto sync = [this, buttons, name, addPoint, removePoint, smoothPoint, closeShape] {
        const CanvasWidget::ShapeFamily family = m_canvas->shapeFamily();
        const QString current = m_canvas->shapeVariant();
        bool reshaped = false;
        auto show = [&](QWidget* w, bool visible) {
            if (w->isHidden() == visible) {
                w->setVisible(visible);
                reshaped = true;
            }
        };
        for (IconButton* b : buttons) {
            show(b, b->property("family").toInt() == int(family));
            b->setActive(b->property("variant").toString() == current);
        }
        show(name, family == CanvasWidget::ShapeFamily::Other);
        if (name->text() != m_canvas->shapeName()) {
            name->setText(m_canvas->shapeName());
            reshaped = true;
        }
        const bool chosen = m_canvas->shapePoint() >= 0;
        const bool smooth = chosen && m_canvas->shapePointSmooth();
        addPoint->setActive(m_canvas->shapeAddArmed());
        removePoint->setEnabled(chosen);
        smoothPoint->setEnabled(chosen);
        // Shows what the chosen point is; pressing it makes it the other.
        smoothPoint->setIconName(chosen && !smooth ? QStringLiteral("point-corner") : QStringLiteral("point-smooth"));
        smoothPoint->setToolTip(!chosen ? tr("Choose a point first, then make it smooth or sharp here (or Alt + click a point)")
                                : smooth ? tr("A smooth point: press to make it a sharp corner (or Alt + click it)")
                                         : tr("A sharp corner: press to make it a smooth point (or Alt + click it)"));
        // Shows what pressing it does.
        const bool closedNow = m_canvas->shapeClosed();
        closeShape->setIconName(closedNow ? QStringLiteral("shape-open") : QStringLiteral("shape-close"));
        closeShape->setEnabled(closedNow || m_canvas->shapeCanClose());
        closeShape->setToolTip(closedNow ? tr("Open the shape at the chosen point (or its first), to pull the ends apart")
                                         : tr("Close the shape: join its two ends"));
        if (reshaped && m_editBar && m_editBar->isVisible()) {
            m_editBar->layout()->activate();
            layoutOverlays();
        }
    };
    // The bar is the context object, so this stops when the bar goes away.
    connect(m_canvas, &CanvasWidget::shapeEditChanged, m_editBar, sync);
    sync();

    l->addSpacing(14);
    auto* cancel = new IconButton(QStringLiteral("close"), m_editBar, 20);
    cancel->setToolTip(tr("Cancel (Esc)"));
    l->addWidget(cancel);
    auto* apply = new IconButton(QStringLiteral("check"), m_editBar, 20);
    apply->setToolTip(tr("Apply (Enter)"));
    apply->setActive(true);
    l->addWidget(apply);
    connect(cancel, &QAbstractButton::clicked, this, [this] { m_canvas->finishShapeEdit(false); });
    connect(apply, &QAbstractButton::clicked, this, [this] { m_canvas->finishShapeEdit(true); });

    m_editBar->show();
    setEditingUi(true);
    layoutOverlays();
    m_canvas->setFocus();
}

// Gradient mode: drags on the canvas lay gradients until the bar is closed.
void MainWindow::showGradientBar(bool show)
{
    if (m_gradientBar) {
        m_gradientBar->hide();
        m_gradientBar->deleteLater();
        m_gradientBar = nullptr;
    }
    if (!show) {
        m_canvas->setFocus();
        return;
    }
    closePopovers();
    showGuideBar(false);
    m_gradientBar = new Panel(m_host, 16);
    m_gradientBar->setObjectName(QStringLiteral("gradient-bar"));
    auto* l = new QHBoxLayout(m_gradientBar);
    l->setContentsMargins(14, 6, 10, 6);
    l->setSpacing(4);
    auto caption = [&](const QString& text) {
        auto* label = new QLabel(text, m_gradientBar);
        label->setFont(theme::font(12, QFont::DemiBold));
        label->setStyleSheet(QStringLiteral("color: #a0a0a8; margin-right: 4px;"));
        l->addWidget(label);
    };
    auto textButton = [&](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), m_gradientBar);
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        l->addWidget(b);
        return b;
    };
    caption(tr("Gradient"));
    // The kinds, as pictures to keep the bar short.
    struct Kind {
        GradientKind kind;
        const char* name;
        QString tip;
    };
    const Kind kinds[] = {
        {GradientKind::Linear, "gradient-linear", tr("Linear: along the line you drag")},
        {GradientKind::Radial, "gradient-radial", tr("Radial: outwards from where the drag starts")},
        {GradientKind::Angle, "gradient-angle", tr("Angle: anticlockwise round where the drag starts, beginning in the direction you drag")},
        {GradientKind::Reflected, "gradient-reflected", tr("Reflected: along the line, both ways from where the drag starts")},
        {GradientKind::Diamond, "gradient-diamond", tr("Diamond: outwards from where the drag starts, in a square turned to the line")},
    };
    QVector<IconButton*> kindButtons;
    for (const Kind& k : kinds) {
        auto* b = new IconButton(QString::fromLatin1(k.name), m_gradientBar, 20);
        b->setObjectName(QString::fromLatin1(k.name));
        b->setToolTip(k.tip);
        b->setProperty("kind", int(k.kind));
        l->addWidget(b);
        kindButtons.push_back(b);
    }
    l->addSpacing(12);
    caption(tr("Fades to"));
    IconButton* clear = textButton(tr("Nothing"), tr("From the current colour to transparent"), "gradient-to-nothing");
    IconButton* other = textButton(tr("Previous colour"), tr("From the current colour to the one used before it"), "gradient-to-colour");
    IconButton* custom = textButton(tr("Colours"), tr("Through colours of your own choosing, as many as you like"), "gradient-colours");
    l->addSpacing(8);
    auto* strip = new GradientStrip(m_gradientBar);
    strip->setObjectName(QStringLiteral("gradient-strip"));
    strip->setToolTip(tr("Press on the strip to put the current colour there. Drag a handle to move its colour; pull it away to remove it."));
    l->addWidget(strip);
    IconButton* setStop = textButton(tr("Set"), tr("The chosen handle takes the current colour"), "gradient-stop-set");
    IconButton* fadeStop = textButton(tr("Fade"), tr("The chosen handle fades to nothing, or back to its colour"), "gradient-stop-fade");
    IconButton* presets = textButton(tr("Presets"), tr("Ready-made gradients, and your own"), "gradient-presets");
    l->addSpacing(6);
    auto* done = new IconButton(QStringLiteral("check"), m_gradientBar, 20);
    done->setObjectName(QStringLiteral("gradient-done"));
    done->setToolTip(tr("Done"));
    done->setActive(true);
    l->addWidget(done);

    // The colours last put together come back; failing that, the current colour and the previous one.
    if (m_canvas->gradientStops().size() < 2) {
        QVector<GradientStop> stops = stopsFromText(QSettings().value(QStringLiteral("gradient/stops")).toStringList());
        if (stops.size() < 2) {
            const QColor previous = m_tools->previousColor();
            stops = {GradientStop{0.0f, m_tools->color()}, GradientStop{1.0f, previous.isValid() ? previous : QColor(Qt::white)}};
        }
        m_canvas->setGradientStops(stops);
    }
    strip->stops = m_canvas->gradientStops();
    strip->newColour = [this] { return m_tools->color(); };
    auto sync = [=, this] {
        const bool own = m_canvas->gradientCustom();
        for (IconButton* b : kindButtons)
            b->setActive(b->property("kind").toInt() == int(m_canvas->gradientKind()));
        clear->setActive(!own && m_canvas->gradientToTransparent());
        other->setActive(!own && !m_canvas->gradientToTransparent());
        custom->setActive(own);
        for (QWidget* w : {static_cast<QWidget*>(strip), static_cast<QWidget*>(setStop), static_cast<QWidget*>(fadeStop), static_cast<QWidget*>(presets)})
            w->setVisible(own);
        // The colour before this one: the same that X swaps to.
        const QColor previous = m_tools->previousColor();
        m_canvas->setGradientSecondColor(previous.isValid() ? previous : QColor(Qt::white));
        layoutOverlays();
    };
    auto keepStops = [=, this] {
        m_canvas->setGradientStops(strip->stops);
        QSettings().setValue(QStringLiteral("gradient/stops"), stopsToText(strip->stops));
    };
    strip->onChanged = keepStops;
    auto fades = [=, this](bool toNothing) {
        m_canvas->setGradientCustom(false);
        m_canvas->setGradientToTransparent(toNothing);
        sync();
    };
    for (IconButton* b : kindButtons) {
        connect(b, &QAbstractButton::clicked, m_gradientBar, [=, this] {
            m_canvas->setGradientKind(GradientKind(b->property("kind").toInt()));
            QSettings().setValue(QStringLiteral("gradient/kind"), b->property("kind").toInt());
            sync();
        });
    }
    // The kind last used comes back.
    m_canvas->setGradientKind(GradientKind(std::clamp(QSettings().value(QStringLiteral("gradient/kind"), 0).toInt(), 0, int(GradientKind::Diamond))));
    connect(clear, &QAbstractButton::clicked, m_gradientBar, [=] { fades(true); });
    connect(other, &QAbstractButton::clicked, m_gradientBar, [=] { fades(false); });
    connect(custom, &QAbstractButton::clicked, m_gradientBar, [=, this] {
        m_canvas->setGradientCustom(true);
        sync();
        m_toast->showMessage(tr("Press on the strip to add the current colour · drag a handle · pull it away to remove"));
    });
    connect(setStop, &QAbstractButton::clicked, m_gradientBar, [=, this] {
        if (strip->selected < 0 || strip->selected >= strip->stops.size())
            return;
        strip->stops[strip->selected].color = m_tools->color();
        strip->update();
        keepStops();
    });
    connect(fadeStop, &QAbstractButton::clicked, m_gradientBar, [=] {
        if (strip->selected < 0 || strip->selected >= strip->stops.size())
            return;
        QColor& c = strip->stops[strip->selected].color;
        c.setAlpha(c.alpha() < 255 ? 255 : 0);
        strip->update();
        keepStops();
    });
    connect(presets, &QAbstractButton::clicked, m_gradientBar, [=, this] {
        auto take = [=](const QVector<GradientStop>& stops) {
            strip->stops = stops;
            strip->selected = 0;
            strip->update();
            keepStops();
        };
        QMenu menu(this);
        for (const GradientPreset& preset : gradientPresets())
            menu.addAction(tr(preset.name), m_gradientBar, [=] { take(preset.stops); });
        // The artist's own, kept by name.
        QSettings store;
        store.beginGroup(QStringLiteral("gradient/presets"));
        const QStringList own = store.childKeys();
        if (!own.isEmpty())
            menu.addSeparator();
        for (const QString& name : own) {
            const QVector<GradientStop> stops = stopsFromText(store.value(name).toStringList());
            if (stops.size() >= 2)
                menu.addAction(name, m_gradientBar, [=] { take(stops); });
        }
        store.endGroup();
        menu.addSeparator();
        menu.addAction(tr("Keep These Colours as a Preset…"), m_gradientBar, [=, this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Gradient Preset"), tr("Name"), QLineEdit::Normal, tr("My gradient"), &ok).trimmed();
            if (ok && !name.isEmpty())
                QSettings().setValue(QStringLiteral("gradient/presets/") + QString(name).replace(QLatin1Char('/'), QLatin1Char(' ')), stopsToText(strip->stops));
        });
        if (!own.isEmpty()) {
            QMenu* forget = menu.addMenu(tr("Forget a Preset"));
            for (const QString& name : own)
                forget->addAction(name, m_gradientBar, [=] { QSettings().remove(QStringLiteral("gradient/presets/") + name); });
        }
        menu.exec(presets->mapToGlobal(QPoint(0, -8 - menu.sizeHint().height())));
    });
    connect(m_tools, &ToolState::colorChanged, m_gradientBar, sync);
    connect(m_tools, &ToolState::recentColorsChanged, m_gradientBar, sync);
    connect(done, &QAbstractButton::clicked, m_gradientBar, [this] { m_canvas->setGradientMode(false); });
    m_gradientBar->show();
    sync();
    m_toast->showMessage(tr("Drag on the canvas to lay a gradient · Shift keeps it straight"));
    m_canvas->setFocus();
}

// Symmetry, perspective and grid. Not a mode: the bar can stay up while
// painting, and while it is up the perspective points can be dragged about.
void MainWindow::showGuideBar(bool show)
{
    if (m_guideBar) {
        m_engine->disconnect(m_guideBar);   // it follows the guides while it is up, and only then
        m_guideBar->hide();
        m_guideBar->deleteLater();
        m_guideBar = nullptr;
    }
    auto guidesOn = [this] {
        const Guides& g = m_engine->guides();
        return g.symmetry != Guides::Symmetry::Off || g.perspective != Guides::Perspective::Off;
    };
    if (!show || editing()) {
        m_canvas->setGuideEditing(false);
        m_guidesBtn->setActive(guidesOn());
        m_canvas->setFocus();
        return;
    }
    closePopovers();
    m_canvas->setSelectMode(false);   // these bars share a place
    m_canvas->setGradientMode(false);
    m_guideBar = new Panel(m_host, 16);
    m_guideBar->setObjectName(QStringLiteral("guide-bar"));
    auto* outer = new QVBoxLayout(m_guideBar);
    outer->setContentsMargins(12, 6, 10, 6);
    outer->setSpacing(2);
    QHBoxLayout* l = nullptr;
    auto newRow = [&](const QString& title) {
        auto* holder = new QWidget(m_guideBar);
        l = new QHBoxLayout(holder);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        auto* caption = new QLabel(title, holder);
        caption->setFont(theme::font(12, QFont::DemiBold));
        caption->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
        caption->setFixedWidth(86);
        l->addWidget(caption);
        outer->addWidget(holder);
        return holder;
    };
    auto textButton = [&](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), l->parentWidget());
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        l->addWidget(b);
        return b;
    };
    auto valueLabel = [&](int width) {
        auto* v = new QLabel(l->parentWidget());
        v->setFixedWidth(width);
        v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->addWidget(v);
        return v;
    };
    using Symmetry = Guides::Symmetry;
    using Perspective = Guides::Perspective;

    newRow(tr("Symmetry"));
    IconButton* off = textButton(tr("Off"), tr("Strokes are not repeated"), "guide-off");
    IconButton* vertical = textButton(tr("Vertical"), tr("Left and right mirror each other"), "guide-vertical");
    IconButton* horizontal = textButton(tr("Horizontal"), tr("Top and bottom mirror each other"), "guide-horizontal");
    IconButton* quadrant = textButton(tr("Quadrant"), tr("Mirrored both ways: four of everything"), "guide-quadrant");
    IconButton* radial = textButton(tr("Radial"), tr("Repeated round the centre, like a mandala"), "guide-radial");
    IconButton* fewer = textButton(QStringLiteral("−"), tr("Fewer repeats"), "guide-fewer");
    auto* count = new QLabel(l->parentWidget());
    count->setFont(theme::font(13, QFont::DemiBold));
    count->setFixedWidth(22);
    count->setAlignment(Qt::AlignCenter);
    l->addWidget(count);
    IconButton* more = textButton(QStringLiteral("+"), tr("More repeats"), "guide-more");
    l->addSpacing(8);
    // Rotational for the mirror kinds, Mirrored for radial: the other way of repeating, each time.
    IconButton* variant = textButton(tr("Rotational"), QString(), "guide-variant");
    auto* turnValue = new QLabel(l->parentWidget());
    turnValue->setObjectName(QStringLiteral("guide-turn"));
    turnValue->setFont(theme::font(12, QFont::Medium));
    turnValue->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    turnValue->setAlignment(Qt::AlignCenter);
    turnValue->setFixedWidth(40);
    l->addWidget(turnValue);
    IconButton* recentre = textButton(tr("Reset"), tr("Back to the middle of the canvas, upright"), "guide-recentre");
    l->addStretch();
    auto* done = new IconButton(QStringLiteral("check"), l->parentWidget(), 20);
    done->setObjectName(QStringLiteral("guide-done"));
    done->setToolTip(tr("Close this bar. The guides stay as set."));
    done->setActive(true);
    l->addWidget(done);

    newRow(tr("Perspective"));
    IconButton* flat = textButton(tr("Off"), tr("No perspective lines"), "guide-perspective-off");
    IconButton* one = textButton(tr("1-point"), tr("One vanishing point, with level and upright lines"), "guide-1-point");
    IconButton* two = textButton(tr("2-point"), tr("Two vanishing points on a horizon, with uprights square to it"), "guide-2-point");
    IconButton* three = textButton(tr("3-point"), tr("Three vanishing points: the uprights converge as well"), "guide-3-point");
    IconButton* fisheye = textButton(tr("Fisheye"), tr("Five-point: level and upright lines bend round a circle"), "guide-fisheye");
    IconButton* iso = textButton(tr("Isometric"), tr("Three sets of parallel lines at angles you choose"), "guide-isometric");
    l->addSpacing(14);
    IconButton* snap = textButton(tr("Snap"), tr("Strokes keep to the guide line they set off along"), "guide-snap");
    l->addStretch();

    QWidget* anglesRow = newRow(tr("Angles"));
    auto* leftAngle = new Slider(anglesRow);
    leftAngle->setObjectName(QStringLiteral("guide-iso-left"));
    leftAngle->setFixedWidth(150);
    leftAngle->setToolTip(tr("How steeply the lines receding to the left rise"));
    l->addWidget(leftAngle);
    QLabel* leftValue = valueLabel(44);
    l->addSpacing(12);
    auto* rightAngle = new Slider(anglesRow);
    rightAngle->setObjectName(QStringLiteral("guide-iso-right"));
    rightAngle->setFixedWidth(150);
    rightAngle->setToolTip(tr("How steeply the lines receding to the right rise"));
    l->addWidget(rightAngle);
    QLabel* rightValue = valueLabel(44);
    l->addStretch();

    newRow(tr("Grid"));
    IconButton* grid = textButton(tr("Show"), tr("A square grid over the canvas; it is never part of the picture"), "guide-grid");
    auto* spacing = new Slider(l->parentWidget());
    spacing->setObjectName(QStringLiteral("guide-grid-spacing"));
    spacing->setFixedWidth(150);
    spacing->setToolTip(tr("How far apart the lines of the grid, and of the isometric guide, are"));
    l->addWidget(spacing);
    QLabel* spacingValue = valueLabel(54);
    l->addSpacing(10);
    auto* hint = new QLabel(l->parentWidget());
    hint->setFont(theme::font(11));
    hint->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    l->addWidget(hint);
    l->addStretch();

    // Grid spacing: 10 px .. 500 px, finer at the small end. Angles: 1° .. 89°.
    auto toSpacing = [](float v) { return 10.0f + v * v * 490.0f; };
    auto toAngle = [](float v) { return 1.0f + v * 88.0f; };
    auto sync = [=, this] {
        const Guides& g = m_engine->guides();
        off->setActive(g.symmetry == Symmetry::Off);
        vertical->setActive(g.symmetry == Symmetry::Vertical);
        horizontal->setActive(g.symmetry == Symmetry::Horizontal);
        quadrant->setActive(g.symmetry == Symmetry::Quadrant);
        radial->setActive(g.symmetry == Symmetry::Radial);
        const bool round = g.symmetry == Symmetry::Radial;
        fewer->setVisible(round);
        more->setVisible(round);
        count->setVisible(round);
        count->setText(QString::number(g.segments));
        const bool symmetryOn = g.symmetry != Symmetry::Off;
        variant->setVisible(symmetryOn);
        variant->setText(round ? tr("Mirrored") : tr("Rotational"));
        variant->setToolTip(round ? tr("Every repeat comes with its mirror image, as in a kaleidoscope")
                                  : tr("Repeats are turned about the centre instead of mirrored"));
        variant->setActive(round ? g.symMirrored : g.symRotational);
        turnValue->setVisible(symmetryOn && g.symAngle != 0.0f);
        turnValue->setText(QStringLiteral("%1°").arg(qRound(g.symAngle)));
        recentre->setVisible(symmetryOn && (!g.symOffset.isNull() || g.symAngle != 0.0f));

        flat->setActive(g.perspective == Perspective::Off);
        one->setActive(g.perspective == Perspective::OnePoint);
        two->setActive(g.perspective == Perspective::TwoPoint);
        three->setActive(g.perspective == Perspective::ThreePoint);
        fisheye->setActive(g.perspective == Perspective::FivePoint);
        iso->setActive(g.perspective == Perspective::Isometric);
        snap->setActive(g.snap);
        snap->setEnabled(g.perspective != Perspective::Off);
        anglesRow->setVisible(g.perspective == Perspective::Isometric);
        leftAngle->setValue((g.isoLeft - 1.0f) / 88.0f);
        rightAngle->setValue((g.isoRight - 1.0f) / 88.0f);
        leftValue->setText(QStringLiteral("%1°").arg(qRound(g.isoLeft)));
        rightValue->setText(QStringLiteral("%1°").arg(qRound(g.isoRight)));

        grid->setActive(g.grid);
        spacing->setValue(std::sqrt(std::clamp((g.gridSpacing - 10.0f) / 490.0f, 0.0f, 1.0f)));
        spacingValue->setText(tr("%1 px").arg(qRound(g.gridSpacing)));
        hint->setText(perspective::handleCount(g.perspective) > 0 ? tr("Drag the coloured points to place them")
                      : symmetryOn ? tr("Drag the blue point to move the symmetry, the green one to turn it") : QString());
        m_guidesBtn->setActive(true);
        layoutOverlays();
    };
    auto change = [=, this](const std::function<void(Guides&)>& edit) {
        Guides g = m_engine->guides();
        edit(g);
        m_engine->setGuides(g);
        sync();
    };
    auto pick = [=, this](Symmetry which, const QString& said) {
        change([&](Guides& g) { g.symmetry = which; });
        m_toast->showMessage(said);
    };
    auto view = [=, this](Perspective which, const QString& said) {
        change([&](Guides& g) {
            if (g.perspective == which)
                return;
            g.perspective = which;
            // Coming back to the kind used last finds its points where they were left.
            if (which != Perspective::Off && which != m_lastPerspective) {
                perspective::placeDefaults(g, m_engine->canvasSize());
                m_lastPerspective = which;
            }
        });
        m_toast->showMessage(said);
    };
    connect(off, &QAbstractButton::clicked, m_guideBar, [=] { pick(Symmetry::Off, tr("Symmetry off")); });
    connect(vertical, &QAbstractButton::clicked, m_guideBar, [=] { pick(Symmetry::Vertical, tr("Symmetry: left and right mirror each other")); });
    connect(horizontal, &QAbstractButton::clicked, m_guideBar, [=] { pick(Symmetry::Horizontal, tr("Symmetry: top and bottom mirror each other")); });
    connect(quadrant, &QAbstractButton::clicked, m_guideBar, [=] { pick(Symmetry::Quadrant, tr("Symmetry: mirrored both ways")); });
    connect(radial, &QAbstractButton::clicked, m_guideBar, [=] { pick(Symmetry::Radial, tr("Symmetry: repeated round the centre")); });
    connect(fewer, &QAbstractButton::clicked, m_guideBar, [=] { change([](Guides& g) { g.segments = std::max(2, g.segments - 1); }); });
    connect(more, &QAbstractButton::clicked, m_guideBar, [=] { change([](Guides& g) { g.segments = std::min(24, g.segments + 1); }); });
    connect(variant, &QAbstractButton::clicked, m_guideBar, [=, this] {
        change([](Guides& g) {
            if (g.symmetry == Symmetry::Radial)
                g.symMirrored = !g.symMirrored;
            else
                g.symRotational = !g.symRotational;
        });
        const Guides& g = m_engine->guides();
        m_toast->showMessage(g.symmetry == Symmetry::Radial
                                 ? (g.symMirrored ? tr("Radial, mirrored: a kaleidoscope") : tr("Radial: repeated round the centre"))
                                 : (g.symRotational ? tr("Repeats are turned about the centre") : tr("Repeats are mirrored")));
    });
    connect(recentre, &QAbstractButton::clicked, m_guideBar, [=, this] {
        change([](Guides& g) {
            g.symOffset = QPointF();
            g.symAngle = 0.0f;
        });
        m_toast->showMessage(tr("Symmetry is back in the middle, upright"));
    });
    // The points on the canvas move the guides too: the bar keeps up with them.
    connect(m_engine, &CanvasEngine::guidesChanged, m_guideBar, sync);
    connect(flat, &QAbstractButton::clicked, m_guideBar, [=] { view(Perspective::Off, tr("Perspective off")); });
    connect(one, &QAbstractButton::clicked, m_guideBar, [=] { view(Perspective::OnePoint, tr("One-point perspective · drag the point to place it")); });
    connect(two, &QAbstractButton::clicked, m_guideBar, [=] { view(Perspective::TwoPoint, tr("Two-point perspective · drag the points to place them")); });
    connect(three, &QAbstractButton::clicked, m_guideBar, [=] { view(Perspective::ThreePoint, tr("Three-point perspective · drag the points to place them")); });
    connect(fisheye, &QAbstractButton::clicked, m_guideBar, [=] { view(Perspective::FivePoint, tr("Fisheye · drag the centre, or the rim to resize")); });
    connect(iso, &QAbstractButton::clicked, m_guideBar, [=] { view(Perspective::Isometric, tr("Isometric · set the two angles below")); });
    connect(snap, &QAbstractButton::clicked, m_guideBar, [=, this] {
        change([](Guides& g) { g.snap = !g.snap; });
        m_toast->showMessage(m_engine->guides().snap ? tr("Strokes snap to the guide lines") : tr("Strokes are free; the guide is only shown"));
    });
    connect(leftAngle, &Slider::valueChanged, m_guideBar, [=, this](float v) {
        Guides g = m_engine->guides();
        g.isoLeft = toAngle(v);
        m_engine->setGuides(g);
        leftValue->setText(QStringLiteral("%1°").arg(qRound(g.isoLeft)));
    });
    connect(rightAngle, &Slider::valueChanged, m_guideBar, [=, this](float v) {
        Guides g = m_engine->guides();
        g.isoRight = toAngle(v);
        m_engine->setGuides(g);
        rightValue->setText(QStringLiteral("%1°").arg(qRound(g.isoRight)));
    });
    connect(grid, &QAbstractButton::clicked, m_guideBar, [=] { change([](Guides& g) { g.grid = !g.grid; }); });
    connect(spacing, &Slider::valueChanged, m_guideBar, [=, this](float v) {
        Guides g = m_engine->guides();
        g.gridSpacing = toSpacing(v);
        m_engine->setGuides(g);
        spacingValue->setText(tr("%1 px").arg(qRound(g.gridSpacing)));
    });
    connect(done, &QAbstractButton::clicked, m_guideBar, [this] { showGuideBar(false); });
    m_canvas->setGuideEditing(true);
    m_guideBar->show();
    sync();
    m_canvas->setFocus();
}

void MainWindow::showSelectBar(bool show)
{
    m_selectBtn->setActive(show);
    if (show && m_guideBar)
        showGuideBar(false);   // the two bars share a place
    if (m_selectBar) {
        m_selectBar->hide();
        m_selectBar->deleteLater();
        m_selectBar = nullptr;
    }
    if (!show) {
        m_canvas->setFocus();
        return;
    }
    closePopovers();
    m_selectBar = new Panel(m_host, 16);
    m_selectBar->setObjectName(QStringLiteral("select-bar"));
    auto* outer = new QVBoxLayout(m_selectBar);
    outer->setContentsMargins(12, 6, 10, 6);
    outer->setSpacing(4);

    // Feather: a slider that only takes up room while it is wanted.
    auto* featherRow = new QWidget(m_selectBar);
    auto* fl = new QHBoxLayout(featherRow);
    fl->setContentsMargins(6, 4, 4, 2);
    fl->setSpacing(10);
    auto* featherLabel = new QLabel(tr("Feather"), featherRow);
    featherLabel->setFont(theme::font(12, QFont::DemiBold));
    auto* featherSlider = new Slider(featherRow);
    featherSlider->setObjectName(QStringLiteral("select-feather-slider"));
    featherSlider->setToolTip(tr("How far the edge of the selection fades out"));
    auto* featherValue = new QLabel(tr("0 px"), featherRow);
    featherValue->setFixedWidth(56);
    featherValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fl->addWidget(featherLabel);
    fl->addWidget(featherSlider, 1);
    fl->addWidget(featherValue);
    featherRow->hide();
    outer->addWidget(featherRow);

    auto* l = new QHBoxLayout;
    l->setSpacing(4);
    outer->addLayout(l);
    auto textButton = [&](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), m_selectBar);
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        l->addWidget(b);
        return b;
    };
    using Shape = CanvasWidget::SelectShape;
    IconButton* automatic = textButton(tr("Automatic"), tr("Tap an area to select it by colour; slide sideways to adjust the threshold"),
                                       "select-automatic");
    IconButton* freehand = textButton(tr("Freehand"), tr("Draw around the area"), "select-freehand");
    IconButton* rectangle = textButton(tr("Rectangle"), tr("Drag out a box"), "select-rectangle");
    IconButton* ellipse = textButton(tr("Ellipse"), tr("Drag out an oval"), "select-ellipse");
    l->addSpacing(12);
    IconButton* add = textButton(tr("Add"), tr("New areas extend the selection"), "select-add");
    IconButton* remove = textButton(tr("Remove"), tr("New areas are cut away from it (or hold Alt)"), "select-remove");
    l->addSpacing(12);
    IconButton* feather = textButton(tr("Feather"), tr("Soften the edge of the selection"), "select-feather");
    IconButton* invert = textButton(tr("Invert"), tr("Select everything else instead (Ctrl+Shift+I)"), "select-invert");
    IconButton* clear = textButton(tr("Clear"), tr("Deselect (Ctrl+D)"), "select-clear");
    l->addSpacing(10);
    auto* done = new IconButton(QStringLiteral("check"), m_selectBar, 20);
    done->setObjectName(QStringLiteral("select-done"));
    done->setToolTip(tr("Done. The selection stays active; Ctrl+D removes it."));
    done->setActive(true);
    l->addWidget(done);

    auto sync = [=, this] {
        automatic->setActive(m_canvas->selectShape() == Shape::Automatic);
        freehand->setActive(m_canvas->selectShape() == Shape::Freehand);
        rectangle->setActive(m_canvas->selectShape() == Shape::Rectangle);
        ellipse->setActive(m_canvas->selectShape() == Shape::Ellipse);
        add->setActive(!m_canvas->selectSubtract());
        remove->setActive(m_canvas->selectSubtract());
    };
    auto shape = [=, this](Shape which) {
        m_canvas->setSelectShape(which);
        sync();
    };
    connect(automatic, &QAbstractButton::clicked, m_selectBar, [=, this] {
        shape(Shape::Automatic);
        if (m_engine->referenceLayerId() && m_engine->referenceLayerId() != m_engine->activeLayerId())
            m_toast->showMessage(tr("Tap an area · it follows the reference layer too"));
        else
            m_toast->showMessage(tr("Tap an area · slide sideways to adjust the threshold"));
    });
    connect(freehand, &QAbstractButton::clicked, m_selectBar, [=] { shape(Shape::Freehand); });
    connect(rectangle, &QAbstractButton::clicked, m_selectBar, [=] { shape(Shape::Rectangle); });
    connect(ellipse, &QAbstractButton::clicked, m_selectBar, [=] { shape(Shape::Ellipse); });
    connect(add, &QAbstractButton::clicked, m_selectBar, [=, this] {
        m_canvas->setSelectSubtract(false);
        sync();
    });
    connect(remove, &QAbstractButton::clicked, m_selectBar, [=, this] {
        m_canvas->setSelectSubtract(true);
        sync();
    });

    // The slider says how soft the edge is compared with how the selection
    // was when it was first touched. Once anything else happens, it starts
    // again from nothing for the selection as it is then.
    auto featherSerial = std::make_shared<quint64>(0);
    auto feathering = std::make_shared<bool>(false);
    auto featherRadius = [](float v) { return v * v * 120.0f; };
    connect(feather, &QAbstractButton::clicked, m_selectBar, [=, this] {
        if (featherRow->isHidden() && !m_engine->hasSelection()) {
            m_toast->showMessage(tr("Select an area first"));
            return;
        }
        featherRow->setVisible(featherRow->isHidden());
        feather->setActive(!featherRow->isHidden());
        layoutOverlays();
    });
    connect(featherSlider, &Slider::valueChanged, m_selectBar, [=, this](float v) {
        *feathering = true;   // the history changes under the slider's own hand
        m_engine->featherSelection(featherRadius(v));
        *feathering = false;
        *featherSerial = m_engine->history().serial();
        featherValue->setText(tr("%1 px").arg(qRound(featherRadius(v))));
    });
    connect(&m_engine->history(), &History::changed, m_selectBar, [=, this] {
        if (*feathering || m_engine->history().serial() == *featherSerial)
            return;
        featherSlider->setValue(0.0f);
        featherValue->setText(tr("0 px"));
    });
    connect(invert, &QAbstractButton::clicked, m_selectBar, [this] { m_engine->invertSelection(); });
    connect(clear, &QAbstractButton::clicked, m_selectBar, [this] { m_engine->clearSelection(); });
    connect(done, &QAbstractButton::clicked, m_selectBar, [this] { m_canvas->setSelectMode(false); });
    sync();

    m_selectBar->show();
    layoutOverlays();
    m_canvas->setFocus();
}

void MainWindow::copySelection(bool cut)
{
    QPoint origin;
    const QImage img = m_engine->copyPixels(&origin);
    if (img.isNull()) {
        m_toast->showMessage(tr("Nothing to copy on this layer"));
        return;
    }
    m_copied = img;
    m_copiedOrigin = origin;
    QGuiApplication::clipboard()->setImage(img);
    if (cut)
        m_engine->clearLayer(m_engine->activeLayerId());
    m_toast->showMessage(cut ? tr("Cut") : tr("Copied"));
}

void MainWindow::paste()
{
    const QClipboard* clipboard = QGuiApplication::clipboard();
    int id = 0;
    if (clipboard->ownsClipboard() && !m_copied.isNull()) {
        // Our own copy: put it back exactly where it came from.
        id = m_engine->addImageLayer(m_copied, tr("Pasted"), m_copiedOrigin);
    } else {
        const QImage img = clipboard->image();
        if (img.isNull()) {
            m_toast->showMessage(tr("The clipboard has no image"));
            return;
        }
        id = m_engine->addImageLayer(img, tr("Pasted"));
    }
    if (id)
        startTransform();   // ready to be placed
}

void MainWindow::applyPreferences()
{
    QSettings st;
    m_canvas->setQuickShape(st.value(QStringLiteral("prefs/quickShape"), true).toBool(),
                            st.value(QStringLiteral("prefs/quickShapeDelay"), 600).toInt());
    m_canvas->setFillTolerance(st.value(QStringLiteral("prefs/fillTolerance"), 0.12).toFloat());
    m_canvas->setSelectTolerance(st.value(QStringLiteral("prefs/selectTolerance"), 0.12).toFloat());
    m_engine->setExtraStabilization(st.value(QStringLiteral("prefs/extraStabilization"), 0.0).toFloat());
    if (m_steadyPanel)
        m_steadyPanel->setEveryBrush(st.value(QStringLiteral("prefs/extraStabilization"), 0.0).toFloat());
    QVector<QPointF> curve;
    const QVariantList stored = st.value(QStringLiteral("prefs/pressureCurve")).toList();
    for (int i = 0; i + 1 < stored.size(); i += 2)
        curve.push_back(QPointF(stored[i].toDouble(), stored[i + 1].toDouble()));
    m_canvas->setPressureCurve(curve.size() >= 2 ? curve : QVector<QPointF>());
}

void MainWindow::showPreferences()
{
    QSettings st;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Preferences"));
    auto* form = new QFormLayout(&dlg);
    form->setContentsMargins(20, 18, 20, 16);
    form->setVerticalSpacing(12);

    auto* quickShape = new QCheckBox(tr("Snap a stroke to a shape when the pen rests"), &dlg);
    quickShape->setChecked(st.value(QStringLiteral("prefs/quickShape"), true).toBool());
    form->addRow(tr("QuickShape"), quickShape);

    auto* startInGallery = new QCheckBox(tr("Open on the gallery when there are artworks in it"), &dlg);
    startInGallery->setChecked(st.value(QStringLiteral("prefs/startInGallery"), true).toBool());
    form->addRow(tr("Start-up"), startInGallery);
    auto* autoSave = new QCheckBox(tr("Save without asking when leaving a canvas"), &dlg);
    autoSave->setChecked(st.value(QStringLiteral("prefs/autoSave"), false).toBool());
    autoSave->setToolTip(tr("Going to the gallery, opening another artwork or quitting then saves the canvas first.\n"
                            "An untitled canvas is named and kept in the gallery."));
    form->addRow(tr("Saving"), autoSave);
    auto* recoveryBox = new QSpinBox(&dlg);
    recoveryBox->setRange(0, 30);
    recoveryBox->setSpecialValueText(tr("Never"));
    recoveryBox->setSuffix(tr(" min"));
    recoveryBox->setValue(st.value(QStringLiteral("prefs/recoveryMinutes"), 2).toInt());
    recoveryBox->setToolTip(tr("How often unsaved work is copied aside while you paint. If the app or the computer\n"
                               "goes down, the copy is offered back the next time Impasto starts."));
    form->addRow(tr("Recovery copy every"), recoveryBox);

    // One slider row with its value shown beside it.
    auto sliderRow = [&](const QString& label, float position, std::function<QString(float)> describe) {
        auto* row = new QHBoxLayout;
        auto* slider = new Slider(&dlg);
        slider->setMinimumWidth(220);
        slider->setValue(position);
        auto* value = new QLabel(describe(position), &dlg);
        value->setFixedWidth(64);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(slider, 1);
        row->addWidget(value);
        form->addRow(label, row);
        connect(slider, &Slider::valueChanged, &dlg, [value, describe](float v) { value->setText(describe(v)); });
        return slider;
    };
    // 0.3 s .. 1.5 s
    const int delayMs = st.value(QStringLiteral("prefs/quickShapeDelay"), 600).toInt();
    Slider* delay = sliderRow(tr("Rest time"), std::clamp((delayMs - 300) / 1200.0f, 0.0f, 1.0f),
                              [](float v) { return tr("%1 s").arg(QString::number((300 + v * 1200) / 1000.0, 'f', 2)); });
    const float tolerance = st.value(QStringLiteral("prefs/fillTolerance"), 0.12).toFloat();
    Slider* fill = sliderRow(tr("Fill threshold"), std::clamp(tolerance, 0.0f, 1.0f),
                             [](float v) { return tr("%1%").arg(qRound(v * 100)); });
    fill->setToolTip(tr("How different a colour may be and still be flooded by a fill.\n"
                        "Sliding sideways while filling changes it too."));
    Slider* steady = sliderRow(tr("Stabilise all brushes"), st.value(QStringLiteral("prefs/extraStabilization"), 0.0).toFloat(),
                               [](float v) { return tr("%1%").arg(qRound(v * 100)); });
    steady->setToolTip(tr("Smoothing added to every brush, on top of its own settings"));

    // The pen's own response: how hard a press has to be for a given weight.
    auto* curveBox = new QVBoxLayout;
    auto* curve = new CurveEditor(&dlg);
    curve->setFixedSize(240, 170);
    curve->setToolTip(tr("Left to right is how hard you press, bottom to top is what the brush feels.\n"
                         "Lift the left half for a lighter touch; lower it for a firmer hand.\n"
                         "Press on the line to add a point; pull a point well above or below to remove it."));
    QVector<QPointF> stored;
    const QVariantList raw = st.value(QStringLiteral("prefs/pressureCurve")).toList();
    for (int i = 0; i + 1 < raw.size(); i += 2)
        stored.push_back(QPointF(raw[i].toDouble(), raw[i + 1].toDouble()));
    if (stored.size() >= 2)
        curve->setPoints(stored);
    auto* resetCurve = new QPushButton(tr("Reset curve"), &dlg);
    curveBox->addWidget(curve);
    curveBox->addWidget(resetCurve, 0, Qt::AlignLeft);
    form->addRow(tr("Pen pressure"), curveBox);
    connect(resetCurve, &QPushButton::clicked, curve, [curve] { curve->setPoints(CurveEditor::identity()); });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    st.setValue(QStringLiteral("prefs/quickShape"), quickShape->isChecked());
    st.setValue(QStringLiteral("prefs/startInGallery"), startInGallery->isChecked());
    st.setValue(QStringLiteral("prefs/autoSave"), autoSave->isChecked());
    st.setValue(QStringLiteral("prefs/recoveryMinutes"), recoveryBox->value());
    st.setValue(QStringLiteral("prefs/quickShapeDelay"), int(300 + delay->value() * 1200));
    st.setValue(QStringLiteral("prefs/fillTolerance"), double(fill->value()));
    st.setValue(QStringLiteral("prefs/extraStabilization"), double(steady->value()));
    QVariantList curvePoints;
    for (const QPointF& pt : curve->points())
        curvePoints << pt.x() << pt.y();
    st.setValue(QStringLiteral("prefs/pressureCurve"), curvePoints);
    applyPreferences();
}

// Liquify: the pen works on the canvas; the bar chooses what it does.
void MainWindow::startLiquify()
{
    if (editing())
        return;
    closePopovers();
    showGuideBar(false);
    if (!m_canvas->startLiquify())
        return;
    m_editBar = new Panel(m_host, 16);
    m_editBar->setObjectName(QStringLiteral("liquify-bar"));
    auto* outer = new QVBoxLayout(m_editBar);
    outer->setContentsMargins(14, 8, 10, 8);
    outer->setSpacing(4);
    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    auto* title = new QLabel(tr("Liquify"), m_editBar);
    title->setFont(theme::font(13, QFont::DemiBold));
    title->setStyleSheet(QStringLiteral("margin-right: 8px;"));
    top->addWidget(title);
    struct Choice {
        LiquifyMode mode;
        QString label, tip;
        const char* name;
    };
    const Choice choices[] = {
        {LiquifyMode::Push, tr("Push"), tr("Drag the paint along with the pen"), "liquify-push"},
        {LiquifyMode::TwirlRight, tr("Twirl right"), tr("Turn the paint clockwise for as long as the pen is down"), "liquify-twirl-right"},
        {LiquifyMode::TwirlLeft, tr("Twirl left"), tr("Turn the paint anticlockwise for as long as the pen is down"), "liquify-twirl-left"},
        {LiquifyMode::Pinch, tr("Pinch"), tr("Draw the paint in towards the pen"), "liquify-pinch"},
        {LiquifyMode::Expand, tr("Expand"), tr("Swell the paint out from under the pen"), "liquify-expand"},
        {LiquifyMode::Restore, tr("Restore"), tr("Ease the paint back to how it was"), "liquify-restore"},
    };
    QVector<IconButton*> buttons;
    for (const Choice& c : choices) {
        auto* b = new IconButton(QString(), m_editBar);
        b->setText(c.label);
        b->setToolTip(c.tip);
        b->setObjectName(QLatin1String(c.name));
        b->setActive(c.mode == m_canvas->liquifyMode());
        top->addWidget(b);
        buttons.push_back(b);
    }
    for (int i = 0; i < buttons.size(); ++i) {
        const LiquifyMode mode = choices[i].mode;
        connect(buttons[i], &QAbstractButton::clicked, m_editBar, [this, buttons, i, mode] {
            m_canvas->setLiquifyMode(mode);
            for (int k = 0; k < buttons.size(); ++k)
                buttons[k]->setActive(k == i);
        });
    }
    top->addSpacing(10);
    auto* cancel = new IconButton(QStringLiteral("close"), m_editBar, 20);
    cancel->setObjectName(QStringLiteral("liquify-cancel"));
    cancel->setToolTip(tr("Cancel (Esc)"));
    top->addWidget(cancel);
    auto* apply = new IconButton(QStringLiteral("check"), m_editBar, 20);
    apply->setObjectName(QStringLiteral("liquify-apply"));
    apply->setToolTip(tr("Apply (Enter)"));
    apply->setActive(true);
    top->addWidget(apply);
    outer->addLayout(top);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(8);
    auto sliderWith = [&](const QString& label, const char* name, float position, std::function<QString(float)> set) {
        auto* caption = new QLabel(label, m_editBar);
        caption->setFont(theme::font(12, QFont::Medium));
        caption->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
        auto* slider = new Slider(m_editBar);
        slider->setObjectName(QLatin1String(name));
        slider->setMinimumWidth(170);
        slider->setValue(position);
        auto* value = new QLabel(set(position), m_editBar);
        value->setFixedWidth(52);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        bottom->addWidget(caption);
        bottom->addWidget(slider, 1);
        bottom->addWidget(value);
        connect(slider, &Slider::valueChanged, m_editBar, [value, set](float v) { value->setText(set(v)); });
    };
    // Size: 20 px .. 5000 px, finer at the small end. Shift + drag on the canvas moves it too.
    sliderWith(tr("Size"), "liquify-size", m_canvas->liquifySizeFraction(), [this](float v) {
        m_canvas->setLiquifySizeFraction(v);
        return sizeText(m_canvas->liquifySize());
    });
    {
        auto* sizeSlider = m_editBar->findChild<Slider*>(QStringLiteral("liquify-size"));
        QLabel* sizeLabel = nullptr;
        for (QLabel* label : m_editBar->findChildren<QLabel*>()) {
            if (label->text() == sizeText(m_canvas->liquifySize()))
                sizeLabel = label;
        }
        connect(m_canvas, &CanvasWidget::liquifySizeChanged, m_editBar, [this, sizeSlider, sizeLabel] {
            sizeSlider->setValue(m_canvas->liquifySizeFraction());
            if (sizeLabel)
                sizeLabel->setText(sizeText(m_canvas->liquifySize()));
        });
    }
    bottom->addSpacing(8);
    sliderWith(tr("Strength"), "liquify-strength", m_canvas->liquifyStrength(), [this](float v) {
        m_canvas->setLiquifyStrength(std::max(0.02f, v));
        return QStringLiteral("%1%").arg(qRound(v * 100.0f));
    });
    auto* reset = new IconButton(QString(), m_editBar);
    reset->setText(tr("Reset"));
    reset->setObjectName(QStringLiteral("liquify-reset"));
    reset->setToolTip(tr("Undo everything done in this session"));
    bottom->addWidget(reset);
    outer->addLayout(bottom);
    connect(reset, &QAbstractButton::clicked, m_editBar, [this] { m_engine->resetLiquify(); });
    connect(cancel, &QAbstractButton::clicked, this, [this] { m_canvas->finishLiquify(false); });
    connect(apply, &QAbstractButton::clicked, this, [this] { m_canvas->finishLiquify(true); });
    m_editBar->show();
    setEditingUi(true);
    layoutOverlays();
    m_toast->showMessage(tr("Press lightly for a little, harder for more"));
    m_canvas->setFocus();
}

// --------------------------------------------------------------------- text

bool MainWindow::placeText(const QString& text, const QFont& chosen, Qt::Alignment alignment, const QColor& color)
{
    if (editing() || text.trimmed().isEmpty())
        return false;
    TextInfo info;
    info.text = text;
    info.setFont(chosen);
    info.alignment = int(alignment & Qt::AlignHorizontal_Mask);
    info.color = color;
    // In the middle of the canvas, whichever way its lines are aligned.
    QRectF box;
    text::layout(info, &box);
    const QSize canvas = m_engine->canvasSize();
    info.anchor = QPointF(std::round(canvas.width() / 2.0 - box.center().x()), std::round(canvas.height() / 2.0 - box.center().y()));
    if (box.width() > CanvasEngine::maxCanvasDimension() || box.height() > CanvasEngine::maxCanvasDimension()) {
        m_toast->showMessage(tr("That text is too large to fit a layer"));
        return false;
    }
    closePopovers();
    m_canvas->setSelectMode(false);
    m_canvas->setGradientMode(false);
    QString name = text.simplified();
    if (name.size() > 24)
        name = name.left(23) + QChar(0x2026);
    if (!m_engine->addTextLayer(info, name))
        return false;
    startTransform();   // ready to be moved, sized and turned
    return true;
}

void MainWindow::addText()
{
    if (editing())
        return;
    closePopovers();
    m_canvas->setTextMode(true);
}

// Text mode: what is typed appears on the canvas; the bar sets how it looks.
void MainWindow::showTextBar(bool show)
{
    if (m_textBar) {
        m_textBar->hide();
        m_textBar->deleteLater();
        m_textBar = nullptr;
    }
    if (!show) {
        m_canvas->setFocus();
        return;
    }
    closePopovers();
    showGuideBar(false);
    QSettings st;
    // Start from the face and style used last time.
    QFont font(st.value(QStringLiteral("text/family"), QApplication::font().family()).toString());
    font.setPixelSize(std::clamp(st.value(QStringLiteral("text/size"), std::max(24, m_engine->canvasSize().height() / 12)).toInt(), 6, 2000));
    font.setBold(st.value(QStringLiteral("text/bold"), false).toBool());
    font.setItalic(st.value(QStringLiteral("text/italic"), false).toBool());
    m_canvas->setTextFont(font);
    const Qt::Alignment sides[3] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight};
    m_canvas->setTextAlignment(sides[std::clamp(st.value(QStringLiteral("text/align"), 0).toInt(), 0, 2)]);
    m_canvas->setTextSpacing(st.value(QStringLiteral("text/letterSpacing"), 0.0).toDouble(), st.value(QStringLiteral("text/lineSpacing"), 1.0).toDouble());

    m_textBar = new Panel(m_host, 16);
    m_textBar->setObjectName(QStringLiteral("text-bar"));
    auto* l = new QHBoxLayout(m_textBar);
    l->setContentsMargins(14, 6, 10, 6);
    l->setSpacing(6);
    auto* family = new QFontComboBox(m_textBar);
    family->setObjectName(QStringLiteral("text-font"));
    family->setCurrentFont(font);
    family->setFixedWidth(210);
    family->setFocusPolicy(Qt::NoFocus);   // the keyboard stays with the text
    family->setEditable(false);
    l->addWidget(family);
    auto* size = new Slider(m_textBar);
    size->setObjectName(QStringLiteral("text-size"));
    size->setFixedWidth(150);
    size->setToolTip(tr("Height of the letters"));
    l->addWidget(size);
    auto* sizeValue = new QLabel(m_textBar);
    sizeValue->setFixedWidth(58);
    sizeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->addWidget(sizeValue);
    auto textButton = [&](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), m_textBar);
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        l->addWidget(b);
        return b;
    };
    IconButton* bold = textButton(tr("Bold"), tr("Heavier letters"), "text-bold");
    IconButton* italic = textButton(tr("Italic"), tr("Slanted letters"), "text-italic");
    l->addSpacing(8);
    IconButton* left = textButton(tr("Left"), tr("Lines start at the same place"), "text-left");
    IconButton* middle = textButton(tr("Centre"), tr("Lines are centred on where you put the text"), "text-centre");
    IconButton* right = textButton(tr("Right"), tr("Lines end at the same place"), "text-right");
    l->addSpacing(8);
    // Room between letters and between lines, each a short slider with what it is set to.
    auto spacing = [&](const char* name, const QString& tip, int width) {
        auto* slider = new Slider(m_textBar);
        slider->setObjectName(QLatin1String(name));
        slider->setFixedWidth(width);
        slider->setToolTip(tip);
        l->addWidget(slider);
        auto* value = new QLabel(m_textBar);
        value->setMinimumWidth(66);
        value->setToolTip(tip);
        l->addWidget(value);
        return std::make_pair(slider, value);
    };
    const auto [letters, lettersValue] = spacing("text-letter-spacing", tr("Room between letters"), 90);
    const auto [lines, linesValue] = spacing("text-line-spacing", tr("Room between lines"), 90);
    l->addSpacing(4);
    auto* cancel = new IconButton(QStringLiteral("close"), m_textBar, 20);
    cancel->setObjectName(QStringLiteral("text-cancel"));
    cancel->setToolTip(tr("Cancel (Esc)"));
    l->addWidget(cancel);
    auto* apply = new IconButton(QStringLiteral("check"), m_textBar, 20);
    apply->setObjectName(QStringLiteral("text-apply"));
    apply->setToolTip(tr("Done (Ctrl+Enter)"));
    apply->setActive(true);
    l->addWidget(apply);

    // 6 px .. 1000 px, with the small sizes spread out.
    auto toSize = [](float v) { return int(std::lround(6.0 * std::pow(1000.0 / 6.0, double(v)))); };
    auto fromSize = [](int px) { return float(std::log(std::max(px, 6) / 6.0) / std::log(1000.0 / 6.0)); };
    auto sync = [=, this] {
        const QFont f = m_canvas->textFont();
        size->setValue(fromSize(f.pixelSize()));
        sizeValue->setText(tr("%1 px").arg(f.pixelSize()));
        bold->setActive(f.bold());
        italic->setActive(f.italic());
        left->setActive(m_canvas->textAlignment() == Qt::AlignLeft);
        middle->setActive(m_canvas->textAlignment() == Qt::AlignHCenter);
        right->setActive(m_canvas->textAlignment() == Qt::AlignRight);
        // Letters: -20% .. +100% of the size; lines: 0.5 .. 3 times the font's own.
        letters->setValue(float((m_canvas->textLetterSpacing() + 0.2) / 1.2));
        lines->setValue(float((m_canvas->textLineSpacing() - 0.5) / 2.5));
        lettersValue->setText(tr("letters %1%").arg(qRound(m_canvas->textLetterSpacing() * 100.0)));
        linesValue->setText(tr("lines %1×").arg(m_canvas->textLineSpacing(), 0, 'f', 2));
    };
    auto setSpacing = [=, this](double lettersNow, double linesNow) {
        m_canvas->setTextSpacing(lettersNow, linesNow);
        QSettings store;
        store.setValue(QStringLiteral("text/letterSpacing"), m_canvas->textLetterSpacing());
        store.setValue(QStringLiteral("text/lineSpacing"), m_canvas->textLineSpacing());
        sync();
    };
    auto change = [=, this](const std::function<void(QFont&)>& edit) {
        QFont f = m_canvas->textFont();
        edit(f);
        m_canvas->setTextFont(f);
        QSettings store;
        store.setValue(QStringLiteral("text/family"), f.family());
        store.setValue(QStringLiteral("text/size"), f.pixelSize());
        store.setValue(QStringLiteral("text/bold"), f.bold());
        store.setValue(QStringLiteral("text/italic"), f.italic());
        sync();
    };
    auto align = [=, this](int which) {
        const Qt::Alignment side[3] = {Qt::AlignLeft, Qt::AlignHCenter, Qt::AlignRight};
        m_canvas->setTextAlignment(side[which]);
        QSettings().setValue(QStringLiteral("text/align"), which);
        sync();
    };
    connect(family, &QFontComboBox::currentFontChanged, m_textBar, [=](const QFont& chosen) { change([&](QFont& f) { f.setFamily(chosen.family()); }); });
    // A text opened for retyping brings its own font, size and alignment.
    connect(m_canvas, &CanvasWidget::textStyleChanged, m_textBar, [=, this] {
        const QSignalBlocker quiet(family);
        family->setCurrentFont(m_canvas->textFont());
        sync();
    });
    connect(size, &Slider::valueChanged, m_textBar, [=](float v) { change([&](QFont& f) { f.setPixelSize(toSize(v)); }); });
    connect(bold, &QAbstractButton::clicked, m_textBar, [=] { change([](QFont& f) { f.setBold(!f.bold()); }); });
    connect(italic, &QAbstractButton::clicked, m_textBar, [=] { change([](QFont& f) { f.setItalic(!f.italic()); }); });
    connect(letters, &Slider::valueChanged, m_textBar, [=, this](float v) {
        setSpacing(std::round((-0.2 + 1.2 * double(v)) * 100.0) / 100.0, m_canvas->textLineSpacing());
    });
    connect(lines, &Slider::valueChanged, m_textBar, [=, this](float v) {
        setSpacing(m_canvas->textLetterSpacing(), std::round((0.5 + 2.5 * double(v)) * 20.0) / 20.0);
    });
    connect(left, &QAbstractButton::clicked, m_textBar, [=] { align(0); });
    connect(middle, &QAbstractButton::clicked, m_textBar, [=] { align(1); });
    connect(right, &QAbstractButton::clicked, m_textBar, [=] { align(2); });
    connect(cancel, &QAbstractButton::clicked, m_textBar, [this] { m_canvas->setTextMode(false); });
    connect(apply, &QAbstractButton::clicked, m_textBar, [this] { m_canvas->commitText(); });
    sync();
    m_textBar->show();
    layoutOverlays();
    m_toast->showMessage(tr("Tap where the text should go, then type"));
}

// Crop: the frame is dragged on the canvas; the bar gives its size in numbers.
void MainWindow::startCrop()
{
    if (editing())
        return;
    closePopovers();
    showGuideBar(false);
    if (!m_canvas->startCrop())
        return;
    m_editBar = new Panel(m_host, 16);
    m_editBar->setObjectName(QStringLiteral("crop-bar"));
    auto* l = new QHBoxLayout(m_editBar);
    l->setContentsMargins(16, 8, 10, 8);
    l->setSpacing(8);
    auto* title = new QLabel(tr("Canvas"), m_editBar);
    title->setFont(theme::font(13, QFont::DemiBold));
    l->addWidget(title);
    auto* width = new QSpinBox(m_editBar);
    auto* height = new QSpinBox(m_editBar);
    width->setObjectName(QStringLiteral("crop-width"));
    height->setObjectName(QStringLiteral("crop-height"));
    for (QSpinBox* box : {width, height}) {
        box->setRange(8, CanvasEngine::maxCanvasDimension());
        box->setSuffix(tr(" px"));
        box->setMinimumWidth(110);
        box->setKeyboardTracking(false);   // act on a finished number, not on every digit
    }
    l->addWidget(width);
    l->addWidget(new QLabel(QStringLiteral("×"), m_editBar));
    l->addWidget(height);
    auto* hint = new QLabel(tr("Drag the frame · past the edge adds room"), m_editBar);
    hint->setFont(theme::font(11));
    hint->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    l->addWidget(hint);
    auto* reset = new IconButton(QString(), m_editBar);
    reset->setText(tr("Reset"));
    reset->setObjectName(QStringLiteral("crop-reset"));
    reset->setToolTip(tr("Back to the canvas as it is"));
    l->addWidget(reset);
    auto* cancel = new IconButton(QStringLiteral("close"), m_editBar, 20);
    cancel->setObjectName(QStringLiteral("crop-cancel"));
    cancel->setToolTip(tr("Cancel (Esc)"));
    l->addWidget(cancel);
    auto* apply = new IconButton(QStringLiteral("check"), m_editBar, 20);
    apply->setObjectName(QStringLiteral("crop-apply"));
    apply->setToolTip(tr("Crop (Enter)"));
    apply->setActive(true);
    l->addWidget(apply);

    auto show = [=, this] {
        const QRect frame = m_canvas->cropRect();
        const QSignalBlocker quietW(width), quietH(height);
        width->setValue(frame.width());
        height->setValue(frame.height());
    };
    connect(m_canvas, &CanvasWidget::cropChanged, m_editBar, show);
    auto typed = [=, this] {
        const QRect frame = m_canvas->cropRect();
        m_canvas->setCropRect(QRect(frame.topLeft(), QSize(width->value(), height->value())));
    };
    connect(width, &QSpinBox::valueChanged, m_editBar, typed);
    connect(height, &QSpinBox::valueChanged, m_editBar, typed);
    connect(reset, &QAbstractButton::clicked, m_editBar, [this] { m_canvas->setCropRect(m_engine->canvasRect()); });
    connect(cancel, &QAbstractButton::clicked, this, [this] { m_canvas->finishCrop(false); });
    connect(apply, &QAbstractButton::clicked, this, [this] {
        const QSize size = m_canvas->cropRect().size();
        if (qint64(size.width()) * size.height() > CanvasEngine::maxCanvasPixels()) {
            m_toast->showMessage(tr("That would be %1 megapixels; the limit is %2").arg(qint64(size.width()) * size.height() / 1'000'000).arg(CanvasEngine::maxCanvasPixels() / 1'000'000));
            return;
        }
        m_canvas->finishCrop(true);
    });
    show();
    m_editBar->show();
    setEditingUi(true);
    layoutOverlays();
    m_canvas->setFocus();
}

// Resize: the whole picture is scaled to a new number of pixels.
void MainWindow::resizeImage()
{
    if (editing())
        return;
    closePopovers();
    const QSize now = m_engine->canvasSize();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Resize Image"));
    auto* form = new QFormLayout(&dlg);
    form->setContentsMargins(20, 18, 20, 16);
    form->setVerticalSpacing(10);
    auto* w = new QSpinBox(&dlg);
    auto* h = new QSpinBox(&dlg);
    for (QSpinBox* box : {w, h}) {
        box->setRange(8, CanvasEngine::maxCanvasDimension());
        box->setSuffix(tr(" px"));
        box->setMinimumWidth(130);
    }
    w->setValue(now.width());
    h->setValue(now.height());
    auto* keep = new QCheckBox(tr("Keep proportions"), &dlg);
    keep->setChecked(true);
    auto* summary = new QLabel(&dlg);
    form->addRow(tr("Width"), w);
    form->addRow(tr("Height"), h);
    form->addRow(QString(), keep);
    form->addRow(QString(), summary);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Resize"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto describe = [&] {
        summary->setText(tr("%1% of the size it is now · everything on every layer is scaled")
                             .arg(qRound(100.0 * w->value() / now.width())));
    };
    connect(w, &QSpinBox::valueChanged, &dlg, [&](int value) {
        if (keep->isChecked()) {
            const QSignalBlocker quiet(h);
            h->setValue(std::max(8, qRound(double(value) * now.height() / now.width())));
        }
        describe();
    });
    connect(h, &QSpinBox::valueChanged, &dlg, [&](int value) {
        if (keep->isChecked()) {
            const QSignalBlocker quiet(w);
            w->setValue(std::max(8, qRound(double(value) * now.width() / now.height())));
        }
        describe();
    });
    describe();
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QSize size(w->value(), h->value());
    if (size == now)
        return;
    if (qint64(size.width()) * size.height() > CanvasEngine::maxCanvasPixels()) {
        QMessageBox::warning(this, tr("Resize Image"),
                             tr("That is %1 megapixels; the limit is %2.").arg(qint64(size.width()) * size.height() / 1'000'000).arg(CanvasEngine::maxCanvasPixels() / 1'000'000));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_engine->resizeCanvas(size, QTransform::fromScale(double(size.width()) / now.width(), double(size.height()) / now.height()),
                           tr("Resize Image"));
    QApplication::restoreOverrideCursor();
}

void MainWindow::startAdjustment(Adjustment type)
{
    if (editing())
        return;
    closePopovers();
    showGuideBar(false);
    m_canvas->setSelectMode(false);
    m_canvas->setGradientMode(false);
    m_canvas->setTextMode(false);
    m_canvas->armFill(false);
    if (!m_engine->beginAdjustment(type))
        return;
    m_adjusting = true;
    m_adjustTouched = false;
    m_adjust = {};

    m_editBar = new Panel(m_host, 16);
    m_editBar->setObjectName(QStringLiteral("adjust-bar"));
    auto* outer = new QVBoxLayout(m_editBar);
    outer->setContentsMargins(18, 12, 14, 12);
    outer->setSpacing(6);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(CanvasEngine::adjustmentName(type), m_editBar);
    title->setFont(theme::font(14, QFont::DemiBold));
    header->addWidget(title);
    header->addStretch();
    auto* cancel = new IconButton(QStringLiteral("close"), m_editBar, 20);
    cancel->setObjectName(QStringLiteral("adjust-cancel"));
    cancel->setToolTip(tr("Cancel (Esc)"));
    header->addWidget(cancel);
    auto* apply = new IconButton(QStringLiteral("check"), m_editBar, 20);
    apply->setObjectName(QStringLiteral("adjust-apply"));
    apply->setToolTip(tr("Apply (Enter)"));
    apply->setActive(true);
    header->addWidget(apply);
    outer->addLayout(header);
    connect(cancel, &QAbstractButton::clicked, this, [this] { finishAdjustment(false); });
    connect(apply, &QAbstractButton::clicked, this, [this] { finishAdjustment(true); });

    // Each row: label, slider, live value. `set` writes the slider position into m_adjust.
    auto addRow = [&](const QString& label, float initial, std::function<QString(float)> set) {
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(label, m_editBar);
        name->setFixedWidth(84);
        name->setFont(theme::font(12, QFont::Medium));
        name->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
        auto* slider = new Slider(m_editBar);
        slider->setMinimumWidth(300);
        slider->setValue(initial);
        auto* value = new QLabel(set(initial), m_editBar);
        value->setFixedWidth(54);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setFont(theme::font(12));
        row->addWidget(name);
        row->addWidget(slider, 1);
        row->addWidget(value);
        outer->addLayout(row);
        connect(slider, &Slider::valueChanged, this, [this, value, set](float v) {
            value->setText(set(v));
            m_adjustTouched = true;
            m_engine->setAdjustment(m_adjust);
        });
    };
    auto percent = [](float x) { return QStringLiteral("%1%").arg(qRound(x * 100.0f)); };
    // A slider from -100% to +100% that rests in the middle, writing into m_adjust.v[index].
    auto addSigned = [&](const QString& label, int index) {
        addRow(label, 0.5f, [this, index, percent](float v) {
            m_adjust.v[index] = (v - 0.5f) * 2.0f;
            return percent(m_adjust.v[index]);
        });
    };
    auto addAmount = [&](const QString& label, int index, float initial, float scale = 1.0f) {
        addRow(label, initial, [this, index, scale, percent](float v) {
            m_adjust.v[index] = v * scale;
            return percent(v);
        });
    };
    switch (type) {
    case Adjustment::None:
        break;
    case Adjustment::GaussianBlur:
        addRow(tr("Radius"), 0.0f, [this](float v) {
            m_adjust.radius = v * v * 120.0f;
            return sizeText(m_adjust.radius);
        });
        break;
    case Adjustment::HueSaturation:
        addRow(tr("Hue"), 0.5f, [this](float v) {
            m_adjust.hue = v - 0.5f;
            return QStringLiteral("%1°").arg(qRound((v - 0.5f) * 360.0f));
        });
        addRow(tr("Saturation"), 0.5f, [this](float v) {
            m_adjust.saturation = (v - 0.5f) * 2.0f;
            return QStringLiteral("%1%").arg(qRound((v - 0.5f) * 200.0f));
        });
        addRow(tr("Brightness"), 0.5f, [this](float v) {
            m_adjust.brightness = (v - 0.5f) * 2.0f;
            return QStringLiteral("%1%").arg(qRound((v - 0.5f) * 200.0f));
        });
        break;
    case Adjustment::Levels: {
        m_adjust.v[1] = 1.0f;   // the white point, before the black point's row is made to stay below it
        // How the layer's tones are spread, with the three points marked on it.
        auto* histogram = new HistogramView(m_editBar);
        histogram->setObjectName(QStringLiteral("adjust-histogram"));
        histogram->setBins(m_engine->layerHistogram(m_engine->activeLayerId())[0]);
        histogram->setToolTip(tr("How much of the layer has each tone, dark on the left. The lines are the black point, the midtone and the white point."));
        outer->addWidget(histogram);
        // The tone that comes out as middle grey lies between the two points, where the midtone bend puts it.
        auto mark = [this, histogram] {
            const float black = m_adjust.v[0], white = m_adjust.v[1];
            histogram->setMarkers(black, black + (white - black) * std::pow(0.5f, std::max(m_adjust.v[2], 0.01f)), white);
        };
        // Black and white points say which tones become pure black and white;
        // midtones bend what lies between; the output pair limits the result.
        addRow(tr("Black point"), 0.0f, [this, mark](float v) {
            m_adjust.v[0] = std::min(v, m_adjust.v[1] - 0.004f);
            mark();
            return QString::number(qRound(m_adjust.v[0] * 255.0f));
        });
        addRow(tr("White point"), 1.0f, [this, mark](float v) {
            m_adjust.v[1] = std::max(v, m_adjust.v[0] + 0.004f);
            mark();
            return QString::number(qRound(m_adjust.v[1] * 255.0f));
        });
        addRow(tr("Midtones"), 0.5f, [this, mark](float v) {
            m_adjust.v[2] = std::pow(10.0f, (v - 0.5f) * 2.0f);   // gamma 0.1 .. 10, 1 in the middle
            mark();
            return QString::number(m_adjust.v[2], 'f', 2);
        });
        addRow(tr("Output black"), 0.0f, [this](float v) {
            m_adjust.v[3] = v;
            return QString::number(qRound(v * 255.0f));
        });
        addRow(tr("Output white"), 1.0f, [this](float v) {
            m_adjust.v[4] = v;
            return QString::number(qRound(v * 255.0f));
        });
        m_adjust.v[1] = 1.0f;   // the white point row ran before these could see each other
        m_adjust.v[0] = 0.0f;
        mark();
        break;
    }
    case Adjustment::Curves: {
        // One curve for all channels and one each for red, green and blue,
        // applied one after the other.
        auto curves = std::make_shared<std::array<QVector<QPointF>, 4>>();
        const QVector<QPointF> straight{QPointF(0, 0), QPointF(1, 1)};   // points are added by pressing on the line
        for (QVector<QPointF>& c : *curves)
            c = straight;
        auto current = std::make_shared<int>(0);
        auto* row = new QHBoxLayout;
        auto* editor = new CurveEditor(m_editBar);
        editor->setObjectName(QStringLiteral("adjust-curve"));
        editor->setFixedSize(300, 220);
        editor->setPoints(straight);
        editor->setToolTip(tr("Left to right is how light a tone is now, bottom to top what it becomes.\n"
                              "Press on the line to add a point and drag it; pull a point well above or below to remove it.\n"
                              "Behind the line: how much of the layer has each tone."));
        row->addWidget(editor);
        // Behind the curve, the tones of the channel it works on.
        const auto tones = std::make_shared<std::array<QVector<float>, 4>>(m_engine->layerHistogram(m_engine->activeLayerId()));
        const QColor tints[4] = {QColor(255, 255, 255, 46), QColor(255, 80, 70, 70), QColor(70, 220, 100, 60), QColor(80, 140, 255, 80)};
        const auto showTones = [editor, tones, tints](int channel) { editor->setHistogram((*tones)[size_t(channel)], tints[channel]); };
        showTones(0);
        auto* side = new QVBoxLayout;
        side->setSpacing(4);
        QVector<IconButton*> channelButtons;
        const QString names[4] = {tr("All"), tr("Red"), tr("Green"), tr("Blue")};
        const char* ids[4] = {"adjust-curve-all", "adjust-curve-red", "adjust-curve-green", "adjust-curve-blue"};
        for (int i = 0; i < 4; ++i) {
            auto* b = new IconButton(QString(), m_editBar);
            b->setText(names[i]);
            b->setObjectName(QLatin1String(ids[i]));
            b->setActive(i == 0);
            side->addWidget(b);
            channelButtons.push_back(b);
        }
        side->addStretch();
        auto* reset = new QPushButton(tr("Reset"), m_editBar);
        reset->setFocusPolicy(Qt::NoFocus);
        side->addWidget(reset);
        row->addLayout(side);
        outer->addLayout(row);
        auto rebuild = [this, curves] {
            QByteArray lut(1024, char(255));
            for (int i = 0; i < 256; ++i) {
                const float all = std::clamp(CurveEditor::evaluate((*curves)[0], float(i) / 255.0f), 0.0f, 1.0f);
                for (int c = 0; c < 3; ++c)
                    lut[i * 4 + c] = char(qRound(std::clamp(CurveEditor::evaluate((*curves)[size_t(c) + 1], all), 0.0f, 1.0f) * 255.0f));
            }
            m_adjust.lut = lut;
            m_adjustTouched = true;
            m_engine->setAdjustment(m_adjust);
        };
        connect(editor, &CurveEditor::changed, m_editBar, [=] {
            (*curves)[size_t(*current)] = editor->points();
            rebuild();
        });
        for (int i = 0; i < 4; ++i) {
            connect(channelButtons[i], &QAbstractButton::clicked, m_editBar, [=] {
                *current = i;
                for (int k = 0; k < 4; ++k)
                    channelButtons[k]->setActive(k == i);
                editor->setPoints((*curves)[size_t(i)]);
                showTones(i);
            });
        }
        connect(reset, &QPushButton::clicked, m_editBar, [=] {
            (*curves)[size_t(*current)] = straight;
            editor->setPoints(straight);
            rebuild();
        });
        break;
    }
    case Adjustment::BrightnessContrast:
        addSigned(tr("Brightness"), 0);
        addSigned(tr("Contrast"), 1);
        break;
    case Adjustment::ColorBalance:
        addSigned(tr("Cyan – Red"), 0);
        addSigned(tr("Magenta – Green"), 1);
        addSigned(tr("Yellow – Blue"), 2);
        break;
    case Adjustment::BlackAndWhite:
        addAmount(tr("Amount"), 0, 1.0f);
        addAmount(tr("Warmth"), 1, 0.0f);
        break;
    case Adjustment::Invert:
        addAmount(tr("Amount"), 0, 1.0f);
        break;
    case Adjustment::Posterize:
        addRow(tr("Steps"), 0.2f, [this](float v) {
            m_adjust.v[0] = float(2 + qRound(v * 30.0f));
            return QString::number(int(m_adjust.v[0]));
        });
        break;
    case Adjustment::Threshold:
        addAmount(tr("Level"), 0, 0.5f);
        break;
    case Adjustment::Sharpen:
        addRow(tr("Amount"), 0.25f, [this, percent](float v) {
            m_adjust.v[0] = v * 4.0f;
            return percent(v * 4.0f);
        });
        break;
    case Adjustment::Noise:
        addRow(tr("Amount"), 0.2f, [this, percent](float v) {
            m_adjust.v[0] = v * v;
            return percent(v * v);
        });
        addRow(tr("Colour"), 0.0f, [this, percent](float v) {
            m_adjust.v[1] = 1.0f - v;   // 1 = grey grain
            return percent(v);
        });
        m_adjust.v[4] = float(QRandomGenerator::global()->bounded(1000));
        m_adjust.v[5] = float(QRandomGenerator::global()->bounded(1000));
        break;
    case Adjustment::Mosaic:
        addRow(tr("Square"), 0.3f, [this](float v) {
            m_adjust.v[0] = 2.0f + v * v * 198.0f;
            return sizeText(m_adjust.v[0]);
        });
        break;
    }
    // These do something at their starting values: show it at once, and count it as wanted.
    switch (type) {
    case Adjustment::BlackAndWhite:
    case Adjustment::Invert:
    case Adjustment::Posterize:
    case Adjustment::Threshold:
    case Adjustment::Sharpen:
    case Adjustment::Noise:
    case Adjustment::Mosaic:
        m_adjustTouched = true;
        m_engine->setAdjustment(m_adjust);
        break;
    default:
        break;
    }

    m_editBar->show();
    setEditingUi(true);
    layoutOverlays();
}

void MainWindow::finishAdjustment(bool commit)
{
    if (!m_adjusting)
        return;
    m_adjusting = false;
    if (commit && m_adjustTouched)
        m_engine->commitEdit();
    else
        m_engine->cancelEdit();
    endEditBar();
}

void MainWindow::endEditBar()
{
    if (m_editBar) {
        m_editBar->hide();
        m_editBar->deleteLater();
        m_editBar = nullptr;
    }
    setEditingUi(false);
    m_canvas->setFocus();
    m_canvas->update();
}
