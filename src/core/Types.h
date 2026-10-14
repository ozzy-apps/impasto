#pragma once

#include <QString>
#include <array>

// Order matters: the integer value is passed straight to composite.frag.
enum class BlendMode : int {
    Normal = 0,
    Multiply, Darken, ColorBurn, LinearBurn, DarkerColor,
    Screen, Lighten, ColorDodge, Add, LighterColor,
    Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight, HardMix,
    Difference, Exclusion, Subtract, Divide,
    Hue, Saturation, Color, Luminosity,
    Count
};

struct BlendModeInfo {
    BlendMode mode;
    const char* name;
    const char* code;   // short badge shown on the layer row
    const char* group;
};

inline const std::array<BlendModeInfo, int(BlendMode::Count)>& blendModes()
{
    static const std::array<BlendModeInfo, int(BlendMode::Count)> table = {{
        {BlendMode::Normal, "Normal", "N", ""},
        {BlendMode::Multiply, "Multiply", "M", "Darken"},
        {BlendMode::Darken, "Darken", "Da", "Darken"},
        {BlendMode::ColorBurn, "Color Burn", "Cb", "Darken"},
        {BlendMode::LinearBurn, "Linear Burn", "Lb", "Darken"},
        {BlendMode::DarkerColor, "Darker Color", "Dc", "Darken"},
        {BlendMode::Screen, "Screen", "S", "Lighten"},
        {BlendMode::Lighten, "Lighten", "Li", "Lighten"},
        {BlendMode::ColorDodge, "Color Dodge", "Cd", "Lighten"},
        {BlendMode::Add, "Add", "A", "Lighten"},
        {BlendMode::LighterColor, "Lighter Color", "Lc", "Lighten"},
        {BlendMode::Overlay, "Overlay", "O", "Contrast"},
        {BlendMode::SoftLight, "Soft Light", "Sl", "Contrast"},
        {BlendMode::HardLight, "Hard Light", "Hl", "Contrast"},
        {BlendMode::VividLight, "Vivid Light", "Vl", "Contrast"},
        {BlendMode::LinearLight, "Linear Light", "Ll", "Contrast"},
        {BlendMode::PinLight, "Pin Light", "Pl", "Contrast"},
        {BlendMode::HardMix, "Hard Mix", "Hm", "Contrast"},
        {BlendMode::Difference, "Difference", "D", "Difference"},
        {BlendMode::Exclusion, "Exclusion", "E", "Difference"},
        {BlendMode::Subtract, "Subtract", "Su", "Difference"},
        {BlendMode::Divide, "Divide", "Di", "Difference"},
        {BlendMode::Hue, "Hue", "H", "Color"},
        {BlendMode::Saturation, "Saturation", "Sa", "Color"},
        {BlendMode::Color, "Color", "C", "Color"},
        {BlendMode::Luminosity, "Luminosity", "Lu", "Color"},
    }};
    return table;
}

inline const BlendModeInfo& blendModeInfo(BlendMode m)
{
    int i = int(m);
    if (i < 0 || i >= int(BlendMode::Count))
        i = 0;
    return blendModes()[size_t(i)];
}

enum class StrokeTool { Brush = 0, Smudge = 1, Eraser = 2 };
