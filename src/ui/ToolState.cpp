#include "ui/ToolState.h"

#include <QSettings>
#include <cmath>

namespace {
// Brush sizes span three orders of magnitude, so the slider is not linear.
constexpr float kSizeCurve = 2.2f;
const char* const kToolKeys[3] = {"brush", "smudge", "eraser"};
}

ToolState::ToolState(QObject* parent) : QObject(parent)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    const char* defaults[3] = {"round-brush", "airbrush-medium", "airbrush-hard"};
    for (int i = 0; i < 3; ++i) {
        const BrushPreset* p = lib.find(QString::fromLatin1(defaults[i]));
        if (!p)
            p = &lib.fallback();
        m_slots[i].presetId = p->id;
        m_slots[i].size = p->size;
        m_slots[i].opacity = i == int(StrokeTool::Brush) ? p->opacity : 1.0f;
    }
    m_slots[int(StrokeTool::Smudge)].opacity = 0.85f;
}

void ToolState::setTool(StrokeTool tool)
{
    if (tool == m_tool)
        return;
    m_tool = tool;
    emit toolChanged();
    emit brushChanged();
    emit sizeChanged();
    emit opacityChanged();
}

const BrushPreset& ToolState::preset(StrokeTool tool) const
{
    const BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset* p = lib.find(m_slots[int(tool)].presetId);
    return p ? *p : lib.fallback();
}

void ToolState::setPreset(const QString& id)
{
    const BrushPreset* p = BrushLibrary::instance().find(id);
    if (!p)
        return;
    Slot& s = slot();
    s.presetId = p->id;
    s.size = p->size;
    if (m_tool == StrokeTool::Brush)
        s.opacity = p->opacity;
    emit brushChanged();
    emit sizeChanged();
    emit opacityChanged();
}

void ToolState::notifyBrushEdited()
{
    for (int i = 0; i < 3; ++i) {
        const BrushPreset& p = preset(StrokeTool(i));
        m_slots[i].size = std::clamp(m_slots[i].size, p.minSize, p.maxSize);
    }
    emit brushChanged();
    emit sizeChanged();
}

void ToolState::brushRemoved(const QString& id, const QString& replacementId)
{
    const BrushLibrary& lib = BrushLibrary::instance();
    const BrushPreset* next = lib.find(replacementId);
    if (!next)
        next = &lib.fallback();
    for (int i = 0; i < 3; ++i) {
        if (m_slots[i].presetId != id)
            continue;
        m_slots[i].presetId = next->id;
        m_slots[i].size = next->size;
    }
    emit brushChanged();
    emit sizeChanged();
    emit opacityChanged();
}

void ToolState::setSize(float px)
{
    const BrushPreset& p = preset();
    px = std::clamp(px, p.minSize, p.maxSize);
    if (qFuzzyCompare(px, slot().size))
        return;
    slot().size = px;
    emit sizeChanged();
}

float ToolState::sizeFraction() const
{
    const BrushPreset& p = preset();
    const float t = (slot().size - p.minSize) / std::max(p.maxSize - p.minSize, 0.001f);
    return std::pow(std::clamp(t, 0.0f, 1.0f), 1.0f / kSizeCurve);
}

void ToolState::setSizeFraction(float t)
{
    const BrushPreset& p = preset();
    setSize(p.minSize + (p.maxSize - p.minSize) * std::pow(std::clamp(t, 0.0f, 1.0f), kSizeCurve));
}

void ToolState::setOpacity(float opacity)
{
    opacity = std::clamp(opacity, 0.01f, 1.0f);
    if (qFuzzyCompare(opacity, slot().opacity))
        return;
    slot().opacity = opacity;
    emit opacityChanged();
}

void ToolState::setColor(const QColor& color)
{
    if (!color.isValid())
        return;
    const QColor opaque(color.red(), color.green(), color.blue());
    if (opaque == m_color)
        return;
    m_color = opaque;
    m_swappedFrom = QColor();   // a colour chosen afresh: what it replaces is the last one painted with
    emit colorChanged();
}

// After a swap, the colour swapped away from, so that swapping again returns
// to it even if it was never painted with. Otherwise the last colour painted
// with that is not the current one.
QColor ToolState::previousColor() const
{
    if (m_swappedFrom.isValid() && m_swappedFrom != m_color)
        return m_swappedFrom;
    for (const QColor& c : m_recent) {
        if (c != m_color)
            return c;
    }
    return {};
}

void ToolState::swapColor()
{
    const QColor back = previousColor();
    if (!back.isValid())
        return;
    m_swappedFrom = m_color;
    m_color = back;
    emit colorChanged();
}

void ToolState::noteColorUsed()
{
    if (!m_recent.isEmpty() && m_recent.first() == m_color)
        return;
    m_recent.removeAll(m_color);
    m_recent.prepend(m_color);
    while (m_recent.size() > 10)
        m_recent.removeLast();
    emit recentColorsChanged();
}

void ToolState::load()
{
    QSettings st;
    const BrushLibrary& lib = BrushLibrary::instance();
    for (int i = 0; i < 3; ++i) {
        const QString base = QStringLiteral("tools/%1/").arg(QLatin1String(kToolKeys[i]));
        const QString id = st.value(base + QStringLiteral("preset")).toString();
        if (const BrushPreset* p = lib.find(id)) {
            m_slots[i].presetId = id;
            m_slots[i].size = std::clamp(st.value(base + QStringLiteral("size"), p->size).toFloat(), p->minSize, p->maxSize);
            m_slots[i].opacity = std::clamp(st.value(base + QStringLiteral("opacity"), 1.0f).toFloat(), 0.01f, 1.0f);
        }
    }
    const QColor c(st.value(QStringLiteral("tools/color"), m_color.name()).toString());
    if (c.isValid())
        m_color = c;
    m_swappedFrom = QColor(st.value(QStringLiteral("tools/swappedFrom")).toString());
    m_recent.clear();
    for (const QString& name : st.value(QStringLiteral("tools/recent")).toStringList()) {
        const QColor rc(name);
        if (rc.isValid())
            m_recent.push_back(rc);
    }
}

void ToolState::save() const
{
    QSettings st;
    for (int i = 0; i < 3; ++i) {
        const QString base = QStringLiteral("tools/%1/").arg(QLatin1String(kToolKeys[i]));
        st.setValue(base + QStringLiteral("preset"), m_slots[i].presetId);
        st.setValue(base + QStringLiteral("size"), m_slots[i].size);
        st.setValue(base + QStringLiteral("opacity"), m_slots[i].opacity);
    }
    st.setValue(QStringLiteral("tools/color"), m_color.name());
    st.setValue(QStringLiteral("tools/swappedFrom"), m_swappedFrom.isValid() ? m_swappedFrom.name() : QString());
    QStringList recent;
    for (const QColor& c : m_recent)
        recent << c.name();
    st.setValue(QStringLiteral("tools/recent"), recent);
}
