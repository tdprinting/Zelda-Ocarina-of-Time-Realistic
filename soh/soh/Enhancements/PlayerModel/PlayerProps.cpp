#include "PlayerProps.h"
#include "PlayerModel.h"
#include "PlayerModelData.h"

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/utils/StrHash64.h>
#include <libultraship/libultraship.h>
#include "soh/frame_interpolation.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "soh/ResourceManagerHelpers.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "objects/object_link_boy/object_link_boy.h"
}

namespace PlayerModel {

enum Prop { kPropSword, kPropSwordSheathed, kPropSheath, kNumProps };

static const char* const kPropPaths[kNumProps] = {
    "player-model/master-sword.lmdl",
    "player-model/master-sword-sheathed.lmdl",
    "player-model/master-sheath.lmdl",
};

struct PropMesh {
    std::vector<Vtx> vtx;
    std::vector<Gfx> dl;
};

// What to do with one vanilla display list: which textures' triangles to drop and which props replace them.
struct Swap {
    std::vector<Gfx> filtered;
    uint32_t props = 0; // bit per Prop
};

static int sLoadState = 0; // 0 not tried, 1 loaded, -1 missing
static PropMesh sProps[kNumProps];
static std::unordered_map<const Gfx*, Swap> sSwaps;
static bool sSwapsBuilt = false;

struct Pending {
    MtxF mtx;
    uint32_t props;
};
static std::vector<Pending> sPending;
static uint32_t sLimbProps[PLAYER_LIMB_MAX];
static bool sActive = false;

static bool IsExpanded(uint8_t op) {
    return op == G_SETTIMG_OTR_HASH || op == G_DL_OTR_HASH || op == G_VTX_OTR_HASH || op == G_BRANCH_Z_OTR ||
           op == G_MARKER || op == G_MTX_OTR || op == G_MOVEMEM_OTR;
}

static bool LoadProp(Prop p) {
    auto archive = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
    auto file = archive->LoadFile(kPropPaths[p]);
    if (file == nullptr || file->Buffer == nullptr) {
        SPDLOG_WARN("PlayerProps: {} not found, keeping the vanilla Master Sword", kPropPaths[p]);
        return false;
    }
    Model m;
    std::string error;
    if (!ParseModel(file->Buffer->data(), file->Buffer->size(), m, error)) {
        SPDLOG_ERROR("PlayerProps: failed to read {}: {}", kPropPaths[p], error);
        return false;
    }
    BuildChunks(m, 64);

    PropMesh& mesh = sProps[p];
    mesh.vtx.resize(m.chunkVerts.size());
    for (size_t i = 0; i < m.chunkVerts.size(); i++) {
        const Vertex& s = m.verts[m.chunkVerts[i]];
        Vtx_tn& d = mesh.vtx[i].n;
        for (int k = 0; k < 3; k++) {
            d.ob[k] = (s16)std::lround(s.pos[k]);
            d.n[k] = (s8)std::lround(std::clamp(s.nrm[k], -1.0f, 1.0f) * 127.0f);
        }
        d.flag = 0;
        d.tc[0] = s.tc[0];
        d.tc[1] = s.tc[1];
        d.a = 255;
    }

    // The props are lit like Link's own limbs and go through the material renderer (metal, gold, leather).
    // Textures are named by resource path; the strings live as long as the model list does.
    static std::vector<std::string> sTexNames;
    sTexNames.reserve(64);
    Gfx g;
    auto push = [&](const Gfx& c) { mesh.dl.push_back(c); };
    gDPPipeSync(&g);
    push(g);
    gDPSetCycleType(&g, G_CYC_2CYCLE);
    push(g);
    gDPSetRenderMode(&g, G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2);
    push(g);
    gDPSetCombineMode(&g, G_CC_MODULATEIDECALA, G_CC_PASS2);
    push(g);
    gSPClearGeometryMode(&g, G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_CULL_FRONT);
    push(g);
    gSPSetGeometryMode(&g, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_FOG | G_LIGHTING | G_SHADING_SMOOTH);
    push(g);
    gDPSetPrimColor(&g, 0, 0, 255, 255, 255, 255);
    push(g);
    gSPTexture(&g, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    push(g);
    uint32_t material = UINT32_MAX;
    for (const Chunk& c : m.chunks) {
        if (c.material != material) {
            material = c.material;
            sTexNames.push_back("__OTR__" + m.materials[material].texture);
            // Built into a scratch buffer because the macro emits several commands.
            Gfx tmp[16];
            Gfx* t = tmp;
            gDPPipeSync(t++);
            // No wrap mask and the same texture coordinate scheme as the player model (see PlayerModel.cpp).
            gDPLoadTextureBlock(t++, sTexNames.back().c_str(), G_IM_FMT_RGBA, G_IM_SIZ_32b, kNominalTexSize,
                                kNominalTexSize, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, G_TX_NOMASK,
                                G_TX_NOMASK, kTexShift, kTexShift);
            mesh.dl.insert(mesh.dl.end(), tmp, t);
        }
        gSPVertex(&g, (uintptr_t)&mesh.vtx[c.firstVert], c.numVerts, 0);
        push(g);
        const uint8_t* tri = &m.chunkTris[c.firstTri * 3];
        uint32_t t = 0;
        for (; t + 1 < c.numTris; t += 2) {
            gSP2Triangles(&g, tri[t * 3], tri[t * 3 + 1], tri[t * 3 + 2], 0, tri[t * 3 + 3], tri[t * 3 + 4],
                          tri[t * 3 + 5], 0);
            push(g);
        }
        if (t < c.numTris) {
            gSP1Triangle(&g, tri[t * 3], tri[t * 3 + 1], tri[t * 3 + 2], 0);
            push(g);
        }
    }
    gDPPipeSync(&g);
    push(g);
    gSPEndDisplayList(&g);
    push(g);
    return true;
}

static Gfx* Resolve(const char* name) {
    return ResourceMgr_LoadGfxByName(name);
}

// Copies `dl` without the triangles drawn while one of `drop` is the bound texture. A texture image followed by
// a palette load is the palette, not the texture.
static std::vector<Gfx> Filter(const Gfx* dl, const std::unordered_set<uint64_t>& drop) {
    std::vector<Gfx> out;
    uint64_t pending = 0, current = 0;
    for (int i = 0; dl != nullptr && i < 8192; i++) {
        const uint8_t op = (uint8_t)(dl[i].words.w0 >> 24);
        if (op == G_SETTIMG_OTR_HASH) {
            pending = ((uint64_t)(uint32_t)dl[i + 1].words.w0 << 32) | (uint32_t)dl[i + 1].words.w1;
        } else if (op == (uint8_t)G_SETTIMG || op == G_SETTIMG_OTR_FILEPATH) {
            pending = 0;
        } else if (op == (uint8_t)G_LOADTLUT) {
            pending = current;
        } else if (op == (uint8_t)G_LOADBLOCK || op == (uint8_t)G_LOADTILE) {
            current = pending;
        }
        const bool isTri = op == (uint8_t)G_TRI1 || op == (uint8_t)G_TRI2 || op == (uint8_t)G_QUAD || op == G_TRI1_OTR;
        if (isTri && drop.count(current) != 0) {
            continue;
        }
        out.push_back(dl[i]);
        if (IsExpanded(op)) {
            out.push_back(dl[++i]);
        }
        if (op == (uint8_t)G_ENDDL) {
            return out;
        }
    }
    return {};
}

static void BuildSwaps() {
    sSwapsBuilt = true;
    auto hashes = [](std::initializer_list<const char*> names) {
        std::unordered_set<uint64_t> s;
        for (const char* n : names) {
            s.insert(CRC64(n));
        }
        return s;
    };
    const auto hilt = { "objects/object_link_boy/gLinkAdultSwordPommelTex",
                        "objects/object_link_boy/gLinkAdultSwordGuardTex",
                        "objects/object_link_boy/gLinkAdultSwordEmblemTex" };
    // In the hand lists the blade is the only geometry using the shiny highlight texture; on the back the
    // Mirror Shield uses it too, so it is only dropped from the hand.
    std::unordered_set<uint64_t> hand = hashes(hilt);
    hand.insert(CRC64("objects/gameplay_keep/gHilite1Tex"));
    const std::unordered_set<uint64_t> sheath =
        hashes({ "objects/object_link_boy/gLinkAdultSheathTex", "objects/object_link_boy/gLinkAdultSheathBandTex" });
    std::unordered_set<uint64_t> sheathed = sheath;
    for (uint64_t h : hashes(hilt)) {
        sheathed.insert(h);
    }

    auto add = [&](const char* name, const std::unordered_set<uint64_t>& drop, uint32_t props) {
        Gfx* dl = Resolve(name);
        if (dl == nullptr) {
            return;
        }
        Swap s;
        s.filtered = Filter(dl, drop);
        s.props = props;
        if (!s.filtered.empty()) {
            sSwaps[dl] = std::move(s);
        }
    };
    const uint32_t swordProp = 1u << kPropSword;
    const uint32_t backFull = (1u << kPropSwordSheathed) | (1u << kPropSheath);
    const uint32_t backEmpty = 1u << kPropSheath;
    add(gLinkAdultLeftHandHoldingMasterSwordNearDL, hand, swordProp);
    add(gLinkAdultLeftHandHoldingMasterSwordFarDL, hand, swordProp);
    add(gLinkAdultMasterSwordAndSheathNearDL, sheathed, backFull);
    add(gLinkAdultMasterSwordAndSheathFarDL, sheathed, backFull);
    add(gLinkAdultHylianShieldSwordAndSheathNearDL, sheathed, backFull);
    add(gLinkAdultHylianShieldSwordAndSheathFarDL, sheathed, backFull);
    add(gLinkAdultMirrorShieldSwordAndSheathNearDL, sheathed, backFull);
    add(gLinkAdultMirrorShieldSwordAndSheathFarDL, sheathed, backFull);
    add(gLinkAdultSheathNearDL, sheath, backEmpty);
    add(gLinkAdultSheathFarDL, sheath, backEmpty);
    add(gLinkAdultHylianShieldAndSheathNearDL, sheath, backEmpty);
    add(gLinkAdultHylianShieldAndSheathFarDL, sheath, backEmpty);
    add(gLinkAdultMirrorShieldAndSheathNearDL, sheath, backEmpty);
    add(gLinkAdultMirrorShieldAndSheathFarDL, sheath, backEmpty);
}

} // namespace PlayerModel

using namespace PlayerModel;

extern "C" s32 PlayerProps_Enabled(void) {
    if (!CVarGetInteger(CVAR_PLAYER_MODEL_MASTER_SWORD, 1)) {
        return false;
    }
    if (sLoadState == 0) {
        sLoadState = 1;
        for (int p = 0; p < kNumProps; p++) {
            if (!LoadProp((Prop)p)) {
                sLoadState = -1;
                break;
            }
        }
    }
    return sLoadState == 1;
}

static bool sDebugTurned = false;

extern "C" void PlayerProps_Begin(void) {
    sActive = PlayerProps_Enabled();
    sPending.clear();
    memset(sLimbProps, 0, sizeof(sLimbProps));
    // Same test hook as the young Link model: turn Link in place to see him from the front.
    const s32 yaw = CVarGetInteger(CVAR_PLAYER_MODEL_DEBUG("Yaw"), 0);
    sDebugTurned = yaw != 0;
    if (sDebugTurned) {
        Matrix_Push();
        Matrix_RotateY(yaw * ((float)M_PI / 180.0f), MTXMODE_APPLY);
    }
}

// Test hook: gSettings.PlayerModel.Debug.SwordInHand draws the Master Sword in adult Link's hand (and an empty
// sheath) without having to draw it in game.
static void ApplyDebugSwordInHand(s32 limbIndex, Gfx** dList) {
    if (!CVarGetInteger(CVAR_PLAYER_MODEL_DEBUG("SwordInHand"), 0)) {
        return;
    }
    static std::unordered_map<const Gfx*, const char*> sEmptySheath;
    if (sEmptySheath.empty()) {
        sEmptySheath[Resolve(gLinkAdultMasterSwordAndSheathNearDL)] = gLinkAdultSheathNearDL;
        sEmptySheath[Resolve(gLinkAdultMasterSwordAndSheathFarDL)] = gLinkAdultSheathFarDL;
        sEmptySheath[Resolve(gLinkAdultHylianShieldSwordAndSheathNearDL)] = gLinkAdultHylianShieldAndSheathNearDL;
        sEmptySheath[Resolve(gLinkAdultHylianShieldSwordAndSheathFarDL)] = gLinkAdultHylianShieldAndSheathFarDL;
        sEmptySheath[Resolve(gLinkAdultMirrorShieldSwordAndSheathNearDL)] = gLinkAdultMirrorShieldAndSheathNearDL;
        sEmptySheath[Resolve(gLinkAdultMirrorShieldSwordAndSheathFarDL)] = gLinkAdultMirrorShieldAndSheathFarDL;
    }
    if (limbIndex == PLAYER_LIMB_L_HAND) {
        *dList = Resolve(gLinkAdultLeftHandHoldingMasterSwordNearDL);
    } else if (limbIndex == PLAYER_LIMB_SHEATH) {
        auto it = sEmptySheath.find(*dList);
        if (it != sEmptySheath.end()) {
            *dList = Resolve(it->second);
        }
    }
}

extern "C" void PlayerProps_OverrideLimb(s32 limbIndex, Gfx** dList) {
    if (dList == nullptr || *dList == nullptr || limbIndex < 0 || limbIndex >= PLAYER_LIMB_MAX) {
        return;
    }
    // The pause menu hands over resource names instead of loaded lists.
    Gfx* dl = *dList;
    if (ResourceMgr_OTRSigCheck((char*)dl)) {
        dl = ResourceMgr_LoadGfxByName((const char*)dl);
    }
    if (CVarGetInteger(CVAR_PLAYER_MODEL_DEBUG("SwordInHand"), 0)) {
        ApplyDebugSwordInHand(limbIndex, &dl);
        *dList = dl;
    }
    if (!sActive) {
        return;
    }
    if (!sSwapsBuilt) {
        BuildSwaps();
    }
    auto it = sSwaps.find(dl);
    if (it == sSwaps.end()) {
        return;
    }
    *dList = it->second.filtered.data();
    sLimbProps[limbIndex] = it->second.props;
}

extern "C" void PlayerProps_PostLimb(s32 limbIndex) {
    if (!sActive || limbIndex < 0 || limbIndex >= PLAYER_LIMB_MAX || sLimbProps[limbIndex] == 0) {
        return;
    }
    Pending p;
    Matrix_Get(&p.mtx);
    p.props = sLimbProps[limbIndex];
    sLimbProps[limbIndex] = 0;
    sPending.push_back(p);
}

extern "C" void PlayerProps_End(PlayState* play) {
    if (sDebugTurned) {
        Matrix_Pop();
        sDebugTurned = false;
    }
    if (!sActive || sPending.empty()) {
        return;
    }
    OPEN_DISPS(play->state.gfxCtx);
    for (Pending& p : sPending) {
        Matrix_Push();
        Matrix_Put(&p.mtx);
        gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        Matrix_Pop();
        for (int k = 0; k < kNumProps; k++) {
            if (p.props & (1u << k)) {
                gSPDisplayList(POLY_OPA_DISP++, sProps[k].dl.data());
            }
        }
    }
    CLOSE_DISPS(play->state.gfxCtx);
    sPending.clear();
}
