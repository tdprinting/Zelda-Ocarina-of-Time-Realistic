#include "PlayerModel.h"
#include "PlayerModelData.h"
#include "ClothSim.h"
#include "FaceRig.h"
#include "SurfaceState.h"

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <libultraship/libultraship.h>
#include <libultraship/bridge/gfxbridge.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "objects/object_link_child/object_link_child.h"
#include "soh/ResourceManagerHelpers.h"
extern PlayState* gPlayState;
s32 Player_IsCustomLinkModel(void);
}

namespace PlayerModel {

static constexpr char kModelPath[] = "player-model/child-link.lmdl";
static constexpr int kVertsPerLoad = 64; // Fast3D vertex buffer size (MAX_VERTICES)

static Model sModel;
static int sLoadState = 0; // 0 = not tried, 1 = loaded, -1 = failed

// Per-draw state, filled between Begin and End.
static bool sActive = false;
static Player* sPlayer = nullptr;
static Mat4 sBase;
static Mat4 sLimbWorld[kNumLimbs];
static bool sLimbSeen[kNumLimbs];
static s32 sEyeIndex = 0;
static s32 sMouthIndex = 0;

// Skinned output and the display lists that draw it. Two sets so a frame being drawn is never
// rewritten while the next frame is built.
struct DrawBuffers {
    std::vector<Vtx> vtx;
    std::vector<std::vector<Gfx>> dls; // one per material
};
static std::vector<std::string> sMaterialTextures; // "__OTR__" + path, kept alive for the display lists
static DrawBuffers sBuffers[2];
static int sBufferIndex = 0;

static std::vector<float> sSkinnedPos; // 3 per model vertex
static std::vector<float> sSkinnedNrm;
static std::vector<float> sMorphedPos; // bind pose positions with blendshapes applied

static bool LoadModel() {
    auto archive = Ship::Context::GetRawInstance()->GetResourceManager()->GetArchiveManager();
    auto file = archive->LoadFile(kModelPath);
    if (file == nullptr || file->Buffer == nullptr) {
        SPDLOG_WARN("PlayerModel: {} not found, using the vanilla child model", kModelPath);
        return false;
    }
    std::string error;
    if (!ParseModel(file->Buffer->data(), file->Buffer->size(), sModel, error)) {
        SPDLOG_ERROR("PlayerModel: failed to read {}: {}", kModelPath, error);
        return false;
    }
    BuildChunks(sModel, kVertsPerLoad);
    sSkinnedPos.resize(sModel.verts.size() * 3);
    sSkinnedNrm.resize(sModel.verts.size() * 3);
    sMorphedPos.resize(sModel.verts.size() * 3);
    for (size_t i = 0; i < sModel.verts.size(); i++) {
        memcpy(&sMorphedPos[i * 3], sModel.verts[i].pos, sizeof(float) * 3);
    }
    SPDLOG_INFO("PlayerModel: loaded {} verts, {} tris, {} bones, {} chunks", sModel.verts.size(),
                sModel.tris.size() / 3, sModel.bones.size(), sModel.chunks.size());
    return true;
}

static bool EnsureLoaded() {
    if (sLoadState == 0) {
        sLoadState = LoadModel() ? 1 : -1;
    }
    return sLoadState == 1;
}

static Mat4 FromMtxF(const MtxF& mf) {
    Mat4 m;
    // MtxF is column-major: mf.mf[col][row].
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            m.m[c][r] = mf.mf[c][r];
        }
    }
    return m;
}

static void ToMtxF(const Mat4& m, MtxF* mf) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            mf->mf[c][r] = m.m[c][r];
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Held items. Child Link's hand display lists bake the hand and the held item into one list.
// The new model has its own hands, so the hand geometry is filtered out: every triangle drawn
// while the skin texture is bound is dropped and everything else (sword, shield, slingshot,
// ocarina, boomerang) is kept.

static bool sSkinTexKnown = false;
static uintptr_t sSkinTex[2];
static std::unordered_map<const void*, std::vector<Gfx>> sItemOnlyCache;

static bool IsExpanded(uint8_t op) {
    return op == G_SETTIMG_OTR_HASH || op == G_DL_OTR_HASH || op == G_VTX_OTR_HASH || op == G_BRANCH_Z_OTR ||
           op == G_MARKER || op == G_MTX_OTR || op == G_MOVEMEM_OTR;
}

static Gfx* ResolveDL(Gfx* dl) {
    if (dl != nullptr && ResourceMgr_OTRSigCheck((char*)dl)) {
        return ResourceMgr_LoadGfxByName((const char*)dl);
    }
    return dl;
}

static void FindSkinTexture() {
    sSkinTexKnown = true;
    Gfx* dl = ResolveDL((Gfx*)gLinkChildLeftFistNearDL);
    if (dl == nullptr) {
        return;
    }
    uintptr_t cur[2] = { 0, 0 };
    for (int i = 0; i < 4096; i++) {
        uint8_t op = (uint8_t)(dl[i].words.w0 >> 24);
        if (op == (uint8_t)G_ENDDL) {
            break;
        }
        if (op == G_SETTIMG_OTR_HASH) {
            cur[0] = dl[i + 1].words.w0;
            cur[1] = dl[i + 1].words.w1;
        } else if (op == (uint8_t)G_TRI1 || op == (uint8_t)G_TRI2 || op == (uint8_t)G_QUAD) {
            sSkinTex[0] = cur[0];
            sSkinTex[1] = cur[1];
            return;
        }
        if (IsExpanded(op)) {
            i++;
        }
    }
}

static Gfx* ItemOnlyDL(Gfx* vanilla) {
    if (vanilla == nullptr) {
        return nullptr;
    }
    auto it = sItemOnlyCache.find(vanilla);
    if (it != sItemOnlyCache.end()) {
        return it->second.empty() ? nullptr : it->second.data();
    }
    if (!sSkinTexKnown) {
        FindSkinTexture();
    }

    std::vector<Gfx> out;
    Gfx* dl = ResolveDL(vanilla);
    bool skin = false;
    bool keptTris = false;
    bool ok = false;
    uintptr_t cur[2] = { 0, 0 };
    for (int i = 0; dl != nullptr && i < 8192; i++) {
        uint8_t op = (uint8_t)(dl[i].words.w0 >> 24);
        if (op == G_SETTIMG_OTR_HASH) {
            cur[0] = dl[i + 1].words.w0;
            cur[1] = dl[i + 1].words.w1;
            skin = cur[0] == sSkinTex[0] && cur[1] == sSkinTex[1];
        } else if (op == (uint8_t)G_SETTIMG || op == G_SETTIMG_OTR_FILEPATH) {
            skin = false;
        } else if (op == (uint8_t)G_DL || op == G_DL_OTR_HASH || op == G_DL_OTR_FILEPATH) {
            // Nested lists are not filtered; keep them and assume they are not the hand.
            keptTris = true;
        }
        bool isTri = op == (uint8_t)G_TRI1 || op == (uint8_t)G_TRI2 || op == (uint8_t)G_QUAD || op == G_TRI1_OTR;
        if (isTri && skin) {
            continue;
        }
        keptTris |= isTri;
        out.push_back(dl[i]);
        if (IsExpanded(op)) {
            out.push_back(dl[++i]);
        }
        if (op == (uint8_t)G_ENDDL) {
            ok = true;
            break;
        }
    }
    if (!ok || !keptTris) {
        out.clear();
    }
    auto& stored = sItemOnlyCache[vanilla];
    stored = std::move(out);
    return stored.empty() ? nullptr : stored.data();
}

// ---------------------------------------------------------------------------------------------
// Skinning and draw

static void BuildDrawList(DrawBuffers& buf) {
    const Model& m = sModel;
    buf.vtx.resize(m.chunkVerts.size());
    buf.dls.assign(m.materials.size(), {});
    for (const Chunk& c : m.chunks) {
        std::vector<Gfx>& dl = buf.dls[c.material];
        Gfx g;
        gSPVertex(&g, &buf.vtx[c.firstVert], c.numVerts, 0);
        dl.push_back(g);
        size_t t = 0;
        for (; t + 1 < c.numTris; t += 2) {
            const uint8_t* a = &m.chunkTris[(c.firstTri + t) * 3];
            gSP2Triangles(&g, a[0], a[1], a[2], 0, a[3], a[4], a[5], 0);
            dl.push_back(g);
        }
        if (t < c.numTris) {
            const uint8_t* a = &m.chunkTris[(c.firstTri + t) * 3];
            gSP1Triangle(&g, a[0], a[1], a[2], 0);
            dl.push_back(g);
        }
    }
    for (auto& dl : buf.dls) {
        Gfx g;
        gSPEndDisplayList(&g);
        dl.push_back(g);
    }
}

// Blendshapes are sparse; only touched vertices are reset and re-accumulated each frame.
static void ApplyMorphs(const float* weights) {
    const Model& m = sModel;
    for (const Morph& morph : m.morphs) {
        for (uint32_t v : morph.verts) {
            memcpy(&sMorphedPos[v * 3], m.verts[v].pos, sizeof(float) * 3);
        }
    }
    for (size_t i = 0; i < m.morphs.size(); i++) {
        const float w = weights[i];
        if (w == 0.0f) {
            continue;
        }
        const Morph& morph = m.morphs[i];
        for (size_t k = 0; k < morph.verts.size(); k++) {
            float* p = &sMorphedPos[morph.verts[k] * 3];
            p[0] += w * morph.deltas[k * 3 + 0];
            p[1] += w * morph.deltas[k * 3 + 1];
            p[2] += w * morph.deltas[k * 3 + 2];
        }
    }
}

static void SkinVertices(const Mat4* skinMats) {
    const Model& m = sModel;
    const size_t n = m.verts.size();
    for (size_t i = 0; i < n; i++) {
        const Vertex& v = m.verts[i];
        const float* vp = &sMorphedPos[i * 3];
        float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0;
        for (int k = 0; k < 4; k++) {
            if (v.weight[k] == 0) {
                continue;
            }
            const float w = v.weight[k] * (1.0f / 255.0f);
            const Mat4& s = skinMats[v.bone[k]];
            px += w * (s.m[0][0] * vp[0] + s.m[1][0] * vp[1] + s.m[2][0] * vp[2] + s.m[3][0]);
            py += w * (s.m[0][1] * vp[0] + s.m[1][1] * vp[1] + s.m[2][1] * vp[2] + s.m[3][1]);
            pz += w * (s.m[0][2] * vp[0] + s.m[1][2] * vp[1] + s.m[2][2] * vp[2] + s.m[3][2]);
            nx += w * (s.m[0][0] * v.nrm[0] + s.m[1][0] * v.nrm[1] + s.m[2][0] * v.nrm[2]);
            ny += w * (s.m[0][1] * v.nrm[0] + s.m[1][1] * v.nrm[1] + s.m[2][1] * v.nrm[2]);
            nz += w * (s.m[0][2] * v.nrm[0] + s.m[1][2] * v.nrm[1] + s.m[2][2] * v.nrm[2]);
        }
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        float inv = len > 1e-6f ? 1.0f / len : 0.0f;
        sSkinnedPos[i * 3 + 0] = px;
        sSkinnedPos[i * 3 + 1] = py;
        sSkinnedPos[i * 3 + 2] = pz;
        sSkinnedNrm[i * 3 + 0] = nx * inv;
        sSkinnedNrm[i * 3 + 1] = ny * inv;
        sSkinnedNrm[i * 3 + 2] = nz * inv;
    }
}

static inline s16 ClampS16(float f) {
    f = std::round(f);
    return (s16)(f < -32768.0f ? -32768.0f : (f > 32767.0f ? 32767.0f : f));
}

static inline s8 ToS8(float f) {
    float r = std::round(f * 127.0f);
    return (s8)(r < -127.0f ? -127.0f : (r > 127.0f ? 127.0f : r));
}

static void FillVertices(DrawBuffers& buf) {
    const Model& m = sModel;
    for (size_t i = 0; i < m.chunkVerts.size(); i++) {
        const uint32_t src = m.chunkVerts[i];
        Vtx_tn& d = buf.vtx[i].n;
        d.ob[0] = ClampS16(sSkinnedPos[src * 3 + 0]);
        d.ob[1] = ClampS16(sSkinnedPos[src * 3 + 1]);
        d.ob[2] = ClampS16(sSkinnedPos[src * 3 + 2]);
        d.flag = 0;
        d.tc[0] = m.verts[src].tc[0];
        d.tc[1] = m.verts[src].tc[1];
        d.n[0] = ToS8(sSkinnedNrm[src * 3 + 0]);
        d.n[1] = ToS8(sSkinnedNrm[src * 3 + 1]);
        d.n[2] = ToS8(sSkinnedNrm[src * 3 + 2]);
        d.a = 255;
    }
}

static void Draw(PlayState* play, DrawBuffers& buf) {
    OPEN_DISPS(play->state.gfxCtx);

    Matrix_Push();
    MtxF base;
    ToMtxF(sBase, &base);
    Matrix_Put(&base);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    Matrix_Pop();

    SurfaceState_Apply(&POLY_OPA_DISP);

    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_2CYCLE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_FOG_SHADE_A, G_RM_AA_ZB_OPA_SURF2);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_MODULATEIDECALA, G_CC_PASS2);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_CULL_FRONT);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_CULL_BACK | G_FOG | G_LIGHTING | G_SHADING_SMOOTH);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, 255, 255, 255, 255);
    gSPTexture(POLY_OPA_DISP++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    if (sMaterialTextures.size() != sModel.materials.size()) {
        sMaterialTextures.clear();
        for (const Material& mat : sModel.materials) {
            sMaterialTextures.push_back("__OTR__" + mat.texture);
        }
    }
    for (size_t i = 0; i < buf.dls.size(); i++) {
        if (buf.dls[i].size() <= 1) {
            continue;
        }
        gDPPipeSync(POLY_OPA_DISP++);
        gDPLoadTextureBlock(POLY_OPA_DISP++, sMaterialTextures[i].c_str(), G_IM_FMT_RGBA, G_IM_SIZ_32b,
                            kNominalTexSize, kNominalTexSize, 0, G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR, 5,
                            5, kTexShift, kTexShift);
        gSPDisplayList(POLY_OPA_DISP++, buf.dls[i].data());
    }
    gDPPipeSync(POLY_OPA_DISP++);

    CLOSE_DISPS(play->state.gfxCtx);
}

} // namespace PlayerModel

using namespace PlayerModel;

extern "C" s32 PlayerModel_ShouldReplace(void** skeleton) {
    if (skeleton == nullptr || !CVarGetInteger(CVAR_PLAYER_MODEL_CHILD, 1)) {
        return false;
    }
    if (gSaveContext.linkAge != LINK_AGE_CHILD || Player_IsCustomLinkModel()) {
        return false;
    }
    return EnsureLoaded();
}

extern "C" Vec3s* PlayerModel_DebugJointTable(Vec3s* jointTable) {
    static Vec3s sDebugTable[kNumLimbs + 2];
    const char* anim = CVarGetString(CVAR_PLAYER_MODEL_DEBUG("Anim"), "");
    if (anim == nullptr || anim[0] == '\0') {
        return jointTable;
    }
    std::string path = std::string("__OTR__") + anim;
    LinkAnimationHeader* header = (LinkAnimationHeader*)ResourceMgr_LoadAnimByName(path.c_str());
    if (header == nullptr || header->segment == nullptr) {
        return jointTable;
    }
    const s32 frameCount = header->common.frameCount;
    s32 frame = CVarGetInteger(CVAR_PLAYER_MODEL_DEBUG("Frame"), 0);
    frame = frameCount > 0 ? ((frame % frameCount) + frameCount) % frameCount : 0;
    const size_t frameSize = sizeof(Vec3s) * (kNumLimbs + 1) + 2;
    memcpy(sDebugTable, (const char*)header->segment + frameSize * frame, frameSize);
    return sDebugTable;
}

static bool sDebugTurned = false;

extern "C" void PlayerModel_Begin(PlayState* play, void* player, s32 eyeIndex, s32 mouthIndex) {
    const s32 yaw = CVarGetInteger(CVAR_PLAYER_MODEL_DEBUG("Yaw"), 0);
    sDebugTurned = yaw != 0;
    if (sDebugTurned) {
        Matrix_Push();
        Matrix_RotateY(yaw * (M_PI / 180.0f), MTXMODE_APPLY);
    }
    MtxF base;
    Matrix_Get(&base);
    sBase = FromMtxF(base);
    sPlayer = (Player*)player;
    sEyeIndex = eyeIndex;
    sMouthIndex = mouthIndex;
    memset(sLimbSeen, 0, sizeof(sLimbSeen));
    sActive = true;
}

extern "C" void PlayerModel_OverrideLimb(s32 limbIndex, Gfx** dList, Vec3f* pos) {
    if (!sActive) {
        return;
    }
    const int limb = limbIndex - 1;
    if (limb < 0 || limb >= kNumLimbs) {
        return;
    }
    if (limb == 0) {
        pos->y *= sModel.rootYScale;
    } else {
        const Bone& b = sModel.bones[limb];
        pos->x = b.jointPos[0];
        pos->y = b.jointPos[1];
        pos->z = b.jointPos[2];
    }

    switch (limb) {
        case kLimbLeftHand:
        case kLimbRightHand:
            *dList = ItemOnlyDL(*dList);
            break;
        case kLimbSheath:
            break; // sword sheath and shield on the back stay vanilla
        default:
            *dList = nullptr;
            break;
    }
}

extern "C" void PlayerModel_PostLimb(s32 limbIndex) {
    if (!sActive) {
        return;
    }
    const int limb = limbIndex - 1;
    if (limb < 0 || limb >= kNumLimbs) {
        return;
    }
    MtxF mf;
    Matrix_Get(&mf);
    sLimbWorld[limb] = FromMtxF(mf);
    sLimbSeen[limb] = true;
}

extern "C" void PlayerModel_End(PlayState* play) {
    if (!sActive) {
        return;
    }
    sActive = false;
    if (sDebugTurned) {
        Matrix_Pop();
        sDebugTurned = false;
    }
    for (int i = 0; i < kNumLimbs; i++) {
        if (!sLimbSeen[i]) {
            return;
        }
    }

    const size_t numBones = sModel.bones.size();
    static std::vector<Mat4> pose;
    static std::vector<Mat4> skinMats;
    pose.resize(numBones);
    skinMats.resize(numBones);

    // Limb transforms relative to the actor so skinned positions stay in model units.
    const Mat4 invBase = Inverse(sBase);
    for (int i = 0; i < kNumLimbs; i++) {
        pose[i] = Mul(invBase, sLimbWorld[i]);
    }
    // Extra bones follow their parent rigidly, then cloth and face move them.
    for (size_t i = kNumLimbs; i < numBones; i++) {
        const Bone& b = sModel.bones[i];
        pose[i] = Mul(pose[b.parent], Mul(sModel.bones[b.parent].invBind, b.bind));
    }
    if (sPlayer != nullptr) {
        static s32 sLastSurfaceFrame = -1;
        if (play->gameplayFrames != sLastSurfaceFrame) {
            sLastSurfaceFrame = play->gameplayFrames;
            const float modelScale =
                std::sqrt(sBase.m[0][0] * sBase.m[0][0] + sBase.m[0][1] * sBase.m[0][1] + sBase.m[0][2] * sBase.m[0][2]);
            SurfaceState_Update(play, sPlayer, modelScale);
        }
        if (CVarGetInteger(CVAR_PLAYER_MODEL_CLOTH, 1)) {
            ClothSim_Update(play, sModel, sBase, pose.data(), SurfaceState_ClothWetness());
        }
    }
    static std::vector<float> morphWeights;
    morphWeights.assign(sModel.morphs.size(), 0.0f);
    if (CVarGetInteger(CVAR_PLAYER_MODEL_FACE, 1)) {
        FaceRig_Update(play, sPlayer, sModel, sEyeIndex, sMouthIndex, sBase, pose.data(), morphWeights.data());
    }
    ApplyMorphs(morphWeights.data());
    for (size_t i = 0; i < numBones; i++) {
        skinMats[i] = Mul(pose[i], sModel.bones[i].invBind);
    }

    SkinVertices(skinMats.data());

    DrawBuffers& buf = sBuffers[sBufferIndex];
    sBufferIndex ^= 1;
    if (buf.dls.empty()) {
        BuildDrawList(buf);
    }
    FillVertices(buf);
    Draw(play, buf);
}

extern "C" void PlayerModel_RefreshMaterialMap(const char* path) {
    // The renderer caches material maps; ask it to re-upload the one SurfaceState just rewrote.
    GfxRefreshMaterialMap(path);
}
