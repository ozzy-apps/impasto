#pragma once

#include <QDialog>
#include <QVector>

class QLineEdit;
class QScrollArea;

// The keyboard shortcuts, in sections that fold shut, laid out in columns.
// It is a window of its own and not modal: it can be put anywhere, a second
// screen included, and left open while painting.
class ShortcutSheet : public QDialog {
    Q_OBJECT
public:
    struct Row {
        QString what, keys;
    };
    struct Section {
        QString title;
        QVector<Row> rows;
    };
    static QVector<Section> sections();

    explicit ShortcutSheet(QWidget* parent = nullptr);

    int sectionCount() const { return int(m_boxes.size()); }
    bool isFolded(int section) const;
    void setFolded(int section, bool folded);
    QWidget* sectionHeader(int section) const;   // the row to press to fold or unfold it
    int visibleRows() const;                     // across every unfolded section
    void setFilter(const QString& text);

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    class Box;
    void arrange();

    QLineEdit* m_find = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_content = nullptr;
    QVector<Box*> m_boxes;
    int m_columns = 0;
};
