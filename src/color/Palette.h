#pragma once

#include <QColor>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

// A named set of colours laid out ten to a row. A slot may be empty (an
// invalid colour), so colours can be kept in rows and groups of their own.
struct Palette {
    static constexpr int kColumns = 10;
    static constexpr int kMinRows = 3;
    static constexpr int kMaxSlots = 600;

    QString id;
    QString name;
    QVector<QColor> colors;   // always whole rows, with at least one slot free

    int rows() const { return int(colors.size()) / kColumns; }
    int filled() const;
    QVector<QColor> filledColors() const;
};

// The artist's palettes. One of them is the default: the one kept under the
// pickers, where a tap on an empty slot stores the current colour. Nothing
// here is fixed; the palettes a first start comes with can be changed and
// thrown away like any other.
class PaletteLibrary : public QObject {
    Q_OBJECT
public:
    static PaletteLibrary& instance();

    const QVector<Palette>& palettes() const { return m_palettes; }
    const Palette* find(const QString& id) const;
    int indexOf(const QString& id) const;
    const Palette& current() const;
    QString currentId() const { return m_current; }
    void setCurrent(const QString& id);

    // Each returns the id of the palette it made, or nothing. New palettes go
    // to the top of the list.
    QString create(const QString& name = QString(), const QVector<QColor>& colors = {});
    QString duplicate(const QString& id);
    bool rename(const QString& id, const QString& name);
    bool remove(const QString& id);   // there is always one left: removing the last leaves an empty one
    void addStarters();               // the palettes of a first start, again

    // Swatches. An invalid colour empties the slot.
    bool setSwatch(const QString& id, int slot, const QColor& color);
    // Puts the swatch in another slot; if that one is taken, the two change places.
    bool moveSwatch(const QString& id, int from, int to);
    // Puts a copy of a colour into a palette at `slot`, or, when that slot is
    // taken, into the first free one after it (the palette grows a row if it
    // has to). Returns the slot it went into, or -1.
    int placeSwatch(const QString& id, int slot, const QColor& color);

    // GIMP palettes (.gpl), lists of hex codes (.hex, paint.net .txt), Adobe
    // swatch exchange files (.ase) and Procreate's .swatches are read; .gpl
    // and .swatches are written.
    QString importFile(const QString& path, QString* error);
    bool exportGpl(const QString& id, const QString& path, QString* error) const;
    bool exportSwatches(const QString& id, const QString& path, QString* error) const;
    // Procreate's palette file: a zip holding Swatches.json, colours by hue,
    // saturation and brightness, with null for an empty slot.
    static QVector<QColor> readSwatches(const QString& path, QString* name);
    static QVector<QColor> readColors(const QByteArray& data, QString* name);

    // The colours a picture is made of, most alike merged, greys first and
    // then around the hue circle. Transparent pixels are left out.
    static QVector<QColor> fromImage(const QImage& image, int count = 30);

    // Reads the artist's file in place of what is here. Call before anything shows a palette.
    void load();
    void save() const;
    void resetToStarters();

signals:
    void changed();          // colours, names, the list or the default

private:
    PaletteLibrary();
    Palette* mutableFind(const QString& id);
    QString newId();
    QString uniqueName(const QString& wanted, const QString& exceptId = QString()) const;

    QVector<Palette> m_palettes;
    QString m_current;
    int m_nextId = 1;
};
