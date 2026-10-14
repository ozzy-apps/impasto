uniform sampler2D uSrc;
uniform int uHorizontal;

out vec4 fragColor;

void main()
{
    ivec2 size = textureSize(uSrc, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (uHorizontal != 0)
        p.x = size.x - 1 - p.x;
    else
        p.y = size.y - 1 - p.y;
    fragColor = texelFetch(uSrc, p, 0);
}
