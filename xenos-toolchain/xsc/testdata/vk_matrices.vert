#version 450
// Column-major products: a matrix times a matrix times a vector, and a vector times a matrix.
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_row;
layout(set = 1, binding = 3) uniform Camera { mat4 view; mat4 projection; } c;

void main()
{
    gl_Position = c.projection * c.view * a_position;
    v_row = a_position * c.view;
}
