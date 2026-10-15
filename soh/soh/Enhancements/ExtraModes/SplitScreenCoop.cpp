#include "SplitScreenCoop.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <unordered_set>

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

#define CVAR_COOP_NAME CVAR_ENHANCEMENT("SplitScreenCoop.Enabled")
#define CVAR_COOP_VALUE CVarGetInteger(CVAR_COOP_NAME, 0)
// Player 2 is pulled back to Player 1 when further away than this (0 disables the leash).
#define CVAR_COOP_LEASH CVarGetInteger(CVAR_ENHANCEMENT("SplitScreenCoop.LeashDistance"), 2000)

// 0: side by side, 1: top and bottom
#define CVAR_COOP_LAYOUT CVarGetInteger(CVAR_ENHANCEMENT("SplitScreenCoop.Layout"), 0)

// Controller port that drives Player 2 (index into PlayState::state.input).
#define COOP_P2_PORT 1

static s32 sCoopActorId = -1;
static Player* sP2 = nullptr;
static void* sP2GiAlloc = nullptr;
static Camera sP2Camera;
static bool sP2CameraReady = false;
// Set while Player 2 was frozen for a cutscene or transition, so it is brought back next to Player 1.
static bool sP2Suspended = false;
// Player 2's camera output from the last camera update, used for the split-screen pass.
static Vec3f sP2ViewEye;
static Vec3f sP2ViewAt;
static Vec3f sP2ViewUp;
static f32 sP2ViewFovy;
static bool sP2ViewValid = false;

// #region Actor list helpers

static void ActorList_Unlink(ActorListEntry* list, Actor* actor) {
    if (actor->prev != nullptr) {
        actor->prev->next = actor->next;
    } else {
        list->head = actor->next;
    }
    if (actor->next != nullptr) {
        actor->next->prev = actor->prev;
    }
    actor->prev = actor->next = nullptr;
}

static void ActorList_PushHead(ActorListEntry* list, Actor* actor) {
    actor->prev = nullptr;
    actor->next = list->head;
    if (list->head != nullptr) {
        list->head->prev = actor;
    }
    list->head = actor;
}

static void ActorList_PushTail(ActorListEntry* list, Actor* actor) {
    Actor* tail = list->head;

    actor->next = nullptr;
    if (tail == nullptr) {
        actor->prev = nullptr;
        list->head = actor;
        return;
    }
    while (tail->next != nullptr) {
        tail = tail->next;
    }
    tail->next = actor;
    actor->prev = tail;
}

// #endregion

// #region Player context swap

// While Player 2 updates, draws or moves its camera, the game must believe Player 2 is "the player":
// GET_PLAYER() returns the head of ACTORCAT_PLAYER, gameplay reads controller port 0, and player code
// drives CAM_ID_MAIN. Everything Player 2 must not change for Player 1 is snapshotted and restored.
struct CoopSwap {
    PlayState* play;
    bool swapInput;
    bool swapCamera;
};

// The camera code tells the main camera apart by address (&play->mainCamera) as well as by index, so
// Player 2's camera trades places with Player 1's by contents rather than by pointer.
static void Coop_SwapMainCamera(PlayState* play) {
    static Camera tmp;
    tmp = play->mainCamera;
    play->mainCamera = sP2Camera;
    sP2Camera = tmp;
}

static CoopSwap Coop_BeginSwap(PlayState* play, bool swapInput, bool swapCamera) {
    CoopSwap swap = { play, swapInput, swapCamera };
    ActorListEntry* list = &play->actorCtx.actorLists[ACTORCAT_PLAYER];

    ActorList_Unlink(list, &sP2->actor);
    ActorList_PushHead(list, &sP2->actor);

    if (swap.swapInput) {
        Input tmp = play->state.input[0];
        play->state.input[0] = play->state.input[COOP_P2_PORT];
        play->state.input[COOP_P2_PORT] = tmp;
    }
    if (swap.swapCamera) {
        Coop_SwapMainCamera(play);
    }
    return swap;
}

static void Coop_EndSwap(const CoopSwap& swap) {
    PlayState* play = swap.play;
    ActorListEntry* list = &play->actorCtx.actorLists[ACTORCAT_PLAYER];

    if (swap.swapCamera) {
        Coop_SwapMainCamera(play);
    }
    if (swap.swapInput) {
        Input tmp = play->state.input[0];
        play->state.input[0] = play->state.input[COOP_P2_PORT];
        play->state.input[COOP_P2_PORT] = tmp;
    }

    ActorList_Unlink(list, &sP2->actor);
    ActorList_PushTail(list, &sP2->actor);
}

// World state that Player 2's update or camera may write but that belongs to Player 1.
struct CoopWorldSnapshot {
    InterfaceContext interfaceCtx;
    TargetContext targetCtx;
    u8 lightSettingCur;
    u8 lightSettingPrev;
    f32 lightSettingBlend;
    s8 transitionTrigger;
    u8 transitionType;
    s16 nextEntranceIndex;
    s32 respawnFlag;
    RespawnData respawn[RESPAWN_MODE_MAX];
    u8 nextTransitionType;
};

static void Coop_TakeSnapshot(PlayState* play, CoopWorldSnapshot* snap) {
    snap->interfaceCtx = play->interfaceCtx;
    snap->targetCtx = play->actorCtx.targetCtx;
    snap->lightSettingCur = play->envCtx.unk_BD;
    snap->lightSettingPrev = play->envCtx.unk_BE;
    snap->lightSettingBlend = play->envCtx.unk_D8;
    snap->transitionTrigger = play->transitionTrigger;
    snap->transitionType = play->transitionType;
    snap->nextEntranceIndex = play->nextEntranceIndex;
    snap->respawnFlag = gSaveContext.respawnFlag;
    memcpy(snap->respawn, gSaveContext.respawn, sizeof(snap->respawn));
    snap->nextTransitionType = gSaveContext.nextTransitionType;
}

// Restores the snapshot. Returns true when Player 2 tried to start a scene transition (void out,
// loading zone, respawn), which co-op leaves to Player 1.
static bool Coop_RestoreSnapshot(PlayState* play, const CoopWorldSnapshot* snap) {
    bool startedTransition = (play->transitionTrigger != snap->transitionTrigger) ||
                             (play->nextEntranceIndex != snap->nextEntranceIndex) ||
                             (gSaveContext.respawnFlag != snap->respawnFlag);

    play->interfaceCtx = snap->interfaceCtx;
    play->actorCtx.targetCtx = snap->targetCtx;
    play->envCtx.unk_BD = snap->lightSettingCur;
    play->envCtx.unk_BE = snap->lightSettingPrev;
    play->envCtx.unk_D8 = snap->lightSettingBlend;
    play->transitionTrigger = snap->transitionTrigger;
    play->transitionType = snap->transitionType;
    play->nextEntranceIndex = snap->nextEntranceIndex;
    gSaveContext.respawnFlag = snap->respawnFlag;
    memcpy(gSaveContext.respawn, snap->respawn, sizeof(snap->respawn));
    gSaveContext.nextTransitionType = snap->nextTransitionType;

    return startedTransition;
}

// #endregion

// #region Player 2 actor

static bool Coop_ShouldSuspendP2(PlayState* play) {
    Player* p1 = GET_PLAYER(play);

    return (play->csCtx.state != CS_STATE_IDLE) || (play->transitionTrigger != TRANS_TRIGGER_OFF) ||
           (play->transitionMode != TRANS_MODE_OFF) || (gSaveContext.health <= 0) ||
           (p1->stateFlags1 & (PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_LOADING | PLAYER_STATE1_DEAD));
}

static void Coop_WarpP2ToP1(PlayState* play) {
    Player* p1 = GET_PLAYER(play);
    Actor* p2 = &sP2->actor;
    // Land behind Player 1 so both are on screen and not stuck inside each other.
    s16 yaw = p1->actor.shape.rot.y + 0x8000;

    p2->world.pos.x = p1->actor.world.pos.x + Math_SinS(yaw) * 40.0f;
    p2->world.pos.y = p1->actor.world.pos.y;
    p2->world.pos.z = p1->actor.world.pos.z + Math_CosS(yaw) * 40.0f;
    p2->prevPos = p2->home.pos = p2->world.pos;
    p2->shape.rot = p2->world.rot = p1->actor.shape.rot;
    p2->velocity = { 0.0f, 0.0f, 0.0f };
    p2->speedXZ = 0.0f;
    sP2->linearVelocity = 0.0f;
    sP2->yaw = p1->actor.shape.rot.y;
    sP2->stateFlags1 &= ~(PLAYER_STATE1_LOADING | PLAYER_STATE1_IN_CUTSCENE);
    sP2->fallStartHeight = (s16)p2->world.pos.y;

    if (!(sP2->stateFlags1 & PLAYER_STATE1_DEAD)) {
        play->func_11D54(sP2, play);
    }
    if (sP2CameraReady) {
        CoopSwap swap = Coop_BeginSwap(play, false, true);
        Camera_InitPlayerSettings(&play->mainCamera, sP2);
        Coop_EndSwap(swap);
    }
}

static void CoopPlayer_Init(Actor* thisx, PlayState* play) {
    Player* this_ = (Player*)thisx;

    sP2 = this_;
    thisx->room = -1;
    this_->itemAction = this_->heldItemAction = -1;
    this_->heldItemId = ITEM_NONE;

    // Same setup as Player_Init, minus everything that belongs to the scene's one true player (title
    // cards, respawn data, start-mode cutscenes, Navi).
    Player_UseItem(play, this_, ITEM_NONE);
    Player_SetEquipmentData(play, this_);
    this_->prevBoots = this_->currentBoots;
    play->playerInit(this_, play, gPlayerSkelHeaders[gSaveContext.linkAge]);

    sP2GiAlloc = ZELDA_ARENA_MALLOC_DEBUG(0x3008);
    this_->giObjectSegment = (void*)(((uintptr_t)sP2GiAlloc + 8) & ~0xF);

    play->func_11D54(this_, play);

    // Set up Player 2's camera in the main camera's place, as Play_Init does for Player 1's.
    CoopSwap swap = Coop_BeginSwap(play, false, true);
    Camera_Init(&play->mainCamera, &play->view, &play->colCtx, play);
    Camera_ChangeStatus(&play->mainCamera, CAM_STAT_ACTIVE);
    Camera_InitPlayerSettings(&play->mainCamera, this_);
    Camera_RequestMode(&play->mainCamera, CAM_MODE_NORMAL);
    play->mainCamera.childCamIdx = play->mainCamera.parentCamIdx = SUBCAM_FREE;
    Coop_EndSwap(swap);
    sP2CameraReady = true;
    sP2ViewValid = false;
}

static void CoopPlayer_Destroy(Actor* thisx, PlayState* play) {
    Player* this_ = (Player*)thisx;

    Effect_Delete(play, this_->meleeWeaponEffectIndex);
    Collider_DestroyCylinder(play, &this_->cylinder);
    Collider_DestroyQuad(play, &this_->meleeWeaponQuads[0]);
    Collider_DestroyQuad(play, &this_->meleeWeaponQuads[1]);
    Collider_DestroyQuad(play, &this_->shieldQuad);

    if (sP2GiAlloc != nullptr) {
        ZELDA_ARENA_FREE_DEBUG(sP2GiAlloc);
        sP2GiAlloc = nullptr;
    }
    if (sP2 == this_) {
        sP2 = nullptr;
        sP2CameraReady = false;
    }
}

static void CoopPlayer_Update(Actor* thisx, PlayState* play) {
    Player* p1 = GET_PLAYER(play);

    if (Coop_ShouldSuspendP2(play)) {
        sP2Suspended = true;
        return;
    }
    if (sP2Suspended) {
        sP2Suspended = false;
        Coop_WarpP2ToP1(play);
    }

    s32 leash = CVAR_COOP_LEASH;
    if ((leash > 0) && ((Actor_WorldDistXZToActor(thisx, &p1->actor) > leash) ||
                        (thisx->world.pos.y < p1->actor.world.pos.y - 1500.0f))) {
        Coop_WarpP2ToP1(play);
    }

    CoopWorldSnapshot snap;
    Coop_TakeSnapshot(play, &snap);
    CoopSwap swap = Coop_BeginSwap(play, true, true);

    // Mirrors the input handling in Player_Update.
    Input input;
    if (sP2->stateFlags1 & (PLAYER_STATE1_INPUT_DISABLED | PLAYER_STATE1_IN_CUTSCENE)) {
        memset(&input, 0, sizeof(input));
    } else {
        input = play->state.input[0];
        if (sP2->textboxBtnCooldownTimer != 0) {
            input.cur.button &= ~(BTN_A | BTN_B | BTN_CUP);
            input.press.button &= ~(BTN_A | BTN_B | BTN_CUP);
        }
    }

    play->playerUpdate(sP2, play, &input);

    Coop_EndSwap(swap);

    if (Coop_RestoreSnapshot(play, &snap)) {
        // Player 2 fell out or walked into an exit: scene changes are Player 1's call, so pull
        // Player 2 back instead. A void out still costs a heart, as it does for Player 1.
        if (sP2->actor.world.pos.y < p1->actor.world.pos.y - 100.0f) {
            Health_ChangeBy(play, -0x10);
        }
        Coop_WarpP2ToP1(play);
    }
}

static void CoopPlayer_Draw(Actor* thisx, PlayState* play) {
    if (sP2Suspended) {
        return;
    }

    CoopSwap swap = Coop_BeginSwap(play, false, false);
    Player_Draw(thisx, play);
    Coop_EndSwap(swap);
}

// Runs before the game updates its own cameras each frame, so Player 1's camera has the final word on
// play->view. Player 2's resulting view is left in sP2Camera (eye/at/up/fov) for the split-screen pass.
static void Coop_UpdateP2Camera(PlayState* play) {
    if ((sP2 == nullptr) || !sP2CameraReady || sP2Suspended || (play->pauseCtx.state != 0) ||
        (play->pauseCtx.debugState != 0)) {
        return;
    }

    View savedView = play->view;
    EnvironmentContext savedEnv = play->envCtx;
    InterfaceContext savedInterface = play->interfaceCtx;
    CoopSwap swap = Coop_BeginSwap(play, true, true);

    Camera_Update(&play->mainCamera);
    sP2ViewEye = play->view.eye;
    sP2ViewAt = play->view.lookAt;
    sP2ViewUp = play->view.up;
    sP2ViewFovy = play->view.fovy;
    sP2ViewValid = true;

    Coop_EndSwap(swap);
    play->interfaceCtx = savedInterface;
    play->envCtx = savedEnv;
    play->view = savedView;
}

// #endregion

// #region Enemies see both players

// Enemies aim at GET_PLAYER(). For each enemy whose nearer target is Player 2, Player 2 is made "the
// player" for the duration of that enemy's update, so its AI, attacks and grabs go after Player 2.
// Bosses stay on Player 1: their intros and death sequences drive Player 1's cutscene state.
static Actor* sTargetSwapActor = nullptr;
static std::unordered_set<Actor*> sEnemiesTargetingP2;

static void Coop_EndTargetSwap(PlayState* play) {
    if (sTargetSwapActor == nullptr) {
        return;
    }
    sTargetSwapActor = nullptr;
    if ((play == nullptr) || (sP2 == nullptr)) {
        return;
    }

    ActorListEntry* list = &play->actorCtx.actorLists[ACTORCAT_PLAYER];
    ActorList_Unlink(list, &sP2->actor);
    ActorList_PushTail(list, &sP2->actor);
}

static void Coop_OnShouldActorUpdate(void* actorRef, bool* should) {
    Actor* actor = static_cast<Actor*>(actorRef);
    PlayState* play = gPlayState;

    Coop_EndTargetSwap(play);

    if (!*should || (actor->category != ACTORCAT_ENEMY) || (Coop_GetPlayer2() == nullptr) || sP2Suspended ||
        Coop_ShouldSuspendP2(play) || (sP2->stateFlags1 & PLAYER_STATE1_DEAD)) {
        sEnemiesTargetingP2.erase(actor);
        return;
    }

    f32 p2XZ = Actor_WorldDistXZToActor(actor, &sP2->actor);
    f32 p2Y = Actor_HeightDiff(actor, &sP2->actor);
    f32 p2DistSq = SQ(p2XZ) + SQ(p2Y);
    bool wasTargetingP2 = sEnemiesTargetingP2.count(actor) != 0;

    // Only switch targets when the other player is clearly closer, so enemies don't flip-flop.
    bool targetP2 =
        wasTargetingP2 ? (actor->xyzDistToPlayerSq * 0.64f >= p2DistSq) : (p2DistSq < actor->xyzDistToPlayerSq * 0.64f);
    if (!targetP2) {
        sEnemiesTargetingP2.erase(actor);
        return;
    }
    sEnemiesTargetingP2.insert(actor);

    actor->xzDistToPlayer = p2XZ;
    actor->yDistToPlayer = p2Y;
    actor->xyzDistToPlayerSq = p2DistSq;
    actor->yawTowardsPlayer = Actor_WorldYawTowardActor(actor, &sP2->actor);

    ActorListEntry* list = &play->actorCtx.actorLists[ACTORCAT_PLAYER];
    ActorList_Unlink(list, &sP2->actor);
    ActorList_PushHead(list, &sP2->actor);
    sTargetSwapActor = actor;
}

// #endregion

static void Coop_RegisterActor() {
    if (sCoopActorId >= 0) {
        return;
    }

    const ActorDBEntry& player = ActorDB::Instance->RetrieveEntry(ACTOR_PLAYER).entry;
    ActorDBInit init;
    init.name = "Coop_Player";
    init.desc = "Split-screen co-op Player 2";
    init.category = ACTORCAT_PLAYER;
    init.flags = player.flags;
    init.objectId = player.objectId;
    init.instanceSize = sizeof(Player);
    init.init = CoopPlayer_Init;
    init.destroy = CoopPlayer_Destroy;
    init.update = CoopPlayer_Update;
    init.draw = CoopPlayer_Draw;
    sCoopActorId = ActorDB::Instance->AddEntry(init).entry.id;
}

static void Coop_SpawnP2() {
    if ((gPlayState == nullptr) || (sP2 != nullptr) || (sCoopActorId < 0)) {
        return;
    }

    Player* p1 = GET_PLAYER(gPlayState);
    if (p1 == nullptr) {
        return;
    }

    Actor* p2 = Actor_Spawn(&gPlayState->actorCtx, gPlayState, sCoopActorId, p1->actor.world.pos.x,
                            p1->actor.world.pos.y, p1->actor.world.pos.z, 0, p1->actor.shape.rot.y, 0, 0);
    if (p2 == nullptr) {
        return;
    }

    // Actor_Spawn puts new actors at the head of their category. Player 2 goes to the tail so that
    // GET_PLAYER() still finds Player 1, and so Player 1 is initialised (and sets up the shared
    // play->playerInit/playerUpdate entry points) before Player 2.
    ActorListEntry* list = &gPlayState->actorCtx.actorLists[ACTORCAT_PLAYER];
    ActorList_Unlink(list, p2);
    ActorList_PushTail(list, p2);

    // Claim the slot now so a second spawn request before init does not create another Player 2.
    sP2 = (Player*)p2;
    sP2Suspended = true;
}

static void Coop_KillP2() {
    Coop_EndTargetSwap(gPlayState);
    if ((gPlayState != nullptr) && (sP2 != nullptr)) {
        Actor_Kill(&sP2->actor);
    }
    sP2 = nullptr;
    sP2CameraReady = false;
}

extern "C" int Coop_IsEnabled(void) {
    return CVAR_COOP_VALUE != 0;
}

extern "C" Player* Coop_GetPlayer2(void) {
    // sP2 is claimed at spawn time; it only counts as present once its init has run.
    return (sP2 != nullptr && sP2->actor.init == nullptr) ? sP2 : nullptr;
}

extern "C" Camera* Coop_GetPlayer2Camera(void) {
    return (Coop_GetPlayer2() != nullptr && sP2CameraReady) ? &sP2Camera : nullptr;
}

extern "C" int Coop_IsSplitScreenActive(PlayState* play) {
    return (Coop_GetPlayer2() != nullptr) && sP2CameraReady && sP2ViewValid && !sP2Suspended &&
           !Coop_ShouldSuspendP2(play);
}

extern "C" void Coop_GetSplitViewport(int viewIndex, Viewport* viewport) {
    viewport->topY = 0;
    viewport->bottomY = SCREEN_HEIGHT;
    viewport->leftX = 0;
    viewport->rightX = SCREEN_WIDTH;

    if (CVAR_COOP_LAYOUT == 1) {
        if (viewIndex == 0) {
            viewport->bottomY = SCREEN_HEIGHT / 2;
        } else {
            viewport->topY = SCREEN_HEIGHT / 2;
        }
    } else {
        if (viewIndex == 0) {
            viewport->rightX = SCREEN_WIDTH / 2;
        } else {
            viewport->leftX = SCREEN_WIDTH / 2;
        }
    }
}

extern "C" void Coop_ApplyPlayer2Camera(View* view) {
    view->eye = sP2ViewEye;
    view->lookAt = sP2ViewAt;
    view->up = sP2ViewUp;
    view->fovy = sP2ViewFovy;
}

static void RegisterSplitScreenCoop() {
    if (CVAR_COOP_VALUE) {
        Coop_RegisterActor();
        Coop_SpawnP2();
    } else {
        Coop_KillP2();
    }

    // The whole actor context is torn down on scene change, so Player 2 is spawned again each scene.
    COND_HOOK(OnSceneSpawnActors, CVAR_COOP_VALUE, []() { Coop_SpawnP2(); });
    COND_HOOK(OnPlayDestroy, CVAR_COOP_VALUE, []() {
        sP2 = nullptr;
        sP2CameraReady = false;
        sTargetSwapActor = nullptr;
        sEnemiesTargetingP2.clear();
    });
    COND_HOOK(ShouldActorUpdate, CVAR_COOP_VALUE, Coop_OnShouldActorUpdate);
    COND_HOOK(OnActorUpdate, CVAR_COOP_VALUE, [](void* actor) {
        if (actor == sTargetSwapActor) {
            Coop_EndTargetSwap(gPlayState);
        }
    });
    COND_HOOK(OnCameraState, CVAR_COOP_VALUE, [](PlayState* play) {
        // Actor updates are over for this frame; make sure Player 1 is "the player" again.
        Coop_EndTargetSwap(play);
        Coop_UpdateP2Camera(play);
    });
}

static RegisterShipInitFunc initFunc(RegisterSplitScreenCoop, { CVAR_COOP_NAME });
