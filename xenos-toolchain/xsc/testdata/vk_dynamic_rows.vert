#version 450
// Matrix rows picked at run time by a vertex attribute, as Aurora's matrices picked per vertex are: reads at a0.
layout(location = 0) in vec4 a_position;
layout(location = 2) in float a_index;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform Rows { vec4 rows[12]; vec4 colors[4]; } u;

void main()
{
    int i = int(a_index);
    gl_Position = vec4(dot(u.rows[i], a_position), dot(u.rows[i + 1], a_position), dot(u.rows[i + 2], a_position),
                       1.0);
    v_color = u.colors[i / 3];
}
