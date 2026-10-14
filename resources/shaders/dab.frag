uniform sampler2D uGrain;
uniform int uGrainMode;    // 0 = off, 1 = multiply, 2 = height (paper tooth)
uniform float uGrainScale;
uniform float uGrainDepth;

in vec4 vColor;
in vec2 vSeed;

layout(location = 0) out vec4 fragColor;   // paint, blended dab over dab
layout(location = 1) out vec4 fragEdge;    // outline coverage, blended with MAX

void main()
{
    float profile, edge;
    tipSample(profile, edge);
    float a = profile * vColor.a;
    if (uGrainMode != 0) {
        // Only grain that travels with the dab is handled here; canvas-fixed
        // grain is applied once per stroke in merge.frag.
        vec2 gp = vLocalPx + vSeed * 4096.0;
        float g = texture(uGrain, gp / (512.0 * uGrainScale)).r;
        if (uGrainMode == 1) {
            a *= mix(1.0, g, uGrainDepth);
        } else {
            float th = (1.0 - a * (uGrainMode == 3 ? 1.4 : 1.0)) * uGrainDepth;
            float soft = uGrainMode == 3 ? 0.09 : 0.25;
            a *= smoothstep(th - soft, th + soft, g);
        }
    }
    fragColor = vec4(vColor.rgb * a, a);
    fragEdge = vec4(edge);
}
