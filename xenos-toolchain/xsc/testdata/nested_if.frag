#version 450
// nested if/else: lowers branch-free to chained cnd selects.
//   if (tex.r > 0.5) { if (color.g > 0.5) A else B } else C
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    if (tex.r > 0.5) {
        if (v_color.g > 0.5)
            o_color = tex;
        else
            o_color = v_color;
    } else {
        o_color = tex * v_color;
    }
}
