#version 450
// screen-space derivatives: dFdx/dFdy/fwidth via the Xenos getGradients fetch.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    vec2 gx = dFdx(v_uv);
    vec2 gy = dFdy(v_uv);
    float w = fwidth(v_uv.x + v_uv.y);
    o_color = vec4(abs(gx + gy) * 32.0, w * 32.0, 1.0);
}
