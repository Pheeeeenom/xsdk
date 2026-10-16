#version 450
// gl_VertexIndex, which xsc doesn't lower: refused, not fetched as a vertex input.
layout(location = 0) in vec4 a_position;

void main()
{
	gl_Position = a_position + vec4(float(gl_VertexIndex), 0.0, 0.0, 0.0);
}
