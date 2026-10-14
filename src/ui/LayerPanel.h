#pragma once

#include "engine/CanvasEngine.h"

#include <QHash>
#include <QPixmap>
#include <QWidget>

class QComboBox;
class QLabel;
class QScrollArea;
class Slider;
class ToolState;

// Layer stack: reorder by dragging, toggle visibility, pick blend mode and
// opacity for the selected layer, and reach the per-layer actions.
class LayerPanel : public QWidget {
    Q_OBJECT
public:
    LayerPanel(CanvasEngine* engine, ToolState* tools, QWidget* parent = nullptr);

    // Where a part of a layer's row is ("thumb", "mask", "eye", "chevron",
    // "row"), in the list widget's coordinates. For tests.
    QRectF rowPart(int layerId, const QString& part) const;

public slots:
    void refresh();

signals:
    void editTextRequested(int layerId);   // "Edit Text" on a layer that is still text

protected:
    void showEvent(QShowEvent*) override;

private:
    class List;
    friend class List;

    void showLayerMenu(int layerId, const QPoint& globalPos);
    void renameLayer(int layerId);

    CanvasEngine* m_engine;
    ToolState* m_tools;
    QComboBox* m_blend;
    Slider* m_opacity;
    QLabel* m_opacityValue;
    List* m_list;
    QScrollArea* m_scroll;
    bool m_updating = false;
};
