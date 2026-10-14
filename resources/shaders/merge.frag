// Bakes the in-progress stroke buffer onto a copy of the layer. The stroke is
// kept apart from the layer until the pen lifts, which is what makes per-stroke
// opacity behave as a glaze instead of building up on self-overlap.
uniform sampler2D uLayer;
uniform sampler2D uStroke;
uniform sampler2D uCoverage;
uniform sampler2D uSelection;
uniform sampler2D uGrain;
uniform float uOpacity;
uniform int uErase;
uniform int uAlphaLock;
uniform int uHasSelection;
uniform int uHasLayer;
uniform int uGrainMode;    // 0 = off, 1 = multiply, 2 = height (paper tooth)
uniform float uGrainScale;
uniform float uGrainDepth;
uniform float uWetEdges;
uniform int uBlend;        // how the paint meets what the layer already holds: a layer blend mode, 0 = plainly on top
// A fill needs no stroke buffer: one colour everywhere stands in for the
// paint, and full coverage for the outline. Either can be on by itself: a
// flood fill is one colour let through where its region is, a gradient is
// painted colours with nothing held back.
uniform int uFlatColorOn;
uniform vec4 uFlatColor;
uniform int uFullCoverage;

out vec4 fragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 L = uHasLayer != 0 ? texelFetch(uLayer, p, 0) : vec4(0.0);
    vec4 S = uFlatColorOn != 0 ? uFlatColor : texelFetch(uStroke, p, 0);
    if (uFullCoverage == 0)
        S *= texelFetch(uCoverage, p, 0).r;

    // Canvas-fixed grain is applied to the accumulated stroke rather than to
    // each dab: overlapping dabs would otherwise saturate into hard blotches.
    float cover = S.a;   // how much of the stroke reached this pixel, before texture
    if (uGrainMode != 0 && cover > 0.0) {
        float g = texture(uGrain, gl_FragCoord.xy / (512.0 * uGrainScale)).r;
        if (uGrainMode == 1) {
            S *= mix(1.0, g, uGrainDepth);
        } else if (uGrainMode == 2) {
            // Light coverage only catches the peaks of the tooth; as the stroke
            // builds, the threshold drops and the valleys fill in.
            float th = (1.0 - cover) * uGrainDepth;
            S *= smoothstep(th - 0.25, th + 0.25, g);
        } else {
            // Same idea, shifted so full coverage is always solid: the tooth
            // only shows where the stroke fades out, giving a ragged ink edge.
            // A narrow transition keeps that edge crisp rather than hazy.
            float th = (1.0 - cover * 1.4) * uGrainDepth;
            S *= smoothstep(th - 0.09, th + 0.09, g);
        }
    }
    // Wet edges: pigment gathers at the rim of a wash, so the well-covered
    // middle ends up paler than the edge around it.
    if (uWetEdges > 0.0)
        S *= 1.0 - uWetEdges * smoothstep(0.5, 0.97, cover);

    S *= uOpacity;
    if (uHasSelection != 0)
        S *= texelFetch(uSelection, p, 0).r;

    if (uErase != 0) {
        fragColor = uAlphaLock != 0 ? L : L * (1.0 - S.a);
    } else if (uBlend != 0 && S.a > 0.0 && L.a > 0.0) {
        // A brush with a blend mode of its own: where there is paint already,
        // the two are blended as two layers would be; on bare canvas it is
        // laid down as it is.
        vec3 Cb = clamp(L.rgb / L.a, 0.0, 1.0);
        vec3 Cs = clamp(S.rgb / S.a, 0.0, 1.0);
        vec3 Bl = clamp(blendMode(uBlend, Cb, Cs), 0.0, 1.0);
        if (uAlphaLock != 0) {
            fragColor = vec4((S.a * Bl + (1.0 - S.a) * Cb) * L.a, L.a);
        } else {
            vec3 co = S.a * (1.0 - L.a) * Cs + S.a * L.a * Bl + (1.0 - S.a) * L.a * Cb;
            fragColor = vec4(co, S.a + L.a * (1.0 - S.a));
        }
    } else if (uAlphaLock != 0) {
        fragColor = vec4(L.rgb * (1.0 - S.a) + S.rgb * L.a, L.a);
    } else {
        fragColor = S + L * (1.0 - S.a);
    }
}
