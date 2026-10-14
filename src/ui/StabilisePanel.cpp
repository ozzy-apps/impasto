#include "ui/StabilisePanel.h"

#include "brush/BrushPreset.h"
#include "ui/Theme.h"
#include "ui/ToolState.h"
#include "ui/Widgets.h"

#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace {

// The Brush Lab's Stabilisation section, in the same words.
struct Setting {
    const char* name;
    const char* label;
    const char* tip;
    float BrushPreset::*field;
};
const Setting kSettings[4] = {
    {"steady-streamline", QT_TRANSLATE_NOOP("BrushLab", "StreamLine"),
     QT_TRANSLATE_NOOP("BrushLab", "The line glides after the pen: smoother, with a little lag"), &BrushPreset::streamline},
    {"steady-stabilization", QT_TRANSLATE_NOOP("BrushLab", "Stabilisation"),
     QT_TRANSLATE_NOOP("BrushLab", "Averages the path; the faster the stroke, the more it is smoothed"), &BrushPreset::stabilization},
    {"steady-string", QT_TRANSLATE_NOOP("BrushLab", "Pull string"),
     QT_TRANSLATE_NOOP("BrushLab", "The line only follows once the pen pulls a string taut: slow, exact lines with crisp corners"),
     &BrushPreset::stringLength},
    {"steady-pressure", QT_TRANSLATE_NOOP("BrushLab", "Weight smoothing"),
     QT_TRANSLATE_NOOP("BrushLab", "Steadies the line weight against small changes in pressure"), &BrushPreset::pressureSmoothing},
};

QString percent(float v) { return QStringLiteral("%1%").arg(qRound(v * 100.0f)); }

} // namespace

StabilisePanel::StabilisePanel(ToolState* tools, QWidget* parent) : QWidget(parent), m_tools(tools)
{
    setFixedWidth(360);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 12, 12, 14);
    outer->setSpacing(4);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("Stabilisation"), this);
    title->setFont(theme::font(17, QFont::DemiBold));
    header->addWidget(title, 1);
    auto* lab = new IconButton(QString(), this);
    lab->setText(tr("Brush Lab"));
    lab->setObjectName(QStringLiteral("steady-lab"));
    lab->setToolTip(tr("Everything else about this brush"));
    header->addWidget(lab);
    outer->addLayout(header);
    m_brush = new QLabel(this);
    m_brush->setObjectName(QStringLiteral("steady-brush"));
    m_brush->setFont(theme::font(12, QFont::Medium));
    m_brush->setStyleSheet(QStringLiteral("color: #a0a0a8;"));
    outer->addWidget(m_brush);
    outer->addSpacing(8);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);
    auto row = [&](int r, const QString& label, const QString& tip, const char* name, QLabel** value) {
        auto* text = new QLabel(label, this);
        text->setFont(theme::font(13));
        text->setToolTip(tip);
        auto* slider = new Slider(this);
        slider->setObjectName(QLatin1String(name));
        slider->setToolTip(tip);
        *value = new QLabel(this);
        (*value)->setFont(theme::font(12));
        (*value)->setMinimumWidth(38);
        (*value)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(text, r, 0);
        grid->addWidget(slider, r, 1);
        grid->addWidget(*value, r, 2);
        return slider;
    };
    for (int i = 0; i < 4; ++i) {
        m_sliders[i] = row(i, QCoreApplication::translate("BrushLab", kSettings[i].label), QCoreApplication::translate("BrushLab", kSettings[i].tip),
                            kSettings[i].name, &m_values[i]);
    }

    // What Preferences adds on top of every brush's own, under a rule, in line with the rest.
    auto* rule = new QWidget(this);
    rule->setFixedHeight(1);
    rule->setStyleSheet(QStringLiteral("background: rgba(255, 255, 255, 28);"));
    grid->addWidget(rule, 4, 0, 1, 3);
    grid->setRowMinimumHeight(4, 18);
    m_every = row(5, tr("All brushes"), tr("Smoothing added to every brush, on top of its own (also in Preferences)"), "steady-every", &m_everyValue);
    outer->addLayout(grid);

    for (int i = 0; i < 4; ++i) {
        connect(m_sliders[i], &Slider::valueChanged, this, [this, i](float v) {
            BrushPreset brush = m_tools->preset();
            brush.*kSettings[i].field = v;
            BrushLibrary::instance().update(brush);
            m_tools->notifyBrushEdited();   // brings sync() round
        });
        connect(m_sliders[i], &Slider::released, this, [] { BrushLibrary::instance().save(); });
    }
    connect(m_every, &Slider::valueChanged, this, [this](float v) {
        m_everyValue->setText(percent(v));
        emit everyBrushChanged(v);
    });
    connect(lab, &QAbstractButton::clicked, this, &StabilisePanel::labRequested);
    connect(m_tools, &ToolState::brushChanged, this, &StabilisePanel::sync);
    connect(m_tools, &ToolState::toolChanged, this, &StabilisePanel::sync);
    sync();
}

float StabilisePanel::everyBrush() const { return m_every->value(); }

void StabilisePanel::setEveryBrush(float amount)
{
    m_every->setValue(amount);
    m_everyValue->setText(percent(amount));
}

void StabilisePanel::showEvent(QShowEvent*) { sync(); }

void StabilisePanel::sync()
{
    const BrushPreset& brush = m_tools->preset();
    const QString tool = m_tools->tool() == StrokeTool::Smudge ? tr("smudging")
                         : m_tools->tool() == StrokeTool::Eraser ? tr("erasing")
                                                                 : tr("painting");
    m_brush->setText(tr("%1, the brush for %2").arg(brush.name, tool));
    for (int i = 0; i < 4; ++i) {
        const float v = brush.*kSettings[i].field;
        m_sliders[i]->setValue(v);
        m_values[i]->setText(percent(v));
    }
}
