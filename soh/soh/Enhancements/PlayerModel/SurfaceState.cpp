#include "SurfaceState.h"
#include "PlayerModel.h"

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <fast/resource/type/Texture.h>
#include <libultraship/libultraship.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
void Player_Action_Roll(Player* this_, PlayState* play);
void Player_Action_8084377C(Player* this_, PlayState* play); // knocked down
void Player_Action_80843954(Player* this_, PlayState* play); // lying on the ground
void Player_Action_80843A38(Player* this_, PlayState* play); // getting up
}

namespace PlayerModel {

namespace {

constexpr char kSurfPath[] = "player-model/child-link.surf";
constexpr char kBaseColorPath[] = "textures/player-model/child-link/basecolor";
constexpr char kRoughnessPath[] = "textures/player-model/child-link/rm";

enum SurfaceClass { kNone, kCloth, kLinen, kSkin, kHair, kLeather, kMetal, kNumClasses };

// Seconds to dry from soaked, per class.
constexpr float kDryTime[kNumClasses] = { 1, 50, 40, 12, 30, 25, 6 };
// How much soaking darkens the color and how much glossier it gets.
constexpr float kWetDarken[kNumClasses] = { 0, 0.45f, 0.38f, 0.08f, 0.32f, 0.28f, 0.0f };
constexpr float kWetGloss[kNumClasses] = { 0, 0.75f, 0.7f, 0.45f, 0.6f, 0.55f, 0.1f };
// How readily dirt sticks.
constexpr float kDirtStick[kNumClasses] = { 0, 1.0f, 1.0f, 0.55f, 0.6f, 0.75f, 0.3f };

// Floor sound types (SurfaceType_GetSfx) that leave dirt, and how much.
float FloorDirt(u16 sfx, bool* grass) {
    *grass = false;
    switch (sfx) {
        case 0:  // dirt
        case 11: // soft dirt
            return 1.0f;
        case 1: // sand
            return 0.7f;
        case 6: // tall grass
        case 8: // grass
            *grass = true;
            return 0.6f;
        default:
            return 0.0f;
    }
}

struct Texel {
    u8 cls;
    u8 pad;
    u16 height;
    s16 z;
    s16 pad2;
};

struct State {
    int tried = 0; // 0 untried, 1 ready, -1 unavailable
    int size = 0;
    std::vector<Texel> texels;
    std::vector<float> wet, dirt, grass, noise;
    float clothWet = 0.0f;

    std::shared_ptr<Fast::Texture> baseTex, rmTex;
    std::vector<u8> baseOrig, rmOrig;
    int texW = 0, texH = 0;

    bool dirty = false;
    float pendingChange = 0.0f;
    int framesSinceWrite = 0;
    bool written = false;
    bool invalidate = false;
    int rollSeed = 0;
    void* lastAction = nullptr;
};

State sState;

float Hash(int x, int y, int s) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

float ValueNoise(float x, float y, int period, int seed) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    auto h = [&](int a, int b) {
        return Hash(((a % period) + period) % period, ((b % period) + period) % period, seed);
    };
    float a = h(x0, y0) + (h(x0 + 1, y0) - h(x0, y0)) * fx;
    float b = h(x0, y0 + 1) + (h(x0 + 1, y0 + 1) - h(x0, y0 + 1)) * fx;
    return a + (b - a) * fy;
}

bool Load() {
    auto archive = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
    auto file = archive->LoadFile(kSurfPath);
    if (file == nullptr || file->Buffer == nullptr || file->Buffer->size() < 16) {
        SPDLOG_WARN("PlayerModel: {} missing, wet and dirty clothes are off", kSurfPath);
        return false;
    }
    const char* d = file->Buffer->data();
    uint32_t hdr[4];
    memcpy(hdr, d, 16);
    if (memcmp(d, "LSRF", 4) != 0 || hdr[1] != 1 || hdr[2] != hdr[3] || hdr[2] == 0 || hdr[2] > 1024 ||
        file->Buffer->size() < 16 + (size_t)hdr[2] * hdr[2] * sizeof(Texel)) {
        SPDLOG_ERROR("PlayerModel: {} is malformed", kSurfPath);
        return false;
    }
    sState.size = (int)hdr[2];
    const size_t n = (size_t)sState.size * sState.size;
    sState.texels.resize(n);
    memcpy(sState.texels.data(), d + 16, n * sizeof(Texel));
    for (Texel& t : sState.texels) {
        if (t.cls >= kNumClasses) {
            t.cls = kNone;
        }
    }
    sState.wet.assign(n, 0.0f);
    sState.dirt.assign(n, 0.0f);
    sState.grass.assign(n, 0.0f);
    sState.noise.resize(n);
    for (int y = 0; y < sState.size; y++) {
        for (int x = 0; x < sState.size; x++) {
            // Blotchy, tiling noise for where dirt grabs.
            float v = 0.55f * ValueNoise(x / 16.0f, y / 16.0f, sState.size / 16, 1) +
                      0.3f * ValueNoise(x / 6.0f, y / 6.0f, sState.size / 6 + 1, 2) +
                      0.15f * ValueNoise(x / 2.5f, y / 2.5f, sState.size, 3);
            sState.noise[y * sState.size + x] = v;
        }
    }

    auto rm = Ship::Context::GetRawInstance()->GetResourceManager();
    sState.baseTex = std::dynamic_pointer_cast<Fast::Texture>(rm->LoadResourceProcess(kBaseColorPath));
    sState.rmTex = std::dynamic_pointer_cast<Fast::Texture>(rm->LoadResourceProcess(kRoughnessPath));
    auto validTex = [](const std::shared_ptr<Fast::Texture>& t, int& w, int& h) {
        if (t == nullptr || t->ImageData == nullptr || t->Type != Fast::TextureType::RGBA32bpp) {
            return false;
        }
        // Width/Height are the real image size, also for HD textures.
        w = (int)t->Width;
        h = (int)t->Height;
        return (size_t)w * h * 4 <= t->ImageDataSize;
    };
    int w2 = 0, h2 = 0;
    if (!validTex(sState.baseTex, sState.texW, sState.texH)) {
        SPDLOG_ERROR("PlayerModel: {} must be an RGBA32 texture", kBaseColorPath);
        return false;
    }
    sState.baseOrig.assign(sState.baseTex->ImageData,
                           sState.baseTex->ImageData + (size_t)sState.texW * sState.texH * 4);
    if (validTex(sState.rmTex, w2, h2) && w2 == sState.texW && h2 == sState.texH) {
        sState.rmOrig.assign(sState.rmTex->ImageData, sState.rmTex->ImageData + (size_t)w2 * h2 * 4);
    } else {
        sState.rmTex = nullptr;
    }
    return true;
}

bool Ready() {
    if (sState.tried == 0) {
        sState.tried = Load() ? 1 : -1;
    }
    return sState.tried == 1;
}

// Rewrites the base color and roughness textures from the pristine copies.
void Composite() {
    const int W = sState.texW, H = sState.texH, N = sState.size;
    u8* base = sState.baseTex->ImageData;
    u8* rm = sState.rmTex != nullptr ? sState.rmTex->ImageData : nullptr;
    const float sx = (float)N / W, sy = (float)N / H;

    for (int y = 0; y < H; y++) {
        const float gy = std::max(0.0f, (y + 0.5f) * sy - 0.5f);
        const int y0 = std::min((int)gy, N - 1), y1 = std::min(y0 + 1, N - 1);
        const float fy = gy - y0;
        for (int x = 0; x < W; x++) {
            const float gx = std::max(0.0f, (x + 0.5f) * sx - 0.5f);
            const int x0 = std::min((int)gx, N - 1), x1 = std::min(x0 + 1, N - 1);
            const float fx = gx - x0;
            auto bl = [&](const std::vector<float>& a) {
                float t = a[y0 * N + x0] + (a[y0 * N + x1] - a[y0 * N + x0]) * fx;
                float b = a[y1 * N + x0] + (a[y1 * N + x1] - a[y1 * N + x0]) * fx;
                return t + (b - t) * fy;
            };
            const size_t p = ((size_t)y * W + x) * 4;
            const int cls =
                sState.texels[std::min((int)((y + 0.5f) * sy), N - 1) * N + std::min((int)((x + 0.5f) * sx), N - 1)]
                    .cls;
            const float wet = bl(sState.wet);
            const float dirt = bl(sState.dirt);
            float r = sState.baseOrig[p], g = sState.baseOrig[p + 1], b = sState.baseOrig[p + 2];
            float rough = rm != nullptr ? sState.rmOrig[p + 1] / 255.0f : 0.5f;

            float mudA = 0.0f;
            if (dirt > 0.01f) {
                const float grain = Hash(x, y, 7);
                mudA = std::clamp((dirt - 0.15f * grain) * 1.25f, 0.0f, 0.92f);
                const float grassy = bl(sState.grass);
                // dusty dirt -> dark wet mud, with a green tint for grass stains
                float mr = 128 - 58 * wet, mg = 102 - 48 * wet, mb = 70 - 34 * wet;
                mr += (92 - mr) * grassy * 0.6f;
                mg += (108 - mg) * grassy * 0.6f;
                mb += (52 - mb) * grassy * 0.6f;
                const float shade = 0.82f + 0.36f * grain;
                r += (mr * shade - r) * mudA;
                g += (mg * shade - g) * mudA;
                b += (mb * shade - b) * mudA;
            }
            if (wet > 0.01f) {
                const float k = 1.0f - kWetDarken[cls] * wet;
                r *= k;
                g *= k;
                b *= k;
                rough *= 1.0f - kWetGloss[cls] * wet;
            }
            if (mudA > 0.0f) {
                rough += (0.95f - 0.45f * wet - rough) * mudA;
            }
            base[p] = (u8)std::clamp(r, 0.0f, 255.0f);
            base[p + 1] = (u8)std::clamp(g, 0.0f, 255.0f);
            base[p + 2] = (u8)std::clamp(b, 0.0f, 255.0f);
            if (rm != nullptr) {
                rm[p + 1] = (u8)std::clamp(rough * 255.0f, 3.0f, 255.0f);
            }
        }
    }
}

} // namespace

void SurfaceState_Reset() {
    if (sState.tried == 1) {
        std::fill(sState.wet.begin(), sState.wet.end(), 0.0f);
        std::fill(sState.dirt.begin(), sState.dirt.end(), 0.0f);
        std::fill(sState.grass.begin(), sState.grass.end(), 0.0f);
        sState.dirty = true;
        sState.pendingChange = 1.0f;
    }
}

float SurfaceState_ClothWetness() {
    return sState.clothWet;
}

void SurfaceState_Update(PlayState* play, Player* player, float modelScale) {
    if (!CVarGetInteger(CVAR_PLAYER_MODEL_SURFACE, 1) || !Ready() || player == nullptr || modelScale <= 0.0f) {
        return;
    }
    const float dt = 1.0f / 20.0f;
    const int N = sState.size;
    const size_t n = (size_t)N * N;

    // Water: everything below the water line soaks, and the water rinses dirt off.
    float waterLine = -1.0f; // model units above the feet
    if (player->actor.bgCheckFlags & BGCHECKFLAG_WATER) {
        waterLine = player->actor.yDistToWater / modelScale;
        if (player->stateFlags1 & PLAYER_STATE1_IN_WATER) {
            waterLine = 1e9f; // swimming: soaked head to toe
        }
    }

    // Dirt: rolling or lying on soft ground grinds it in; walking scuffs the boots.
    bool grass = false;
    const float floorDirt =
        (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) ? FloorDirt(player->floorSfxOffset, &grass) : 0.0f;
    const void* action = (const void*)player->actionFunc;
    const bool onGround = action == (const void*)Player_Action_Roll || action == (const void*)Player_Action_8084377C ||
                          action == (const void*)Player_Action_80843954 ||
                          action == (const void*)Player_Action_80843A38;
    if (action != sState.lastAction && onGround) {
        sState.rollSeed++; // each roll lands in a slightly different pattern
    }
    sState.lastAction = (void*)action;
    const float bodyRate = onGround ? floorDirt * 1.6f : 0.0f;
    const float bootRate =
        (player->actor.speedXZ > 1.0f) ? floorDirt * 0.05f * std::min(player->actor.speedXZ / 6.0f, 1.0f) : 0.0f;
    const int ox = (sState.rollSeed * 53) % N, oy = (sState.rollSeed * 97) % N;

    float change = 0.0f;
    float clothWet = 0.0f;
    int clothCount = 0;
    for (size_t i = 0; i < n; i++) {
        const Texel& t = sState.texels[i];
        if (t.cls == kNone) {
            continue;
        }
        float& w = sState.wet[i];
        float& d = sState.dirt[i];
        const float w0 = w, d0 = d;
        if (t.height < waterLine) {
            w = std::min(1.0f, w + dt * 2.0f);
            d = std::max(0.0f, d - dt * 0.35f);
        } else if (w > 0.0f) {
            w = std::max(0.0f, w - dt / kDryTime[t.cls]);
        }
        if (bodyRate > 0.0f || bootRate > 0.0f) {
            const int x = (int)(i % N), y = (int)(i / N);
            const float nz = sState.noise[((y + oy) % N) * N + (x + ox) % N];
            const float grab = std::max(0.0f, nz - 0.42f) * 2.4f * kDirtStick[t.cls] * (1.0f + 0.8f * w);
            float add = bodyRate * grab * dt * (t.height < 1600 ? 1.3f : 1.0f);
            if (t.height < 500) {
                add += bootRate * grab;
            }
            if (add > 0.0f) {
                const float cap = onGround ? 1.0f : 0.45f;
                if (d < cap) {
                    float nd = std::min(cap, d + add);
                    float& gr = sState.grass[i];
                    gr = (gr * d + (grass ? 1.0f : 0.0f) * (nd - d)) / std::max(nd, 1e-4f);
                    d = nd;
                }
            }
        }
        change += std::fabs(w - w0) + std::fabs(d - d0);
        if (t.cls == kCloth) {
            clothWet += w;
            clothCount++;
        }
    }
    sState.clothWet = clothCount > 0 ? clothWet / clothCount : 0.0f;
    sState.pendingChange += change / (float)n;
    sState.framesSinceWrite++;
    if (change > 0.0f) {
        sState.dirty = true;
    }
}

// Test hook: force a uniform wetness and a dirt level everywhere.
static void ApplyDebugOverrides() {
    static float sLastWet = -1.0f, sLastDirt = -1.0f;
    const float wet = CVarGetFloat(CVAR_PLAYER_MODEL_DEBUG("Wet"), -1.0f);
    const float dirt = CVarGetFloat(CVAR_PLAYER_MODEL_DEBUG("Dirt"), -1.0f);
    if (wet == sLastWet && dirt == sLastDirt) {
        return;
    }
    // Turning an override off (back to -1) leaves the clothes clean and dry.
    const bool clearWet = wet < 0.0f && sLastWet >= 0.0f;
    const bool clearDirt = dirt < 0.0f && sLastDirt >= 0.0f;
    sLastWet = wet;
    sLastDirt = dirt;
    const size_t n = sState.wet.size();
    for (size_t i = 0; i < n; i++) {
        if (wet >= 0.0f || clearWet) {
            sState.wet[i] = std::max(wet, 0.0f);
        }
        if (clearDirt) {
            sState.dirt[i] = 0.0f;
            sState.grass[i] = 0.0f;
        } else if (dirt >= 0.0f) {
            const float grab = std::max(0.0f, sState.noise[i] - 0.42f) * 2.4f * kDirtStick[sState.texels[i].cls];
            sState.dirt[i] = std::min(1.0f, dirt * grab * 2.0f);
            sState.grass[i] = 0.0f;
        }
    }
    sState.dirty = true;
    sState.pendingChange = 1.0f;
}

void SurfaceState_Apply(Gfx** gfx) {
    if (sState.tried == 0 && CVarGetInteger(CVAR_PLAYER_MODEL_SURFACE, 1)) {
        Ready();
    }
    if (sState.tried == 1) {
        ApplyDebugOverrides();
    }
    if (sState.tried != 1 || !sState.dirty) {
        return;
    }
    // Rewrite at most twice a second, or right away once a visible amount has changed.
    if (sState.framesSinceWrite < 10 && sState.pendingChange < 0.02f) {
        return;
    }
    Composite();
    sState.dirty = false;
    sState.pendingChange = 0.0f;
    sState.framesSinceWrite = 0;
    __gSPInvalidateTexCache((*gfx)++, (uintptr_t)sState.baseTex->ImageData);
    if (sState.rmTex != nullptr) {
        PlayerModel_RefreshMaterialMap(kRoughnessPath);
    }
}

} // namespace PlayerModel
