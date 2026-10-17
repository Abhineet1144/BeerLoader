#version 450

layout(set = 0, binding = 8192) uniform texture3D source_image;
layout(set = 0, binding = 40960) uniform sampler source_sampler;
layout(location = 0) out vec4 color;

void main()
{
    vec4 sampled = texture(sampler3D(source_image, source_sampler),
                           vec3(0.5, 0.5, 0.625));
    color = sampled + vec4(0.0, 0.0, 0.0, 1.0);
}
