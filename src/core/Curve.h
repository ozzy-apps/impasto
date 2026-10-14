#pragma once

#include <QPointF>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <vector>

// A response curve from 0..1 to 0..1 through a handful of points, as the pen
// pressure curves are. Monotone cubic interpolation (Fritsch-Carlson): it
// passes through every point and never overshoots between them.
inline float curveValue(const QVector<QPointF>& p, float x)
{
    const int n = int(p.size());
    if (n < 2)
        return x;
    if (x <= p[0].x())
        return float(p[0].y());
    if (x >= p[n - 1].x())
        return float(p[n - 1].y());
    std::vector<double> d(size_t(n - 1)), m(static_cast<size_t>(n));
    for (int i = 0; i + 1 < n; ++i)
        d[size_t(i)] = (p[i + 1].y() - p[i].y()) / std::max(p[i + 1].x() - p[i].x(), 1e-6);
    m[0] = d[0];
    m[size_t(n - 1)] = d[size_t(n - 2)];
    for (int i = 1; i + 1 < n; ++i)
        m[size_t(i)] = d[size_t(i - 1)] * d[size_t(i)] <= 0.0 ? 0.0 : (d[size_t(i - 1)] + d[size_t(i)]) * 0.5;
    for (int i = 0; i + 1 < n; ++i) {
        if (std::abs(d[size_t(i)]) < 1e-9) {
            m[size_t(i)] = m[size_t(i + 1)] = 0.0;
            continue;
        }
        const double a = m[size_t(i)] / d[size_t(i)], b = m[size_t(i + 1)] / d[size_t(i)];
        const double len = a * a + b * b;
        if (len > 9.0) {
            const double t = 3.0 / std::sqrt(len);
            m[size_t(i)] = t * a * d[size_t(i)];
            m[size_t(i + 1)] = t * b * d[size_t(i)];
        }
    }
    int i = 0;
    while (i + 2 < n && x > p[i + 1].x())
        ++i;
    const double h = std::max(p[i + 1].x() - p[i].x(), 1e-6);
    const double t = (x - p[i].x()) / h;
    const double t2 = t * t, t3 = t2 * t;
    const double y = (2 * t3 - 3 * t2 + 1) * p[i].y() + (t3 - 2 * t2 + t) * h * m[size_t(i)]
                     + (-2 * t3 + 3 * t2) * p[i + 1].y() + (t3 - t2) * h * m[size_t(i + 1)];
    return float(std::clamp(y, 0.0, 1.0));
}
