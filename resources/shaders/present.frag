// Draws the composited canvas into the viewport with the current pan/zoom/rotation.
uniform sampler2D uCanvas;
uniform mat3 uViewToCanvas;  // viewport pixels (y down) -> canvas pixels
uniform vec2 uCanvasSize;
uniform vec2 uViewport;
uniform float uZoom;         // viewport pixels per canvas pixel
uniform vec3 uWorkspace;
uniform int uChecker;
uniform sampler2D uSelection;   // how much of each canvas pixel is selected
uniform int uHasSelection;
uniform float uAntsPhase;       // how far the dashes have crawled, in pixels
uniform float uSelDim;          // strength of the hatching over what is not selected

out vec4 fragColor;

float hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float selected(vec2 vp)
{
    vec2 c = (uViewToCanvas * vec3(vp, 1.0)).xy;
    if (c.x < 0.0 || c.y < 0.0 || c.x > uCanvasSize.x || c.y > uCanvasSize.y)
        return 0.0;
    return textureLod(uSelection, c / uCanvasSize, 0.0).r;
}

void main()
{
    vec2 vp = vec2(gl_FragCoord.x, uViewport.y - gl_FragCoord.y);
    vec2 cp = (uViewToCanvas * vec3(vp, 1.0)).xy;
    vec2 outside = max(max(-cp, cp - uCanvasSize), 0.0);
    float dist = length(outside) * uZoom;

    vec3 col;
    if (dist <= 0.0) {
        vec2 t = cp;
        if (uZoom > 2.5) {
            // Smooth interpolation at moderate zoom; past that, ease into crisp
            // texels with antialiased edges so individual pixels can be judged.
            vec2 seam = floor(t + 0.5);
            vec2 crisp = seam + clamp((t - seam) / fwidth(t), -0.5, 0.5);
            t = mix(t, crisp, smoothstep(2.5, 6.0, uZoom));
        }
        // Mipmaps are only refreshed while zoomed out, so pin level 0 otherwise.
        vec4 c = uZoom < 1.0 ? texture(uCanvas, t / uCanvasSize) : textureLod(uCanvas, t / uCanvasSize, 0.0);
        vec3 under = uWorkspace;
        if (uChecker != 0) {
            vec2 cell = floor(vp / 12.0);
            under = mod(cell.x + cell.y, 2.0) < 1.0 ? vec3(0.80) : vec3(0.64);
        }
        col = c.rgb + under * (1.0 - c.a);
    } else {
        float shadow = 0.45 * exp(-dist / 14.0);
        col = uWorkspace * (1.0 - shadow);
    }
    if (uHasSelection != 0) {
        float s = selected(vp);
        if (dist <= 0.0 && uSelDim > 0.0) {
            // Thin diagonal lines over what is left out, fading across soft edges.
            float stripe = 1.0 - smoothstep(0.5, 1.3, abs(mod(vp.x - vp.y, 12.0) - 6.0));
            vec3 ink = vec3(dot(col, vec3(0.299, 0.587, 0.114)) > 0.5 ? 0.0 : 1.0);
            col = mix(col, ink, stripe * uSelDim * (1.0 - s));
        }
        // Marching ants along the contour where the selection is at one half.
        // The slope comes from neighbouring screen pixels, so the line keeps
        // its width at any zoom and on edges of any softness.
        vec2 grad = 0.5 * vec2(selected(vp + vec2(1.0, 0.0)) - selected(vp - vec2(1.0, 0.0)),
                               selected(vp + vec2(0.0, 1.0)) - selected(vp - vec2(0.0, 1.0)));
        float slope = length(grad);
        if (slope > 1e-4) {
            float line = clamp(1.75 - abs(s - 0.5) / slope, 0.0, 1.0);
            // Dashes are measured along whichever of eight fixed directions
            // runs closest to the edge, so they keep their length at every
            // angle and travel round the selection in one direction.
            vec2 t = vec2(-grad.y, grad.x) / slope;
            vec2 way = sign(t);
            float along = max(abs(t.x), abs(t.y)) > 0.9239
                              ? (abs(t.x) > abs(t.y) ? vp.x * way.x : vp.y * way.y)
                              : (vp.x * way.x + vp.y * way.y) * 0.7071;
            float dash = step(4.0, mod(along - uAntsPhase, 8.0));
            col = mix(col, vec3(dash), line);
        }
    }
    // The canvas is 16-bit float; dither on the way down to the 8-bit framebuffer.
    col += (hash(gl_FragCoord.xy) - 0.5) / 255.0;
    fragColor = vec4(col, 1.0);
}
