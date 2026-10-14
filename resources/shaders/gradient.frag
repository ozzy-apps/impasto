// A gradient over the whole canvas through any number of colours: along the
// line from uFrom to uTo; outwards from uFrom as far as uTo; round uFrom,
// starting in the direction of uTo; along the line both ways from uFrom; or
// out from uFrom in a square turned to the line. Colours are premultiplied,
// so fading to transparent does not darken on the way.
uniform vec2 uFrom;
uniform vec2 uTo;
uniform int uKind;   // 0 linear, 1 radial, 2 angle, 3 reflected, 4 diamond
uniform int uCount;  // stops in use, in rising order of position
uniform float uAt[12];
uniform vec4 uColor[12];

out vec4 fragColor;

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 d = uTo - uFrom;
    float len2 = max(dot(d, d), 1e-6);
    vec2 q = p - uFrom;
    float t;
    if (uKind == 0) {
        t = dot(q, d) / len2;
    } else if (uKind == 1) {
        t = length(q) / sqrt(len2);
    } else if (uKind == 2) {
        // Once round, anticlockwise as seen (as in Photoshop), from the direction dragged in.
        t = fract((atan(d.y, d.x) - atan(q.y, q.x)) / 6.28318530718);
    } else if (uKind == 3) {
        t = abs(dot(q, d)) / len2;
    } else {
        vec2 u = d * inversesqrt(len2);
        t = (abs(dot(q, u)) + abs(dot(q, vec2(-u.y, u.x)))) * inversesqrt(len2);
    }
    t = clamp(t, 0.0, 1.0);
    vec4 c = uColor[0];
    for (int i = 1; i < uCount; ++i) {
        float span = max(uAt[i] - uAt[i - 1], 1e-6);
        c = mix(c, uColor[i], clamp((t - uAt[i - 1]) / span, 0.0, 1.0));
    }
    fragColor = c;
}
