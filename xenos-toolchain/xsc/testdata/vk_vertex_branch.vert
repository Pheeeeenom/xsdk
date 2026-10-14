#version 450
// A ?: of calls, which glslang makes a branch: a vertex shader lowers as straight-line code, so it's refused rather
// than lowered as its last arm (tests/test_vertex_branches.py).
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_scaled;

void main()
{
    float len2 = dot(a_position.xyz, a_position.xyz);
    gl_Position = a_position;
    v_scaled = a_position * (len2 > 1e-10 ? inversesqrt(len2) : 1.0);
}
