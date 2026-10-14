#version 450
// 0/1 literal fold proof: identities and export lanes need no literal pool.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    float r = tex.r + 0.0;
    float g = v_color.g * 1.0;
    o_color = vec4(r, g, 0.0, 1.0);
}
