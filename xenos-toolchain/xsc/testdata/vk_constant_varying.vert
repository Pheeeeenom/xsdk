#version 450
// Exports straight from constants: a uniform passed on as a varying.
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform Colors { vec4 color; } u;

void main()
{
    gl_Position = a_position;
    v_color = u.color;
}
