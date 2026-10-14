// The blend modes, shared by the pass that lays one layer on the next and the
// pass that bakes a stroke into its layer. Colours here are not premultiplied.
float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

vec3 clipColor(vec3 c)
{
    float l = lum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) c = l + (c - l) * l / max(l - n, 1e-6);
    if (x > 1.0) c = l + (c - l) * (1.0 - l) / max(x - l, 1e-6);
    return c;
}

vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }
float sat(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }

vec3 setSat(vec3 c, float s)
{
    float mx = max(max(c.r, c.g), c.b);
    float mn = min(min(c.r, c.g), c.b);
    return mx > mn ? (c - mn) * s / (mx - mn) : vec3(0.0);
}

float burn(float b, float s) { return b >= 1.0 ? 1.0 : (s <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - b) / s)); }
float dodge(float b, float s) { return b <= 0.0 ? 0.0 : (s >= 1.0 ? 1.0 : min(1.0, b / (1.0 - s))); }
float hardLight(float b, float s) { return s <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s); }

float softLight(float b, float s)
{
    if (s <= 0.5) return b - (1.0 - 2.0 * s) * b * (1.0 - b);
    float d = b <= 0.25 ? ((16.0 * b - 12.0) * b + 4.0) * b : sqrt(b);
    return b + (2.0 * s - 1.0) * (d - b);
}

float vividLight(float b, float s) { return s <= 0.5 ? burn(b, 2.0 * s) : dodge(b, 2.0 * s - 1.0); }
float pinLight(float b, float s) { return s <= 0.5 ? min(b, 2.0 * s) : max(b, 2.0 * s - 1.0); }

vec3 blendMode(int mode, vec3 b, vec3 s)
{
    switch (mode) {
    case 1: return b * s;
    case 2: return min(b, s);
    case 3: return vec3(burn(b.r, s.r), burn(b.g, s.g), burn(b.b, s.b));
    case 4: return max(b + s - 1.0, 0.0);
    case 5: return lum(s) < lum(b) ? s : b;
    case 6: return b + s - b * s;
    case 7: return max(b, s);
    case 8: return vec3(dodge(b.r, s.r), dodge(b.g, s.g), dodge(b.b, s.b));
    case 9: return min(b + s, 1.0);
    case 10: return lum(s) > lum(b) ? s : b;
    case 11: return vec3(hardLight(s.r, b.r), hardLight(s.g, b.g), hardLight(s.b, b.b));
    case 12: return vec3(softLight(b.r, s.r), softLight(b.g, s.g), softLight(b.b, s.b));
    case 13: return vec3(hardLight(b.r, s.r), hardLight(b.g, s.g), hardLight(b.b, s.b));
    case 14: return vec3(vividLight(b.r, s.r), vividLight(b.g, s.g), vividLight(b.b, s.b));
    case 15: return clamp(b + 2.0 * s - 1.0, 0.0, 1.0);
    case 16: return vec3(pinLight(b.r, s.r), pinLight(b.g, s.g), pinLight(b.b, s.b));
    case 17: return step(1.0, vec3(vividLight(b.r, s.r), vividLight(b.g, s.g), vividLight(b.b, s.b)) + 0.5);
    case 18: return abs(b - s);
    case 19: return b + s - 2.0 * b * s;
    case 20: return max(b - s, 0.0);
    case 21: return vec3(s.r <= 0.0 ? 1.0 : min(b.r / s.r, 1.0),
                         s.g <= 0.0 ? 1.0 : min(b.g / s.g, 1.0),
                         s.b <= 0.0 ? 1.0 : min(b.b / s.b, 1.0));
    case 22: return setLum(setSat(s, sat(b)), lum(b));
    case 23: return setLum(setSat(b, sat(s)), lum(b));
    case 24: return setLum(s, lum(b));
    case 25: return setLum(b, lum(s));
    }
    return s;
}
