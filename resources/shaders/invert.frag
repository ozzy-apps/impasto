// Inverts a mask: what was hidden shows, what showed is hidden.
uniform sampler2D uSrc;

out vec4 fragColor;

void main()
{
    vec4 c = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0);
    fragColor = vec4(1.0 - c.rgb, 1.0);
}
