// One fragment for each block of a layer (uPage pixels square): is there
// anything in it? A block that turns out empty need not take up memory.
uniform sampler2D uSrc;
uniform sampler2D uHeld;   // one texel per block: not zero where the block has memory at all
uniform int uPage;

out vec4 fragColor;

void main()
{
    ivec2 block = ivec2(gl_FragCoord.xy);
    if (texelFetch(uHeld, block, 0).r == 0.0) {
        fragColor = vec4(0.0);
        return;
    }
    ivec2 from = block * uPage;
    ivec2 to = min(from + ivec2(uPage), textureSize(uSrc, 0));
    float found = 0.0;
    for (int y = from.y; y < to.y && found == 0.0; ++y) {
        for (int x = from.x; x < to.x; ++x) {
            if (texelFetch(uSrc, ivec2(x, y), 0) != vec4(0.0)) {
                found = 1.0;
                break;
            }
        }
    }
    fragColor = vec4(found);
}
