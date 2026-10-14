#version 450
// Vertex lighting proof: VS literals feed normalize/dot/max math.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 3) in vec3 a_normal;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_mvp;

void main()
{
    vec3 n = normalize(a_normal);
    float lit = max(dot(n, vec3(0.0, 0.0, 1.0)), 0.0);
    gl_Position = u_mvp * a_position;
    v_texcoord = a_texcoord;
    v_color = vec4(a_color.rgb * lit, a_color.a);
}
