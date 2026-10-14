// Resamples a layer through an affine/perspective map (destination -> source).
// With a selection only the selected pixels travel; the rest stays put.
uniform sampler2D uSrc;
uniform sampler2D uSelection;
uniform mat3 uInverse;
uniform vec2 uCanvasSize;
uniform int uSmooth;
uniform int uHasSelection;
uniform int uFillWhite;   // masks: whatever moves in from outside the canvas is revealed, not hidden

out vec4 fragColor;

void main()
{
    vec3 h = uInverse * vec3(gl_FragCoord.xy, 1.0);
    vec2 sp = h.xy / h.z;
    // Fade out over the last half pixel so the layer edge stays antialiased.
    vec2 edge = clamp(min(sp, uCanvasSize - sp) + 0.5, 0.0, 1.0);
    vec4 c = uSmooth != 0 ? texture(uSrc, sp / uCanvasSize)
                          : texelFetch(uSrc, clamp(ivec2(floor(sp)), ivec2(0), ivec2(uCanvasSize) - 1), 0);
    // A perspective map folds back on itself beyond its horizon: nothing belongs there.
    c *= edge.x * edge.y * step(1e-6, h.z);
    vec4 result = c;
    if (uHasSelection != 0) {
        ivec2 p = ivec2(gl_FragCoord.xy);
        vec4 moved = c * texture(uSelection, sp / uCanvasSize).r;
        vec4 left = texelFetch(uSrc, p, 0) * (1.0 - texelFetch(uSelection, p, 0).r);
        result = moved + left * (1.0 - moved.a);
    }
    if (uFillWhite != 0)
        result += (1.0 - result.a) * vec4(1.0);
    fragColor = result;
}
