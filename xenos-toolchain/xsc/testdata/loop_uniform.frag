#version 450
// Real-loop frontend proof: uniform-bound loop, no aL-relative data use.
layout(location = 0) uniform int u_count;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 sum = vec4(0.0);
    for (int i = 0; i < u_count; i++)
        sum += vec4(0.12);
    o_color = sum;
}
