#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 vertex_color;
layout(location = 2) in vec2 texcoord;
layout(location = 0) out vec4 color;

void main()
{
    gl_Position = vec4(position, 1.0);
    color = vertex_color + vec4(texcoord * 0.0, 0.0, 0.0);
}
