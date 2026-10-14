#version 450
// ternary ?: as a value (OpSelect): pick vertex color where u>0.5, else a tint.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = (v_uv.x > 0.5) ? v_color : vec4(0.1, 0.2, 0.3, 1.0);
}
