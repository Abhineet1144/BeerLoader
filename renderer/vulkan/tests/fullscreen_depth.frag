#version 450
layout(set = 0, binding = 0) uniform texture2D depth_texture;
layout(set = 0, binding = 1) uniform sampler depth_sampler;
layout(location = 0) out vec4 output_color;
void main()
{
    float depth = texture(sampler2D(depth_texture, depth_sampler), vec2(0.5)).r;
    output_color = vec4(depth, depth, depth, 1.0);
}
