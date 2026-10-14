#pragma once

#include <QColor>
#include <QVector>
#include <cmath>

// Colours that go with a colour: the same saturation and brightness at other
// places around the hue circle.
enum class Harmony : int { Complementary = 0, SplitComplementary, Analogous, Triadic, Tetradic, Count };

inline const char* harmonyName(Harmony mode)
{
    switch (mode) {
    case Harmony::Complementary: return "Complementary";
    case Harmony::SplitComplementary: return "Split Complementary";
    case Harmony::Analogous: return "Analogous";
    case Harmony::Triadic: return "Triadic";
    case Harmony::Tetradic: return "Tetradic";
    default: return "";
    }
}

// How far round the circle each companion sits, in turns.
inline QVector<float> harmonyOffsets(Harmony mode)
{
    switch (mode) {
    case Harmony::Complementary: return {0.5f};
    case Harmony::SplitComplementary: return {150.0f / 360.0f, 210.0f / 360.0f};
    case Harmony::Analogous: return {30.0f / 360.0f, 330.0f / 360.0f};
    case Harmony::Triadic: return {120.0f / 360.0f, 240.0f / 360.0f};
    case Harmony::Tetradic: return {0.25f, 0.5f, 0.75f};
    default: return {};
    }
}

inline float wrapHue(float h)
{
    h -= std::floor(h);
    return h >= 1.0f ? 0.0f : h;
}

// The companions of a colour, without the colour itself.
inline QVector<QColor> harmonyColors(Harmony mode, float h, float s, float v)
{
    QVector<QColor> out;
    for (float offset : harmonyOffsets(mode))
        out.push_back(QColor::fromHsvF(wrapHue(h + offset), s, v));
    return out;
}
