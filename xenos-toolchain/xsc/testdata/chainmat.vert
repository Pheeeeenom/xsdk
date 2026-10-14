#version 450
// Chained vertex transforms: model-space position becomes world, then clip.
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_texcoord;
layout(location = 0) out vec2 v_texcoord;
layout(location = 1) out vec4 v_color;
layout(location = 0) uniform mat4 u_model;
layout(location = 4) uniform mat4 u_viewproj;

void main()
{
    vec4 world = u_model * a_position;
    gl_Position = u_viewproj * world;
    v_texcoord = a_texcoord;
    v_color = a_color;
}
