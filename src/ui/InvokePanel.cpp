#include "ui/InvokePanel.h"

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr qint64 maxFramePixels = 32LL * 1024 * 1024;
// The footer lines up with the list above it, which leaves room for its scroll bar.
constexpr int kFooterInset = 16;

const char* kTextStyle =
    "QPlainTextEdit { background: #1f1f22; color: #f2f2f7; border: 1px solid #48484d; border-radius: 8px;"
    "                 padding: 4px 6px; selection-background-color: #0a84ff; font-size: 13px; }"
    "QPlainTextEdit:focus { border-color: #0a84ff; }"
    "QPlainTextEdit:disabled { color: #77777d; }";

const char* kPrimaryStyle =
    "QPushButton { background: #0a84ff; color: white; border-radius: 9px; padding: 9px 16px; font-size: 14px; font-weight: 600; }"
    "QPushButton:hover { background: #2b95ff; }"
    "QPushButton:pressed { background: #0a6fd6; }"
    "QPushButton:disabled { background: #3a3a3e; color: #77777d; }";

const char* kSecondaryStyle =
    "QPushButton { background: #3a3a3e; color: #f2f2f7; border-radius: 9px; padding: 9px 16px; font-size: 14px; }"
    "QPushButton:hover { background: #46464b; }"
    "QPushButton:pressed { background: #505056; }";

QString modelKey(const QComboBox* combo)
{
    return combo->currentData().toJsonObject().value(QStringLiteral("key")).toString();
}

QImage alphaMask(const QImage& painted)
{
    const QImage rgba = painted.convertToFormat(QImage::Format_ARGB32);
    if (rgba.isNull())
        return {};
    QImage mask(rgba.size(), QImage::Format_Grayscale8);
    if (mask.isNull())
        return {};
    bool hasPaint = false;
    for (int y = 0; y < mask.height(); ++y) {
        const auto* pixels = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        uchar* coverage = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            coverage[x] = qAlpha(pixels[x]);
            hasPaint |= coverage[x] != 0;
        }
    }
    return hasPaint ? mask : QImage();
}

// Softens the edge of the mask by about `radius` pixels each way: three box
// blurs across and three down, which come close to a Gaussian.
QImage feathered(const QImage& coverage, int radius)
{
    if (radius <= 0 || coverage.isNull())
        return coverage;
    const int w = coverage.width(), h = coverage.height();
    const int r = std::max(1, radius / 2);   // three passes of r spread about 2r
    std::vector<int> a(size_t(w) * h), b(a.size());
    for (int y = 0; y < h; ++y) {
        const uchar* row = coverage.constScanLine(y);
        for (int x = 0; x < w; ++x)
            a[size_t(y) * w + x] = row[x];
    }
    const int span = 2 * r + 1;
    auto pass = [&](std::vector<int>& from, std::vector<int>& to, bool across) {
        const int lines = across ? h : w, length = across ? w : h;
        for (int line = 0; line < lines; ++line) {
            auto at = [&](int i) -> int& {
                i = std::clamp(i, 0, length - 1);
                return across ? from[size_t(line) * w + i] : from[size_t(i) * w + line];
            };
            int sum = 0;
            for (int i = -r; i <= r; ++i)
                sum += at(i);
            for (int i = 0; i < length; ++i) {
                (across ? to[size_t(line) * w + i] : to[size_t(i) * w + line]) = (sum + span / 2) / span;
                sum += at(i + r + 1) - at(i - r);
            }
        }
    };
    for (int i = 0; i < 3; ++i) {
        pass(a, b, true);
        pass(b, a, false);
    }
    QImage out(coverage.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* row = out.scanLine(y);
        for (int x = 0; x < w; ++x)
            row[x] = uchar(std::clamp(a[size_t(y) * w + x], 0, 255));
    }
    return out;
}

QLabel* heading(const QString& text, QWidget* parent)
{
    auto* h = new QLabel(text, parent);
    h->setFont(theme::font(12, QFont::DemiBold));
    h->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    h->setContentsMargins(0, 10, 0, 0);
    return h;
}

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setFont(theme::font(13));
    return l;
}

IconButton* textButton(const QString& text, const QString& tip, const char* name, QWidget* parent)
{
    auto* b = new IconButton(QString(), parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setObjectName(QLatin1String(name));
    b->setMinimumHeight(34);
    return b;
}

QComboBox* combo(const char* name, QWidget* parent)
{
    auto* c = new QComboBox(parent);
    c->setObjectName(QLatin1String(name));
    c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    c->setMinimumContentsLength(10);
    c->setMinimumHeight(32);
    return c;
}

bool isCtrlEnter(const QKeyEvent* k)
{
    return (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier);
}

} // namespace

InvokePanel::InvokePanel(CanvasEngine* engine, CanvasWidget* canvas, QWidget* parent)
    : Panel(parent, 14), m_engine(engine), m_canvas(canvas)
{
    setObjectName(QStringLiteral("invoke-panel"));
    setFixedWidth(kWidth);
    QSettings options;
    options.beginGroup(QStringLiteral("invoke"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 12, 10, 14);   // the list's scroll bar takes the rest of the right margin
    outer->setSpacing(6);

    // ---- header
    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 6, 4);
    auto* mark = new QLabel(this);
    mark->setPixmap(theme::icon(QStringLiteral("invoke"), 22, theme::text, devicePixelRatioF()));
    header->addWidget(mark);
    header->addSpacing(4);
    auto* title = new QLabel(tr("Invoke"), this);
    title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(title, 1);
    m_close = new IconButton(QStringLiteral("close"), this, 18);
    m_close->setObjectName(QStringLiteral("invoke-close"));
    m_close->setToolTip(tr("Close the Invoke panel"));
    m_close->setFixedSize(34, 34);
    header->addWidget(m_close);
    outer->addLayout(header);

    // ---- the settings, scrolling (by dragging too, for the pen)
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("invoke-scroll"));
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    auto* body = new QWidget(scroll);
    body->setObjectName(QStringLiteral("invoke-body"));
    body->setAutoFillBackground(false);
    scroll->viewport()->setObjectName(QStringLiteral("invoke-viewport"));
    scroll->setStyleSheet(QStringLiteral("#invoke-scroll, #invoke-viewport, #invoke-body { background: transparent; }"));
    scroll->setWidget(body);
    new DragScroller(scroll, body);
    outer->addWidget(scroll, 1);
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(0, 0, 8, 6);
    column->setSpacing(6);

    // Server
    column->addWidget(heading(tr("Server"), body));
    auto* serverRow = new QHBoxLayout;
    m_address = new QLineEdit(options.value(QStringLiteral("address"), QStringLiteral("http://127.0.0.1:9090")).toString(), body);
    m_address->setObjectName(QStringLiteral("invoke-address"));
    m_address->setMinimumHeight(32);
    m_address->setAccessibleName(tr("Server address"));
    serverRow->addWidget(m_address, 1);
    m_connect = textButton(tr("Connect"), tr("Connect to this Invoke server and list its models"), "invoke-connect", body);
    serverRow->addWidget(m_connect);
    column->addLayout(serverRow);
    m_token = new QLineEdit(body);
    m_token->setPlaceholderText(tr("API token (optional, not kept)"));
    m_token->setAccessibleName(tr("API token"));
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setMinimumHeight(32);
    column->addWidget(m_token);

    // Model
    column->addWidget(heading(tr("Model"), body));
    m_model = combo("invoke-model", body);
    m_model->setToolTip(tr("The Anima model that paints the result"));
    column->addWidget(m_model);

    // Canvas: what to change, and where
    column->addWidget(heading(tr("Canvas"), body));
    m_mode = new SegmentBar({tr("Image to image"), tr("Inpaint")}, body);
    m_mode->setObjectName(QStringLiteral("invoke-mode"));
    m_mode->setMinimumHeight(34);
    m_mode->setCurrent(std::clamp(options.value(QStringLiteral("mode"), 0).toInt(), 0, 1));
    column->addWidget(m_mode);
    auto* frameRow = new QHBoxLayout;
    frameRow->setSpacing(2);
    m_fullFrame = textButton(tr("Full canvas"), tr("Generate over the whole canvas"), "invoke-full-frame", body);
    m_selectionFrame = textButton(tr("Selection"), tr("Generate inside the bounds of the current selection"), "invoke-selection-frame", body);
    m_paintMask = textButton(tr("Paint mask"),
                             tr("Paint on the Invoke Mask layer. Generate hides the layer and uses the opacity of your strokes as the inpaint mask."),
                             "invoke-paint-mask", body);
    frameRow->addWidget(m_fullFrame);
    frameRow->addWidget(m_selectionFrame);
    frameRow->addStretch();
    frameRow->addWidget(m_paintMask);
    column->addLayout(frameRow);
    m_frameInfo = new QLabel(body);
    m_frameInfo->setObjectName(QStringLiteral("invoke-frame-info"));
    m_frameInfo->setFont(theme::font(12));
    m_frameInfo->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    m_frameInfo->setWordWrap(true);
    column->addWidget(m_frameInfo);

    // Prompt
    column->addWidget(heading(tr("Prompt"), body));
    m_prompt = new QPlainTextEdit(body);
    m_prompt->setObjectName(QStringLiteral("invoke-prompt"));
    m_prompt->setPlaceholderText(tr("Describe the result. Ctrl+Enter generates."));
    m_prompt->setAccessibleName(tr("Prompt"));
    m_prompt->setStyleSheet(QLatin1String(kTextStyle));
    m_prompt->setFixedHeight(96);
    m_prompt->setTabChangesFocus(true);
    m_prompt->setPlainText(options.value(QStringLiteral("prompt")).toString());
    column->addWidget(m_prompt);
    m_negative = new QPlainTextEdit(body);
    m_negative->setObjectName(QStringLiteral("invoke-negative"));
    m_negative->setPlaceholderText(tr("Negative prompt: what to keep out"));
    m_negative->setAccessibleName(tr("Negative prompt"));
    m_negative->setStyleSheet(QLatin1String(kTextStyle));
    m_negative->setFixedHeight(58);
    m_negative->setTabChangesFocus(true);
    m_negative->setPlainText(options.value(QStringLiteral("negative")).toString());
    column->addWidget(m_negative);
    for (QPlainTextEdit* edit : {m_prompt, m_negative})
        edit->installEventFilter(this);

    // Generation
    column->addWidget(heading(tr("Generation"), body));
    m_strength = {nullptr, nullptr, nullptr, 5, 100, 1, 0, 0, QStringLiteral("%")};
    column->addWidget(addSetting(m_strength, tr("Denoising strength"),
                                 tr("How much the picture may change: low keeps your drawing, high paints it anew"), "invoke-strength"));
    m_steps = {nullptr, nullptr, nullptr, 4, 100, 1, 0, 0, QString()};
    column->addWidget(addSetting(m_steps, tr("Steps"), tr("More steps take longer and settle more detail. 30 suits Anima."), "invoke-steps"));
    m_guidance = {nullptr, nullptr, nullptr, 1, 15, 0.5, 0, 1, QString()};
    column->addWidget(addSetting(m_guidance, tr("CFG scale"),
                                 tr("How closely the prompt is followed. 4 to 5 suits Anima; 1 turns the negative prompt off."), "invoke-guidance"));
    m_size = {nullptr, nullptr, nullptr, 512, 2048, 64, 0, 0, tr(" px")};
    column->addWidget(addSetting(m_size, tr("Render size"),
                                 tr("The longer side of the picture Invoke paints. The result is scaled to fit the frame."), "invoke-size"));
    m_feather = {nullptr, nullptr, nullptr, 0, 64, 1, 0, 0, tr(" px")};
    column->addWidget(addSetting(m_feather, tr("Mask feather"),
                                 tr("Softens the edge where the inpainted part meets your painting"), "invoke-feather"));
    setSetting(m_strength, options.value(QStringLiteral("strength"), 55).toDouble());
    setSetting(m_steps, options.value(QStringLiteral("steps"), 30).toDouble());
    setSetting(m_guidance, options.value(QStringLiteral("guidance"), 4.5).toDouble());
    setSetting(m_size, options.value(QStringLiteral("size"), 1024).toDouble());
    setSetting(m_feather, options.value(QStringLiteral("feather"), 8).toDouble());

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 4, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);
    m_scheduler = combo("invoke-scheduler", body);
    m_scheduler->addItem(tr("Euler"), QStringLiteral("euler"));
    m_scheduler->addItem(tr("Heun"), QStringLiteral("heun"));
    m_scheduler->addItem(tr("DPM++ 2M"), QStringLiteral("dpmpp_2m"));
    m_scheduler->addItem(tr("DPM++ 2M SDE"), QStringLiteral("dpmpp_2m_sde"));
    m_scheduler->addItem(tr("ER-SDE"), QStringLiteral("er_sde"));
    m_scheduler->addItem(tr("LCM"), QStringLiteral("lcm"));
    m_scheduler->setCurrentIndex(std::max(0, m_scheduler->findData(options.value(QStringLiteral("scheduler")))));
    grid->addWidget(caption(tr("Sampler"), body), 0, 0);
    grid->addWidget(m_scheduler, 0, 1, 1, 2);
    m_seed = new QLineEdit(body);
    m_seed->setObjectName(QStringLiteral("invoke-seed"));
    m_seed->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{0,10}")), m_seed));
    m_seed->setPlaceholderText(tr("Random"));
    m_seed->setToolTip(tr("Leave empty for a new seed each time; the same seed and settings paint the same picture"));
    m_seed->setMinimumHeight(32);
    m_lastSeed = textButton(tr("Reuse"), tr("Use the seed of the last result"), "invoke-last-seed", body);
    grid->addWidget(caption(tr("Seed"), body), 1, 0);
    grid->addWidget(m_seed, 1, 1);
    grid->addWidget(m_lastSeed, 1, 2);
    column->addLayout(grid);

    // Components the model needs
    column->addWidget(heading(tr("Components"), body));
    auto* parts = new QGridLayout;
    parts->setHorizontalSpacing(10);
    parts->setVerticalSpacing(8);
    parts->setColumnStretch(1, 1);
    m_encoder = combo("invoke-encoder", body);
    m_encoder->setToolTip(tr("The Qwen3 0.6B text encoder that reads the prompt"));
    m_vae = combo("invoke-vae", body);
    m_vae->setToolTip(tr("The VAE that turns pixels into latents and back"));
    parts->addWidget(caption(tr("Encoder"), body), 0, 0);
    parts->addWidget(m_encoder, 0, 1);
    parts->addWidget(caption(tr("VAE"), body), 1, 0);
    parts->addWidget(m_vae, 1, 1);
    column->addLayout(parts);
    column->addStretch(1);

    // ---- always in reach: what is happening, and the buttons that act
    auto* rule = new QWidget(this);
    rule->setFixedHeight(1);
    rule->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 28);"));
    rule->setContentsMargins(0, 0, kFooterInset, 0);
    outer->addWidget(rule);
    m_status = new QLabel(tr("Connect to an Invoke server."), this);
    m_status->setObjectName(QStringLiteral("invoke-status"));
    m_status->setFont(theme::font(12));
    m_status->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->setContentsMargins(0, 2, kFooterInset, 0);
    outer->addWidget(m_status);

    auto* run = new QHBoxLayout;
    run->setContentsMargins(0, 0, kFooterInset, 0);
    m_generate = new QPushButton(tr("Generate"), this);
    m_generate->setObjectName(QStringLiteral("invoke-generate"));
    m_generate->setStyleSheet(QLatin1String(kPrimaryStyle));
    m_generate->setMinimumHeight(42);
    m_generate->setToolTip(tr("Generate (Ctrl+Enter in the prompt)"));
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("invoke-cancel"));
    m_cancel->setStyleSheet(QLatin1String(kSecondaryStyle));
    m_cancel->setMinimumHeight(42);
    run->addWidget(m_generate, 1);
    run->addWidget(m_cancel);
    outer->addLayout(run);

    m_resultRow = new QWidget(this);
    auto* result = new QHBoxLayout(m_resultRow);
    result->setContentsMargins(0, 0, kFooterInset, 0);
    result->setSpacing(4);
    m_accept = new QPushButton(tr("Add as layer"), m_resultRow);
    m_accept->setObjectName(QStringLiteral("invoke-accept"));
    m_accept->setStyleSheet(QLatin1String(kPrimaryStyle));
    m_accept->setMinimumHeight(42);
    m_preview = textButton(tr("Compare"), tr("Show the canvas without the result, to compare; press again for the result"), "invoke-preview", m_resultRow);
    m_discard = textButton(tr("Discard"), tr("Throw the result away"), "invoke-discard", m_resultRow);
    result->addWidget(m_accept, 1);
    result->addWidget(m_preview);
    result->addWidget(m_discard);
    outer->addWidget(m_resultRow);

    // ---- wiring
    const auto connectServer = [this] {
        if (m_client.busy())
            return;
        m_connectionEdited = false;
        m_client.connectTo(m_address->text(), m_token->text());
    };
    const auto connectionEdited = [this] {
        m_connectionEdited = true;
        if (m_client.connected())
            m_status->setText(tr("Reconnect to use these connection settings."));
        refresh();
    };
    connect(m_close, &QAbstractButton::clicked, this, &InvokePanel::closeRequested);
    connect(m_address, &QLineEdit::textChanged, this, connectionEdited);
    connect(m_token, &QLineEdit::textChanged, this, connectionEdited);
    connect(m_connect, &QAbstractButton::clicked, this, connectServer);
    connect(m_address, &QLineEdit::returnPressed, this, connectServer);
    connect(m_token, &QLineEdit::returnPressed, this, connectServer);
    connect(m_selectionFrame, &QAbstractButton::clicked, this, [this] {
        m_frame = m_engine->selectionBounds();
        refresh();
        refreshOverlay();
        m_canvas->setFocus();
    });
    connect(m_fullFrame, &QAbstractButton::clicked, this, [this] {
        m_frame = {};
        refresh();
        refreshOverlay();
        m_canvas->setFocus();
    });
    connect(m_paintMask, &QAbstractButton::clicked, this, &InvokePanel::paintMask);
    connect(m_lastSeed, &QAbstractButton::clicked, this, [this] { m_seed->setText(QString::number(m_lastSeedValue)); });
    connect(m_generate, &QPushButton::clicked, this, &InvokePanel::generate);
    connect(m_cancel, &QPushButton::clicked, &m_client, &InvokeClient::cancel);
    connect(m_accept, &QPushButton::clicked, this, &InvokePanel::accept);
    connect(m_preview, &QAbstractButton::clicked, this, [this] {
        m_previewShown = !m_previewShown;
        refreshOverlay();
        refresh();
    });
    connect(m_discard, &QAbstractButton::clicked, this, [this] {
        clearResult();
        m_status->setText(tr("Result discarded."));
    });
    connect(m_mode, &SegmentBar::currentChanged, this, &InvokePanel::refresh);
    connect(m_prompt, &QPlainTextEdit::textChanged, this, &InvokePanel::refresh);
    for (QComboBox* c : {m_model, m_encoder, m_vae})
        connect(c, &QComboBox::currentIndexChanged, this, &InvokePanel::refresh);

    connect(&m_client, &InvokeClient::stateChanged, this, [this] {
        if (!m_client.busy() && m_result.isNull()) {
            m_jobFrame = {};
            m_jobMask = {};
        }
        refresh();
    });
    connect(&m_client, &InvokeClient::statusChanged, m_status, &QLabel::setText);
    connect(&m_client, &InvokeClient::modelsChanged, this, &InvokePanel::loadModels);
    connect(&m_client, &InvokeClient::imageReady, this, &InvokePanel::receive);

    connect(m_engine, &CanvasEngine::selectionChanged, this, &InvokePanel::refresh);
    connect(m_engine, &CanvasEngine::documentChanged, this, &InvokePanel::resetCanvas);
    connect(m_engine, &CanvasEngine::canvasFlipped, this, &InvokePanel::resetFrame);
    connect(m_engine, &CanvasEngine::animationChanged, this, &InvokePanel::resetCanvas);
    resetCanvas();
}

// Opaque, unlike the floating bars: a column this tall should not let the
// canvas show through it.
void InvokePanel::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor fill = theme::bar;
    fill.setAlpha(255);
    p.setPen(QPen(theme::panelBorder, 1));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
}

// A setting as the Brush Lab shows one: its name and value on a line, and a
// slider the width of the panel under it, tall enough to land the pen on.
QWidget* InvokePanel::addSetting(Setting& s, const QString& label, const QString& tip, const char* name)
{
    auto* box = new QWidget;
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 2, 0, 2);
    v->setSpacing(0);
    auto* top = new QHBoxLayout;
    auto* title = caption(label, box);
    title->setToolTip(tip);
    top->addWidget(title);
    top->addStretch();
    s.pill = new QLabel(box);
    s.pill->setObjectName(QLatin1String(name) + QStringLiteral("-value"));
    s.pill->setFont(theme::font(12, QFont::Medium));
    s.pill->setAlignment(Qt::AlignCenter);
    s.pill->setMinimumWidth(56);
    s.pill->setStyleSheet(QStringLiteral("background: #141416; border-radius: 9px; padding: 2px 9px;"));
    top->addWidget(s.pill);
    v->addLayout(top);
    s.slider = new Slider(box);
    s.slider->setObjectName(QLatin1String(name));
    s.slider->setToolTip(tip);
    s.slider->setFixedHeight(32);
    v->addWidget(s.slider);
    s.row = box;
    Setting* setting = &s;
    connect(s.slider, &Slider::valueChanged, this, [this, setting](float f) {
        setSetting(*setting, setting->min + double(f) * (setting->max - setting->min));
        refresh();
    });
    connect(s.slider, &Slider::released, this, &InvokePanel::saveSettings);
    return box;
}

void InvokePanel::setSetting(Setting& s, double value)
{
    const double steps = std::round((std::clamp(value, s.min, s.max) - s.min) / s.step);
    s.value = std::min(s.max, s.min + steps * s.step);
    s.slider->setValue(float((s.value - s.min) / (s.max - s.min)));   // the knob lands on the step
    s.pill->setText(QString::number(s.value, 'f', s.decimals) + s.suffix);
}

void InvokePanel::saveSettings() const
{
    QSettings options;
    options.beginGroup(QStringLiteral("invoke"));
    options.setValue(QStringLiteral("mode"), m_mode->current());
    options.setValue(QStringLiteral("strength"), m_strength.value);
    options.setValue(QStringLiteral("size"), m_size.value);
    options.setValue(QStringLiteral("steps"), m_steps.value);
    options.setValue(QStringLiteral("guidance"), m_guidance.value);
    options.setValue(QStringLiteral("feather"), m_feather.value);
    options.setValue(QStringLiteral("scheduler"), m_scheduler->currentData().toString());
    options.setValue(QStringLiteral("prompt"), m_prompt->toPlainText());
    options.setValue(QStringLiteral("negative"), m_negative->toPlainText());
}

bool InvokePanel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_prompt || obj == m_negative) {
        // The window's own Ctrl+Enter (which finishes text on the canvas) would
        // take the key first; claim it here, where it means Generate.
        if (e->type() == QEvent::ShortcutOverride && isCtrlEnter(static_cast<QKeyEvent*>(e))) {
            e->accept();
            return true;
        }
        if (e->type() == QEvent::KeyPress && isCtrlEnter(static_cast<QKeyEvent*>(e))) {
            generateFromKeyboard();
            return true;
        }
    }
    return Panel::eventFilter(obj, e);
}

void InvokePanel::loadModels(const QJsonArray& models)
{
    for (QComboBox* c : {m_model, m_encoder, m_vae}) {
        const QSignalBlocker block(c);
        c->clear();
    }
    if (!m_client.connected())
        return;

    QSettings options;
    options.beginGroup(QStringLiteral("invoke"));
    options.setValue(QStringLiteral("address"), m_address->text().trimmed());
    for (const QJsonValue& entry : models) {
        auto data = entry.toObject();
        const QString base = data.value(QStringLiteral("base")).toString();
        const QString type = data.value(QStringLiteral("type")).toString();
        const QString variant = data.value(QStringLiteral("variant")).toString();
        const QString name = data.value(QStringLiteral("name")).toString();
        if (name.isEmpty() || data.value(QStringLiteral("key")).toString().isEmpty())
            continue;
        if (type == QLatin1String("main") && base == QLatin1String("anima") && (variant.isEmpty() || variant == QLatin1String("anima_qwen3")))
            m_model->addItem(name, data);
        else if (type == QLatin1String("qwen3_encoder") &&
                 (variant.isEmpty() ? name.contains(QLatin1String("Anima"), Qt::CaseInsensitive) : variant == QLatin1String("qwen3_06b")))
            m_encoder->addItem(name, data);
        else if (type == QLatin1String("vae") && (base == QLatin1String("anima") || base == QLatin1String("qwen-image")))
            m_vae->addItem(name, data);
    }
    auto select = [&options](QComboBox* c, const QString& field, const QString& preferred) {
        const QString key = options.value(field).toString();
        for (int i = 0; i < c->count(); ++i) {
            if (c->itemData(i).toJsonObject().value(QStringLiteral("key")).toString() == key) {
                c->setCurrentIndex(i);
                return;
            }
        }
        const int preferredIndex = c->findText(preferred, Qt::MatchFixedString);
        if (preferredIndex >= 0)
            c->setCurrentIndex(preferredIndex);
    };
    select(m_model, QStringLiteral("model"), QStringLiteral("CakePop"));
    select(m_encoder, QStringLiteral("encoder"), QStringLiteral("Anima Qwen3 0.6B Text Encoder"));
    select(m_vae, QStringLiteral("vae"), QStringLiteral("Anima QwenImage VAE"));
    refresh();
}

bool InvokePanel::inpainting() const { return m_mode->current() == 1; }

QRect InvokePanel::frame() const
{
    return m_frame.isEmpty() ? m_engine->canvasRect() : m_frame.intersected(m_engine->canvasRect());
}

QSize InvokePanel::renderSize(const QSize& frameSize) const
{
    if (frameSize.isEmpty())
        return {};
    const double scale = m_size.value / std::max(frameSize.width(), frameSize.height());
    auto align = [scale](int value) { return std::max(32, int(std::round(value * scale / 16.0)) * 16); };
    return {align(frameSize.width()), align(frameSize.height())};
}

void InvokePanel::refresh()
{
    const bool busy = m_client.busy();
    const bool supported = m_engine->hasDocument() && !m_engine->animation().enabled;
    const bool hasResult = !m_result.isNull();
    const bool missingModels = m_model->count() == 0 || m_encoder->count() == 0 || m_vae->count() == 0;
    const bool settingsOpen = !busy && !hasResult;
    m_connect->setText(m_client.connected() && !m_connectionEdited ? tr("Reconnect") : tr("Connect"));
    m_connect->setEnabled(!busy);
    m_address->setEnabled(!busy);
    m_token->setEnabled(!busy);
    m_selectionFrame->setEnabled(supported && m_engine->hasSelection() && settingsOpen);
    m_fullFrame->setEnabled(supported && settingsOpen);
    m_selectionFrame->setActive(!m_frame.isEmpty());
    m_fullFrame->setActive(m_frame.isEmpty());
    m_paintMask->setVisible(inpainting());
    m_paintMask->setEnabled(supported && settingsOpen);
    m_feather.row->setVisible(inpainting());
    m_mode->setEnabled(settingsOpen);
    for (Setting* s : {&m_strength, &m_steps, &m_guidance, &m_size, &m_feather})
        s->slider->setEnabled(settingsOpen);
    for (QComboBox* c : {m_model, m_encoder, m_vae, m_scheduler})
        c->setEnabled(settingsOpen);
    m_seed->setEnabled(settingsOpen);
    m_lastSeed->setEnabled(settingsOpen && m_hasLastSeed);
    m_generate->setEnabled(m_client.connected() && !m_connectionEdited && !missingModels && supported && settingsOpen &&
                           !m_prompt->toPlainText().trimmed().isEmpty());
    m_generate->setText(busy ? tr("Generating…") : tr("Generate"));
    m_generate->setVisible(!hasResult);
    m_cancel->setVisible(busy && m_client.canCancel());
    m_resultRow->setVisible(hasResult);
    m_preview->setActive(!m_previewShown);
    m_preview->setText(m_previewShown ? tr("Compare") : tr("Result"));
    if (m_engine->animation().enabled) {
        m_frameInfo->setText(tr("Turn off Animation Assist to generate."));
    } else if (m_client.connected() && missingModels) {
        m_frameInfo->setText(tr("Install an Anima model, the Qwen3 0.6B encoder and an Anima VAE in Invoke."));
    } else {
        const QSize px = frame().size();
        const QSize render = renderSize(px);
        m_frameInfo->setText(px.isEmpty() ? QString()
                                          : tr("%1 × %2 on the canvas, painted at %3 × %4")
                                                .arg(px.width()).arg(px.height()).arg(render.width()).arg(render.height()));
    }
}

void InvokePanel::setOverlayVisible(bool visible)
{
    m_overlayVisible = visible;
    refreshOverlay();
}

void InvokePanel::refreshOverlay()
{
    if (!m_overlayVisible || m_engine->animation().enabled) {
        m_canvas->setInvokeOverlay({}, {}, {});
        return;
    }
    m_canvas->setInvokeOverlay(m_frame, m_jobFrame, m_previewShown ? m_result : QImage());
}

void InvokePanel::resetFrame()
{
    const bool hadPreview = !m_result.isNull();
    m_frame = {};
    clearResult();
    if (hadPreview)
        m_status->setText(tr("Preview cleared because the canvas changed."));
}

void InvokePanel::resetCanvas()
{
    m_maskLayer = 0;
    m_layerBeforeMask = 0;
    if (!m_engine->animation().enabled) {
        for (const auto& layer : m_engine->layers()) {
            if (!layer.isGroup && layer.parent == 0 && layer.name == tr("Invoke Mask"))
                m_maskLayer = layer.id;
        }
    }
    resetFrame();
}

void InvokePanel::paintMask()
{
    const int active = m_engine->activeLayerId();
    if (active != m_maskLayer)
        m_layerBeforeMask = active;
    if (!m_engine->layerInfo(m_maskLayer).id)
        m_maskLayer = m_engine->addLayer(tr("Invoke Mask"), true);
    if (!m_maskLayer) {
        m_status->setText(tr("Could not create a mask layer."));
        return;
    }
    m_engine->setActiveLayer(m_maskLayer);
    m_engine->setLayerVisible(m_maskLayer, true);
    m_engine->setMaskEditing(false);
    emit paintMaskRequested();
    m_canvas->setFocus();
    m_status->setText(tr("Paint the area to regenerate on the Invoke Mask layer."));
}

// Ctrl+Enter in a prompt: generate if it can, and say why not if it cannot.
void InvokePanel::generateFromKeyboard()
{
    if (m_generate->isEnabled() && m_generate->isVisible()) {
        generate();
        return;
    }
    if (m_client.busy())
        m_status->setText(tr("Still working on the last one."));
    else if (!m_result.isNull())
        m_status->setText(tr("Add the result as a layer or discard it first."));
    else if (m_engine->animation().enabled)
        m_status->setText(tr("Turn off Animation Assist to generate."));
    else if (!m_client.connected() || m_connectionEdited)
        m_status->setText(tr("Connect to an Invoke server first."));
    else if (m_model->count() == 0 || m_encoder->count() == 0 || m_vae->count() == 0)
        m_status->setText(tr("Invoke needs an Anima model, a Qwen3 0.6B encoder and a compatible VAE."));
    else if (m_prompt->toPlainText().trimmed().isEmpty())
        m_status->setText(tr("Write a prompt first."));
}

void InvokePanel::generate()
{
    if (m_engine->strokeActive() || m_engine->floodActive() || m_engine->editActive())
        return;
    const QRect bounds = frame();
    if (bounds.width() < 16 || bounds.height() < 16 || qint64(bounds.width()) * bounds.height() > maxFramePixels) {
        m_status->setText(tr("Choose a frame at least 16 × 16 pixels and at most 32 megapixels."));
        return;
    }
    QImage coverage;
    QImage mask;
    const QSize size = renderSize(bounds.size());
    if (inpainting()) {
        coverage = alphaMask(m_engine->rasterLayerImage(m_maskLayer, bounds));
        if (coverage.isNull()) {
            m_status->setText(tr("Paint a mask inside the generation frame."));
            return;
        }
        // The Anima denoise node interprets black as regenerate.
        mask = coverage.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        mask.invertPixels();
        // The result is laid in through a softened copy of the painted mask.
        coverage = feathered(coverage, int(m_feather.value));
    }

    InvokeClient::Generation generation;
    generation.model = m_model->currentData().toJsonObject();
    generation.encoder = m_encoder->currentData().toJsonObject();
    generation.vae = m_vae->currentData().toJsonObject();
    generation.prompt = m_prompt->toPlainText().trimmed();
    generation.negative = m_negative->toPlainText().trimmed();
    generation.steps = int(m_steps.value);
    generation.strength = m_strength.value / 100.0;
    generation.guidance = m_guidance.value;
    generation.scheduler = m_scheduler->currentData().toString();
    bool seedValid = false;
    const quint64 seed = m_seed->text().toULongLong(&seedValid);
    if (!m_seed->text().isEmpty() && (!seedValid || seed > std::numeric_limits<quint32>::max())) {
        m_status->setText(tr("Seed must be between 0 and 4,294,967,295."));
        return;
    }
    generation.seed = seedValid ? quint32(seed) : QRandomGenerator::global()->generate();

    if (m_engine->layerInfo(m_maskLayer).id) {
        m_engine->setLayerVisible(m_maskLayer, false);
        if (m_engine->activeLayerId() == m_maskLayer) {
            int restore = m_engine->layerInfo(m_layerBeforeMask).id;
            if (!restore) {
                const auto layers = m_engine->layers();
                for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
                    if (it->id != m_maskLayer && !it->isGroup && it->visible) {
                        restore = it->id;
                        break;
                    }
                }
            }
            m_engine->setActiveLayer(restore);
        }
    }
    const QImage canvas = m_engine->renderImage(bounds);
    if (canvas.isNull()) {
        m_status->setText(tr("Could not read the canvas."));
        return;
    }
    QImage source(size, QImage::Format_RGB32);
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(source.rect(), canvas);
    }
    m_jobFrame = bounds;
    m_jobMask = coverage;
    m_jobSeed = generation.seed;
    saveSettings();
    QSettings options;
    options.beginGroup(QStringLiteral("invoke"));
    options.setValue(QStringLiteral("model"), modelKey(m_model));
    options.setValue(QStringLiteral("encoder"), modelKey(m_encoder));
    options.setValue(QStringLiteral("vae"), modelKey(m_vae));
    m_client.generate(generation, source, mask);
    m_canvas->setFocus();
}

void InvokePanel::receive(const QImage& result)
{
    if (m_jobFrame.isEmpty()) {
        m_status->setText(tr("Result skipped because the canvas changed."));
        return;
    }
    m_result = result.scaled(m_jobFrame.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                   .convertToFormat(QImage::Format_ARGB32);
    if (m_result.isNull()) {
        clearResult();
        m_status->setText(tr("Could not prepare the result for this canvas."));
        return;
    }
    if (!m_jobMask.isNull()) {
        for (int y = 0; y < m_result.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(m_result.scanLine(y));
            const uchar* coverage = m_jobMask.constScanLine(y);
            for (int x = 0; x < m_result.width(); ++x)
                row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]), (qAlpha(row[x]) * coverage[x] + 127) / 255);
        }
        m_jobMask = {};
    }
    m_hasLastSeed = true;
    m_lastSeedValue = m_jobSeed;
    m_status->setText(tr("Ready to compare, add or discard · seed %1").arg(m_jobSeed));
    m_previewShown = true;
    refreshOverlay();
    refresh();
}

void InvokePanel::clearResult()
{
    m_result = {};
    m_jobMask = {};
    m_jobFrame = {};
    m_previewShown = true;
    refreshOverlay();
    refresh();
}

void InvokePanel::accept()
{
    if (m_engine->addImageLayer(m_result, tr("Invoke Generation"), m_jobFrame.topLeft(), tr("Add Invoke Layer"), true)) {
        clearResult();
        m_status->setText(tr("Generation added as a layer."));
        m_canvas->setFocus();
    } else {
        m_status->setText(tr("Could not add the layer. The preview is still available."));
    }
}
