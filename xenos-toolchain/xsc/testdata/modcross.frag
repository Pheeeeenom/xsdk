#version 450
// mod(x,y) = x - y*floor(x/y) (OpFMod), and cross(a,b) (vec3 cross product
// via yzx/zxy swizzles + mad). Result assembled with a general vec4(...).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) uniform vec4 u_period;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex, v_uv);
    vec3 c = cross(t.xyz, v_color.xyz);
    vec4 m = mod(t, u_period);
    o_color = vec4(c, m.w);
}
