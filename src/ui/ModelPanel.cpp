#include "ui/ModelPanel.h"

#include "model/Model3D.h"
#include "ui/ModelView.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <cmath>

ModelPanel::ModelPanel(ToolState* tools, QWidget* parent) : QWidget(parent), m_tools(tools)
{
    setFixedSize(470, 600);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 12, 12);
    outer->setSpacing(8);

    auto textButton = [this](const QString& label, const QString& tip, const char* name) {
        auto* b = new IconButton(QString(), this);
        b->setText(label);
        b->setToolTip(tip);
        b->setObjectName(QLatin1String(name));
        return b;
    };
    auto* header = new QHBoxLayout;
    header->setSpacing(2);
    m_title = new QLabel(tr("3D Reference"), this);
    m_title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(m_title, 1);
    IconButton* figure = textButton(tr("Figure"), tr("The jointed figure, to pose"), "model-figure");
    IconButton* open = textButton(tr("Open"), tr("A model of your own: glTF, GLB, OBJ or STL"), "model-open");
    IconButton* place = textButton(tr("Place"), tr("The model as the view shows it, without the floor, as a new layer to draw over"), "model-place");
    m_pick = new IconButton(QStringLiteral("eyedropper"), this, 20);
    m_pick->setObjectName(QStringLiteral("model-pick"));
    m_pick->setToolTip(tr("Pick a colour from the model"));
    for (QWidget* w : {static_cast<QWidget*>(figure), static_cast<QWidget*>(open), static_cast<QWidget*>(place), static_cast<QWidget*>(m_pick)})
        header->addWidget(w);
    outer->addLayout(header);

    m_view = new ModelView(this);
    m_view->setObjectName(QStringLiteral("model-view"));
    outer->addWidget(m_view, 1);

    auto* modeRow = new QHBoxLayout;
    m_modes = new SegmentBar({tr("Turn"), tr("Pose"), tr("Light")}, this);
    m_modes->setObjectName(QStringLiteral("model-modes"));
    m_modes->setToolTip(tr("What a drag in the view does: turn the model round, move its joints, or move the light"));
    modeRow->addWidget(m_modes, 1);
    IconButton* fit = textButton(tr("Fit"), tr("All of the model in view"), "model-fit");
    modeRow->addWidget(fit);
    outer->addLayout(modeRow);

    // The figure's poses, and what posing a joint does.
    m_poses = new QWidget(this);
    auto* poseRow = new QHBoxLayout(m_poses);
    poseRow->setContentsMargins(0, 0, 0, 0);
    poseRow->setSpacing(0);
    for (const QString& pose : Model3D::figurePoses()) {
        IconButton* b = textButton(pose, tr("Put the figure in this pose"), "model-pose");
        b->setProperty("pose", pose);
        poseRow->addWidget(b);
        connect(b, &QAbstractButton::clicked, this, [this, pose] {
            if (Model3D* m = m_view->model(); m && m->figure) {
                m->applyFigurePose(pose);
                m_view->modelEdited();
                keep();
            }
        });
    }
    poseRow->addStretch();
    outer->addWidget(m_poses);

    auto* tuneRow = new QHBoxLayout;
    m_twist = textButton(tr("Twist"), tr("A drag on a joint turns the bone about itself"), "model-twist");
    IconButton* reset = textButton(tr("Reset"), tr("Back to the pose it came in"), "model-reset");
    m_hard = textButton(tr("Hard light"), tr("Two tones: where the light falls and where it does not"), "model-hard");
    m_floor = textButton(tr("Floor"), tr("A floor of squares to stand it on"), "model-floor");
    tuneRow->addWidget(m_twist);
    tuneRow->addWidget(reset);
    tuneRow->addStretch();
    tuneRow->addWidget(m_hard);
    tuneRow->addWidget(m_floor);
    outer->addLayout(tuneRow);

    auto* lensRow = new QHBoxLayout;
    auto* lensLabel = new QLabel(tr("Lens"), this);
    lensLabel->setFont(theme::font(12, QFont::Medium));
    lensLabel->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    lensLabel->setToolTip(tr("Wide (low numbers) for strong perspective, long for flat"));
    m_lens = new Slider(this);
    m_lens->setObjectName(QStringLiteral("model-lens"));
    m_lens->setToolTip(lensLabel->toolTip());
    m_lensValue = new QLabel(this);
    m_lensValue->setFont(theme::font(12));
    m_lensValue->setMinimumWidth(56);
    m_lensValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lensRow->addWidget(lensLabel);
    lensRow->addWidget(m_lens, 1);
    lensRow->addWidget(m_lensValue);
    outer->addLayout(lensRow);

    m_keep.setSingleShot(true);
    m_keep.setInterval(400);
    connect(&m_keep, &QTimer::timeout, this, [this] {
        QSettings st;
        if (Model3D* m = m_view->model()) {
            st.setValue(QStringLiteral("model/path"), m->path);
            st.setValue(QStringLiteral("model/pose"), m->poseState());
        }
        st.setValue(QStringLiteral("model/view"), m_view->viewState());
    });

    // Lens 14..200 mm, the middle of the slider near 50.
    auto toLens = [](float v) { return 14.0 * std::pow(200.0 / 14.0, double(v)); };
    connect(m_lens, &Slider::valueChanged, this, [=, this](float v) {
        m_view->setLens(toLens(v));
        m_lensValue->setText(tr("%1 mm").arg(qRound(m_view->lens())));
    });
    connect(m_modes, &SegmentBar::currentChanged, this, [this](int i) {
        m_view->setMode(ModelView::Mode(i));
        sync();
    });
    connect(fit, &QAbstractButton::clicked, this, [this] {
        m_view->fit();
        keep();
    });
    connect(m_twist, &QAbstractButton::clicked, this, [this] {
        m_view->setTwist(!m_view->twist());
        if (m_view->twist() && m_modes->current() != 1) {
            m_modes->setCurrent(1);
            m_view->setMode(ModelView::Mode::Pose);
        }
        sync();
    });
    connect(reset, &QAbstractButton::clicked, this, [this] {
        if (Model3D* m = m_view->model()) {
            m->resetPose();
            m_view->modelEdited();
            keep();
        }
    });
    connect(m_hard, &QAbstractButton::clicked, this, [this] {
        m_view->setHardLight(!m_view->hardLight());
        sync();
        keep();
    });
    connect(m_floor, &QAbstractButton::clicked, this, [this] {
        m_view->setFloor(!m_view->floor());
        sync();
        keep();
    });
    connect(m_pick, &QAbstractButton::clicked, this, [this] { m_view->setPicking(!m_view->picking()); });
    connect(m_view, &ModelView::pickingChanged, this, [this] { sync(); });
    connect(m_view, &ModelView::colorPicked, this, [this](const QColor& c) {
        m_tools->setColor(c);
        m_tools->noteColorUsed();
    });
    connect(m_view, &ModelView::edited, this, &ModelPanel::keep);
    connect(figure, &QAbstractButton::clicked, this, [this] {
        showFigure();
        keep();
    });
    connect(open, &QAbstractButton::clicked, this, [this] {
        QSettings st;
        const QString dir = st.value(QStringLiteral("model/folder"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
        const QString path = QFileDialog::getOpenFileName(window(), tr("Open a 3D Model"), dir, Model3D::fileFilters().join(QStringLiteral(";;")));
        if (path.isEmpty())
            return;
        st.setValue(QStringLiteral("model/folder"), QFileInfo(path).absolutePath());
        if (openModel(path))
            keep();
    });
    connect(place, &QAbstractButton::clicked, this, [this] {
        const QImage image = placement();
        if (!image.isNull())
            emit placeRequested(image);
    });
}

void ModelPanel::showEvent(QShowEvent*)
{
    if (m_restored)
        return;
    m_restored = true;
    // What was on show last time, as it was posed and seen.
    QSettings st;
    const QString path = st.value(QStringLiteral("model/path")).toString();
    if (path.isEmpty() || !openModel(path))
        showFigure();
    if (Model3D* m = m_view->model())
        m->setPoseState(st.value(QStringLiteral("model/pose")).toList());
    m_view->setViewState(st.value(QStringLiteral("model/view")).toMap());
    m_view->modelEdited();
    sync();
}

bool ModelPanel::openModel(const QString& path)
{
    auto model = std::make_unique<Model3D>();
    QString error;
    if (!Model3D::load(path, model.get(), &error)) {
        emit message(error);
        return false;
    }
    m_title->setText(model->name);
    const bool posable = !model->handles.empty();
    m_view->setModel(std::move(model));
    if (!posable && m_modes->current() == 1) {
        m_modes->setCurrent(0);
        m_view->setMode(ModelView::Mode::Turn);
    }
    sync();
    return true;
}

void ModelPanel::showFigure()
{
    m_view->setModel(std::make_unique<Model3D>(Model3D::makeFigure()));
    m_title->setText(tr("Figure"));
    sync();
}

void ModelPanel::setCanvasSize(const QSize& size)
{
    m_canvas = size;
    m_view->setFrameAspect(size.isEmpty() ? 0.0 : double(size.width()) / size.height());
}

QImage ModelPanel::placement() const
{
    if (m_canvas.isEmpty())
        return {};
    return m_view->renderImage(m_canvas, true);
}

void ModelPanel::sync()
{
    Model3D* m = m_view->model();
    m_poses->setVisible(m && m->figure);
    m_twist->setActive(m_view->twist());
    m_twist->setEnabled(m && !m->handles.empty());
    m_hard->setActive(m_view->hardLight());
    m_floor->setActive(m_view->floor());
    m_pick->setActive(m_view->picking());
    m_modes->setCurrent(int(m_view->mode()));
    m_lens->setValue(float(std::log(m_view->lens() / 14.0) / std::log(200.0 / 14.0)));
    m_lensValue->setText(tr("%1 mm").arg(qRound(m_view->lens())));
}

void ModelPanel::keep()
{
    m_keep.start();
}
