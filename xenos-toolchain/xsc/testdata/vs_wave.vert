#version 450
// Unsupported for now: VS literals need a host manifest, but must fail cleanly.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_mvp;

void main()
{
    float wave = sin(a_position.x + 0.5);
    vec4 p = a_position + vec4(0.0, wave, 0.0, 0.0);
    gl_Position = u_mvp * p;
    v_texcoord = a_texcoord;
    v_color = a_color;
}
