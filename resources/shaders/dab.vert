// Instanced brush dab: one quad per instance, expanded by a pixel for antialiasing.
layout(location = 0) in vec4 aPosSize; // x, y, diameter, angle
layout(location = 1) in vec4 aColor;   // r, g, b, flow
layout(location = 2) in vec4 aMisc;    // roundness, seedX, seedY, unused

uniform vec2 uCanvasSize;

out vec2 vUv;
out vec2 vLocalPx;
out float vCoverage;
out vec4 vColor;
out vec2 vSeed;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1)) * 2.0 - 1.0;
    // Dabs thinner than a pixel are drawn one pixel wide and faded instead,
    // otherwise fine lines break up into dots.
    float diameter = max(aPosSize.z, 1.0);
    float thin = min(aPosSize.z, 1.0);
    float roundness = clamp(aMisc.x, 0.02, 1.0);
    vec2 radii = vec2(diameter * 0.5, max(diameter * 0.5 * roundness, 0.5));
    vec2 local = corner * (radii + 1.0);
    float c = cos(aPosSize.w);
    float s = sin(aPosSize.w);
    vec2 pos = aPosSize.xy + vec2(c * local.x - s * local.y, s * local.x + c * local.y);

    vUv = local / radii;
    vLocalPx = local;
    vCoverage = thin * thin;
    vColor = aColor;
    vSeed = aMisc.yz;
    gl_Position = vec4(pos / uCanvasSize * 2.0 - 1.0, 0.0, 1.0);
}
