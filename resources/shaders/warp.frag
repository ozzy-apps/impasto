// The layer as carried by the warp mesh. With a selection only the selected
// paint travels; the rest was laid down beforehand and is drawn over.
uniform sampler2D uSrc;
uniform sampler2D uSelection;
uniform int uHasSelection;
uniform vec2 uCanvasSize;

in vec2 vSrc;
out vec4 fragColor;

void main()
{
    vec4 c = texture(uSrc, vSrc / uCanvasSize);
    if (uHasSelection != 0)
        c *= texture(uSelection, vSrc / uCanvasSize).r;
    fragColor = c;
}
