#version 450
// inverse trig: asin/acos/atan via polynomial approximations on the scalar
// pipe (like sin/cos, these are approximate + range-noted). asin uses the
// Abramowitz-Stegun sqrt form; acos = pi/2 - asin; atan a [-1,1] odd poly.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform float u_a;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex, v_uv);
    o_color = t * asin(u_a) + t * acos(u_a) - t * atan(u_a);
}
