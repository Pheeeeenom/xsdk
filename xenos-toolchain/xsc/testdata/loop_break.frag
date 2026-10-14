#version 450
// data-dependent while-break: accumulate until a body value crosses a
// threshold, then break early (the predicated-LOOP_END primitive, G30).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) uniform int u_count;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 sum = vec4(0.0);
    for (int i = 0; i < u_count; i++) {
        sum += vec4(0.0625);
        if (sum.x > 0.25)
            break;
    }
    o_color = sum;
}
