#include "SplitScreenCoop.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <unordered_set>
#include <utility>

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
void Interface_LoadActionLabel(InterfaceContext* interfaceCtx, u16 action, s16 loadOffset);
void func_8008A994(InterfaceContext* interfaceCtx);
float OTRGetAspectRatio(void);
void OTRSetHudAspectRatioOverride(float aspectRatio);
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

// Player 2's own A-button state (the do-action label and its flip animation). Everything else on Player 2's
// HUD is shared save data and comes from Player 1's interface context.
struct CoopHudState {
    s16 actionFlipState; // InterfaceContext::unk_1EC
    u16 actionShown;     // unk_1EE
    u16 actionNext;      // unk_1F0
    f32 actionFlipRot;   // unk_1F4
    char* doActionSegment[3];
};
static CoopHudState sP2Hud;

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

// #region Player 2's HUD state

static void Coop_InitP2Hud(PlayState* play) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;
    char** p1Segment = interfaceCtx->doActionSegment;

    sP2Hud.actionFlipState = 0;
    sP2Hud.actionShown = sP2Hud.actionNext = DO_ACTION_NONE;
    sP2Hud.actionFlipRot = 0.0f;
    for (s32 i = 0; i < 3; i++) {
        sP2Hud.doActionSegment[i] = p1Segment[i];
    }
    interfaceCtx->doActionSegment = sP2Hud.doActionSegment;
    Interface_LoadActionLabel(interfaceCtx, DO_ACTION_NONE, 0);
    Interface_LoadActionLabel(interfaceCtx, DO_ACTION_NONE, 1);
    interfaceCtx->doActionSegment = p1Segment;
}

// Points the interface context at an A-button state. Callers restore the interface context afterwards.
static void Coop_LoadHud(PlayState* play, CoopHudState* hud) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    interfaceCtx->unk_1EC = hud->actionFlipState;
    interfaceCtx->unk_1EE = hud->actionShown;
    interfaceCtx->unk_1F0 = hud->actionNext;
    interfaceCtx->unk_1F4 = hud->actionFlipRot;
    interfaceCtx->doActionSegment = hud->doActionSegment;
}

// Keeps the interface context's A-button state.
static void Coop_StoreHud(PlayState* play, CoopHudState* hud) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    hud->actionFlipState = interfaceCtx->unk_1EC;
    hud->actionShown = interfaceCtx->unk_1EE;
    hud->actionNext = interfaceCtx->unk_1F0;
    hud->actionFlipRot = interfaceCtx->unk_1F4;
    if (interfaceCtx->doActionSegment != hud->doActionSegment) {
        for (s32 i = 0; i < 3; i++) {
            hud->doActionSegment[i] = interfaceCtx->doActionSegment[i];
        }
    }
}

// Points the interface context at Player 2's A-button state. Callers restore Player 1's context afterwards
// (the update snapshot, or a copy of the interface context for drawing).
static void Coop_BeginP2Hud(PlayState* play) {
    // The B-button label follows Player 1's sword state, which is shared.
    sP2Hud.doActionSegment[2] = play->interfaceCtx.doActionSegment[2];
    Coop_LoadHud(play, &sP2Hud);
}

// Runs the A-button label flip that Interface_Update runs for Player 1 (see the unk_1EC state machine there).
static void Coop_AnimateActionLabel(PlayState* play) {
    InterfaceContext* interfaceCtx = &play->interfaceCtx;

    switch (interfaceCtx->unk_1EC) {
        case 1:
        case 3:
            interfaceCtx->unk_1F4 += 31400.0f / WREG(5);
            if (interfaceCtx->unk_1F4 >= 15700.0f) {
                interfaceCtx->unk_1F4 = -15700.0f;
                interfaceCtx->unk_1EC = 2;
            }
            break;
        case 2:
        case 4:
            interfaceCtx->unk_1F4 += 31400.0f / WREG(5);
            if (interfaceCtx->unk_1F4 >= 0.0f) {
                interfaceCtx->unk_1F4 = 0.0f;
                interfaceCtx->unk_1EC = 0;
                interfaceCtx->unk_1EE = interfaceCtx->unk_1F0;
                u16 action = interfaceCtx->unk_1EE;
                if ((action == DO_ACTION_MAX) || (action == DO_ACTION_MAX + 1)) {
                    action = DO_ACTION_NONE;
                }
                Interface_LoadActionLabel(interfaceCtx, action, 0);
            }
            break;
    }
}

// Keeps what Player 2's update did to the A button, with the label flip Player 1's gets from Interface_Update.
static void Coop_SaveP2Hud(PlayState* play) {
    Coop_AnimateActionLabel(play);
    Coop_StoreHud(play, &sP2Hud);
}

static void Coop_ResetHudViewport(PlayState* play);

// #endregion

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
    Coop_InitP2Hud(play);

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

    Coop_BeginP2Hud(play);
    play->playerUpdate(sP2, play, &input);
    Coop_SaveP2Hud(play);

    Coop_EndSwap(swap);

    // The message box reads controller 1. While Player 2 is the one talking, Player 2's buttons also count,
    // so Player 2 can read and answer its own conversation. Player 1 has already updated this frame.
    if ((sP2->stateFlags1 & PLAYER_STATE1_TALKING) && !(p1->stateFlags1 & PLAYER_STATE1_TALKING)) {
        Input* p1Input = &play->state.input[0];
        const Input* p2Input = &play->state.input[COOP_P2_PORT];
        p1Input->cur.button |= p2Input->cur.button;
        p1Input->press.button |= p2Input->press.button;
        p1Input->rel.button |= p2Input->rel.button;
        if (p1Input->rel.stick_x == 0 && p1Input->rel.stick_y == 0) {
            p1Input->rel.stick_x = p2Input->rel.stick_x;
            p1Input->rel.stick_y = p2Input->rel.stick_y;
        }
    }

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

// #region The world sees both players

// Actors aim at, talk to and hand items to GET_PLAYER(). For enemies, NPCs and a few pick-up-able objects
// whose nearer player is Player 2, Player 2 is made "the player" for the duration of that actor's update, so
// enemy AI and attacks go after Player 2, and Player 2 can talk to NPCs, collect drops and lift pots.
// Bosses, doors and chests stay on Player 1: they drive cutscenes, room changes and item-get sequences
// that the rest of the game expects Player 1 to be in.
static Actor* sTargetSwapActor = nullptr;
static std::unordered_set<Actor*> sActorsTargetingP2;

static bool Coop_ActorSeesBothPlayers(Actor* actor) {
    if ((actor->category == ACTORCAT_ENEMY) || (actor->category == ACTORCAT_NPC)) {
        return true;
    }
    switch (actor->id) {
        case ACTOR_EN_ITEM00: // Rupees, hearts, ammo and other drops
        case ACTOR_EN_SI:     // Gold Skulltula tokens
        case ACTOR_OBJ_TSUBO: // Pots
        case ACTOR_EN_KUSA:   // Bushes
        case ACTOR_EN_ISHI:   // Rocks
        case ACTOR_OBJ_KIBAKO:
        case ACTOR_OBJ_KIBAKO2: // Crates
        case ACTOR_EN_KANBAN:   // Signs
        case ACTOR_EN_NIW:      // Cuccos
            return true;
    }
    return false;
}

static bool Coop_IsBoundToPlayer(Actor* actor, Player* player) {
    return (player->talkActor == actor) || (player->heldActor == actor) || (actor->parent == &player->actor) ||
           (player->naviActor == actor);
}

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

    if (!*should || !Coop_ActorSeesBothPlayers(actor) || (Coop_GetPlayer2() == nullptr) || sP2Suspended ||
        Coop_ShouldSuspendP2(play) || (sP2->stateFlags1 & PLAYER_STATE1_DEAD) ||
        Coop_IsBoundToPlayer(actor, GET_PLAYER(play))) {
        sActorsTargetingP2.erase(actor);
        return;
    }

    f32 p2XZ = Actor_WorldDistXZToActor(actor, &sP2->actor);
    f32 p2Y = Actor_HeightDiff(actor, &sP2->actor);
    f32 p2DistSq = SQ(p2XZ) + SQ(p2Y);
    bool wasTargetingP2 = sActorsTargetingP2.count(actor) != 0;

    // Only switch players when the other one is clearly closer, so actors don't flip-flop. An actor Player 2
    // is talking to, carrying or riding stays with Player 2.
    bool targetP2 =
        Coop_IsBoundToPlayer(actor, sP2) || (wasTargetingP2 ? (actor->xyzDistToPlayerSq * 0.64f >= p2DistSq)
                                                            : (p2DistSq < actor->xyzDistToPlayerSq * 0.64f));
    if (!targetP2) {
        sActorsTargetingP2.erase(actor);
        return;
    }
    sActorsTargetingP2.insert(actor);

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

// #region Per-player pause menu

// Each player has their own copy of the stock pause menu, opened with their own START and drawn at its usual
// 4:3 shape, scaled down into their half of the screen. While only one player has it open the game keeps
// running and only that player's controller goes to the menu. When both players have it open the game pauses,
// as it does in single player.
//
// The pause menu works on play->pauseCtx, the REG pages, the HUD visibility and button state in the save
// context, the A-button label and controller port 0. Each menu keeps its own copy of all of those, swapped in
// around every call into the pause menu code, so the world and the other player's menu never see them.
struct CoopPauseMenu {
    PauseContext pauseCtx;
    s16 regs[ARRAY_COUNT(((GameInfo*)nullptr)->data)];
    u8 buttonStatus[ARRAY_COUNT(gSaveContext.buttonStatus)];
    u16 nextHudVisibilityMode;
    u16 hudVisibilityMode;
    u16 hudVisibilityModeTimer;
    u16 prevHudVisibilityMode;
    CoopHudState hud; // The A-button label the menu shows ("Decide", "Equip", ...)
    s16 startAlpha;
    Input input;      // This player's controller this frame (gameplay gets none of it while the menu is open)
    bool extraUpdate; // The menu runs 3 updates every 2 frames: its usual 30 Hz while the game runs at 20 Hz
    bool drawn;       // The menu has drawn since it opened (its update uses vertices its draw makes)
    bool ready;       // pauseCtx has been set up for this scene
};
static CoopPauseMenu sPauseMenus[2];
// Where each menu builds its pause-screen Link. The stock menu uses the start of the object bank; a buffer per
// menu keeps the two menus apart and leaves the running game's object memory alone.
alignas(64) static u8 sPauseSegments[2][0x10000];
// Set when co-op runs the pause menus this frame, instead of the stock single-player pause.
static bool sPauseMenusActive = false;

extern "C" s8 D_80130608; // Set while the game's sound is paused for the pause menu

static bool Coop_PauseMenuOpen(s32 i) {
    return sPauseMenus[i].pauseCtx.state != 0;
}

static Player* Coop_MenuPlayer(PlayState* play, s32 i) {
    return (i == 0) ? GET_PLAYER(play) : sP2;
}

// The world's state that a pause menu swaps out while it runs.
struct CoopPauseWorld {
    PauseContext pauseCtx;
    s16 regs[ARRAY_COUNT(((GameInfo*)nullptr)->data)];
    u8 buttonStatus[ARRAY_COUNT(gSaveContext.buttonStatus)];
    u16 hudVisibility[4];
    InterfaceContext interfaceCtx;
    Input input;
    void* objectSpace;
    CoopSwap playerSwap;
};
static CoopPauseWorld sPauseWorld;

static void Coop_SwapHudVisibility(u8* buttonStatus, u16* visibility) {
    for (size_t i = 0; i < ARRAY_COUNT(gSaveContext.buttonStatus); i++) {
        std::swap(buttonStatus[i], gSaveContext.buttonStatus[i]);
    }
    std::swap(visibility[0], gSaveContext.nextHudVisibilityMode);
    std::swap(visibility[1], gSaveContext.hudVisibilityMode);
    std::swap(visibility[2], gSaveContext.hudVisibilityModeTimer);
    std::swap(visibility[3], gSaveContext.prevHudVisibilityMode);
}

// Makes menu i's state the live state: play->pauseCtx, REGs, HUD visibility, A-button label, controller port 0
// and, for Player 2, "the player".
static void Coop_EnterPauseMenu(PlayState* play, s32 i) {
    CoopPauseMenu* menu = &sPauseMenus[i];
    CoopPauseWorld* world = &sPauseWorld;
    u16 visibility[4] = { menu->nextHudVisibilityMode, menu->hudVisibilityMode, menu->hudVisibilityModeTimer,
                          menu->prevHudVisibilityMode };

    world->pauseCtx = play->pauseCtx;
    play->pauseCtx = menu->pauseCtx;
    memcpy(world->regs, gGameInfo->data, sizeof(world->regs));
    memcpy(gGameInfo->data, menu->regs, sizeof(world->regs));
    memcpy(world->buttonStatus, menu->buttonStatus, sizeof(world->buttonStatus));
    Coop_SwapHudVisibility(world->buttonStatus, visibility);
    memcpy(world->hudVisibility, visibility, sizeof(visibility));

    world->interfaceCtx = play->interfaceCtx;
    Coop_LoadHud(play, &menu->hud);
    play->interfaceCtx.startAlpha = menu->startAlpha;

    world->input = play->state.input[0];
    play->state.input[0] = menu->input;
    world->objectSpace = play->objectCtx.spaceStart;
    play->objectCtx.spaceStart = sPauseSegments[i];

    if (i == 1) {
        world->playerSwap = Coop_BeginSwap(play, false, false);
    }
}

// Keeps what the menu did and puts the world's state back.
static void Coop_ExitPauseMenu(PlayState* play, s32 i) {
    CoopPauseMenu* menu = &sPauseMenus[i];
    CoopPauseWorld* world = &sPauseWorld;

    if (i == 1) {
        Coop_EndSwap(world->playerSwap);
    }

    play->objectCtx.spaceStart = world->objectSpace;
    play->state.input[0] = world->input;

    Coop_StoreHud(play, &menu->hud);
    menu->startAlpha = play->interfaceCtx.startAlpha;
    play->interfaceCtx = world->interfaceCtx;

    Coop_SwapHudVisibility(world->buttonStatus, world->hudVisibility);
    memcpy(menu->buttonStatus, world->buttonStatus, sizeof(menu->buttonStatus));
    menu->nextHudVisibilityMode = world->hudVisibility[0];
    menu->hudVisibilityMode = world->hudVisibility[1];
    menu->hudVisibilityModeTimer = world->hudVisibility[2];
    menu->prevHudVisibilityMode = world->hudVisibility[3];

    memcpy(menu->regs, gGameInfo->data, sizeof(menu->regs));
    memcpy(gGameInfo->data, world->regs, sizeof(world->regs));
    menu->pauseCtx = play->pauseCtx;
    play->pauseCtx = world->pauseCtx;
}

// A closed menu starts from the world's current state the next time it opens (its pages and cursors are kept).
static void Coop_RefreshClosedPauseMenu(PlayState* play, s32 i) {
    CoopPauseMenu* menu = &sPauseMenus[i];

    memcpy(menu->regs, gGameInfo->data, sizeof(menu->regs));
    memcpy(menu->buttonStatus, gSaveContext.buttonStatus, sizeof(menu->buttonStatus));
    menu->nextHudVisibilityMode = gSaveContext.nextHudVisibilityMode;
    menu->hudVisibilityMode = gSaveContext.hudVisibilityMode;
    menu->hudVisibilityModeTimer = gSaveContext.hudVisibilityModeTimer;
    menu->prevHudVisibilityMode = gSaveContext.prevHudVisibilityMode;
    if (i == 0) {
        Coop_StoreHud(play, &menu->hud);
    } else {
        menu->hud = sP2Hud;
        menu->hud.doActionSegment[2] = play->interfaceCtx.doActionSegment[2];
    }
    menu->startAlpha = play->interfaceCtx.startAlpha;
}

// The game's sound pauses only while both menus are open, as it does for the single-player pause menu.
static void Coop_UpdatePauseSound() {
    bool paused = Coop_PauseMenuOpen(0) && Coop_PauseMenuOpen(1);

    if (paused != (D_80130608 != 0)) {
        Audio_QueueCmdS32(paused ? 0xF1000000 : 0xF2000000, 0);
        D_80130608 = paused;
    }
}

// Brings the pause menu code in for an open menu. Closing a menu unloads it, and so does every player update or
// draw (the player and the pause menu share one overlay slot), which keep happening while a menu is open.
static void Coop_KeepPauseMenuLoaded() {
    KaleidoMgrOverlay* kaleidoScopeOvl = &gKaleidoMgrOverlayTable[KALEIDO_OVL_KALEIDO_SCOPE];

    if ((Coop_PauseMenuOpen(0) || Coop_PauseMenuOpen(1)) && (gKaleidoMgrCurOvl != kaleidoScopeOvl)) {
        if (gKaleidoMgrCurOvl != nullptr) {
            KaleidoManager_ClearOvl(gKaleidoMgrCurOvl);
        }
        KaleidoManager_LoadOvl(kaleidoScopeOvl);
    }
}

// Closes any open menu at once, for when the split screen ends (a cutscene, a scene change, a game over).
static void Coop_ClosePauseMenus(PlayState* play) {
    for (s32 i = 0; i < 2; i++) {
        if (!Coop_PauseMenuOpen(i) || ((i == 1) && (sP2 == nullptr))) {
            sPauseMenus[i].pauseCtx.state = 0;
            continue;
        }
        Coop_EnterPauseMenu(play, i);
        // The pause menu's own last closing step: restores the HUD and the player's equipment.
        play->pauseCtx.state = 0x13;
        KaleidoScopeCall_Update(play);
        play->pauseCtx.state = 0;
        play->pauseCtx.debugState = 0;
        Coop_ExitPauseMenu(play, i);
    }
    Coop_UpdatePauseSound();
}

static void Coop_ResetPauseMenus() {
    for (s32 i = 0; i < 2; i++) {
        sPauseMenus[i].pauseCtx.state = 0;
        sPauseMenus[i].ready = false;
    }
    sPauseMenusActive = false;
    if (D_80130608 != 0) {
        Audio_QueueCmdS32(0xF2000000, 0);
        D_80130608 = 0;
    }
}

// Runs at the start of each game frame, after the controllers are read: an open menu takes its player's
// controller, so that player's Link stands still while they are in the menu.
static void Coop_TakeMenuInput() {
    PlayState* play = gPlayState;

    if ((play == nullptr) || (Coop_GetPlayer2() == nullptr)) {
        return;
    }
    for (s32 i = 0; i < 2; i++) {
        Input* input = &play->state.input[(i == 0) ? 0 : COOP_P2_PORT];
        sPauseMenus[i].input = *input;
        if (Coop_PauseMenuOpen(i)) {
            memset(input, 0, sizeof(*input));
        }
    }
}

// Called by Play_Update where the single-player game checks for START. Returns -1 when the stock pause menu
// is in charge this frame, otherwise whether the game is paused (both menus open).
extern "C" int Coop_UpdatePauseSetup(PlayState* play) {
    sPauseMenusActive = false;

    if (!Coop_IsSplitScreenActive(play) || (play->pauseCtx.state != 0) || (play->pauseCtx.debugState != 0) ||
        (play->gameOverCtx.state != GAMEOVER_INACTIVE)) {
        if (Coop_PauseMenuOpen(0) || Coop_PauseMenuOpen(1)) {
            Coop_ClosePauseMenus(play);
        }
        return -1;
    }

    sPauseMenusActive = true;
    bool canOpen = (gSaveContext.gameMode == GAMEMODE_NORMAL) && (play->msgCtx.msgMode == MSGMODE_NONE);

    for (s32 i = 0; i < 2; i++) {
        CoopPauseMenu* menu = &sPauseMenus[i];

        if (!menu->ready) {
            menu->pauseCtx = play->pauseCtx;
            menu->ready = true;
        }
        if (Coop_PauseMenuOpen(i)) {
            continue;
        }

        Coop_RefreshClosedPauseMenu(play, i);
        Player* player = Coop_MenuPlayer(play, i);
        if (canOpen && !(player->stateFlags1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_DEAD)) &&
            CHECK_BTN_ALL(menu->input.press.button, BTN_START)) {
            Coop_EnterPauseMenu(play, i);
            KaleidoSetup_Update(play);
            Coop_ExitPauseMenu(play, i);
            menu->extraUpdate = false;
            menu->drawn = false;
        }
    }

    Coop_UpdatePauseSound();
    return Coop_PauseMenuOpen(0) && Coop_PauseMenuOpen(1);
}

// Called by Play_Update where the single-player game updates its pause menu.
extern "C" void Coop_UpdatePauseMenus(PlayState* play) {
    if (!sPauseMenusActive) {
        return;
    }

    for (s32 i = 0; i < 2; i++) {
        CoopPauseMenu* menu = &sPauseMenus[i];

        if (!Coop_PauseMenuOpen(i)) {
            continue;
        }

        s32 updates = (menu->extraUpdate && menu->drawn) ? 2 : 1;
        menu->extraUpdate = !menu->extraUpdate;
        for (s32 n = 0; n < updates; n++) {
            Coop_KeepPauseMenuLoaded();
            Coop_EnterPauseMenu(play, i);
            if (n > 0) {
                // A button press counts once.
                play->state.input[0].press.button = 0;
            }

            KaleidoScopeCall_Update(play);

            // The stock pause menu waits for the frame to be captured as its background. Here the game keeps
            // drawing behind it, so there is nothing to wait for.
            if ((play->pauseCtx.state != 0) && ((R_PAUSE_MENU_MODE == 1) || (R_PAUSE_MENU_MODE == 2))) {
                R_PAUSE_MENU_MODE = 3;
            }
            if (n == 0) {
                Coop_AnimateActionLabel(play);
            }
            Coop_ExitPauseMenu(play, i);

            if (!Coop_PauseMenuOpen(i)) {
                // Equipment is shared: the closing menu dressed its own player, this dresses the other one.
                Player* other = Coop_MenuPlayer(play, 1 - i);
                if (i == 0) {
                    CoopSwap swap = Coop_BeginSwap(play, false, false);
                    Player_SetEquipmentData(play, other);
                    Coop_EndSwap(swap);
                } else {
                    Player_SetEquipmentData(play, other);
                }
                break;
            }
        }
    }

    Coop_KeepPauseMenuLoaded();
    Coop_UpdatePauseSound();
}

// The largest 4:3 area (as it looks on screen) centered in a viewport, in the game's 320x240 coordinates.
static void Coop_FitPauseMenu(const Viewport* viewport, float windowAspect, s32* x, s32* y, s32* w, s32* h) {
    float width = (float)(viewport->rightX - viewport->leftX);
    float height = (float)(viewport->bottomY - viewport->topY);
    // One 320x240 unit is windowAspect * 3/4 times as wide on screen as it is tall.
    float unitAspect = windowAspect * ((float)SCREEN_HEIGHT / SCREEN_WIDTH);
    float fitHeight = width * unitAspect * 3.0f / 4.0f;
    float fitWidth = width;

    if (fitHeight > height) {
        fitHeight = height;
        fitWidth = height * 4.0f / 3.0f / unitAspect;
    }
    *w = (s32)(fitWidth + 0.5f);
    *h = (s32)(fitHeight + 0.5f);
    *x = viewport->leftX + (s32)((width - *w) / 2.0f + 0.5f);
    *y = viewport->topY + (s32)((height - *h) / 2.0f + 0.5f);
}

// Draws menu i's pages in the 4:3 area of the current half, in the overlay pass (on top of both views).
static void Coop_DrawPauseMenu(PlayState* play, s32 i, const Viewport* viewport, float windowAspect) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    s32 x, y, w, h;

    Coop_FitPauseMenu(viewport, windowAspect, &x, &y, &w, &h);
    OPEN_DISPS(gfxCtx);
    gSPSetHudRegion(OVERLAY_DISP++, x, y, w, h, 100);
    Coop_ResetHudViewport(play);
    CLOSE_DISPS(gfxCtx);
    OTRSetHudAspectRatioOverride(4.0f / 3.0f);

    // The pause menu draws into the opaque world list, which runs before the second view and the translucent
    // pass. Pointing that list at the overlay list keeps the menu on top of the game.
    TwoHeadGfxArena polyOpa = gfxCtx->polyOpa;
    gfxCtx->polyOpa = gfxCtx->overlay;
    Coop_KeepPauseMenuLoaded();
    KaleidoScopeCall_Draw(play);
    sPauseMenus[i].drawn = sPauseMenus[i].drawn || ((play->pauseCtx.state >= 4) && (play->pauseCtx.state <= 7));
    gfxCtx->overlay = gfxCtx->polyOpa;
    gfxCtx->polyOpa = polyOpa;
}

// #endregion

// #region Per-player HUD

// Viewport and scissor are resolved to pixels when they are set, so they are set again whenever the HUD
// region changes.
static void Coop_ResetHudViewport(PlayState* play) {
    OPEN_DISPS(play->state.gfxCtx);
    gDPPipeSync(OVERLAY_DISP++);
    gDPSetScissor(OVERLAY_DISP++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    CLOSE_DISPS(play->state.gfxCtx);
    func_8008A994(&play->interfaceCtx);
}

// Draws the HUD once per half: each half gets the full HUD laid out for that half's shape, with Player 2's
// half showing Player 2's A-button action and minimap arrow. A player with the pause menu open gets the menu in
// their half, under their HUD, as in single player.
extern "C" void Coop_DrawHuds(PlayState* play) {
    float windowAspect = OTRGetAspectRatio();

    for (s32 i = 0; i < 2; i++) {
        Viewport viewport;
        Coop_GetSplitViewport(i, &viewport);
        s32 width = viewport.rightX - viewport.leftX;
        s32 height = viewport.bottomY - viewport.topY;
        float aspect = windowAspect * ((float)width / SCREEN_WIDTH) / ((float)height / SCREEN_HEIGHT);
        // A half narrower than 4:3 (side by side) gets the 4:3 layout across its width, with each element kept in
        // shape at the top or bottom, so the hearts and the item buttons don't run into each other.
        s32 scalePct = 100;
        if (aspect < 4.0f / 3.0f) {
            scalePct = (s32)(aspect / (4.0f / 3.0f) * 100.0f);
            aspect = 4.0f / 3.0f;
        }

        InterfaceContext worldInterface = play->interfaceCtx;
        bool menuOpen = Coop_PauseMenuOpen(i) && sPauseMenusActive;
        CoopSwap swap;
        if (menuOpen) {
            Coop_EnterPauseMenu(play, i);
            Coop_DrawPauseMenu(play, i, &viewport, windowAspect);
        } else if (i == 1) {
            Coop_BeginP2Hud(play);
            swap = Coop_BeginSwap(play, false, false);
        }

        OPEN_DISPS(play->state.gfxCtx);
        gSPSetHudRegion(OVERLAY_DISP++, viewport.leftX, viewport.topY, width, height, scalePct);
        Coop_ResetHudViewport(play);
        CLOSE_DISPS(play->state.gfxCtx);
        OTRSetHudAspectRatioOverride(aspect);

        Interface_Draw(play);

        if (menuOpen) {
            Coop_ExitPauseMenu(play, i);
        } else if (i == 1) {
            Coop_EndSwap(swap);
        }
        play->interfaceCtx = worldInterface;
    }

    OTRSetHudAspectRatioOverride(0.0f);
    OPEN_DISPS(play->state.gfxCtx);
    gSPClearHudRegion(OVERLAY_DISP++);
    Coop_ResetHudViewport(play);
    CLOSE_DISPS(play->state.gfxCtx);
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
        Coop_ResetPauseMenus();
        sP2 = nullptr;
        sP2CameraReady = false;
        sTargetSwapActor = nullptr;
        sActorsTargetingP2.clear();
    });
    COND_HOOK(ShouldActorUpdate, CVAR_COOP_VALUE, Coop_OnShouldActorUpdate);
    COND_HOOK(OnActorUpdate, CVAR_COOP_VALUE, [](void* actor) {
        if (actor == sTargetSwapActor) {
            Coop_EndTargetSwap(gPlayState);
        }
    });
    COND_HOOK(OnGameStateMainStart, CVAR_COOP_VALUE, []() { Coop_TakeMenuInput(); });
    COND_HOOK(OnCameraState, CVAR_COOP_VALUE, [](PlayState* play) {
        // Actor updates are over for this frame; make sure Player 1 is "the player" again.
        Coop_EndTargetSwap(play);
        Coop_UpdateP2Camera(play);
    });
}

static RegisterShipInitFunc initFunc(RegisterSplitScreenCoop, { CVAR_COOP_NAME });
