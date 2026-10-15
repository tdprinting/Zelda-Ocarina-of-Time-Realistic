// Automated render tests for the modern rendering features.
//
// Set SOH_RENDER_TEST to a JSON file and the game boots straight into each listed scene, freezes the
// world, and captures one frame per variant (a set of CVars), so features can be compared on and off
// without anyone playing. See docs/modern-rendering/TESTING.md.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
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

struct TestScene {
    std::string name;
    int32_t entrance = 0;
    int32_t age = LINK_AGE_CHILD;
    int32_t dayTime = 0x8000;
    bool hasSpawn = false;
    int8_t room = 0;
    Vec3f pos = {};
    int16_t yaw = 0;
};

struct TestVariant {
    std::string name;
    nlohmann::json cvars;
};

enum class Phase { WaitTitle, Loading, Capturing, Done };

struct RenderTest {
    std::string outputDir;
    int warmupTicks = 80;
    int settleTicks = 4;
    std::vector<TestScene> scenes;
    std::vector<TestVariant> variants;
    nlohmann::json manifest = nlohmann::json::array();

    Phase phase = Phase::WaitTitle;
    size_t sceneIndex = 0;
    size_t variantIndex = 0;
    int ticks = 0;
    bool captureRequested = false;
};

RenderTest sTest;

void SetSpawn(const TestScene& scene) {
    gSaveContext.respawn[RESPAWN_MODE_DOWN].entranceIndex = scene.entrance;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].roomIndex = scene.room;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].pos = scene.pos;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].yaw = scene.yaw;
    gSaveContext.respawn[RESPAWN_MODE_DOWN].playerParams = 0xDFF;
    gSaveContext.respawnFlag = scene.hasSpawn ? 1 : 0;
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

void BootIntoScene(GameState* state, const TestScene& scene) {
    gSaveContext.gameMode = GAMEMODE_NORMAL;
    gSaveContext.fileNum = 0xFE;
    Sram_InitDebugSave();
    gSaveContext.fileNum = 0xFF;
    gSaveContext.sceneLayer = 0;
    gSaveContext.cutsceneIndex = 0;
    gSaveContext.linkAge = scene.age;
    gSaveContext.skyboxTime = gSaveContext.dayTime = scene.dayTime;
    gSaveContext.nightFlag = (scene.dayTime < 0x4555 || scene.dayTime >= 0xC001) ? 1 : 0;
    for (int i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
        gSaveContext.buttonStatus[i] = BTN_ENABLED;
    }
    gSaveContext.nextHudVisibilityMode = gSaveContext.hudVisibilityMode = gSaveContext.hudVisibilityModeTimer = 0;
    gSaveContext.entranceIndex = scene.entrance;
    gSaveContext.seqId = (u8)NA_BGM_DISABLED;
    gSaveContext.natureAmbienceId = 0xFF;
    gSaveContext.showTitleCard = false;
    gWeatherMode = 0;
    SetSpawn(scene);
    state->running = false;
    SET_NEXT_GAMESTATE(state, Play_Init, PlayState);
}

void WarpToScene(const TestScene& scene) {
    gPlayState->frameAdvCtx.enabled = false;
    gSaveContext.linkAge = scene.age;
    gSaveContext.skyboxTime = gSaveContext.dayTime = scene.dayTime;
    gSaveContext.nightFlag = (scene.dayTime < 0x4555 || scene.dayTime >= 0xC001) ? 1 : 0;
    gSaveContext.showTitleCard = false;
    SetSpawn(scene);
    gPlayState->nextEntranceIndex = scene.entrance;
    gPlayState->transitionTrigger = TRANS_TRIGGER_START;
    gPlayState->transitionType = TRANS_TYPE_INSTANT;
}

void Finish() {
    std::ofstream out(sTest.outputDir + "/manifest.json");
    out << sTest.manifest.dump(2);
    out.close();
    SPDLOG_INFO("Render test finished: {} captures in {}", sTest.manifest.size(), sTest.outputDir);
    fflush(nullptr);
    // Skip static destructors and thread joins; the test only needs the files on disk.
    std::_Exit(0);
}

void OnFrame() {
    if (sTest.phase == Phase::WaitTitle || sTest.phase == Phase::Done || gPlayState == nullptr) {
        return;
    }
    const TestScene& scene = sTest.scenes[sTest.sceneIndex];

    if (sTest.phase == Phase::Loading) {
        // Count only ticks spent in a settled scene (no transition running)
        if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF) {
            sTest.ticks = 0;
            return;
        }
        if (++sTest.ticks < sTest.warmupTicks) {
            return;
        }
        // Freeze actors, camera and animated textures so every variant renders the same frame
        gPlayState->frameAdvCtx.enabled = true;
        sTest.phase = Phase::Capturing;
        sTest.variantIndex = 0;
        sTest.ticks = 0;
        sTest.captureRequested = false;
        ApplyCVars(sTest.variants[0].cvars);
        return;
    }

    // Phase::Capturing
    sTest.ticks++;
    const TestVariant& variant = sTest.variants[sTest.variantIndex];
    if (!sTest.captureRequested && sTest.ticks >= sTest.settleTicks) {
        std::string file = scene.name + "__" + variant.name + ".ppm";
        RequestCapture(sTest.outputDir + "/" + file);
        sTest.manifest.push_back({ { "scene", scene.name }, { "variant", variant.name }, { "file", file } });
        sTest.captureRequested = true;
        sTest.ticks = 0;
        return;
    }
    if (!sTest.captureRequested || sTest.ticks < sTest.settleTicks) {
        return;
    }

    sTest.captureRequested = false;
    sTest.ticks = 0;
    if (++sTest.variantIndex < sTest.variants.size()) {
        ApplyCVars(sTest.variants[sTest.variantIndex].cvars);
        return;
    }

    if (++sTest.sceneIndex >= sTest.scenes.size()) {
        sTest.phase = Phase::Done;
        Finish();
        return;
    }
    sTest.phase = Phase::Loading;
    WarpToScene(sTest.scenes[sTest.sceneIndex]);
}

bool LoadTest(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        SPDLOG_ERROR("Render test: cannot open {}", path);
        return false;
    }
    nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded()) {
        SPDLOG_ERROR("Render test: {} is not valid JSON", path);
        return false;
    }
    sTest.outputDir = json.value("output", std::string("render-test-output"));
    sTest.warmupTicks = json.value("warmupTicks", 80);
    sTest.settleTicks = json.value("settleTicks", 4);
    for (const auto& s : json["scenes"]) {
        TestScene scene;
        scene.name = s.value("name", std::string("scene"));
        scene.entrance = s.value("entrance", 0);
        scene.age = s.value("age", std::string("child")) == "adult" ? LINK_AGE_ADULT : LINK_AGE_CHILD;
        scene.dayTime = s.value("dayTime", 0x8000);
        if (s.contains("pos")) {
            scene.hasSpawn = true;
            scene.pos = { s["pos"][0].get<float>(), s["pos"][1].get<float>(), s["pos"][2].get<float>() };
            scene.yaw = (int16_t)s.value("yaw", 0);
            scene.room = (int8_t)s.value("room", 0);
        }
        sTest.scenes.push_back(scene);
    }
    for (const auto& v : json["variants"]) {
        sTest.variants.push_back(
            { v.value("name", std::string("variant")), v.value("cvars", nlohmann::json::object()) });
    }
    if (sTest.scenes.empty() || sTest.variants.empty()) {
        SPDLOG_ERROR("Render test: {} needs at least one scene and one variant", path);
        return false;
    }
    if (json.contains("cvars")) {
        ApplyCVars(json["cvars"]);
    }
    return true;
}

void RegisterRenderTest() {
    const char* path = getenv("SOH_RENDER_TEST");
    if (path == nullptr || !LoadTest(path)) {
        return;
    }
    SPDLOG_INFO("Render test: {} scenes x {} variants", sTest.scenes.size(), sTest.variants.size());

    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnZTitleUpdate>([](void* gameState) {
        if (sTest.phase != Phase::WaitTitle) {
            return;
        }
        sTest.phase = Phase::Loading;
        sTest.ticks = 0;
        BootIntoScene((GameState*)gameState, sTest.scenes[0]);
    });
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnGameFrameUpdate>(OnFrame);
}

} // namespace

static RegisterShipInitFunc initFunc(RegisterRenderTest);
