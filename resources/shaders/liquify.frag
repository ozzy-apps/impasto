// One touch of the liquify brush. The field holds, for every pixel, the offset
// to where it takes its colour from; pushing paint about is done by reading the
// field from where the paint came from and adding that step to it.
uniform sampler2D uField;
uniform vec2 uSize;
uniform vec2 uCentre;
uniform vec2 uMove;      // push: how far the brush travelled in this step
uniform float uRadius;
uniform float uAmount;
uniform int uMode;       // 0 push, 1 twirl right, 2 twirl left, 3 pinch, 4 expand, 5 restore

out vec4 fragColor;

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 here = texelFetch(uField, ivec2(p), 0).rg;
    vec2 d = p - uCentre;
    float r = length(d) / uRadius;
    if (r >= 1.0) {
        fragColor = vec4(here, 0.0, 1.0);
        return;
    }
    float w = (1.0 - r * r) * (1.0 - r * r);   // full in the middle, easing to nothing at the rim
    if (uMode == 5) {
        fragColor = vec4(here * (1.0 - clamp(uAmount * w, 0.0, 1.0)), 0.0, 1.0);
        return;
    }
    vec2 q = p;
    if (uMode == 0) {
        q = p - uMove * (w * uAmount);
    } else if (uMode == 1 || uMode == 2) {
        // To turn the paint one way, each pixel looks back the other way.
        float a = (uMode == 1 ? -1.0 : 1.0) * uAmount * w;
        float c = cos(a), s = sin(a);
        q = uCentre + vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    } else if (uMode == 3) {
        q = uCentre + d * (1.0 + uAmount * w);
    } else {
        q = uCentre + d * (1.0 - uAmount * w);
    }
    fragColor = vec4(texture(uField, q / uSize).rg + (q - p), 0.0, 1.0);
}
