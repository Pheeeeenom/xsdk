#version 450
// additive glow, clamped to [0,1] — exercises clamp() -> saturate modifier
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = clamp(tex + v_color, 0.0, 1.0);
}
