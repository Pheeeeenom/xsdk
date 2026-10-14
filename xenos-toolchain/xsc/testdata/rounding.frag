#version 450
// rounding family: fract / trunc / ceil / round. fract + trunc are native
// Xenos vector ops (kFrc/kTrunc); ceil = -floor(-x); round = floor(x+0.5).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex, v_uv);
    o_color = fract(t) + trunc(v_color) + ceil(t) + round(v_color);
}
