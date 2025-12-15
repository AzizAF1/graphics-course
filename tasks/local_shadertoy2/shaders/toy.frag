#version 450

layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;

layout(set=0, binding=0) uniform sampler2D iChannel0; // procedural (pass A)
layout(set=0, binding=1) uniform sampler2D iChannel1; // loaded image (step 3)

layout(push_constant) uniform Push {
    vec2 iResolution;
    float iTime;
    float _pad0;
    vec4 iMouse;
} pc;

vec3  iResolution = vec3(pc.iResolution, 1.0);
float iTime       = pc.iTime;
vec4  iMouse      = pc.iMouse;

// ---- helpers ----
vec3 sampleSky(vec3 rd)
{
    vec2 uv = rd.xy * 0.5 + 0.5;
    uv = clamp(uv, 0.0, 1.0);
    vec3 p = texture(iChannel0, uv).rgb;
    float g = 0.5 + 0.5 * rd.y;
    vec3 base = mix(vec3(0.08,0.10,0.14), vec3(0.25,0.35,0.6), g);
    return mix(base, p, 0.6);
}

// -------------------- YOUR CODE (adapted) --------------------

float sdSphere(vec3 p, float r){ return length(p) - r; }
float sdPlane(vec3 p, vec3 n, float h){ return dot(p, n) + h; }

float sdBox(vec3 p, vec3 b)
{
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float mapScene(vec3 p, out int materialID)
{
    float t = iTime;
    vec3 spherePos = vec3(sin(t) * 1.0, 0.5, 0.0);
    float dSphere = sdSphere(p - spherePos, 0.7);

    float dPlane = sdPlane(p, vec3(0.0, 1.0, 0.0), 1.0); // y = -1

    vec3 boxPos = vec3(-1.5, -0.2, 0.5);
    float dBox = sdBox(p - boxPos, vec3(0.4, 0.8, 0.4));

    float d = dSphere;
    materialID = 1;

    if (dPlane < d) { d = dPlane; materialID = 2; }
    if (dBox   < d) { d = dBox;   materialID = 3; }
    return d;
}

float mapScene(vec3 p){ int dummy; return mapScene(p, dummy); }

vec3 getNormal(vec3 p)
{
    float eps = 0.001;
    vec2 e = vec2(1.0, -1.0);
    return normalize(
        e.xyy * mapScene(p + e.xyy * eps) +
        e.yyx * mapScene(p + e.yyx * eps) +
        e.yxy * mapScene(p + e.yxy * eps) +
        e.xxx * mapScene(p + e.xxx * eps)
    );
}

float rayMarch(vec3 ro, vec3 rd, out vec3 pos, out int materialID)
{
    float t = 0.0;
    float maxDist = 100.0;
    const int MAX_STEPS = 128;
    const float EPS = 0.001;

    for (int i = 0; i < MAX_STEPS; i++)
    {
        pos = ro + rd * t;
        float d = mapScene(pos, materialID);
        if (d < EPS) return t;
        t += d;
        if (t > maxDist) break;
    }
    return -1.0;
}

float softShadow(vec3 ro, vec3 rd)
{
    float res = 1.0;
    float t = 0.02;
    float maxDist = 20.0;
    const int MAX_STEPS = 64;

    for (int i = 0; i < MAX_STEPS; i++)
    {
        vec3 p = ro + rd * t;
        float h = mapScene(p);
        if (h < 0.001) return 0.0;
        res = min(res, 10.0 * h / t);
        t += clamp(h, 0.02, 0.5);
        if (t > maxDist) break;
    }
    return clamp(res, 0.0, 1.0);
}

vec3 triplanarTex(vec3 p, vec3 normal)
{
    float scale = 0.5;
    vec3 n = abs(normal) + 1e-5;
    n /= (n.x + n.y + n.z);

    vec2 uvX = p.yz * scale;
    vec2 uvY = p.zx * scale;
    vec2 uvZ = p.xy * scale;

    vec3 texX = texture(iChannel1, uvX).rgb;
    vec3 texY = texture(iChannel1, uvY).rgb;
    vec3 texZ = texture(iChannel1, uvZ).rgb;

    return texX * n.x + texY * n.y + texZ * n.z;
}

vec3 getMaterialColor(int matID, vec3 pos, vec3 normal)
{
    if (matID == 1) return triplanarTex(pos, normal);
    if (matID == 2)
    {
        float h = clamp((pos.y + 1.0) * 0.5, 0.0, 1.0);
        vec3 base = mix(vec3(0.2), vec3(0.5), h);
        float spots = smoothstep(0.0, 0.3, sin(pos.x * 2.5) * sin(pos.z * 2.5));
        base *= 0.6 + 0.4 * spots;
        return base;
    }
    if (matID == 3) return vec3(0.3, 0.9, 0.4);
    return vec3(1.0);
}

vec3 phongLighting(vec3 pos, vec3 normal, vec3 viewDir, int matID)
{
    vec3 baseColor = getMaterialColor(matID, pos, normal);
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    vec3 lightColor = vec3(1.0);

    float ambient = 0.12;
    float diff = max(dot(normal, lightDir), 0.0);

    vec3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfDir), 0.0), 32.0);

    float shadow = softShadow(pos + normal * 0.01, lightDir);

    vec3 col = vec3(0.0);
    col += ambient * baseColor;
    col += diff * baseColor * lightColor * shadow;
    col += spec * lightColor * shadow;
    return col;
}

vec3 getRayDir(vec2 uv, vec3 ro, vec3 target, float fov)
{
    vec3 forward = normalize(target - ro);
    vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), forward));
    vec3 up = cross(forward, right);
    return normalize(forward * fov + uv.x * right + uv.y * up);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;

    vec3 ro = vec3(0.0, 0.5, 4.0);
    vec3 target = vec3(0.0, 0.0, 0.0);
    float fov = 1.5;

    float t = iTime * 0.4;
    ro.xz = vec2(sin(t), cos(t)) * 4.0;

    vec3 rd = getRayDir(uv, ro, target, fov);

    vec3 pos;
    int matID;
    float dist = rayMarch(ro, rd, pos, matID);

    vec3 col;
    if (dist > 0.0)
    {
        vec3 normal = getNormal(pos);
        vec3 viewDir = normalize(ro - pos);
        col = phongLighting(pos, normal, viewDir, matID);

        float fog = exp(-0.05 * dist * dist);
        vec3 fogColor = sampleSky(rd);
        col = mix(fogColor, col, fog);
    }
    else
    {
        col = sampleSky(rd);
    }

    col = pow(col, vec3(0.4545));
    fragColor = vec4(col, 1.0);
}

void main()
{
    vec2 fragCoord = vUV * iResolution.xy;
    vec4 c;
    mainImage(c, fragCoord);
    outColor = c;
}
