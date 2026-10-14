// Brush tip, shared by the paint and smudge fragment shaders.
uniform int uTipMode;     // 0 = analytic round tip, 1 = tip texture
uniform float uHardness;
uniform sampler2D uTip;

in vec2 vUv;              // dab-local, the tip edge sits at length 1
in vec2 vLocalPx;
in float vCoverage;

// A dab is split into two parts. `profile` is how much paint it lays down
// across the tip and builds up from dab to dab. `edge` is the antialiased
// outline; a stroke keeps the maximum edge of all its dabs, so the outline of
// a hard brush stays one clean pixel wide however many dabs overlap.
void tipSample(out float profile, out float edge)
{
    if (uTipMode == 0) {
        float d = length(vUv);
        float aa = max(fwidth(d), 1e-4);
        float outer = 1.0 + 0.5 * aa;
        float t = clamp((outer - d) / max(1.0 - uHardness, 1e-3), 0.0, 1.0);
        profile = t * t * (3.0 - 2.0 * t);
        edge = clamp((outer - d) / aa, 0.0, 1.0);
    } else {
        profile = texture(uTip, vUv * 0.5 + 0.5).r;
        edge = 1.0;
    }
    profile *= vCoverage;
}
