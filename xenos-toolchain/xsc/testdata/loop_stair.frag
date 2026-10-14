#version 450
layout(location=0) in vec2 v_uv;
layout(location=1) in vec4 v_color;
layout(binding=0) uniform sampler2D u_tex;
layout(location=0) out vec4 o_color;
void main(){
  // count how many of 5 evenly-spaced thresholds v_uv.x clears -> a 5-band
  // staircase in x. The threshold float(i)*0.2 forces per-iteration constant
  // folding of the induction variable; a wrong trip count or wrong i-sequence
  // would change the number/spacing of bands.
  float s = 0.0;
  for (int i = 0; i < 5; i++) { s += step(float(i) * 0.2, v_uv.x); }
  s = s * 0.2;                 // normalize 0..5 -> 0..1
  o_color = vec4(s, s, s, 1.0);
}
