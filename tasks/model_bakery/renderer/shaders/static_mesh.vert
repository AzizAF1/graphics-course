#version 450
#extension GL_ARB_separate_shader_objects : enable

// BAKED layout (stride 32):
// loc0: vec3 position  (R32G32B32_SFLOAT) offset 0
// loc1: vec4 normal    (R8G8B8A8_UNORM)   offset 12  (xyz used)
// loc2: vec2 uv        (R32G32_SFLOAT)    offset 16
// loc3: vec4 tangent   (R8G8B8A8_UNORM)   offset 24  (xyz used, w ignored)

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aNormU8;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangU8;

layout(push_constant) uniform params_t
{
  mat4 mProjView;
  mat4 mModel;
} params;

layout(location = 0) out VS_OUT
{
  vec3 wPos;
  vec3 wNorm;
  vec3 wTangent;
  vec2 texCoord;
} vOut;

out gl_PerVertex { vec4 gl_Position; };

vec3 decode_u8_unorm_to_snorm(vec3 u)
{
  // UNORM gives [0..1] where value = byte/255.
  // Your baker encodes snorm [-1..1] as round(255 * 0.5*(x+1)).
  // So decode is x = u*2 - 1.
  return u * 2.0 - 1.0;
}

void main()
{
  vec3 n_obj = normalize(decode_u8_unorm_to_snorm(aNormU8.xyz));
  vec3 t_obj = normalize(decode_u8_unorm_to_snorm(aTangU8.xyz));

  vec3 wPos = (params.mModel * vec4(aPos, 1.0)).xyz;

  mat3 nrmMat = mat3(transpose(inverse(params.mModel)));
  vOut.wPos     = wPos;
  vOut.wNorm    = normalize(nrmMat * n_obj);
  vOut.wTangent = normalize(nrmMat * t_obj);
  vOut.texCoord = aUV;

  gl_Position = params.mProjView * vec4(wPos, 1.0);
}
