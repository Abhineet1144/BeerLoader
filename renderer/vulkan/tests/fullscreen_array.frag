#version 450

layout(set = 0, binding = 8192) uniform texture2DArray source_image;
layout(set = 0, binding = 40960) uniform sampler source_sampler;
layout(location = 0) out vec4 color;

void main()
{
    color = texture(sampler2DArray(source_image, source_sampler),
                    vec3(0.5, 0.5, 1.0));
}
