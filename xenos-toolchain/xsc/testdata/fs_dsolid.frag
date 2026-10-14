#version 450
// DIAGNOSTIC: runs getGradients (fwidth) but outputs a near-SOLID colour.
// fwidth(uv) is tiny so the select always takes v_color -> a clean flat-shaded
// cube IF getGradients is benign to the pipeline; a warped cube means
// getGradients itself perturbs rasterization (not the shading).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    float w = fwidth(v_uv.x + v_uv.y);
    o_color = (w > 100.0) ? vec4(1.0, 0.0, 0.0, 1.0) : v_color;
}
