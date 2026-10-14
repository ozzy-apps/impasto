// How far every pixel is from the colour at the seed point, as the largest
// difference over the four channels: red for the layer being filled, green
// for the reference layer. The flood itself then only compares bytes.
uniform sampler2D uOwn;
uniform sampler2D uRef;
uniform vec2 uSeed;   // the seed pixel
uniform int uHasOwn;
uniform int uHasRef;

out vec4 fragColor;

float apart(sampler2D tex, ivec2 p)
{
    vec4 d = abs(texelFetch(tex, p, 0) - texelFetch(tex, ivec2(uSeed), 0));
    return max(max(d.r, d.g), max(d.b, d.a));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    fragColor = vec4(uHasOwn != 0 ? apart(uOwn, p) : 0.0, uHasRef != 0 ? apart(uRef, p) : 0.0, 0.0, 1.0);
}
