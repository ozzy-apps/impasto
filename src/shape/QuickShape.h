#pragma once

#include "brush/StrokeEngine.h"

#include <QPointF>
#include <QString>
#include <vector>

// A clean geometric shape recognised from a hand-drawn stroke. Everything is in
// canvas pixels. The shape is only geometry: it gets its look by being stroked
// with whatever brush drew the original line.
struct QuickShape {
    enum class Kind { None, Line, Arc, Ellipse, Rectangle, Polygon, Polyline, Curve, Path };

    Kind kind = Kind::None;
    // Line: both ends. Arc: start, a point on the curve, end.
    // Polygon / Polyline: the vertices. Curve: points the smooth line passes through.
    std::vector<QPointF> pts;
    // Ellipse and Rectangle: centre, half extents and rotation in degrees.
    QPointF center;
    double rx = 0.0, ry = 0.0;
    double rot = 0.0;
    // Path: points joined in order, each one a sharp corner or a smooth point
    // with a handle either side of it, the way a pen tool's are. Any other
    // kind can be turned into one, and is as soon as a point is added to it,
    // taken out of it or changed between the two.
    struct Node {
        QPointF pos;
        bool smooth = false;
        // A smooth point's handles follow from its neighbours, so that the
        // line passes through it the way a Curve's does and bends along when
        // a neighbour is moved. What is kept here is how each handle differs
        // from that: a turn and a stretch, as a complex factor (1 is no
        // difference). It is what a handle moved by hand leaves behind, and
        // what lets a point be added without the line moving.
        QPointF bendIn{1.0, 0.0}, bendOut{1.0, 0.0};
    };
    std::vector<Node> nodes;
    bool closedPath = false;

    // Turns a stroke into the shape it most resembles; kind is None when the
    // stroke is too short to tell.
    // `pixel` is the size of a screen pixel in the stroke's units (1 at 100%
    // zoom): how much of the line's wavering is the hand rather than the drawing.
    static QuickShape recognize(const std::vector<QPointF>& stroke, double pixel = 1.0);
    // The stroke without its rest: a pen that waits for the shape to snap
    // never stays quite still, and the points it wanders through while it
    // waits are no part of what was drawn. `radius` is how far it may stray
    // and still count as resting, the same measure that starts the wait.
    static std::vector<QPointF> withoutRest(const std::vector<QPointF>& stroke, double radius);

    bool valid() const { return kind != Kind::None; }
    bool closed() const { return kind == Kind::Ellipse || kind == Kind::Rectangle || kind == Kind::Polygon || (kind == Kind::Path && closedPath); }
    bool perfect() const;      // a circle or a square
    QString name() const;      // "Line", "Circle", "Square", "Triangle", ...
    // A smooth line through `anchors`, sampled about every `step` pixels.
    static std::vector<QPointF> smoothThrough(const std::vector<QPointF>& anchors, double step);

    // Outline as a dense point list for the brush engine; `step` is the spacing
    // in pixels. Vertices are flagged so they stay sharp.
    std::vector<PathPoint> path(double step) const;
    // Points the artist can drag in edit mode (see CanvasWidget for the meaning
    // of each index per kind).
    std::vector<QPointF> handles() const;
    std::vector<QPointF> corners() const;   // Rectangle only
    QPointF centroid() const;

    void translate(const QPointF& delta);
    void transformSimilar(const QPointF& pivot, double scale, double degrees);
    void makePerfect();                     // ellipse -> circle, rectangle -> square
    QuickShape asRectangle() const;         // best-fit rectangle of a four-sided polygon
    QuickShape asPolygon() const;           // rectangle -> free quadrilateral

    // ---- Paths
    // The same outline as a Path. Its points are the handles the shape had,
    // in the same order (an arc gets more where it needs them to stay round).
    QuickShape toPath() const;
    size_t segmentCount() const;            // runs between neighbouring points
    // Where a point's handles are, in canvas pixels: on the point itself when
    // it is a corner, or an end with nothing on that side.
    QPointF handleIn(size_t node) const;
    QPointF handleOut(size_t node) const;
    // The place on the outline nearest to `p`.
    struct Hit {
        int segment = -1;
        double t = 0.0;                     // 0..1 along that segment
        QPointF pos;
        double distance = 0.0;
    };
    Hit nearest(const QPointF& p) const;
    // Puts a point on a segment and returns its index. Not a pixel of the
    // outline moves.
    int insertNode(int segment, double t);
    // Takes a point out; only the run it was on changes, and a point that was
    // just put in leaves the line as it was before. False when the shape
    // needs every point it has.
    bool removeNode(size_t node);
    void setSmooth(size_t node, bool smooth);
    // Puts one handle of a smooth point at `to`; the other stays in line with
    // it at its own length.
    void setHandle(size_t node, bool out, const QPointF& to);
    // Joins the two ends of an open shape with one more run (ends that all
    // but meet become one point), or cuts a closed one open at a point.
    // Either way the shape becomes a Path. Opening moves not a pixel of the
    // outline: the two new ends lie together at that point, to be pulled
    // apart. False when there is nothing to join or cut.
    bool closePath();
    bool openPath(size_t node);

    // Moves `end` so the line from `start` lands on a round angle. Snaps to the
    // nearest 15 degrees when `force` is set, otherwise only when already within
    // a couple of degrees of a multiple of 45. Returns the angle it snapped to
    // (0..180, measured anticlockwise on screen) or -1.
    static double snapLineEnd(const QPointF& start, QPointF& end, bool force);
};
