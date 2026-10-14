// One axis of a separable Gaussian. Bilinear taps between texel pairs halve
// the number of fetches.
uniform sampler2D uSrc;
uniform vec2 uDir;      // one texel along the blur axis, in uv units
uniform float uRadius;  // canvas pixels

out vec4 fragColor;

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uSrc, 0));
    float sigma = max(uRadius * 0.5, 0.001);
    float inv = -0.5 / (sigma * sigma);
    int n = int(ceil(uRadius));
    vec4 acc = texture(uSrc, uv);
    float total = 1.0;
    for (int i = 1; i <= n; i += 2) {
        float a = float(i), b = float(i + 1);
        float wa = exp(a * a * inv), wb = exp(b * b * inv);
        float w = wa + wb;
        float o = (a * wa + b * wb) / w;
        acc += (texture(uSrc, uv + uDir * o) + texture(uSrc, uv - uDir * o)) * w;
        total += 2.0 * w;
    }
    fragColor = acc / total;
}
