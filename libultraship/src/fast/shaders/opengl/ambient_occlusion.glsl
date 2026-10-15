// Screen-space ambient occlusion (Scalable Ambient Obscurance style), applied after the opaque pass.
// The engine prepends a #version header and defines VERTEX_SHADER, or AO_PASS / BLUR_PASS for the two
// fragment stages, plus FRAG_OUT. Mod packs can replace this file (shaders/opengl/ambient_occlusion.glsl).
//
// AO_PASS:   half resolution, 12 spiral samples rotated by a 4x4 interleaved pattern.
// BLUR_PASS: full resolution, depth-aware 4x4 blur of the AO term (removes the 4x4 pattern), multiplied
//            into the scene by blending.

#ifdef VERTEX_SHADER
in vec2 aPos;
out vec2 vUv;

void main() {
    vUv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
#else
in vec2 vUv;

uniform sampler2D uDepth;
uniform vec4 uProj;   // x: 1/P00, y: 1/P11, z: A, w: B  (view distance d = B / (ndcZ + A))
uniform vec4 uParams; // x: radius (world units), y: intensity, z: bias (fraction of radius), w: depth NDC scale
uniform vec2 uTexel;  // 1 / full-resolution framebuffer size

float viewDistance(vec2 uv) {
    float depth = texture(uDepth, uv).r;
    float ndcZ = (depth * 2.0 - 1.0) * uParams.w;
    return uProj.w / (ndcZ + uProj.z);
}

#ifdef AO_PASS
const int SAMPLES = 12;
const float SPIRAL_TURNS = 7.0;

vec3 viewPosition(vec2 uv) {
    float d = viewDistance(uv);
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * d * uProj.x, ndc.y * d * uProj.y, d);
}

void main() {
    if (texture(uDepth, vUv).r >= 0.99999) {
        // Sky and cleared background
        FRAG_OUT = vec4(1.0);
        return;
    }

    vec3 p = viewPosition(vUv);

    // Normal from the smaller of the two one-sided differences avoids halos at depth edges
    vec3 px1 = viewPosition(vUv + vec2(uTexel.x, 0.0)) - p;
    vec3 px2 = p - viewPosition(vUv - vec2(uTexel.x, 0.0));
    vec3 py1 = viewPosition(vUv + vec2(0.0, uTexel.y)) - p;
    vec3 py2 = p - viewPosition(vUv - vec2(0.0, uTexel.y));
    vec3 dx = abs(px1.z) < abs(px2.z) ? px1 : px2;
    vec3 dy = abs(py1.z) < abs(py2.z) ? py1 : py2;
    vec3 n = normalize(cross(dx, dy));
    // Face the normal toward the eye whatever the framebuffer's Y orientation
    if (dot(n, p) > 0.0) {
        n = -n;
    }

    float radius = uParams.x;
    float radius2 = radius * radius;
    float bias = uParams.z * radius;
    // Projected radius in UV units: radius * focal / distance
    float uvRadius = radius / (p.z * uProj.x) * 0.5;
    if (uvRadius < uTexel.x * 2.0) {
        FRAG_OUT = vec4(1.0);
        return;
    }

    // 4x4 interleaved rotation; the blur pass averages exactly one period of it
    vec2 cell = mod(floor(gl_FragCoord.xy), 4.0);
    float angle0 = (cell.x + cell.y * 4.0) * (6.2831853 / 16.0);
    float occlusion = 0.0;
    for (int i = 0; i < SAMPLES; i++) {
        float t = (float(i) + 0.5) / float(SAMPLES);
        float angle = angle0 + t * SPIRAL_TURNS * 6.2831853;
        vec2 offset = vec2(cos(angle), sin(angle)) * (t * uvRadius);
        vec3 v = viewPosition(vUv + offset) - p;
        float vv = dot(v, v);
        float vn = dot(v, n);
        float falloff = max(radius2 - vv, 0.0) / radius2;
        occlusion += falloff * max(vn - bias, 0.0) / (sqrt(vv) + 0.1 * radius);
    }
    occlusion = clamp(1.0 - occlusion * uParams.y * (2.0 / float(SAMPLES)), 0.0, 1.0);
    FRAG_OUT = vec4(vec3(occlusion), 1.0);
}
#endif

#ifdef BLUR_PASS
uniform sampler2D uAo;
uniform vec2 uAoTexel; // 1 / AO texture size

void main() {
    float d = viewDistance(vUv);
    float sum = 0.0;
    float weightSum = 0.0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 offset = (vec2(float(x), float(y)) - 1.5) * uAoTexel;
            float sampleDistance = viewDistance(vUv + offset);
            float w = 1.0 / (0.002 + abs(sampleDistance - d) / d);
            sum += texture(uAo, vUv + offset).r * w;
            weightSum += w;
        }
    }
    FRAG_OUT = vec4(vec3(sum / weightSum), 1.0);
}
#endif
#endif
