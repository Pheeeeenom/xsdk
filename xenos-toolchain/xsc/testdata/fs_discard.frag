#version 450
// Discard half the surface; the gallery slot should visibly cut pixels away.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    if (v_uv.x < 0.5)
        discard;
    o_color = tex * v_color;
}
