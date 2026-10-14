#pragma once

#include <QHash>
#include <QPixmap>
#include <QWidget>

class CanvasEngine;
class IconButton;
class ToolState;
class QLabel;
class QScrollArea;

// Brush library: sets on the left, brushes with live stroke previews on the right.
class BrushPanel : public QWidget {
    Q_OBJECT
public:
    BrushPanel(CanvasEngine* engine, ToolState* tools, QWidget* parent = nullptr);

    // The artist's own brushes: each acts on the current brush and keeps the
    // panel in step with the library afterwards.
    void newBrush();
    void duplicateBrush();
    void deleteBrush();
    bool importBrushFile(const QString& path);

public slots:
    void sync();   // follow the tool state (current tool and its brush)

signals:
    void brushChosen();
    void labRequested();   // the sliders button: open the brush lab on the current brush

protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    class CategoryList;
    class BrushList;

    void reloadLibrary();
    void showBrushMenu(const QPoint& globalPos);

    CanvasEngine* m_engine;
    ToolState* m_tools;
    QLabel* m_title;
    IconButton* m_newBtn = nullptr;
    IconButton* m_menuBtn = nullptr;
    CategoryList* m_categories;
    BrushList* m_brushes;
    QScrollArea* m_scroll;
    QWidget* m_libraryPage = nullptr;
    IconButton* m_editBtn = nullptr;
    bool m_syncing = false;
};
