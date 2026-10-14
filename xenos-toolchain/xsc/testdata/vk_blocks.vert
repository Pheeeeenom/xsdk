#version 450
// Vulkan blocks: push constants, two uniform blocks, a column-major matrix and members inside a register.
layout(location = 0) in vec4 a_position;
layout(location = 0) out vec4 v_color;
layout(set = 0, binding = 0) uniform Transform { mat4 mvp; vec4 tint; } u;
layout(set = 0, binding = 1) uniform Extra { float scale; vec2 offset; } e;
layout(push_constant) uniform Push { vec4 color; } pc;

void main()
{
    gl_Position = u.mvp * a_position;
    v_color = pc.color * u.tint * e.scale + vec4(e.offset, 0.0, 0.0);
}
