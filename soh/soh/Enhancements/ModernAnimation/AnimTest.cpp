// Headless animation tests for the modern animation features.
//
// Set SOH_ANIM_TEST to a JSON file and the game boots straight into each listed clip, plays it once per
// variant (a set of CVars) with the same scripted controller input and random seed, and saves every frame
// plus per-frame pose statistics, so animations can be compared on and off without anyone playing.
// See docs/modern-animation/TESTING.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include "ModernAnimation.h"
#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "z64.h"
void Sram_InitDebugSave(void);
extern PlayState* gPlayState;
}

namespace {

struct InputSpan {
    int from = 0;
    int to = 0;
    s8 stickX = 0;
    s8 stickY = 0;
    u16 buttons = 0;
};

struct SpawnSpec {
    s16 id = 0;
    s16 params = 0;
    Vec3f offset = {}; // relative to Link: x right, y up, z forward
};

struct Clip {
    std::string name;
    int32_t entrance = 0;
    int32_t age = LINK_AGE_CHILD;
    int32_t dayTime = 0x8000;
    bool hasSpawn = false;
    int8_t room = 0;
    Vec3f pos = {};
    int16_t yaw = 0;
    int frames = 60;
    int captureEvery = 1;
    std::vector<InputSpan> inputs;
    std::vector<SpawnSpec> spawns; // spawned when recording starts, facing Link
};

struct Variant {
    std::string name;
    nlohmann::json cvars;
};

enum class Phase { WaitTitle, Loading, Playing, Finishing, Done };

struct AnimTest {
    std::string outputDir;
    int warmupTicks = 60;
    uint32_t seed = 0x5EED;
    std::vector<Clip> clips;
    std::vector<Variant> variants;
    nlohmann::json manifest = nlohmann::json::array();
    nlohmann::json stats = nlohmann::json::array();

    Phase phase = Phase::WaitTitle;
    size_t clipIndex = 0;
    size_t variantIndex = 0;
    int ticks = 0;
    // Each clip is loaded once without recording first, so every recorded variant enters the scene in the
    // same state (first visits play intro cutscenes and set flags)
    bool priming = true;
    // Play_Init seeds the random generator from the clock; reseed before the scene's first actor initialises
    bool reseedOnActorInit = false;
};

AnimTest sTest;

u16 ParseButtons(const nlohmann::json& list) {
    u16 buttons = 0;
    for (const auto& b : list) {
        std::string name = b.get<std::string>();
        if (name == "A")
            buttons |= BTN_A;
        else if (name == "B")
            buttons |= BTN_B;
        else if (name == "Z")
            buttons |= BTN_Z;
        else if (name == "R")
            buttons |= BTN_R;
        else if (name == "L")
            buttons |= BTN_L;
        else if (name == "CLEFT")
            buttons |= BTN_CLEFT;
        else if (name == "CDOWN")
            buttons |= BTN_CDOWN;
        else if (name == "CRIGHT")
            buttons |= BTN_CRIGHT;
    }
    return buttons;
}

void ApplyCVars(const nlohmann::json& cvars) {
    for (auto it = cvars.begin(); it != cvars.end(); ++it) {
        if (it->is_number_float()) {
            CVarSetFloat(it.key().c_str(), it->get<float>());
        } else if (it->is_number_integer() || it->is_boolean()) {
            CVarSetInteger(it.key().c_str(), it->is_boolean() ? (int32_t)it->get<bool>() : it->get<int32_t>());
        } else if (it->is_string()) {
            CVarSetString(it.key().c_str(), it->get<std::string>().c_str());
        }
    }
}

void RequestCapture(const std::string& path) {
    auto window = std::dynamic_pointer_cast<Fast::Fast3dWindow>(Ship::Context::GetRawInstance()->GetWindow());
    auto interpreter = window ? window->GetInterpreterWeak().lock() : nullptr;
    if (interpreter) {
        interpreter->RequestFrameCapture(path);
    }
}

void SetSpawn(const Clip& clip) {
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = clip.entrance;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = clip.room;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = clip.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = clip.yaw;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0xDFF;
    gSaveContext.respawnFlag = clip.hasSpawn ? 1 : 0;
}

void SetTimeAndAge(const Clip& clip) {
    gSaveContext.linkAge = clip.age;
    gSaveContext.skyboxTime = gSaveContext.dayTime = clip.dayTime;
    gSaveContext.nightFlag = (clip.dayTime < 0x4555 || clip.dayTime >= 0xC001) ? 1 : 0;
    gSaveContext.showTitleCard = false;
    // Every variant starts with full health (an earlier variant's fight would otherwise carry over)
    gSaveContext.health = gSaveContext.healthCapacity;
}

void StartClip(GameState* state, const Clip& clip, const Variant& variant) {
    ApplyCVars(variant.cvars);
    Rand_Seed(sTest.seed);
    sTest.reseedOnActorInit = true;
    SetTimeAndAge(clip);
    SetSpawn(clip);
    if (state != nullptr) {
        // First clip: boot from the title screen
        gSaveContext.gameMode = GAMEMODE_NORMAL;
        gSaveContext.fileNum = 0xFE;
        Sram_InitDebugSave();
        gSaveContext.fileNum = 0xFF;
        SetTimeAndAge(clip);
        gSaveContext.sceneLayer = 0;
        gSaveContext.cutsceneIndex = 0;
        for (int i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
            gSaveContext.buttonStatus[i] = BTN_ENABLED;
        }
        gSaveContext.nextHudVisibilityMode = gSaveContext.hudVisibilityMode = gSaveContext.hudVisibilityModeTimer = 0;
        gSaveContext.entranceIndex = clip.entrance;
        gSaveContext.seqId = (u8)NA_BGM_DISABLED;
        gSaveContext.natureAmbienceId = 0xFF;
        gWeatherMode = 0;
        state->running = false;
        SET_NEXT_GAMESTATE(state, Play_Init, PlayState);
        return;
    }
    gPlayState->nextEntranceIndex = clip.entrance;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
}

void Finish() {
    std::ofstream out(sTest.outputDir + "/manifest.json");
    nlohmann::json root = { { "captures", sTest.manifest }, { "stats", sTest.stats } };
    out << root.dump(1);
    out.close();
    SPDLOG_INFO("Animation test finished: {} captures in {}", sTest.manifest.size(), sTest.outputDir);
    fflush(nullptr);
    std::_Exit(0);
}

// Id of the actor whose instance contains this skeleton, or -1
int ActorIdOf(void* skel) {
    if (skel == nullptr) {
        return -1;
    }
    // The closest actor at or below the skeleton's address
    Actor* best = nullptr;
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if ((uintptr_t)a <= (uintptr_t)skel && (best == nullptr || (uintptr_t)a > (uintptr_t)best)) {
                best = a;
            }
        }
    }
    return best != nullptr ? best->id : -1;
}

// Every sampled skeleton as [actor id, nth skeleton of that actor id this frame, max acceleration,
// changed animation in the last two frames]. Runs are
// deterministic, so the same entries line up between variants.
nlohmann::json SkeletonSamples() {
    const ModernAnimSkelSample* samples;
    s32 count = ModernAnim_GetFrameSamples(&samples);
    std::vector<std::tuple<int, float, int>> rows;
    for (s32 i = 0; i < count; i++) {
        rows.push_back({ ActorIdOf(samples[i].skelAnime), samples[i].maxAccel, samples[i].inTransition });
    }
    std::stable_sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });
    nlohmann::json out = nlohmann::json::array();
    int prevId = -2, nth = 0;
    for (auto& [id, accel, transition] : rows) {
        nth = id == prevId ? nth + 1 : 0;
        prevId = id;
        out.push_back({ id, nth, std::round(accel * 10.0f) / 10.0f, transition });
    }
    return out;
}

// Hash of the gameplay state of every actor and every collider registered this frame. With modern animation
// only changing what is drawn, it must match the original frame for frame.
struct WorldHash {
    u64 value = 1469598103934665603ull;
    void Add(const void* data, size_t size) {
        const u8* bytes = (const u8*)data;
        for (size_t i = 0; i < size; i++) {
            value = (value ^ bytes[i]) * 1099511628211ull;
        }
    }
    template <typename T> void Add(const T& v) {
        Add(&v, sizeof(T));
    }
};

bool IsLiveCollider(Collider* collider) {
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            uintptr_t begin = (uintptr_t)a;
            uintptr_t end = begin + ActorDB::Instance->RetrieveEntry(a->id).entry.instanceSize;
            if ((uintptr_t)collider >= begin && (uintptr_t)collider < end) {
                return true;
            }
        }
    }
    return false;
}

void HashCollider(WorldHash& hash, Collider* collider) {
    if (!IsLiveCollider(collider)) {
        hash.Add(-1);
        return;
    }
    hash.Add(collider->shape);
    hash.Add(collider->atFlags);
    hash.Add(collider->acFlags);
    hash.Add(collider->ocFlags1);
    switch (collider->shape) {
        case COLSHAPE_JNTSPH: {
            ColliderJntSph* jntSph = (ColliderJntSph*)collider;
            for (s32 i = 0; i < jntSph->count; i++) {
                hash.Add(jntSph->elements[i].dim.worldSphere);
            }
            break;
        }
        case COLSHAPE_CYLINDER:
            hash.Add(((ColliderCylinder*)collider)->dim);
            break;
        case COLSHAPE_TRIS: {
            ColliderTris* tris = (ColliderTris*)collider;
            for (s32 i = 0; i < tris->count; i++) {
                hash.Add(tris->elements[i].dim.vtx);
            }
            break;
        }
        case COLSHAPE_QUAD:
            hash.Add(((ColliderQuad*)collider)->dim.quad);
            break;
    }
}

nlohmann::json WorldState() {
    WorldHash actors, colliders;
    s32 actorCount = 0;
    // SOH_ANIM_TEST_DETAIL=1 also lists each actor's field hashes, to find what differs
    static const bool detail = getenv("SOH_ANIM_TEST_DETAIL") != nullptr;
    nlohmann::json detailRows = nlohmann::json::array();
    for (int cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = gPlayState->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (detail) {
                WorldHash parts[5];
                parts[0].Add(a->world.pos);
                parts[0].Add(a->world.rot);
                parts[1].Add(a->shape.rot);
                parts[2].Add(a->velocity);
                parts[2].Add(a->speedXZ);
                parts[3].Add(a->focus.pos);
                parts[4].Add(a->flags);
                parts[4].Add(a->colChkInfo.health);
                nlohmann::json row = { a->id };
                for (auto& part : parts) {
                    row.push_back(part.value & 0xFFFF);
                }
                detailRows.push_back(row);
            }
            actors.Add(a->id);
            actors.Add(a->params);
            actors.Add(a->flags);
            actors.Add(a->world.pos);
            actors.Add(a->world.rot);
            actors.Add(a->shape.rot);
            actors.Add(a->velocity);
            actors.Add(a->speedXZ);
            actors.Add(a->colChkInfo.health);
            actors.Add(a->focus.pos);
            actorCount++;
        }
    }
    Player* player = GET_PLAYER(gPlayState);
    actors.Add(player->stateFlags1);
    actors.Add(player->stateFlags2);
    actors.Add(player->meleeWeaponInfo);
    actors.Add(gSaveContext.health);
    CollisionCheckContext* col = &gPlayState->colChkCtx;
    for (s32 i = 0; i < col->colATCount; i++) {
        HashCollider(colliders, col->colAT[i]);
    }
    for (s32 i = 0; i < col->colACCount; i++) {
        HashCollider(colliders, col->colAC[i]);
    }
    for (s32 i = 0; i < col->colOCCount; i++) {
        HashCollider(colliders, col->colOC[i]);
    }
    char actorHex[17], colliderHex[17];
    snprintf(actorHex, sizeof(actorHex), "%016llx", (unsigned long long)actors.value);
    snprintf(colliderHex, sizeof(colliderHex), "%016llx", (unsigned long long)colliders.value);
    nlohmann::json out = { { "actors", actorCount },
                           { "actorHash", actorHex },
                           { "colliders", col->colATCount + col->colACCount + col->colOCCount },
                           { "colliderHash", colliderHex } };
    if (detail) {
        out["detail"] = detailRows;
    }
    return out;
}

// Replaces controller 1's input for this frame with the clip's script (neutral outside any span)
void InjectInput() {
    if (sTest.phase != Phase::Loading && sTest.phase != Phase::Playing) {
        return;
    }
    if (gPlayState == nullptr) {
        return;
    }
    const Clip& clip = sTest.clips[sTest.clipIndex];
    Input* input = &gPlayState->state.input[0];
    OSContPad pad = {};
    if (sTest.phase == Phase::Playing) {
        for (const auto& span : clip.inputs) {
            if (sTest.ticks >= span.from && sTest.ticks < span.to) {
                pad.button |= span.buttons;
                pad.stick_x = span.stickX;
                pad.stick_y = span.stickY;
            }
        }
    }
    OSContPad prev = input->cur;
    input->prev = prev;
    input->cur = pad;
    u16 diff = prev.button ^ pad.button;
    input->press.button = pad.button & diff;
    input->rel.button = prev.button & diff;
    PadUtils_UpdateRelXY(input);
    input->press.stick_x = (s8)(pad.stick_x - prev.stick_x);
    input->press.stick_y = (s8)(pad.stick_y - prev.stick_y);
}

void OnFrame() {
    if (sTest.phase == Phase::Finishing) {
        // One more frame so the last requested capture is written
        sTest.phase = Phase::Done;
        Finish();
        return;
    }
    if (sTest.phase == Phase::WaitTitle || sTest.phase == Phase::Done || gPlayState == nullptr) {
        return;
    }
    const Clip& clip = sTest.clips[sTest.clipIndex];
    const Variant& variant = sTest.variants[sTest.variantIndex];

    if (sTest.phase == Phase::Loading) {
        if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF) {
            sTest.ticks = 0;
            return;
        }
        if (++sTest.ticks < sTest.warmupTicks) {
            return;
        }
        if (sTest.priming) {
            sTest.priming = false;
            sTest.ticks = 0;
            StartClip(nullptr, clip, variant);
            return;
        }
        // Same random state for every variant from here on, so NPCs make the same choices
        Rand_Seed(sTest.seed);
        Player* player = GET_PLAYER(gPlayState);
        s16 yaw = player->actor.shape.rot.y;
        for (const auto& spawn : clip.spawns) {
            f32 sin = Math_SinS(yaw), cos = Math_CosS(yaw);
            Vec3f pos = player->actor.world.pos;
            pos.x += spawn.offset.z * sin - spawn.offset.x * cos;
            pos.z += spawn.offset.z * cos + spawn.offset.x * sin;
            pos.y += spawn.offset.y;
            Actor_Spawn(&gPlayState->actorCtx, gPlayState, spawn.id, pos.x, pos.y, pos.z, 0, yaw + 0x8000, 0,
                        spawn.params);
        }
        sTest.phase = Phase::Playing;
        sTest.ticks = 0;
        return;
    }

    // Phase::Playing: this frame's update and draw have run with input for tick `ticks`
    Player* player = GET_PLAYER(gPlayState);
    sTest.stats.push_back(
        { { "clip", clip.name },
          { "variant", variant.name },
          { "frame", sTest.ticks },
          { "skels", SkeletonSamples() },
          { "world", WorldState() },
          { "playerPos", { player->actor.world.pos.x, player->actor.world.pos.y, player->actor.world.pos.z } } });
    if (sTest.ticks % clip.captureEvery == 0) {
        char file[256];
        snprintf(file, sizeof(file), "%s__%s__%03d.ppm", clip.name.c_str(), variant.name.c_str(), sTest.ticks);
        RequestCapture(sTest.outputDir + "/" + file);
        sTest.manifest.push_back(
            { { "clip", clip.name }, { "variant", variant.name }, { "frame", sTest.ticks }, { "file", file } });
    }
    if (++sTest.ticks < clip.frames) {
        return;
    }

    // Next variant of this clip, or the next clip
    sTest.ticks = 0;
    if (++sTest.variantIndex >= sTest.variants.size()) {
        sTest.variantIndex = 0;
        if (++sTest.clipIndex >= sTest.clips.size()) {
            sTest.phase = Phase::Finishing;
            return;
        }
        sTest.priming = true;
    }
    sTest.phase = Phase::Loading;
    StartClip(nullptr, sTest.clips[sTest.clipIndex], sTest.variants[sTest.variantIndex]);
}

bool LoadTest(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        SPDLOG_ERROR("Animation test: cannot open {}", path);
        return false;
    }
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded()) {
        SPDLOG_ERROR("Animation test: {} is not valid JSON", path);
        return false;
    }
    sTest.outputDir = json.value("output", std::string("anim-test-output"));
    sTest.warmupTicks = json.value("warmupTicks", 60);
    sTest.seed = json.value("seed", 0x5EED);
    for (const auto& c : json["clips"]) {
        Clip clip;
        clip.name = c.value("name", std::string("clip"));
        clip.entrance = c.value("entrance", 0);
        clip.age = c.value("age", std::string("child")) == "adult" ? LINK_AGE_ADULT : LINK_AGE_CHILD;
        clip.dayTime = c.value("dayTime", 0x8000);
        clip.frames = c.value("frames", 60);
        clip.captureEvery = std::max(1, c.value("captureEvery", 1));
        if (c.contains("pos")) {
            clip.hasSpawn = true;
            clip.pos = { c["pos"][0].get<float>(), c["pos"][1].get<float>(), c["pos"][2].get<float>() };
            clip.yaw = (int16_t)c.value("yaw", 0);
            clip.room = (int8_t)c.value("room", 0);
        }
        for (const auto& i : c.value("inputs", nlohmann::json::array())) {
            InputSpan span;
            span.from = i.value("from", 0);
            span.to = i.value("to", span.from + 1);
            if (i.contains("stick")) {
                span.stickX = (s8)i["stick"][0].get<int>();
                span.stickY = (s8)i["stick"][1].get<int>();
            }
            span.buttons = ParseButtons(i.value("buttons", nlohmann::json::array()));
            clip.inputs.push_back(span);
        }
        for (const auto& sp : c.value("spawns", nlohmann::json::array())) {
            SpawnSpec spawn;
            spawn.id = (s16)sp.value("id", 0);
            spawn.params = (s16)sp.value("params", 0);
            if (sp.contains("offset")) {
                spawn.offset = { sp["offset"][0].get<float>(), sp["offset"][1].get<float>(),
                                 sp["offset"][2].get<float>() };
            }
            clip.spawns.push_back(spawn);
        }
        sTest.clips.push_back(clip);
    }
    for (const auto& v : json["variants"]) {
        sTest.variants.push_back(
            { v.value("name", std::string("variant")), v.value("cvars", nlohmann::json::object()) });
    }
    if (sTest.clips.empty() || sTest.variants.empty()) {
        SPDLOG_ERROR("Animation test: {} needs at least one clip and one variant", path);
        return false;
    }
    if (json.contains("cvars")) {
        ApplyCVars(json["cvars"]);
    }
    return true;
}

void RegisterAnimTest() {
    const char* path = getenv("SOH_ANIM_TEST");
    if (path == nullptr || !LoadTest(path)) {
        return;
    }
    SPDLOG_INFO("Animation test: {} clips x {} variants", sTest.clips.size(), sTest.variants.size());
    ModernAnim_SetStatsEnabled(true);

    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnZTitleUpdate>([](void* gameState) {
        if (sTest.phase != Phase::WaitTitle) {
            return;
        }
        sTest.phase = Phase::Loading;
        sTest.ticks = 0;
        StartClip((GameState*)gameState, sTest.clips[0], sTest.variants[0]);
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameStateMainStart>(InjectInput);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
    GameInteractor::Instance->RegisterGameHook<GameInteractor::ShouldActorInit>([](void* actor, bool* result) {
        if (sTest.reseedOnActorInit) {
            sTest.reseedOnActorInit = false;
            Rand_Seed(sTest.seed);
        }
    });
}

} // namespace

static RegisterShipInitFunc initFunc(RegisterAnimTest);
