#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){ float a=asin(v_uv.x)*0.6366; o_color=vec4(a,a,a,1.0); }
