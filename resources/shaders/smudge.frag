// Smear: paint the pixels found under the previous dab at the current dab.
// Dual-source blending gives dst = mix(dst, pickup, k) for premultiplied RGBA.
uniform sampler2D uPickup;
uniform sampler2D uSelection;
uniform sampler2D uLayer;   // the layer as it was before the stroke
uniform vec2 uDelta;        // current dab centre minus previous dab centre
uniform vec4 uSrcRect;      // region of uPickup that holds valid pixels
uniform vec2 uCanvasSize;
uniform float uStrength;
uniform float uCharge;      // 0 = only what was picked up (smudging); above, the brush's own paint is mixed in
uniform float uPull;        // how far behind the dab the paint it deposits is picked up, as a share of the step
uniform int uHasSelection;
uniform int uAlphaLock;

in vec4 vColor;

layout(location = 0, index = 0) out vec4 fragColor;
layout(location = 0, index = 1) out vec4 fragWeight;

void main()
{
    float profile, edge;
    tipSample(profile, edge);
    float k = clamp(profile * edge * vColor.a * uStrength, 0.0, 1.0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (uHasSelection != 0)
        k *= texelFetch(uSelection, p, 0).r;

    vec2 q = clamp(gl_FragCoord.xy - uDelta * uPull, uSrcRect.xy + 0.5, uSrcRect.zw - 0.5);
    vec4 c = texture(uPickup, q / uCanvasSize);
    // A loaded brush smears what is there and lays its own paint over it:
    // uCharge of it with each dab, and all of it where there was nothing to
    // pick up, so that on bare canvas it paints like any brush.
    vec4 paint = vec4(vColor.rgb, 1.0);
    float kp = uCharge > 0.0 ? k * (uCharge + (1.0 - uCharge) * (1.0 - c.a)) : 0.0;
    if (uAlphaLock != 0) {
        // Colour only. Alpha never changes under the lock, so this pixel's
        // alpha is still what the untouched layer has; re-premultiply the
        // picked-up colour to it. Transparent pickup carries no colour.
        float a = texelFetch(uLayer, p, 0).a;
        k *= min(1.0, c.a * 20.0);
        c = vec4(c.rgb / max(c.a, 1e-5) * a, a);
        paint = vec4(vColor.rgb * a, a);
    }
    // dst = mix(mix(dst, picked up, k), paint, kp), as one dual-source blend.
    fragColor = c * k * (1.0 - kp) + paint * kp;
    fragWeight = vec4(k + kp - k * kp);
}
