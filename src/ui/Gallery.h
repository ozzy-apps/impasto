#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class IconButton;
class QLabel;
class QScrollArea;

// The artworks, as a wall of thumbnails: everything in the gallery folder plus
// documents recently opened from elsewhere. Covers the whole window while it
// is up. It only looks at files; opening and saving stay with the main window.
class Gallery : public QWidget {
    Q_OBJECT
public:
    explicit Gallery(QWidget* parent = nullptr);

    // Where new artworks are kept.
    static QString folder();
    static void setFolder(const QString& dir);
    // A file name in the folder that is not taken yet ("Untitled Artwork 3").
    static QString freshPath(const QString& baseName = QString());
    // A document opened or saved outside the folder still gets a place on the wall.
    static void noteRecent(const QString& path);

    // Reads the folder again. `openPath` is the artwork on the canvas right
    // now, which gets a mark and cannot be thrown away.
    void refresh(const QString& openPath);

    QStringList paths() const;                      // as shown, newest first
    QRect cardRect(const QString& path) const;      // in this widget's coordinates; empty if it is not shown
    // Each returns the artwork's (new) path, or nothing and an error.
    QString renameArtwork(const QString& path, const QString& name, QString* error = nullptr);
    QString duplicateArtwork(const QString& path, QString* error = nullptr);
    bool trashArtwork(const QString& path, QString* error = nullptr);

signals:
    void openRequested(const QString& path);
    void newRequested();
    void importRequested();
    void backRequested();
    void artworkRenamed(const QString& from, const QString& to);
    void message(const QString& text);

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    class Grid;
    friend class Grid;

    void showArtworkMenu(const QString& path, const QPoint& globalPos);
    void showFolderMenu();

    Grid* m_grid;
    QScrollArea* m_scroll;
    QLabel* m_where;
    IconButton* m_back;
    IconButton* m_more;
    QString m_open;
};
