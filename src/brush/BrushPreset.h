#pragma once

#include "core/Types.h"

#include <QHash>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <list>
#include <vector>

// Everything that defines how a brush lays down dabs. Sizes are in canvas pixels,
// "fraction" values are relative to the dab diameter.
struct BrushPreset {
    enum class AngleSource { Fixed, Direction, Azimuth };
    enum class Buildup { Accumulate, Max };
    // Multiply: the grain shows evenly. Height: paper tooth, filling in as the
    // stroke builds. Edge: tooth that only bites the soft rim, leaving the core solid.
    enum class GrainMode { Off = 0, Multiply = 1, Height = 2, Edge = 3 };

    QString id;
    QString name;
    QString category;
    bool custom = false;         // made by the artist: can be renamed, moved and deleted

    // Tip shape
    QString tip;                 // empty = analytic round tip, otherwise a texgen or imported ("user:…") tip
    float hardness = 1.0f;       // round tip only
    float roundness = 1.0f;      // 1 = circular, lower squashes the tip
    float angle = 0.0f;          // degrees
    AngleSource angleSource = AngleSource::Fixed;
    float angleJitter = 0.0f;    // 0..1 of a full turn

    // Stroke path
    float spacing = 0.08f;       // fraction of diameter between dabs
    float scatter = 0.0f;        // fraction of diameter
    int count = 1;               // dabs stamped per step
    // Smoothing. These stack: the pen pulls a string, the string's end is
    // averaged over a short time, and StreamLine makes the line chase that.
    float streamline = 0.15f;    // 0..1, the line lags and glides after the pen
    float stabilization = 0.0f;  // 0..1, running average; fast strokes are smoothed more
    float stringLength = 0.0f;   // 0..1 of kMaxStringPx: the line only moves once the pen pulls it taut
    float pressureSmoothing = 0.0f;   // 0..1, steadies line weight against a nervous hand

    // Tapers, in diameters of length. taperTip is how fine the point gets.
    float taper = 0.0f;          // lead-in at the start
    float taperEnd = 0.0f;       // run-out at the end (applied when the pen lifts)
    float taperTip = 0.12f;      // 0..1 of the full width

    // Size and opacity
    float size = 12.0f;
    float minSize = 1.0f;
    float maxSize = 300.0f;
    float opacity = 1.0f;        // default stroke opacity (the glaze ceiling)
    float flow = 1.0f;           // paint laid down per dab
    Buildup buildup = Buildup::Accumulate;
    float wetEdges = 0.0f;       // pigment pooling at the rim: thins the well-covered middle
    // How the paint meets what is already on the layer: any of the layer
    // blend modes. On bare canvas the paint goes down as it is.
    BlendMode blend = BlendMode::Normal;
    // Wet mixing. Above zero the brush is wet: it drags the paint under it
    // along (this far, as a share of each step) and mixes it into its own.
    float wetMix = 0.0f;
    float wetCharge = 0.6f;      // how much of what it lays down is its own paint; the rest is what it picked up

    // Dynamics
    float pressureSize = 0.0f;   // 0 = pressure ignored, 1 = size reaches zero at no pressure
    float pressureFlow = 0.0f;
    float pressureGamma = 1.0f;  // <1 = softer response, >1 = firmer
    // The brush's own pressure curve, from pen pressure to the pressure the
    // brush acts on. Empty means the plain power curve above is used.
    QVector<QPointF> pressureCurve;
    float sizeJitter = 0.0f;
    float flowJitter = 0.0f;
    float tiltSize = 0.0f;       // extra size at full tilt, as a multiple
    float tiltFlow = 0.0f;       // flow lost at full tilt
    float speedSize = 0.0f;      // size lost at high speed
    float speedFlow = 0.0f;      // paint lost at high speed: a fast stroke runs dry

    // Grain
    QString grain;               // empty = none, otherwise a texgen or imported ("user:…") grain
    GrainMode grainMode = GrainMode::Off;
    float grainScale = 1.0f;
    float grainDepth = 1.0f;
    bool grainMoving = false;

    // Colour dynamics
    float hueJitter = 0.0f;
    float satJitter = 0.0f;
    float valJitter = 0.0f;
};

constexpr float kMaxStringPx = 160.0f;   // screen pixels at stringLength = 1

// The built-in brushes, whatever the artist changed about them, and the
// brushes the artist made. Changes to a built-in brush are kept as overrides
// on disk, so it can always be reset to how it shipped; the artist's own
// brushes are stored whole.
class BrushLibrary {
public:
    static BrushLibrary& instance();

    // A list, so that adding and removing brushes leaves pointers to all the
    // others valid. Built-in brushes come first.
    const std::list<BrushPreset>& presets() const { return m_presets; }
    const QStringList& categories() const { return m_categories; }
    // The artist's own brushes first, newest on top, then the built-in ones.
    QVector<const BrushPreset*> inCategory(const QString& category) const;
    const BrushPreset* find(const QString& id) const;
    const BrushPreset& fallback() const { return m_presets.front(); }

    void update(const BrushPreset& preset);   // replaces the brush with the same id
    void reset(const QString& id);            // built-in brushes only
    bool isModified(const QString& id) const { return m_modified.contains(id); }
    int revision() const { return m_revision; }   // changes with every edit; preview caches key on it

    // The artist's own brushes. Each returns the id of the brush it made, or
    // nothing. Renaming, moving and removing only work on such brushes.
    QString createBrush(const QString& category);
    QString duplicate(const QString& id);
    bool rename(const QString& id, const QString& name);
    bool moveToCategory(const QString& id, const QString& category);
    // Puts a brush at another place within its set (0 = first). Any brush
    // may be moved, shipped ones too; the order is kept between sessions.
    bool moveInCategory(const QString& id, int toIndex);
    bool remove(const QString& id);
    // A brush as a file of its own, with any imported tip or grain inside it.
    bool exportBrush(const QString& id, const QString& path, QString* error) const;
    QString importBrush(const QString& path, QString* error);

    // Reads the artist's file over the shipped brushes. Everything handed out
    // before is invalid afterwards: call it before anything holds a brush.
    void load();
    void save() const;

    static const char* fileExtension() { return "impbrush"; }
    static const char* oldFileExtension() { return "sbkbrush"; }   // from when the app was Sketchbook: they import the same

private:
    BrushLibrary();
    void add(BrushPreset preset);
    BrushPreset* mutableFind(const QString& id);
    void rebuildCategories();
    QString uniqueName(const QString& wanted, const QString& exceptId = QString()) const;

    std::list<BrushPreset> m_presets;
    std::vector<BrushPreset> m_defaults;
    QSet<QString> m_modified;
    QHash<QString, QStringList> m_order;   // sets the artist has put in an order of their own: the ids, first to last
    QStringList m_categories;
    int m_revision = 0;
};
