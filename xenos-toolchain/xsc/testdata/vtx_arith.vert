#version 450
// Basic vertex arithmetic feeding both position and interpolator exports.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_mvp;
layout(location = 4) uniform vec4 u_offset;
layout(location = 5) uniform vec4 u_tint;

void main()
{
    vec4 shifted = a_position + u_offset;
    gl_Position = u_mvp * shifted;
    v_texcoord = a_texcoord;
    v_color = a_color * u_tint;
}
