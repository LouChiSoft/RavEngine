#extension GL_EXT_samplerless_texture_functions : enable
// adapted from: https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom

layout(push_constant, std430) uniform UniformBufferObject{
    ivec2 targetDim;
    float filterRadius;
} ubo;

layout(binding = 0) uniform texture2D srcTexture;
layout(binding = 1) uniform sampler srcSampler;

layout (location = 0) out vec4 out_upsample;

void main()
{
    vec3 upsample = vec3(0,0,0);
    vec2 texCoord = gl_FragCoord.xy / ubo.targetDim;
    vec2 srcResolution = textureSize(srcTexture,0).xy;
    
    // The filter kernel is applied with a radius, specified in texture
    // coordinates, so that the radius will vary across mip resolutions.
    float x = ubo.filterRadius;
    float y = ubo.filterRadius;

    // Take 9 samples around current texel:
    // a - b - c
    // d - e - f
    // g - h - i
    // === ('e' is the current texel) ===
    vec3 a = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x - x, texCoord.y + y)).rgb;
    vec3 b = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x,     texCoord.y + y)).rgb;
    vec3 c = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x + x, texCoord.y + y)).rgb;

    vec3 d = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x - x, texCoord.y)).rgb;
    vec3 e = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x,     texCoord.y)).rgb;
    vec3 f = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x + x, texCoord.y)).rgb;

    vec3 g = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x - x, texCoord.y - y)).rgb;
    vec3 h = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x,     texCoord.y - y)).rgb;
    vec3 i = texture(sampler2D(srcTexture, srcSampler), vec2(texCoord.x + x, texCoord.y - y)).rgb;

    // Apply weighted distribution, by using a 3x3 tent filter:
    //  1   | 1 2 1 |
    // -- * | 2 4 2 |
    // 16   | 1 2 1 |
    upsample = e*4.0;
    upsample += (b+d+f+h)*2.0;
    upsample += (a+c+g+i);
    upsample *= 1.0 / 16.0;
    out_upsample = vec4(upsample, 0);
}
