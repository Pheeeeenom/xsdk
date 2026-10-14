#version 450

layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_color;

layout(binding = 0) uniform Transform {
    mat4 mvp;
};

layout(location = 0) out vec4 v_color;

void main() {
    gl_Position = mvp * a_position;
    v_color = a_color;
}
