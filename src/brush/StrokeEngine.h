#pragma once

#include "brush/BrushPreset.h"

#include <QColor>
#include <QPointF>
#include <deque>
#include <vector>

struct StrokeSample {
    QPointF pos;            // canvas pixels
    float pressure = 1.0f;  // 0..1
    float tiltX = 0.0f;     // degrees from vertical, about -60..60
    float tiltY = 0.0f;
    double time = 0.0;      // seconds
};

// A point of a ready-made path (QuickShape outlines). Corners stay sharp
// instead of being rounded by the spline. Each point carries the pen state the
// brush should see there, so a computed outline can keep a hand-drawn stroke's
// swelling and thinning.
struct PathPoint {
    QPointF pos;
    bool corner = false;
    float pressure = 1.0f;
    float tiltX = 0.0f, tiltY = 0.0f;
    float speed = 0.0f;   // canvas px per second
};

// GPU instance layout, see dab.vert.
struct Dab {
    float x, y, size, angle;
    float r, g, b, flow;
    float roundness, seedX, seedY, pad;
};
static_assert(sizeof(Dab) == 48, "Dab must match the instance attribute layout");

// Turns raw pointer samples into dabs: stabilises the path, fits a spline
// through it and stamps at even arc-length intervals with the preset's dynamics.
class StrokeEngine {
public:
    void begin(const BrushPreset& preset, float size, const QColor& color, const StrokeSample& s);
    void add(const StrokeSample& s);
    // Finishes the stroke. Returns true when every dab was generated afresh
    // (an end taper can only be applied once the full length is known); the
    // caller must then discard what it has already drawn of this stroke.
    bool end();

    // Appends a point exactly where given, bypassing stabilisation. Used to run
    // the brush along a computed outline.
    void addDirect(const PathPoint& point);

    // Overrides the preset's spacing (used by the smudge tool, which needs tight steps).
    void setSpacingOverride(float fraction) { m_spacingOverride = fraction; }
    // A fixed seed makes scatter and jitter repeat exactly when the same stroke
    // is rendered again; 0 picks a fresh one per stroke.
    void setSeed(uint32_t seed) { m_seed = seed; }
    // The lead-in taper suits open strokes but would notch a closed outline.
    void setTaperEnabled(bool on) { m_taper = on; }
    // Canvas pixels per screen pixel. The pull-string is a distance on screen,
    // so it has to feel the same at any zoom.
    void setInputScale(float canvasPerScreenPx) { m_inputScale = canvasPerScreenPx > 0.0f ? canvasPerScreenPx : 1.0f; }
    // Smoothing added to every brush on top of its own (a global preference).
    void setExtraStabilization(float amount) { m_extraStabilization = amount; }
    // Where the line currently ends, and whether a string holds it back.
    QPointF tip() const { return m_nodes.empty() ? QPointF() : m_nodes.back().pos; }
    bool stringActive() const { return m_stringLength > 0.0; }

    std::vector<Dab>& dabs() { return m_dabs; }

private:
    struct Node {
        QPointF pos;
        float pressure = 1.0f;
        float tilt = 0.0f;     // 0 = upright, 1 = lying flat
        float azimuth = 0.0f;  // radians
        float speed = 0.0f;    // canvas px per second
        bool corner = false;   // the path turns sharply here
    };

    Node makeNode(const QPointF& pos, float pressure) const;
    void pushNode(const Node& n);
    void renderSegment(size_t i);
    void regenerate();
    void emitDab(const Node& n, float direction, double travel);
    float nextSpacing() const;
    float rand01();
    float randSigned() { return rand01() * 2.0f - 1.0f; }

    BrushPreset m_preset;
    float m_pressureLut[257] = {};   // the brush's pressure response, sampled once per stroke
    float m_size = 10.0f;
    float m_spacingOverride = 0.0f;
    uint32_t m_seed = 0;
    bool m_taper = true;
    float m_inputScale = 1.0f;
    float m_extraStabilization = 0.0f;

    struct Timed {
        double time;
        QPointF pos;
    };
    std::deque<Timed> m_recent;      // input of the running average
    double m_window = 0.0;           // its length in seconds
    double m_stringLength = 0.0;     // canvas px, 0 = no string
    QPointF m_stringPos;             // the end of the string that draws
    bool m_direct = false;           // fed by addDirect: no smoothing, no catch-up
    bool m_endTaper = false;         // regenerating with the run-out applied
    double m_totalLength = 0.0;
    uint32_t m_rngStart = 1;
    float m_rgb[3] = {0, 0, 0};
    float m_hsv[3] = {0, 0, 0};

    std::vector<Node> m_nodes;
    size_t m_rendered = 0;       // segments [i, i+1] with i < m_rendered are done
    std::vector<Dab> m_dabs;
    size_t m_emitted = 0;

    StrokeSample m_lastRaw;
    QPointF m_smoothPos;
    float m_smoothPressure = 1.0f;
    float m_speed = 0.0f;
    double m_lastTime = 0.0;

    double m_distToNext = 0.0;
    double m_travel = 0.0;
    float m_lastDabSize = 1.0f;
    QPointF m_dir;
    bool m_haveDir = false;
    uint32_t m_rng = 1;
};
