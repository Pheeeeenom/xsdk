#version 450
// A colour read at the index the position read its scale at: a0, loaded for the position, read again past the
// interpolators' alloc, twice, where xost loads a0 again before the first (xsdk:docs/HARDWARE.md, "a0 doesn't survive
// an alloc").
layout(location = 0) in vec4 a_vertex;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform Rows { vec4 scales[16]; vec4 colors[16]; } u;

void main()
{
    int i = int(a_vertex.z);
    gl_Position = vec4(a_vertex.xy, 0.5, 1.0) * u.scales[i];
    v_color = u.colors[i] * u.scales[i].w;
}
