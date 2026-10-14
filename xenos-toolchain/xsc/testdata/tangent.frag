#version 450
// tan(x) = sin(x) / cos(x): scalar transcendental pair plus a reciprocal.
// Tints the vertex color by the tangent of a phase uniform. Exercises
// GLSL.std.450 Tan (#15), which lowers on top of the (already-supported)
// Sin/Cos scalar ops and the new reciprocal path.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform float u_phase;
layout(location = 0) out vec4 o_color;

void main()
{
    float t = tan(u_phase);
    o_color = v_color * t;
}
