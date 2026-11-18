#include "ui/InvokePanel.h"

#include "engine/CanvasEngine.h"
#include "ui/CanvasWidget.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QGridLayout>
#include <QJsonArray>
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

QString roleName(InvokePanel::Guide kind)
{
    switch (kind) {
    case InvokePanel::Guide::Inpaint: return InvokePanel::tr("Inpaint mask");
    case InvokePanel::Guide::Region: return InvokePanel::tr("Region");
    case InvokePanel::Guide::Control: return InvokePanel::tr("Control");
    case InvokePanel::Guide::None: break;
    }
    return {};
}

QString roleKey(InvokePanel::Guide kind)
{
    switch (kind) {
    case InvokePanel::Guide::Inpaint: return QStringLiteral("inpaint");
    case InvokePanel::Guide::Region: return QStringLiteral("region");
    case InvokePanel::Guide::Control: return QStringLiteral("control");
    case InvokePanel::Guide::None: break;
    }
    return {};
}

// The adapter a control layer uses unless another is chosen: the sketch one,
// which reads lines drawn by hand, when the server has it.
QString defaultControlModel(const QJsonArray& models)
{
    QString key;
    for (const QJsonValue& v : models) {
        const QJsonObject m = v.toObject();
        if (key.isEmpty() || m.value(QStringLiteral("name")).toString().contains(QLatin1String("sketch"), Qt::CaseInsensitive))
            key = m.value(QStringLiteral("key")).toString();
    }
    return key;
}

// Lines for a control adapter: white on black. A layer of lines on
// transparency gives its coverage; a layer painted edge to edge (a scan, a
// drawing on white) gives its dark marks.
QImage controlLines(const QImage& painted)
{
    const QImage rgba = painted.convertToFormat(QImage::Format_ARGB32);
    if (rgba.isNull())
        return {};
    qint64 opaque = 0;
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* px = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x)
            opaque += qAlpha(px[x]) > 240;
    }
    const bool filled = opaque > qint64(rgba.width()) * rgba.height() * 9 / 10;
    QImage out(rgba.size(), QImage::Format_RGB32);
    bool any = false;
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* px = reinterpret_cast<const QRgb*>(rgba.constScanLine(y));
        auto* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < rgba.width(); ++x) {
            int v;
            if (filled)
                v = 255 - qGray(px[x]);
            else
                v = qAlpha(px[x]);
            any |= v > 8;
            o[x] = qRgb(v, v, v);
        }
    }
    return any ? out : QImage();
}

} // namespace

namespace invokeui {

// One mask or guide in the panel: its name and what it is, whether Generate
// uses it, a button to paint it, and what else it needs (a region's prompt, a
// control's adapter and strength).
class GuideRow : public QWidget {
public:
    std::function<void(int id, const QJsonObject& invoke, bool now)> changed;
    std::function<void(int id)> paint, remove;

    GuideRow(int id, InvokePanel::Guide kind, QWidget* parent) : QWidget(parent), m_id(id), m_kind(kind)
    {
        setObjectName(QStringLiteral("invoke-guide-%1").arg(id));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(10, 6, 6, 10);
        v->setSpacing(6);
        auto* top = new QHBoxLayout;
        top->setSpacing(2);
        auto* names = new QVBoxLayout;
        names->setSpacing(0);
        m_name = new QLabel(this);
        m_name->setFont(theme::font(13, QFont::DemiBold));
        m_role = new QLabel(roleName(kind), this);
        m_role->setFont(theme::font(11));
        m_role->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
        names->addWidget(m_name);
        names->addWidget(m_role);
        top->addLayout(names, 1);
        m_eye = new IconButton(QStringLiteral("eye"), this, 18);
        m_eye->setObjectName(QStringLiteral("invoke-guide-use"));
        m_eye->setToolTip(InvokePanel::tr("Use it when generating, or leave it out"));
        m_eye->setFixedSize(34, 34);
        m_paint = new IconButton(QString(), this);
        m_paint->setText(InvokePanel::tr("Paint"));
        m_paint->setObjectName(QStringLiteral("invoke-guide-paint"));
        m_paint->setToolTip(InvokePanel::tr("Paint on this layer with any brush; the eraser takes paint away"));
        m_paint->setMinimumHeight(34);
        m_remove = new IconButton(QStringLiteral("trash"), this, 18);
        m_remove->setObjectName(QStringLiteral("invoke-guide-remove"));
        m_remove->setToolTip(InvokePanel::tr("Delete this layer (it can be undone)"));
        m_remove->setFixedSize(34, 34);
        top->addWidget(m_eye);
        top->addWidget(m_paint);
        top->addWidget(m_remove);
        v->addLayout(top);
        connect(m_eye, &QAbstractButton::clicked, this, [this] {
            m_data[QStringLiteral("enabled")] = !enabled();
            showEnabled();
            emitChange(true);
        });
        connect(m_paint, &QAbstractButton::clicked, this, [this] {
            if (paint)
                paint(m_id);
        });
        connect(m_remove, &QAbstractButton::clicked, this, [this] {
            if (remove)
                remove(m_id);
        });

        if (kind == InvokePanel::Guide::Region) {
            m_prompt = new QPlainTextEdit(this);
            m_prompt->setObjectName(QStringLiteral("invoke-guide-prompt"));
            m_prompt->setPlaceholderText(InvokePanel::tr("What goes where this is painted"));
            m_prompt->setStyleSheet(QLatin1String(kTextStyle));
            m_prompt->setFixedHeight(58);
            m_prompt->setTabChangesFocus(true);
            v->addWidget(m_prompt);
            m_negative = new QLineEdit(this);
            m_negative->setObjectName(QStringLiteral("invoke-guide-negative"));
            m_negative->setPlaceholderText(InvokePanel::tr("What to keep out of it (negative)"));
            m_negative->setMinimumHeight(30);
            v->addWidget(m_negative);
            connect(m_prompt, &QPlainTextEdit::textChanged, this, [this] {
                m_data[QStringLiteral("prompt")] = m_prompt->toPlainText();
                emitChange(false);
            });
            connect(m_negative, &QLineEdit::textEdited, this, [this] {
                m_data[QStringLiteral("negative")] = m_negative->text();
                emitChange(false);
            });
        } else if (kind == InvokePanel::Guide::Control) {
            m_model = new QComboBox(this);
            m_model->setObjectName(QStringLiteral("invoke-guide-model"));
            m_model->setToolTip(InvokePanel::tr("The ControlNet-LLLite adapter that reads the lines"));
            m_model->setMinimumHeight(32);
            m_model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            m_model->setMinimumContentsLength(10);
            m_model->setPlaceholderText(InvokePanel::tr("Connect to choose an adapter"));
            v->addWidget(m_model);
            connect(m_model, &QComboBox::activated, this, [this] {
                m_data[QStringLiteral("model")] = m_model->currentData().toString();
                emitChange(true);
            });
            m_weight = addSlider(v, InvokePanel::tr("Strength"), InvokePanel::tr("How firmly the lines are followed"), "invoke-guide-weight",
                                 &m_weightPill, QStringLiteral("weight"), 0.0, 2.0, 1.0);
            m_end = addSlider(v, InvokePanel::tr("Until"), InvokePanel::tr("How far through the steps the lines are followed; less leaves the last details free"),
                              "invoke-guide-end", &m_endPill, QStringLiteral("end"), 0.0, 1.0, 1.0);
        }
    }

    int id() const { return m_id; }
    InvokePanel::Guide kind() const { return m_kind; }
    bool enabled() const { return m_data.value(QStringLiteral("enabled")).toBool(true); }
    const QJsonObject& data() const { return m_data; }

    void update(const LayerInfo& layer, bool active, const QJsonArray& controlModels)
    {
        if (layer.invoke.isEmpty())
            m_data = {{QStringLiteral("role"), roleKey(m_kind)}};   // an Invoke Mask from before masks were tagged
        else
            m_data = layer.invoke;
        m_name->setText(layer.name);
        if (m_active != active) {
            m_active = active;
            QWidget::update();
        }
        showEnabled();
        if (m_prompt && !m_prompt->hasFocus()) {
            const QString text = m_data.value(QStringLiteral("prompt")).toString();
            if (m_prompt->toPlainText() != text) {
                const QSignalBlocker b(m_prompt);
                m_prompt->setPlainText(text);
            }
        }
        if (m_negative && !m_negative->hasFocus())
            m_negative->setText(m_data.value(QStringLiteral("negative")).toString());
        if (m_model) {
            const QSignalBlocker b(m_model);
            m_model->clear();
            for (const QJsonValue& v : controlModels) {
                const QJsonObject m = v.toObject();
                m_model->addItem(m.value(QStringLiteral("name")).toString(), m.value(QStringLiteral("key")).toString());
            }
            QString key = m_data.value(QStringLiteral("model")).toString();
            if (key.isEmpty())
                key = defaultControlModel(controlModels);
            const int at = m_model->findData(key);
            if (at >= 0) {
                m_model->setCurrentIndex(at);
            } else if (!key.isEmpty()) {
                m_model->insertItem(0, InvokePanel::tr("Not on this server"), key);
                m_model->setCurrentIndex(0);
            }
            m_model->setEnabled(m_model->count() > 0);
        }
        for (auto [slider, pill, field, lo, hi, fallback] :
             {std::tuple{m_weight, m_weightPill, QStringLiteral("weight"), 0.0, 2.0, 1.0}, std::tuple{m_end, m_endPill, QStringLiteral("end"), 0.0, 1.0, 1.0}}) {
            if (!slider)
                continue;
            const double value = m_data.value(field).toDouble(fallback);
            slider->setValue(float((value - lo) / (hi - lo)));
            pill->setText(field == QLatin1String("end") ? QStringLiteral("%1 %").arg(int(std::lround(value * 100))) : QString::number(value, 'f', 2));
        }
    }

    void setEditable(bool on)
    {
        for (QWidget* w : std::initializer_list<QWidget*>{m_eye, m_paint, m_remove, m_prompt, m_negative, m_model, m_weight, m_end})
            if (w)
                w->setEnabled(on);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(m_active ? theme::accent : QColor(0x48, 0x48, 0x4d), m_active ? 1.5 : 1.0));
        p.setBrush(QColor(0x1f, 0x1f, 0x22));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75), 10, 10);
    }

private:
    Slider* addSlider(QVBoxLayout* v, const QString& label, const QString& tip, const char* name, QLabel** pill, const QString& field,
                      double lo, double hi, double fallback)
    {
        auto* top = new QHBoxLayout;
        top->addWidget(caption(label, this));
        top->addStretch();
        *pill = new QLabel(this);
        (*pill)->setObjectName(QLatin1String(name) + QStringLiteral("-value"));
        (*pill)->setFont(theme::font(12, QFont::Medium));
        (*pill)->setAlignment(Qt::AlignCenter);
        (*pill)->setMinimumWidth(56);
        (*pill)->setStyleSheet(QStringLiteral("background: #141416; border-radius: 9px; padding: 2px 9px;"));
        top->addWidget(*pill);
        v->addLayout(top);
        auto* slider = new Slider(this);
        slider->setObjectName(QLatin1String(name));
        slider->setToolTip(tip);
        slider->setFixedHeight(30);
        v->addWidget(slider);
        QLabel* shown = *pill;
        Q_UNUSED(fallback);
        connect(slider, &Slider::valueChanged, this, [this, shown, field, lo, hi](float f) {
            // Strength in hundredths, the end in whole percent.
            const double step = field == QLatin1String("end") ? 0.01 : 0.05;
            const double value = std::round((lo + double(f) * (hi - lo)) / step) * step;
            m_data[field] = value;
            shown->setText(field == QLatin1String("end") ? QStringLiteral("%1 %").arg(int(std::lround(value * 100))) : QString::number(value, 'f', 2));
            emitChange(false);
        });
        connect(slider, &Slider::released, this, [this] { emitChange(true); });
        return slider;
    }
    void showEnabled()
    {
        m_eye->setIconName(enabled() ? QStringLiteral("eye") : QStringLiteral("eye-off"));
        m_eye->setActive(enabled());
        m_name->setStyleSheet(enabled() ? QString() : QStringLiteral("color: #77777d;"));
    }
    void emitChange(bool now)
    {
        if (changed)
            changed(m_id, m_data, now);
    }

    int m_id;
    InvokePanel::Guide m_kind;
    QJsonObject m_data;
    bool m_active = false;
    QLabel *m_name, *m_role;
    IconButton *m_eye, *m_paint, *m_remove;
    QPlainTextEdit* m_prompt = nullptr;
    QLineEdit* m_negative = nullptr;
    QComboBox* m_model = nullptr;
    Slider *m_weight = nullptr, *m_end = nullptr;
    QLabel *m_weightPill = nullptr, *m_endPill = nullptr;
};

} // namespace invokeui

using invokeui::GuideRow;

InvokePanel::Guide InvokePanel::guideOf(const LayerInfo& layer)
{
    if (layer.isGroup)
        return Guide::None;
    const QString role = layer.invoke.value(QStringLiteral("role")).toString();
    if (role == QLatin1String("inpaint"))
        return Guide::Inpaint;
    if (role == QLatin1String("region"))
        return Guide::Region;
    if (role == QLatin1String("control"))
        return Guide::Control;
    // The mask layer as 0.14 made it, before layers carried what they are.
    if (layer.invoke.isEmpty() && layer.parent == 0 && layer.name == tr("Invoke Mask"))
        return Guide::Inpaint;
    return Guide::None;
}

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

    // Canvas: where to generate
    column->addWidget(heading(tr("Canvas"), body));
    auto* frameRow = new QHBoxLayout;
    frameRow->setSpacing(2);
    m_fullFrame = textButton(tr("Full canvas"), tr("Generate over the whole canvas"), "invoke-full-frame", body);
    m_selectionFrame = textButton(tr("Selection"), tr("Generate inside the bounds of the current selection"), "invoke-selection-frame", body);
    frameRow->addWidget(m_fullFrame);
    frameRow->addWidget(m_selectionFrame);
    frameRow->addStretch();
    column->addLayout(frameRow);
    m_frameInfo = new QLabel(body);
    m_frameInfo->setObjectName(QStringLiteral("invoke-frame-info"));
    m_frameInfo->setFont(theme::font(12));
    m_frameInfo->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    m_frameInfo->setWordWrap(true);
    column->addWidget(m_frameInfo);

    // Masks and guides: layers that tell Invoke where and how
    column->addWidget(heading(tr("Masks and guides"), body));
    auto* addRow = new QHBoxLayout;
    addRow->setSpacing(2);
    m_addInpaint = textButton(tr("+ Inpaint"), tr("A mask layer: only where it is painted is generated anew"), "invoke-add-inpaint", body);
    m_addRegion = textButton(tr("+ Region"), tr("A region layer with its own prompt, for where it is painted"), "invoke-add-region", body);
    m_addControl = textButton(tr("+ Control"),
                              tr("A layer of lines for a ControlNet-LLLite adapter to follow. While the layer is shown it is part of the picture too; hide it to use it only as a guide."),
                              "invoke-add-control", body);
    addRow->addWidget(m_addInpaint);
    addRow->addWidget(m_addRegion);
    addRow->addWidget(m_addControl);
    addRow->addStretch();
    column->addLayout(addRow);
    m_guideList = new QWidget(body);
    m_guideList->setObjectName(QStringLiteral("invoke-guides"));
    auto* guides = new QVBoxLayout(m_guideList);
    guides->setContentsMargins(0, 0, 0, 0);
    guides->setSpacing(6);
    column->addWidget(m_guideList);
    m_guideHint = new QLabel(tr("Paint masks and guides with any brush, on layers of their own. Generate leaves masks and regions out of the picture it sends; hide them before exporting."), body);
    m_guideHint->setObjectName(QStringLiteral("invoke-guide-hint"));
    m_guideHint->setFont(theme::font(12));
    m_guideHint->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    m_guideHint->setWordWrap(true);
    column->addWidget(m_guideHint);
    m_guideTimer.setSingleShot(true);
    m_guideTimer.setInterval(600);
    connect(&m_guideTimer, &QTimer::timeout, this, &InvokePanel::flushGuides);

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
    m_inpaintAdapter = combo("invoke-inpaint-adapter", body);
    m_inpaintAdapter->setToolTip(tr("A ControlNet-LLLite inpainting adapter helps the inpainted part fit what is around it"));
    parts->addWidget(caption(tr("Inpaint"), body), 2, 0);
    parts->addWidget(m_inpaintAdapter, 2, 1);
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
    connect(m_addInpaint, &QAbstractButton::clicked, this, [this] { paintGuide(addGuide(Guide::Inpaint)); });
    connect(m_addRegion, &QAbstractButton::clicked, this, [this] { paintGuide(addGuide(Guide::Region)); });
    connect(m_addControl, &QAbstractButton::clicked, this, [this] { paintGuide(addGuide(Guide::Control)); });
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
    connect(m_inpaintAdapter, &QComboBox::activated, this, &InvokePanel::saveSettings);
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
    connect(m_engine, &CanvasEngine::layersChanged, this, &InvokePanel::refreshGuides);
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
    options.setValue(QStringLiteral("inpaintAdapter"), modelKey(m_inpaintAdapter));
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
    for (QComboBox* c : {m_model, m_encoder, m_vae, m_inpaintAdapter}) {
        const QSignalBlocker block(c);
        c->clear();
    }
    m_controlModels = {};
    if (!m_client.connected()) {
        refreshGuides();
        return;
    }
    m_inpaintAdapter->addItem(tr("No adapter"), QJsonObject());

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
        else if (type == QLatin1String("controlnet") && base == QLatin1String("anima")) {
            if (name.contains(QLatin1String("inpaint"), Qt::CaseInsensitive))
                m_inpaintAdapter->addItem(name, data);
            else
                m_controlModels.append(data);
        }
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
    select(m_inpaintAdapter, QStringLiteral("inpaintAdapter"), tr("No adapter"));
    refreshGuides();
    refresh();
}

bool InvokePanel::hasInpaintMask() const
{
    for (const GuideRow* row : m_rows)
        if (row->kind() == Guide::Inpaint && row->enabled())
            return true;
    return false;
}

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
    for (IconButton* b : {m_addInpaint, m_addRegion, m_addControl})
        b->setEnabled(supported && settingsOpen);
    for (GuideRow* row : m_rows)
        row->setEditable(supported && settingsOpen);
    m_guideHint->setVisible(m_rows.isEmpty());
    m_feather.row->setVisible(hasInpaintMask());
    for (Setting* s : {&m_strength, &m_steps, &m_guidance, &m_size, &m_feather})
        s->slider->setEnabled(settingsOpen);
    for (QComboBox* c : {m_model, m_encoder, m_vae, m_scheduler})
        c->setEnabled(settingsOpen);
    m_inpaintAdapter->setEnabled(settingsOpen && m_inpaintAdapter->count() > 1);
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
        QString text = px.isEmpty() ? QString()
                                    : tr("%1 × %2 on the canvas, painted at %3 × %4")
                                          .arg(px.width()).arg(px.height()).arg(render.width()).arg(render.height());
        int regions = 0, controls = 0;
        for (const GuideRow* row : m_rows) {
            regions += row->enabled() && row->kind() == Guide::Region;
            controls += row->enabled() && row->kind() == Guide::Control;
        }
        QStringList uses;
        if (hasInpaintMask())
            uses << tr("inpainting the masked part");
        if (regions)
            uses << (regions == 1 ? tr("1 region") : tr("%1 regions").arg(regions));
        if (controls)
            uses << (controls == 1 ? tr("1 control") : tr("%1 controls").arg(controls));
        if (!uses.isEmpty() && !text.isEmpty())
            text += QStringLiteral(" · ") + uses.join(QStringLiteral(" · "));
        m_frameInfo->setText(text);
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
    m_pendingGuides.clear();
    m_guideTimer.stop();
    refreshGuides();
    resetFrame();
}

// The rows follow the tagged layers, top first, as the layer panel shows them.
void InvokePanel::refreshGuides()
{
    QVector<LayerInfo> found;
    const QVector<LayerInfo> layers = (m_engine->hasDocument() && !m_engine->animation().enabled) ? m_engine->layers() : QVector<LayerInfo>();
    for (auto it = layers.crbegin(); it != layers.crend(); ++it)
        if (guideOf(*it) != Guide::None)
            found.push_back(*it);
    // Rows are kept for layers still there, so what is being typed into one stays put.
    QVector<GuideRow*> rows;
    auto* list = static_cast<QVBoxLayout*>(m_guideList->layout());
    for (const LayerInfo& layer : found) {
        GuideRow* row = nullptr;
        for (GuideRow* old : m_rows)
            if (old->id() == layer.id && old->kind() == guideOf(layer))
                row = old;
        if (!row) {
            row = new GuideRow(layer.id, guideOf(layer), m_guideList);
            row->changed = [this](int id, const QJsonObject& invoke, bool now) { storeGuide(id, invoke, now); };
            row->paint = [this](int id) { paintGuide(id); };
            row->remove = [this](int id) {
                m_pendingGuides.remove(id);
                m_engine->removeLayer(id);
                m_canvas->setFocus();
            };
        }
        rows.push_back(row);
    }
    for (GuideRow* old : m_rows) {
        if (!rows.contains(old)) {
            // It may be the row whose button is being pressed: let it finish first.
            list->removeWidget(old);
            old->setObjectName(QString());
            old->hide();
            old->deleteLater();
        }
    }
    for (int i = 0; i < rows.size(); ++i) {
        if (list->indexOf(rows[i]) != i) {
            list->removeWidget(rows[i]);
            list->insertWidget(i, rows[i]);
        }
        rows[i]->show();
    }
    m_rows = rows;
    const int active = m_engine->hasDocument() ? m_engine->activeLayerId() : 0;
    for (int i = 0; i < found.size(); ++i) {
        LayerInfo layer = found[i];
        if (m_pendingGuides.contains(layer.id))
            layer.invoke = m_pendingGuides.value(layer.id);   // what is being typed wins over what the layer has yet
        m_rows[i]->update(layer, layer.id == active, m_controlModels);
    }
    refresh();
}

int InvokePanel::addGuide(Guide kind)
{
    if (!m_engine->hasDocument() || m_engine->animation().enabled || m_engine->strokeActive() || m_engine->editActive())
        return 0;
    int count = 1;
    for (const GuideRow* row : m_rows)
        count += row->kind() == kind;
    const QString name = kind == Guide::Inpaint ? tr("Inpaint Mask %1").arg(count)
                         : kind == Guide::Region ? tr("Region %1").arg(count)
                                                 : tr("Control %1").arg(count);
    const int id = m_engine->addLayer(name, true);
    if (!id) {
        m_status->setText(tr("Could not create a layer."));
        return 0;
    }
    QJsonObject invoke{{QStringLiteral("role"), roleKey(kind)}, {QStringLiteral("enabled"), true}};
    if (kind == Guide::Control) {
        invoke.insert(QStringLiteral("model"), defaultControlModel(m_controlModels));
        invoke.insert(QStringLiteral("weight"), 1.0);
        invoke.insert(QStringLiteral("end"), 1.0);
    } else {
        // Masks and regions show through, so the picture can be seen under them.
        m_engine->setLayerOpacity(id, 0.5f);
    }
    m_engine->setLayerInvoke(id, invoke, tr("New %1").arg(roleName(kind)));
    return id;
}

void InvokePanel::storeGuide(int id, const QJsonObject& invoke, bool now)
{
    m_pendingGuides.insert(id, invoke);
    if (now)
        flushGuides();
    else
        m_guideTimer.start();
    refresh();
}

void InvokePanel::flushGuides()
{
    m_guideTimer.stop();
    const QHash<int, QJsonObject> pending = std::exchange(m_pendingGuides, {});
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        m_engine->setLayerInvoke(it.key(), it.value());
}

void InvokePanel::paintGuide(int id)
{
    if (!id || !m_engine->layerInfo(id).id)
        return;
    m_engine->setActiveLayer(id);
    if (!m_engine->layerInfo(id).visible)
        m_engine->setLayerVisible(id, true);
    m_engine->setMaskEditing(false);
    emit paintMaskRequested();
    m_canvas->setFocus();
    const Guide kind = guideOf(m_engine->layerInfo(id));
    m_status->setText(kind == Guide::Inpaint  ? tr("Paint where the picture is to be generated anew.")
                      : kind == Guide::Region ? tr("Paint where this region's prompt applies.")
                                              : tr("Draw the lines for Invoke to follow."));
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
    flushGuides();
    QImage coverage;
    QImage mask;
    const QSize size = renderSize(bounds.size());
    InvokeClient::Generation generation;
    QVector<int> leftOut;      // masks and regions are not part of the picture sent
    QStringList skipped;
    bool anyInpaint = false;
    for (const GuideRow* row : m_rows) {
        const LayerInfo layer = m_engine->layerInfo(row->id());
        if (!layer.id)
            continue;
        if (row->kind() != Guide::Control)
            leftOut.push_back(layer.id);
        if (!row->enabled())
            continue;
        const QImage painted = m_engine->rasterLayerImage(layer.id, bounds);
        if (row->kind() == Guide::Inpaint) {
            anyInpaint = true;
            const QImage part = alphaMask(painted);
            if (part.isNull())
                continue;
            if (coverage.isNull()) {
                coverage = part;
            } else {
                // Several masks: whatever any of them covers.
                for (int y = 0; y < coverage.height(); ++y) {
                    uchar* to = coverage.scanLine(y);
                    const uchar* from = part.constScanLine(y);
                    for (int x = 0; x < coverage.width(); ++x)
                        to[x] = std::max(to[x], from[x]);
                }
            }
        } else if (row->kind() == Guide::Region) {
            const QString prompt = row->data().value(QStringLiteral("prompt")).toString().trimmed();
            const QString negative = row->data().value(QStringLiteral("negative")).toString().trimmed();
            const QImage part = alphaMask(painted);
            if (part.isNull() || (prompt.isEmpty() && (negative.isEmpty() || m_guidance.value <= 1.0))) {
                skipped << layer.name;
                continue;
            }
            // White, as opaque as the paint: Invoke reads the alpha.
            const QImage scaled = part.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QImage regionMask(size, QImage::Format_ARGB32);
            for (int y = 0; y < size.height(); ++y) {
                auto* to = reinterpret_cast<QRgb*>(regionMask.scanLine(y));
                const uchar* from = scaled.constScanLine(y);
                for (int x = 0; x < size.width(); ++x)
                    to[x] = qRgba(255, 255, 255, from[x]);
            }
            generation.regions.push_back({prompt, negative, regionMask});
        } else if (row->kind() == Guide::Control) {
            QString key = row->data().value(QStringLiteral("model")).toString();
            if (key.isEmpty())
                key = defaultControlModel(m_controlModels);
            QJsonObject model;
            for (const QJsonValue& v : m_controlModels)
                if (v.toObject().value(QStringLiteral("key")).toString() == key)
                    model = v.toObject();
            const QImage lines = controlLines(painted);
            if (model.isEmpty() || lines.isNull()) {
                skipped << layer.name;
                continue;
            }
            InvokeClient::Generation::Control control;
            control.model = model;
            control.image = lines.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            control.weight = row->data().value(QStringLiteral("weight")).toDouble(1.0);
            control.end = row->data().value(QStringLiteral("end")).toDouble(1.0);
            generation.controls.push_back(control);
        }
    }
    if (anyInpaint && coverage.isNull()) {
        m_status->setText(tr("Paint the inpaint mask inside the generation frame, or turn the mask off."));
        return;
    }
    if (!coverage.isNull()) {
        // The Anima denoise node interprets black as regenerate.
        mask = coverage.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        mask.invertPixels();
        // The result is laid in through a softened copy of the painted mask.
        coverage = feathered(coverage, int(m_feather.value));
        generation.inpaintAdapter = m_inpaintAdapter->currentData().toJsonObject();
    }

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

    const QImage canvas = m_engine->renderImageWithout(bounds, leftOut);
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
    if (!skipped.isEmpty())
        m_status->setText(m_status->text() + QStringLiteral(" · ") + tr("left out, with nothing painted in the frame or nothing to say: %1").arg(skipped.join(QStringLiteral(", "))));
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
