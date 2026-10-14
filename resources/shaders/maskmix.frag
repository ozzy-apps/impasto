// Keeps uA where the mask is 0 and takes uB where it is 1.
uniform sampler2D uA;
uniform sampler2D uB;
uniform sampler2D uMask;

out vec4 fragColor;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    fragColor = mix(texelFetch(uA, p, 0), texelFetch(uB, p, 0), texelFetch(uMask, p, 0).r);
}
