#version 450
// threshold builtins: step(edge,x) = (x>=edge) -> native kSge; and
// smoothstep(e0,e1,x) = t*t*(3-2t), t=clamp((x-e0)/(e1-e0),0,1) (uses the
// new division + clamp + hermite).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(u_tex, v_uv);
    o_color = step(0.5, t) * smoothstep(0.2, 0.8, v_color);
}
