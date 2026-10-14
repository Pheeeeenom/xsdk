#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){
  float s = 0.0;
  int i = 0;
  while (i < 4) { s += v_uv.x; i++; }   // 4*uv.x
  o_color = vec4(s, s, s, 1.0);
}
