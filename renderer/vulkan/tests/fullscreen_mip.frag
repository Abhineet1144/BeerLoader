#version 450
layout(location = 0) out vec4 color;
layout(set = 0, binding = 8192) uniform texture2D sourceImage;
layout(set = 0, binding = 40960) uniform sampler sourceSampler;
void main()
{
    color = textureLod(sampler2D(sourceImage, sourceSampler), vec2(0.5), 1.0);
}
