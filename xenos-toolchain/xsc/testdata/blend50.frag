#version 450
// fixed 50/50 blend of texture and vertex color — exercises a float literal
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 tex = texture(u_tex, v_uv);
    o_color = mix(tex, v_color, 0.5);
}
