// One corner of the warp mesh: where it goes on the canvas, and which point
// of the layer it carries there.
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aSrc;

uniform vec2 uCanvasSize;

out vec2 vSrc;

void main()
{
    vSrc = aSrc;
    gl_Position = vec4(aPos / uCanvasSize * 2.0 - 1.0, 0.0, 1.0);
}
