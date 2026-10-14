#version 450
// Branches assign a Function-local, then the merged value feeds output math.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    vec4 c;
    if (tex.r > v_color.g) {
        c = tex;
    } else {
        c = v_color;
    }
    o_color = c * v_color;
}
