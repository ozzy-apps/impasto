#pragma once

#include "brush/BrushPreset.h"
#include "core/Types.h"

#include <QColor>
#include <QObject>
#include <QVector>

// The artist's current choices: which tool, which brush each tool uses, its
// size and opacity, and the colour. Paint, smudge and erase each remember
// their own brush, as in Procreate.
class ToolState : public QObject {
    Q_OBJECT
public:
    explicit ToolState(QObject* parent = nullptr);

    StrokeTool tool() const { return m_tool; }
    void setTool(StrokeTool tool);

    const BrushPreset& preset() const { return preset(m_tool); }
    const BrushPreset& preset(StrokeTool tool) const;
    void setPreset(const QString& id);
    void notifyBrushEdited();   // the current brush's settings changed in the library
    // A brush was deleted: every tool that used it moves on to `replacementId`.
    void brushRemoved(const QString& id, const QString& replacementId);

    float size() const { return slot().size; }
    float size(StrokeTool tool) const { return m_slots[int(tool)].size; }
    float opacity(StrokeTool tool) const { return m_slots[int(tool)].opacity; }
    void setSize(float px);
    float sizeFraction() const;            // slider position, 0..1
    void setSizeFraction(float t);
    float opacity() const { return slot().opacity; }
    void setOpacity(float opacity);

    QColor color() const { return m_color; }
    void setColor(const QColor& color);
    void noteColorUsed();                  // records the colour in the recents row
    const QVector<QColor>& recentColors() const { return m_recent; }
    // The colour to go back to: the last one painted with (or, after a swap,
    // the one swapped away from). Invalid while there has only been one.
    QColor previousColor() const;
    void swapColor();                      // the current colour and the previous one change places

    void load();
    void save() const;

signals:
    void toolChanged();
    void brushChanged();
    void sizeChanged();
    void opacityChanged();
    void colorChanged();
    void recentColorsChanged();

private:
    struct Slot {
        QString presetId;
        float size = 12.0f;
        float opacity = 1.0f;
    };
    Slot& slot() { return m_slots[int(m_tool)]; }
    const Slot& slot() const { return m_slots[int(m_tool)]; }

    Slot m_slots[3];
    StrokeTool m_tool = StrokeTool::Brush;
    QColor m_color = QColor(28, 28, 30);
    QColor m_swappedFrom;
    QVector<QColor> m_recent;
};
