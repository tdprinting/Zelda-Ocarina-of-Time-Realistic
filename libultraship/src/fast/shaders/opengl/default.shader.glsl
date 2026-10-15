@prism(type='fragment', name='Fast3D Fragment Shader', version='1.0.0', description='Ported shader to prism', author='Emill & Prism Team')

@{GLSL_VERSION}

@if(VERTEX_SHADER)
    @{attr} vec4 aVtxPos;

    @for(i in 0..2)
        @if(o_textures[i])
            @{attr} vec2 aTexCoord@{i};
            @{out} vec2 vTexCoord@{i};
            @{update_floats(2)}
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        @{attr} float aTexClampS@{i};
                        @{out} float vTexClampS@{i};
                    @else
                        @{attr} float aTexClampT@{i};
                        @{out} float vTexClampT@{i};
                    @end
                    @{update_floats(1)}
                @end
            @end
        @end
    @end

    @if(o_fog)
        @{attr} vec4 aFog;
        @{out} vec4 vFog;
        @{update_floats(4)}
    @end

    @if(o_grayscale)
        @{attr} vec4 aGrayscaleColor;
        @{out} vec4 vGrayscaleColor;
        @{update_floats(4)}
    @end

    @for(i in 0..o_inputs)
        @if(o_alpha)
            @{attr} vec4 aInput@{i + 1};
            @{out} vec4 vInput@{i + 1};
            @{update_floats(4)}
        @else
            @{attr} vec3 aInput@{i + 1};
            @{out} vec3 vInput@{i + 1};
            @{update_floats(3)}
        @end
    @end

    @if(o_modern_lighting)
        @{attr} vec3 aNormal;
        @{attr} vec3 aWorldPos;
        @{out} vec3 vNormal;
        @{out} @{highp} vec3 vWorldPos;
        @{update_floats(3)}
        @{update_floats(3)}
    @end

    void main() {
        @for(i in 0..2)
            @if(o_textures[i])
                vTexCoord@{i} = aTexCoord@{i};
                @for(j in 0..2)
                    @if(o_clamp[i][j])
                        @if(j == 0)
                            vTexClampS@{i} = aTexClampS@{i};
                        @else
                            vTexClampT@{i} = aTexClampT@{i};
                        @end
                    @end
                @end
            @end
        @end
        @if(o_fog)
            vFog = aFog;
        @end
        @if(o_grayscale)
            vGrayscaleColor = aGrayscaleColor;
        @end
        @for(i in 0..o_inputs)
            vInput@{i + 1} = aInput@{i + 1};
        @end
        @if(o_modern_lighting)
            vNormal = aNormal;
            vWorldPos = aWorldPos;
        @end
        gl_Position = aVtxPos;
        @if(opengles)
            gl_Position.z *= 0.3f;
        @end
    }
@else
    @if(core_opengl || opengles)
    out vec4 vOutColor;
    @end

    @for(i in 0..2)
        @if(o_textures[i])
            @{attr} vec2 vTexCoord@{i};
            @for(j in 0..2)
                @if(o_clamp[i][j])
                    @if(j == 0)
                        @{attr} float vTexClampS@{i};
                    @else
                        @{attr} float vTexClampT@{i};
                    @end
                @end
            @end
        @end
    @end

    @if(o_fog) @{attr} vec4 vFog;
    @if(o_grayscale) @{attr} vec4 vGrayscaleColor;

    @for(i in 0..o_inputs)
        @if(o_alpha)
            @{attr} vec4 vInput@{i + 1};
        @else
            @{attr} vec3 vInput@{i + 1};
        @end
    @end

    @if(o_textures[0]) uniform sampler2D uTex0;
    @if(o_textures[1]) uniform sampler2D uTex1;

    @if(o_masks[0]) uniform sampler2D uTexMask0;
    @if(o_masks[1]) uniform sampler2D uTexMask1;

    @if(o_blend[0]) uniform sampler2D uTexBlend0;
    @if(o_blend[1]) uniform sampler2D uTexBlend1;

    uniform int frame_count;
    uniform float noise_scale;

    @if(o_modern_lighting)
    // Modern per-pixel lighting. Replaces the N64's per-vertex SHADE color on lit geometry
    // (characters, items, weapons) and adds specular and rim light on top of the combiner output.
    uniform vec3 uLightAmbient;
    uniform int uLightCount;
    uniform vec3 uLightDir[@{MODERN_LIGHTING_MAX_LIGHTS}];
    uniform vec3 uLightColor[@{MODERN_LIGHTING_MAX_LIGHTS}];
    uniform @{highp} vec3 uEyePos;
    uniform vec4 uLightParams; // x: diffuse wrap, y: specular strength, z: specular power, w: rim strength

    @{attr} vec3 vNormal;
    @{attr} @{highp} vec3 vWorldPos;

    vec3 modernDiffuse(vec3 n) {
        vec3 c = uLightAmbient;
        float wrap = uLightParams.x;
        for (int i = 0; i < @{MODERN_LIGHTING_MAX_LIGHTS}; i++) {
            if (i >= uLightCount) break;
            float d = clamp((dot(n, uLightDir[i]) + wrap) / (1.0 + wrap), 0.0, 1.0);
            c += uLightColor[i] * d;
        }
        return min(c, vec3(1.0));
    }

    vec3 modernRim(vec3 n, vec3 v) {
        float fresnel = 1.0 - clamp(dot(n, v), 0.0, 1.0);
        // Rim is for silhouettes: fade it out on upward-facing surfaces (floors, terrain), which are
        // otherwise seen at grazing angles and wash out
        fresnel *= 1.0 - clamp(n.y, 0.0, 1.0);
        return (uLightAmbient + (uLightCount > 0 ? uLightColor[0] : vec3(0.0))) *
               (fresnel * fresnel * fresnel * uLightParams.w);
    }

    vec3 modernSpecularRim(vec3 n, vec3 v) {
        vec3 s = vec3(0.0);
        float power = max(uLightParams.z, 1.0);
        // Normalized Blinn-Phong so highlights keep their energy as they tighten
        float norm = (power + 8.0) / 25.1327;
        for (int i = 0; i < @{MODERN_LIGHTING_MAX_LIGHTS}; i++) {
            if (i >= uLightCount) break;
            float ndl = max(dot(n, uLightDir[i]), 0.0);
            vec3 h = normalize(uLightDir[i] + v);
            s += uLightColor[i] * (pow(max(dot(n, h), 0.0), power) * norm * ndl);
        }
        return s * uLightParams.y + modernRim(n, v);
    }

    @if(o_material)
    // Material maps from a mod pack (see docs/modern-rendering/MATERIALS.md), sampled with TEXEL0's UVs
    uniform sampler2D uMatNormal;         // tangent-space normal, OpenGL convention (+Y up)
    uniform sampler2D uMatRoughnessMetal; // glTF layout: G roughness, B metalness
    uniform sampler2D uMatSkin;           // R: skin mask
    uniform vec4 uMatParams;              // x: roughness scale, y: metalness scale, z: normal strength,
                                          // w: skin (eyes: >0 eye mask in uMatSkin.r, 0 whole texture, <0 auto mask)
    uniform vec2 uMatMirror;              // 1 where the base texture mirrors along S, T
    @if(o_material_eye)
    // Expressive eyes. uEyeInfo: what the renderer found in the eye texture (MaterialState::eyeInfo); uFace: the
    // face from G_SETFACEPARAMS ([0] look target, weight; [1] saccade xy, upper lid, lower lid; [2] tilt, pupil,
    // active)
    uniform vec4 uEyeInfo[9];
    uniform @{highp} vec4 uFace[3];
    @end

    // Tangent frame from screen-space derivatives (Schüler's cotangent frame), so meshes need no tangents
    vec3 perturbNormal(vec3 n, @{highp} vec3 p, vec2 uv, vec3 tn) {
        @{highp} vec3 dp1 = dFdx(p);
        @{highp} vec3 dp2 = dFdy(p);
        vec2 duv1 = dFdx(uv);
        vec2 duv2 = dFdy(uv);
        vec3 dp2perp = cross(dp2, n);
        vec3 dp1perp = cross(n, dp1);
        vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
        float len2 = max(dot(t, t), dot(b, b));
        if (len2 < 1e-20) {
            return n;
        }
        float invmax = inversesqrt(len2);
        // Texture V runs down the image while the map's +Y points up it
        return normalize(mat3(t * invmax, -b * invmax, n) * tn);
    }

    float ggxSpecular(float ndh, float ndl, float ndv, float roughness) {
        float a = roughness * roughness;
        float a2 = a * a;
        float d = ndh * ndh * (a2 - 1.0) + 1.0;
        float k = a * 0.5;
        float vis = 1.0 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k));
        // D * Vis / 4, times pi because the game's lights are scaled for a diffuse of color * N.L
        return a2 / (d * d) * vis * 0.25;
    }

    vec3 materialDiffuse(vec3 n, float skin) {
        vec3 c = uLightAmbient;
        for (int i = 0; i < @{MODERN_LIGHTING_MAX_LIGHTS}; i++) {
            if (i >= uLightCount) break;
            float ndl = dot(n, uLightDir[i]);
            float lit = clamp(ndl, 0.0, 1.0);
            // Skin: light wraps past the terminator with a warm tint, a cheap stand-in for subsurface scattering
            float wrapped = clamp((ndl + 0.5) / 1.5, 0.0, 1.0);
            vec3 skinLit = vec3(wrapped) + vec3(0.25, 0.04, 0.02) * (wrapped - lit);
            c += uLightColor[i] * mix(vec3(lit), skinLit, skin);
        }
        return min(c, vec3(1.0));
    }

    vec3 materialSpecular(vec3 n, vec3 v, vec3 base, float roughness, float metal) {
        vec3 f0 = mix(vec3(0.04), base, metal);
        float ndv = max(dot(n, v), 1e-3);
        vec3 s = vec3(0.0);
        for (int i = 0; i < @{MODERN_LIGHTING_MAX_LIGHTS}; i++) {
            if (i >= uLightCount) break;
            float ndl = max(dot(n, uLightDir[i]), 0.0);
            vec3 h = normalize(uLightDir[i] + v);
            float vdh = max(dot(v, h), 0.0);
            vec3 f = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
            s += uLightColor[i] * f * (ggxSpecular(max(dot(n, h), 0.0), ndl, ndv, roughness) * ndl);
        }
        // Environment reflection from a sky/ground hemisphere built from the scene's lights, so metals
        // reflect something instead of going black away from the highlight. Rough surfaces see its average.
        vec3 r = reflect(-v, n);
        vec3 sky = uLightAmbient + (uLightCount > 0 ? uLightColor[0] : vec3(0.0)) * 0.6;
        vec3 ground = uLightAmbient * 0.6;
        vec3 env = mix(mix(ground, sky, smoothstep(-0.4, 0.4, r.y)), 0.5 * (sky + ground), roughness);
        vec3 fAmb = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - ndv, 5.0);
        return s + env * fAmb;
    }
    @end

    @if(o_material_eye)
    // Eyes: a wet cornea (clearcoat-like, very smooth) over the iris and sclera, plus a soft catchlight
    // from the camera side so they stay alive even in shadow
    vec3 eyeCornea(vec3 n, vec3 v) {
        vec3 s = vec3(0.0);
        float ndv = max(dot(n, v), 1e-3);
        for (int i = 0; i < @{MODERN_LIGHTING_MAX_LIGHTS}; i++) {
            if (i >= uLightCount) break;
            float ndl = max(dot(n, uLightDir[i]), 0.0);
            vec3 h = normalize(uLightDir[i] + v);
            float f = 0.02 + 0.98 * pow(1.0 - max(dot(v, h), 0.0), 5.0);
            s += uLightColor[i] * (f * ggxSpecular(max(dot(n, h), 0.0), ndl, ndv, 0.08) * ndl);
        }
        vec3 catchDir = normalize(v + vec3(0.0, 0.35, 0.0));
        vec3 hc = normalize(catchDir + v);
        float catchlight = pow(max(dot(n, hc), 0.0), 120.0);
        return s + vec3(catchlight) * (uLightAmbient + 0.3);
    }
    @end
    @end

    @if(o_prim_depth)
    uniform float prim_depth;
    @end

    uniform int texture_width[2];
    uniform int texture_height[2];
    uniform int texture_filtering[2];

    #define TEX_OFFSET(off) @{texture}(tex, texCoord - off / texSize)
    #define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)

    float random(in vec3 value) {
        float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));
        return fract(sin(random) * 143758.5453);
    }

    vec4 fromLinear(vec4 linearRGB){
        bvec3 cutoff = lessThan(linearRGB.rgb, vec3(0.0031308));
        vec3 higher = vec3(1.055)*pow(linearRGB.rgb, vec3(1.0/2.4)) - vec3(0.055);
        vec3 lower = linearRGB.rgb * vec3(12.92);
        return vec4(mix(higher, lower, cutoff), linearRGB.a);
    }

    vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {
        vec2 offset = fract(texCoord*texSize - vec2(0.5));
        offset -= step(1.0, offset.x + offset.y);
        vec4 c0 = TEX_OFFSET(offset);
        vec4 c1 = TEX_OFFSET(vec2(offset.x - sign(offset.x), offset.y));
        vec4 c2 = TEX_OFFSET(vec2(offset.x, offset.y - sign(offset.y)));
        return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);
    }

    vec4 hookTexture2D(in int id, sampler2D tex, in vec2 uv, in vec2 texSize) {
    @if(o_three_point_filtering)
        if(texture_filtering[id] == @{FILTER_THREE_POINT}) {
            return filter3point(tex, uv, texSize);
        }
    @end
        return @{texture}(tex, uv);
    }

    #define TEX_SIZE(tex) vec2(texture_width[tex], texture_height[tex])

    void main() {
        @if(o_modern_lighting)
            vec3 mlNormal = normalize(vNormal);
            @{highp} vec3 mlToEye = uEyePos - vWorldPos;
            vec3 mlView = normalize(mlToEye);
            @if(o_material)
                vec3 mlGeomNormal = mlNormal;
                vec3 matTangentNormal = @{texture}(uMatNormal, vTexCoord0).xyz * 2.0 - 1.0;
                matTangentNormal.xy *= uMatParams.z;
                mlNormal = perturbNormal(mlNormal, vWorldPos, vTexCoord0, normalize(matTangentNormal));
                vec4 matRM = @{texture}(uMatRoughnessMetal, vTexCoord0);
                float matRoughness = clamp(matRM.g * uMatParams.x, 0.04, 1.0);
                float matMetal = clamp(matRM.b * uMatParams.y, 0.0, 1.0);
                @if(o_material_eye)
                    // Eyes keep the regular diffuse so a painted eye matches the face around it
                    vec3 mlDiffuse = modernDiffuse(mlNormal);
                @else
                    float matSkin = @{texture}(uMatSkin, vTexCoord0).r * uMatParams.w;
                    // Metals have no diffuse; their color comes from the specular below
                    vec3 mlDiffuse = materialDiffuse(mlNormal, matSkin) * (1.0 - matMetal);
                @end
            @else
                vec3 mlDiffuse = modernDiffuse(mlNormal);
            @end
            @for(i in 0..o_inputs)
                @if(o_shade_input[i])
                    @if(o_alpha)
                        vec4 vInputLit@{i + 1} = vec4(mlDiffuse, vInput@{i + 1}.a);
                    @else
                        vec3 vInputLit@{i + 1} = mlDiffuse;
                    @end
#define vInput@{i + 1} vInputLit@{i + 1}
                @end
            @end
        @end

        @for(i in 0..2)
            @if(o_textures[i])
                @{s = o_clamp[i][0]}
                @{t = o_clamp[i][1]}

                vec2 texSize@{i} = TEX_SIZE(@{i});

                @if(!s && !t)
                    vec2 vTexCoordAdj@{i} = vTexCoord@{i};
                @else
                    @if(s && t)
                        vec2 vTexCoordAdj@{i} = clamp(vTexCoord@{i}, 0.5 / texSize@{i}, vec2(vTexClampS@{i}, vTexClampT@{i}));
                    @elseif(s)
                        vec2 vTexCoordAdj@{i} = vec2(clamp(vTexCoord@{i}.s, 0.5 / texSize@{i}.s, vTexClampS@{i}), vTexCoord@{i}.t);
                    @else
                        vec2 vTexCoordAdj@{i} = vec2(vTexCoord@{i}.s, clamp(vTexCoord@{i}.t, 0.5 / texSize@{i}.t, vTexClampT@{i}));
                    @end
                @end

                vec4 texVal@{i} = hookTexture2D(@{i}, uTex@{i}, vTexCoordAdj@{i}, texSize@{i});

                @if(o_masks[i])
                    @if(opengles) 
                        vec2 maskSize@{i} = vec2(textureSize(uTexMask@{i}, 0));
                    @else 
                        vec2 maskSize@{i} = textureSize(uTexMask@{i}, 0);
                    @end

                    vec4 maskVal@{i} = hookTexture2D(@{i}, uTexMask@{i}, vTexCoordAdj@{i}, maskSize@{i});

                    @if(o_blend[i])
                        vec4 blendVal@{i} = hookTexture2D(@{i}, uTexBlend@{i}, vTexCoordAdj@{i}, texSize@{i});
                    @else
                        vec4 blendVal@{i} = vec4(0, 0, 0, 0);
                    @end

                    texVal@{i} = mix(texVal@{i}, blendVal@{i}, maskVal@{i}.a);
                @end
            @end
        @end

        @if(o_modern_lighting && o_material_eye)
            // Expressive eyes: repaint the vanilla eye with a procedural one that can look around, widen or narrow
            // its pupil and close its lids, driven by G_SETFACEPARAMS
            float eyeLidCover = 0.0;
            if (uMatParams.w < 0.0 && uFace[2].z > 0.5 && uEyeInfo[0].w > 0.5) {
                // Eye space, x across the eye in half widths (towards +u), y up in half heights: the renderer stored
                // each texel's offset from its eye's centre in the mask, which filters exactly like the texture so
                // it follows any tiling, clamping or mirroring
                vec4 eyeMap = @{texture}(uMatSkin, vTexCoord0);
                vec2 eyeQ = (eyeMap.gb * 2.0 - 1.0) * vec2(2.0, -2.0);
                bool eyeSecond = uEyeInfo[0].w > 1.5 && eyeMap.a > 0.5;
                vec4 eyeGeo = eyeSecond ? uEyeInfo[5] : uEyeInfo[1];
                vec4 eyeIris = eyeSecond ? uEyeInfo[6] : uEyeInfo[2];
                vec4 eyeSclera = eyeSecond ? uEyeInfo[7] : uEyeInfo[3];
                vec2 eyeRest = (eyeSecond ? uEyeInfo[8] : uEyeInfo[4]).xy;
                vec2 eyeTexSize = vec2(textureSize(uTex0, 0));
                float eyeAspect = (eyeGeo.z * eyeTexSize.x) / max(eyeGeo.w * eyeTexSize.y, 1e-4);

                // The texture's axes on the face, in world space
                @{highp} vec3 eyeDpx = dFdx(vWorldPos);
                @{highp} vec3 eyeDpy = dFdy(vWorldPos);
                vec2 eyeDuvx = dFdx(eyeQ) * vec2(1.0, -1.0);
                vec2 eyeDuvy = dFdy(eyeQ) * vec2(1.0, -1.0);
                float eyeDet = eyeDuvx.x * eyeDuvy.y - eyeDuvx.y * eyeDuvy.x;
                vec3 eyeU = vec3(1.0, 0.0, 0.0), eyeUp = vec3(0.0, 1.0, 0.0);
                if (abs(eyeDet) > 1e-12) {
                    eyeU = normalize((eyeDpx * eyeDuvy.y - eyeDpy * eyeDuvx.y) / eyeDet);
                    eyeUp = -normalize((eyeDpy * eyeDuvx.x - eyeDpx * eyeDuvy.x) / eyeDet);
                }
                // Gaze: towards the look target, plus the saccade (x = the character's left)
                @{highp} vec3 eyeLook = normalize(uFace[0].xyz - vWorldPos);
                vec2 eyeGaze = vec2(dot(eyeLook, eyeU), dot(eyeLook, eyeUp)) * uFace[0].w;
                vec3 eyeLeft = cross(eyeUp, mlGeomNormal);
                eyeGaze += vec2(uFace[1].x * sign(dot(eyeLeft, eyeU) + 1e-4), uFace[1].y);
                float irisR = eyeIris.w;
                vec2 irisC = clamp(vec2((eyeRest.x + eyeGaze.x) * eyeAspect, eyeRest.y + eyeGaze.y), -vec2(max(eyeAspect - irisR * 0.8, 0.0), max(1.0 - irisR * 0.6, 0.0)),
                                   vec2(max(eyeAspect - irisR * 0.8, 0.0), max(1.0 - irisR * 0.6, 0.0)));

                // Iris and pupil, at screen resolution: radial fibres, a brighter ring round the pupil, a dark
                // limbal ring at the edge, and a crisp pupil
                vec2 eyeD = vec2(eyeQ.x * eyeAspect, eyeQ.y) - irisC;
                float eyeR = length(eyeD);
                float eyeAa = max(fwidth(eyeR), 1e-3);
                float eyeAng = atan(eyeD.y, eyeD.x + 1e-6);
                float fibres = mix(0.5 + 0.5 * sin(eyeAng * 31.0 + 3.0 * sin(eyeAng * 7.0)),
                                   0.5 + 0.5 * sin(eyeAng * 53.0 + 1.7), 0.4);
                float eyeRn = eyeR / irisR;
                vec3 irisCol = eyeIris.rgb * (0.72 + 0.28 * fibres) * mix(1.3, 1.0, smoothstep(0.45, 0.7, eyeRn));
                irisCol = mix(irisCol, eyeIris.rgb * 0.3, smoothstep(0.78, 1.0, eyeRn));
                float pupilR = irisR * 0.45 * uFace[2].y;
                float irisA = 1.0 - smoothstep(irisR - eyeAa, irisR + eyeAa, eyeR);
                float pupilA = 1.0 - smoothstep(pupilR - eyeAa, pupilR + eyeAa, eyeR);
                float eyeRho = length(eyeQ);
                vec3 eyeCol = eyeSclera.rgb * (1.0 - 0.3 * smoothstep(0.35, 1.0, eyeRho));
                eyeCol = mix(eyeCol, irisCol, irisA);
                eyeCol = mix(eyeCol, vec3(0.03), pupilA);
                eyeCol *= 1.0 - 0.25 * smoothstep(0.2, 1.0, eyeQ.y); // the upper lid's shadow

                // Lids: skin from the top down to the upper lid line (slanted by tilt, towards the nose) and from
                // the bottom up to the lower lid, which rises along an upward curve (a smile or squint)
                float upperLid = clamp(uFace[1].z, 0.0, 1.0), lowerLid = clamp(uFace[1].w, 0.0, 1.0);
                float towardNose = eyeQ.x * eyeSclera.w;
                float lidU = 1.25 - 2.45 * upperLid + 0.25 * (1.0 - eyeQ.x * eyeQ.x) * (1.0 - upperLid) -
                             clamp(uFace[2].x, -1.0, 1.0) * 0.45 * towardNose * (1.0 - 0.5 * upperLid);
                float lidL = mix(-1.25, lidU - 0.02, lowerLid) + 0.3 * lowerLid * (1.0 - lowerLid) * (1.0 - eyeQ.x * eyeQ.x);
                float lidAa = max(fwidth(eyeQ.y), 1e-3);
                eyeLidCover = max(smoothstep(lidU - lidAa, lidU + lidAa, eyeQ.y), 1.0 - smoothstep(lidL - lidAa, lidL + lidAa, eyeQ.y));
                eyeCol = mix(eyeCol, uEyeInfo[0].rgb * 0.95, eyeLidCover);
                float lashU = 1.0 - smoothstep(0.0, 0.12 + lidAa, abs(eyeQ.y - lidU));
                float lashL = (1.0 - smoothstep(0.0, 0.05 + lidAa, abs(eyeQ.y - lidL))) * step(0.02, lowerLid);
                eyeCol = mix(eyeCol, vec3(0.07, 0.05, 0.04), max(lashU, lashL * 0.7));

                // Keep the vanilla eyeliner round the rim of the eye
                float vanillaLine = (1.0 - smoothstep(0.08, 0.2, dot(texVal0.rgb, vec3(0.3, 0.59, 0.11)))) * smoothstep(0.7, 0.95, eyeRho);
                eyeCol = mix(eyeCol, texVal0.rgb, vanillaLine * (1.0 - eyeLidCover));
                float eyeMaskV = eyeMap.r;
                texVal0.rgb = mix(texVal0.rgb, eyeCol, eyeMaskV);
                eyeLidCover *= eyeMaskV;
            }
        @end

        @if(o_alpha) 
            vec4 texel;
        @else 
            vec3 texel;
        @end

        @if(o_2cyc)
            @{f_range = 2}
        @else
            @{f_range = 1}
        @end

        @for(c in 0..f_range)
            @if(c == 1)
                @if(o_alpha)
                    @if(o_c[c][1][2] == SHADER_COMBINED)
                        texel.a = WRAP(texel.a, -1.01, 1.01);
                    @else
                        texel.a = WRAP(texel.a, -0.51, 1.51);
                    @end
                @end

                @if(o_c[c][0][2] == SHADER_COMBINED)
                    texel.rgb = WRAP(texel.rgb, -1.01, 1.01);
                @else
                    texel.rgb = WRAP(texel.rgb, -0.51, 1.51);
                @end
            @end

            @if(!o_color_alpha_same[c] && o_alpha)
                texel = vec4(@{
                append_formula(o_c[c], o_do_single[c][0],
                            o_do_multiply[c][0], o_do_mix[c][0], false, false, true, c == 0)
                }, @{append_formula(o_c[c], o_do_single[c][1],
                            o_do_multiply[c][1], o_do_mix[c][1], true, true, true, c == 0)
                });
            @else
                texel = @{append_formula(o_c[c], o_do_single[c][0],
                            o_do_multiply[c][0], o_do_mix[c][0], o_alpha, false,
                            o_alpha, c == 0)};
            @end
        @end

        texel = WRAP(texel, -0.51, 1.51);
        texel = clamp(texel, 0.0, 1.0);

        @if(o_modern_lighting && o_uses_shade)
            @if(o_material)
                // Base color for the specular tint: the texel with lighting divided back out
                vec3 matBase = texVal0.rgb;
                vec3 mlSpec = materialSpecular(mlNormal, mlView, matBase, matRoughness, matMetal);
                @if(o_material_eye)
                    if (uMatParams.w < 0.0) {
                        // Vanilla eyes ("eyeMask": "auto") are painted on flat faces, with skin and brows around
                        // them. Keep the face's own lighting and add a cornea over the eye only, rounded from
                        // the offsets the renderer stored in the mask (G, B) so the highlight and catchlight
                        // sit on the eye as on a real eyeball.
                        vec4 eyeMap = @{texture}(uMatSkin, vTexCoord0);
                        vec2 eyeOffset = clamp((eyeMap.gb * 2.0 - 1.0) * 2.0, -1.0, 1.0);
                        // On the mirrored copies of the texture its texels run against the UVs
                        vec2 eyeFlip = mix(vec2(1.0), 1.0 - 2.0 * mod(floor(vTexCoord0), 2.0), uMatMirror);
                        eyeOffset *= eyeFlip;
                        vec3 eyeNormal = perturbNormal(mlGeomNormal, vWorldPos, vTexCoord0,
                                                       normalize(vec3(eyeOffset.x * 1.6, -eyeOffset.y * 1.6, 1.0)));
                        mlSpec = modernSpecularRim(mlGeomNormal, mlView) +
                                 eyeCornea(eyeNormal, mlView) * eyeMap.r * (1.0 - eyeLidCover);
                    } else {
                        // A mask map (skinMask) or the whole texture
                        float eyeMask = uMatParams.w > 0.0 ? @{texture}(uMatSkin, vTexCoord0).r : 1.0;
                        mlSpec = mix(modernSpecularRim(mlGeomNormal, mlView), mlSpec + eyeCornea(mlGeomNormal, mlView),
                                     eyeMask);
                    }
                @else
                    mlSpec += modernRim(mlNormal, mlView);
                @end
            @else
                vec3 mlSpec = modernSpecularRim(mlNormal, mlView);
            @end
            @if(o_alpha)
                texel.rgb = clamp(texel.rgb + mlSpec * texel.a, 0.0, 1.0);
            @else
                texel = clamp(texel + mlSpec, 0.0, 1.0);
            @end
        @end
        // TODO discard if alpha is 0?
        @if(o_fog)
            @if(o_alpha)
                texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);
            @else
                texel = mix(texel, vFog.rgb, vFog.a);
            @end
        @end

        @if(o_texture_edge && o_alpha)
            if (texel.a > 0.19) texel.a = 1.0; else discard;
        @end

        @if(o_alpha && o_noise)
            texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + texel.a, 0.0, 1.0));
        @end

        @if(o_grayscale)
            float intensity = (texel.r + texel.g + texel.b) / 3.0;
            vec3 new_texel = vGrayscaleColor.rgb * intensity;
            texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);
        @end

        @if(o_alpha)
            @if(o_alpha_threshold)
                if (texel.a < 8.0 / 256.0) discard;
            @end
            @if(o_invisible)
                texel.a = 0.0;
            @end
            @{vOutColor} = texel;
        @else
            @{vOutColor} = vec4(texel, 1.0);
        @end

        @if(srgb_mode)
            @{vOutColor} = fromLinear(@{vOutColor});
        @end

        @if(o_prim_depth)
            gl_FragDepth = prim_depth;
        @end
    }
@end