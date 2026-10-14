#version 450
// uniform array indexed by the loop counter -> aL-relative constant addressing
// inside a real hardware loop (convolution / palette / weight-sum pattern).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) uniform int u_count;
layout(location = 1) uniform vec4 u_w[4];
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 sum = vec4(0.0);
    for (int i = 0; i < u_count; i++)
        sum += u_w[i];
    o_color = sum;
}
