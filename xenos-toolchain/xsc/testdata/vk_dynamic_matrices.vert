#version 450
// A whole matrix picked at run time: its four columns read at a0, a0 the index times 4.
layout(location = 0) in vec4 a_position;
layout(location = 2) in float a_index;
layout(set = 0, binding = 0) uniform Matrices { mat4 m[3]; } u;

void main()
{
    gl_Position = u.m[int(a_index)] * a_position;
}
