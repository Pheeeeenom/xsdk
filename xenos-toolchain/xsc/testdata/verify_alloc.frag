#version 450
// register-allocator proof: two independent asin chains held live at once
// (sin(asin)=identity) force >16 GPRs, but the result is a clean two-axis
// ramp: R = uv.x, G = uv.y. Garbage => allocator mis-shared a register.
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){
  float a = sin(asin(v_uv.x));
  float b = sin(asin(v_uv.y));
  o_color = vec4(a, b, 0.25, 1.0);
}
