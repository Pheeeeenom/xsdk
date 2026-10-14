#version 450
// vector relational (greaterThan -> bvec) + any/all/not reductions.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    bvec3 gt = greaterThan(v_color.rgb, vec3(0.5));
    bvec3 ng = not(gt);
    o_color = vec4(any(gt) ? 1.0 : 0.0,
                   all(gt) ? 1.0 : 0.0,
                   any(ng) ? 1.0 : 0.0,
                   1.0);
}
