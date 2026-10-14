#version 450
// MatrixTimesMatrix proof: combined matrix immediately feeds a vector.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_model;
layout(location = 4) uniform mat4 u_viewproj;

void main()
{
    gl_Position = (u_viewproj * u_model) * a_position;
    v_texcoord = a_texcoord;
    v_color = a_color;
}
