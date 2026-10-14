#version 450
// VectorTimesMatrix proof: row-vector style transform.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_basis;

void main()
{
    gl_Position = a_position * u_basis;
    v_texcoord = a_texcoord;
    v_color = a_color;
}
