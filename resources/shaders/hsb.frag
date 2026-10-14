uniform sampler2D uSrc;
uniform float uHue;         // -0.5..0.5 turns
uniform float uSaturation;  // -1..1
uniform float uBrightness;  // -1..1

out vec4 fragColor;

vec3 rgb2hsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main()
{
    vec4 c = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0);
    if (c.a <= 0.0) {
        fragColor = c;
        return;
    }
    vec3 hsv = rgb2hsv(clamp(c.rgb / c.a, 0.0, 1.0));
    hsv.x = fract(hsv.x + uHue);
    hsv.y = clamp(hsv.y * (uSaturation >= 0.0 ? 1.0 + 2.0 * uSaturation : 1.0 + uSaturation), 0.0, 1.0);
    vec3 rgb = hsv2rgb(hsv);
    rgb = uBrightness >= 0.0 ? mix(rgb, vec3(1.0), uBrightness) : rgb * (1.0 + uBrightness);
    fragColor = vec4(rgb * c.a, c.a);
}
