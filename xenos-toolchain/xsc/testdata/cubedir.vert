#version 450
// Gallery cubemap vertex shader: export position.xyz as sample direction.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec3 v_dir;
layout(location = 0) uniform mat4 u_mvp;

void main()
{
    gl_Position = u_mvp * a_position;
    v_dir = a_position.xyz;
}
