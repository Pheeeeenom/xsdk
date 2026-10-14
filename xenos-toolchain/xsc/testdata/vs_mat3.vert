#version 450
// mat3 normal matrix (the ubiquitous lighting transform) alongside the mat4 MVP.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_mvp;
layout(location = 4) uniform mat3 u_normal;

void main()
{
    gl_Position = u_mvp * a_position;
    vec3 n = u_normal * a_normal;
    v_texcoord = a_texcoord;
    v_color = vec4(n * 0.5 + 0.5, 1.0);
}
