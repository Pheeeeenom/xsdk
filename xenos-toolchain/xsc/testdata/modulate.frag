#version 450
// combiner-style modulate fragment shader, in real GLSL (xsc test input #2)
layout(location = 0) in vec2 v_texcoord;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
uniform sampler2D u_tex0;

void main()
{
    o_color = texture(u_tex0, v_texcoord) * v_color;
}
