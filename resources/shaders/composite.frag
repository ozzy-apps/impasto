// Blends one layer onto the accumulated backdrop. All colours are premultiplied
// and gamma-encoded (sRGB), matching what Procreate and Photoshop do by default.
uniform sampler2D uBackdrop;
uniform sampler2D uSrc;
uniform sampler2D uClip;
uniform sampler2D uMask;       // the layer's own mask
uniform sampler2D uClipMask;   // the mask of the layer it is clipped to
uniform int uHasMask;
uniform int uHasClipMask;
uniform vec4 uBackdropColor;
uniform int uBackdropIsColor;
uniform int uHasClip;
uniform float uOpacity;
uniform int uMode;
// Where this fragment is in each texture. The canvas is put together a band
// at a time, in buffers the size of a band: those and the layers, which are
// the size of the canvas, do not share an origin.
uniform ivec2 uBackdropAt;
uniform ivec2 uSrcAt;
uniform ivec2 uClipAt;
uniform ivec2 uMaskAt;
uniform ivec2 uClipMaskAt;

out vec4 fragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 B = uBackdropIsColor != 0 ? uBackdropColor : texelFetch(uBackdrop, p + uBackdropAt, 0);
    vec4 S = texelFetch(uSrc, p + uSrcAt, 0) * uOpacity;
    if (uHasMask != 0)
        S *= texelFetch(uMask, p + uMaskAt, 0).r;
    if (uHasClip != 0) {
        // Clipped to what is visible of the base layer, so its mask counts too.
        float base = texelFetch(uClip, p + uClipAt, 0).a;
        if (uHasClipMask != 0)
            base *= texelFetch(uClipMask, p + uClipMaskAt, 0).r;
        S *= base;
    }

    if (uMode == 0 || S.a <= 0.0) {
        fragColor = S + B * (1.0 - S.a);
        return;
    }
    vec3 Cb = B.a > 0.0 ? clamp(B.rgb / B.a, 0.0, 1.0) : vec3(0.0);
    vec3 Cs = clamp(S.rgb / S.a, 0.0, 1.0);
    vec3 Bl = clamp(blendMode(uMode, Cb, Cs), 0.0, 1.0);
    vec3 co = S.a * (1.0 - B.a) * Cs + S.a * B.a * Bl + (1.0 - S.a) * B.a * Cb;
    fragColor = vec4(co, S.a + B.a * (1.0 - S.a));
}
