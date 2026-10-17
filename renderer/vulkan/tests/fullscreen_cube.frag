#version 450

layout(set = 0, binding = 8192) uniform samplerCube source_image;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(source_image, vec3(1.0, 0.0, 0.0));
}
