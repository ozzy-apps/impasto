// Colour adjustments and simple filters on one layer. Colour work is done on
// straight (un-premultiplied) values and put back under the pixel's own alpha.
uniform sampler2D uSrc;
uniform sampler2D uLut;    // curves: what each channel value becomes
uniform int uOp;
uniform vec4 uA;           // parameters; their meaning depends on uOp
uniform vec4 uB;

out vec4 fragColor;

const vec3 kLuma = vec3(0.299, 0.587, 0.114);

float hash(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec4 texel(ivec2 p)
{
    return texelFetch(uSrc, clamp(p, ivec2(0), textureSize(uSrc, 0) - 1), 0);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(uSrc, p, 0);

    if (uOp == 9) {
        // Sharpen: push each pixel away from the average of its neighbours.
        vec4 around = (texel(p + ivec2(1, 0)) + texel(p - ivec2(1, 0)) + texel(p + ivec2(0, 1)) + texel(p - ivec2(0, 1))) * 2.0
                      + texel(p + ivec2(1, 1)) + texel(p - ivec2(1, 1)) + texel(p + ivec2(1, -1)) + texel(p - ivec2(1, -1));
        vec4 r = c + (c - around / 12.0) * uA.x;
        r.a = clamp(r.a, 0.0, 1.0);
        fragColor = vec4(clamp(r.rgb, vec3(0.0), vec3(r.a)), r.a);
        return;
    }
    if (uOp == 11) {
        // Mosaic: every pixel of a square takes the colour at its middle.
        float cell = max(uA.x, 1.0);
        fragColor = texel(ivec2(floor(gl_FragCoord.xy / cell) * cell + cell * 0.5));
        return;
    }
    if (c.a <= 0.0) {
        fragColor = c;
        return;
    }

    vec3 rgb = c.rgb / c.a;
    if (uOp == 1) {
        // Levels: stretch between the black and white points, bend the middle, squeeze to the output range.
        vec3 x = clamp((rgb - uA.x) / max(uA.y - uA.x, 1e-4), 0.0, 1.0);
        x = pow(x, vec3(1.0 / max(uA.z, 0.01)));
        rgb = mix(vec3(uA.w), vec3(uB.x), x);
    } else if (uOp == 2) {
        vec3 at = (clamp(rgb, 0.0, 1.0) * 255.0 + 0.5) / 256.0;
        rgb = vec3(texture(uLut, vec2(at.r, 0.5)).r, texture(uLut, vec2(at.g, 0.5)).g, texture(uLut, vec2(at.b, 0.5)).b);
    } else if (uOp == 3) {
        // Brightness and contrast about middle grey.
        float k = uA.y > 0.0 ? 1.0 / max(1.0 - uA.y, 0.01) : 1.0 + uA.y;
        rgb = (rgb - 0.5) * k + 0.5 + uA.x;
    } else if (uOp == 4) {
        // Colour balance: strongest in the midtones, and without changing how light the pixel is.
        float before = dot(rgb, kLuma);
        float mid = 1.0 - pow(abs(2.0 * before - 1.0), 2.0);
        rgb = clamp(rgb + uA.rgb * 0.5 * mid, 0.0, 1.0);
        float after = dot(rgb, kLuma);
        rgb += before - after;
    } else if (uOp == 5) {
        // Black and white, optionally warmed towards sepia.
        float grey = dot(rgb, kLuma);
        vec3 toned = grey * mix(vec3(1.0), vec3(1.10, 1.0, 0.82), uA.y);
        rgb = mix(rgb, toned, uA.x);
    } else if (uOp == 6) {
        rgb = mix(rgb, 1.0 - rgb, uA.x);
    } else if (uOp == 7) {
        float steps = max(uA.x, 2.0);
        rgb = floor(clamp(rgb, 0.0, 1.0) * steps * 0.9999) / (steps - 1.0);
    } else if (uOp == 8) {
        rgb = vec3(step(uA.x, dot(rgb, kLuma)));
    } else if (uOp == 10) {
        // Noise: grain, either grey or separately per channel.
        vec2 at = gl_FragCoord.xy + uB.xy;
        vec3 n = vec3(hash(at), hash(at + 17.31), hash(at + 43.77)) - 0.5;
        rgb += mix(n, vec3(n.r), uA.y) * uA.x;
    }
    fragColor = vec4(clamp(rgb, 0.0, 1.0) * c.a, c.a);
}
