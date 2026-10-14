#version 450
// threshold highlight: where the texture is bright, show the vertex color
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform vec4 u_threshold;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    if (tex.r > u_threshold.r) {
        o_color = v_color;
    } else {
        o_color = tex;
    }
}
