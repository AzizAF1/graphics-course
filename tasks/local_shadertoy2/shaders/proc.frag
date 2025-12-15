#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;

layout(push_constant) uniform Push {
  vec2 iResolution;
  float iTime;
  float _pad0;
  vec4 iMouse;
} pc;

float hash(vec2 p){
  p = fract(p * vec2(123.34, 456.21));
  p += dot(p, p + 78.233);
  return fract(p.x * p.y);
}

void main() {
  vec2 uv = vUV;
  vec2 p = uv * 8.0;

  float n = hash(floor(p) + floor(pc.iTime * 2.0));
  float grid = smoothstep(0.48, 0.50, abs(fract(p.x) - 0.5)) *
               smoothstep(0.48, 0.50, abs(fract(p.y) - 0.5));

  vec3 col = mix(vec3(0.08,0.10,0.14), vec3(0.9,0.6,0.2)*n, 0.65);
  col += 0.15 * vec3(grid);

  outColor = vec4(col, 1.0);
}
