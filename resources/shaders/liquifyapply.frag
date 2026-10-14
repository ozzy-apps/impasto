// Shows a layer through the liquify field: each pixel is fetched from where
// the field says it now comes from. With a selection, only what is selected
// is affected.
uniform sampler2D uSrc;
uniform sampler2D uField;
uniform sampler2D uSelection;
uniform int uHasSelection;
uniform vec2 uSize;

out vec4 fragColor;

void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec2 q = gl_FragCoord.xy + texelFetch(uField, ip, 0).rg;
    vec2 inside = clamp(min(q, uSize - q) + 0.5, 0.0, 1.0);
    vec4 c = texture(uSrc, q / uSize) * (inside.x * inside.y);
    if (uHasSelection != 0)
        c = mix(texelFetch(uSrc, ip, 0), c, texelFetch(uSelection, ip, 0).r);
    fragColor = c;
}
