#pragma once

#include "engine/CanvasEngine.h"

#include <QPointF>
#include <QSizeF>
#include <QVector>

// The geometry behind perspective guides: which lines pass through a point,
// and how to keep a stroke on one of them.
namespace perspective {

// A path a stroke may be held to: a straight line through `origin`, or a
// circle (fisheye perspective bends its horizontals and verticals into arcs).
struct Constraint {
    bool valid = false;
    bool circle = false;
    QPointF origin;       // where the stroke started
    QPointF dir;          // unit direction of a line
    QPointF centre;       // of a circle
    double radius = 0.0;

    QPointF apply(const QPointF& p) const;       // the nearest point on the path
    QPointF tangentAt(const QPointF& p) const;   // unit vector along it there
};

// How many vanishing points (or, for fisheye, handles) a mode has.
int handleCount(Guides::Perspective mode);
// Where a mode's points start out on a canvas of this size.
void placeDefaults(Guides& guides, const QSizeF& canvas);

// Fisheye: the four points on the rim, in the order left, right, top, bottom.
void fisheyeRim(const Guides& guides, QPointF rim[4], double* radius);
// Isometric: the three directions lines run in.
void isometricDirections(const Guides& guides, QPointF dirs[3]);

// The circle through a and b that also passes through p (a line if they are in a row).
Constraint arc(const QPointF& a, const QPointF& b, const QPointF& p);

// Every guide line that passes through `start`.
QVector<Constraint> candidates(const Guides& guides, const QPointF& start);
// The one that runs closest to the way the pen set off. `margin` receives how
// many degrees closer it is than the runner-up: small when the pen has not
// yet shown which of two lines it means.
Constraint choose(const Guides& guides, const QPointF& start, const QPointF& towards, double* margin = nullptr);

} // namespace perspective
