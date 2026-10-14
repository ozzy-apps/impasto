// Box-filtered downscale of a source rectangle, used for thumbnails and previews.
uniform sampler2D uSrc;
uniform vec4 uSrcRect;    // x, y, w, h in source texels
uniform vec2 uTargetSize;

out vec4 fragColor;

void main()
{
    const int N = 6;
    vec2 texSize = vec2(textureSize(uSrc, 0));
    vec4 acc = vec4(0.0);
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            vec2 o = (vec2(float(i), float(j)) + 0.5) / float(N) - 0.5;
            vec2 uv = (gl_FragCoord.xy + o) / uTargetSize;
            acc += texture(uSrc, (uSrcRect.xy + uv * uSrcRect.zw) / texSize);
        }
    }
    fragColor = acc / float(N * N);
}
