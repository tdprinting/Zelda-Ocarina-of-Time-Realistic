// Expressive NPC and enemy faces. See NpcFace.h.

#include "NpcFace.h"
#include "ModernAnimation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/frame_interpolation.h"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "z64.h"
extern PlayState* gPlayState;
}

namespace {

enum Mood : s32 {
    kNeutral,
    kHappy,
    kJoy,
    kAngry,
    kStrain,
    kTired,
    kWorried,
    kFear,
    kSurprise,
    kSad,
    kSleepy,
    kAsleep,
    kBored,
    kSmug,
    kPain,
    kFocused,
    kNumMoods,
};

const char* const kMoodNames[kNumMoods] = {
    "neutral",  "happy", "joy",    "angry",  "strain", "tired", "worried", "fear",
    "surprise", "sad",   "sleepy", "asleep", "bored",  "smug",  "pain",    "focused",
};

struct Expression {
    f32 upperLid, lowerLid, tilt, pupil;
};

// Lid shapes per mood. Lids are fractions of the eye's height; tilt slants the upper lid toward the nose.
const Expression kExpressions[kNumMoods] = {
    { 0.12f, 0.00f, 0.00f, 1.00f },  // neutral
    { 0.15f, 0.30f, -0.10f, 1.05f }, // happy: relaxed, slight lower-lid smile
    { 0.30f, 0.60f, -0.25f, 1.10f }, // joy: laughing crescent eyes
    { 0.35f, 0.15f, 0.85f, 0.90f },  // angry
    { 0.50f, 0.35f, 0.65f, 0.90f },  // strain: lifting, pushing, digging
    { 0.45f, 0.05f, -0.30f, 1.00f }, // tired
    { 0.08f, 0.00f, -0.70f, 0.95f }, // worried
    { 0.00f, 0.00f, -0.60f, 0.70f }, // fear: wide, small pupils
    { 0.00f, 0.00f, -0.20f, 0.80f }, // surprise
    { 0.35f, 0.05f, -0.85f, 1.00f }, // sad
    { 0.70f, 0.00f, -0.10f, 1.00f }, // sleepy
    { 1.00f, 0.00f, 0.00f, 1.00f },  // asleep
    { 0.45f, 0.00f, 0.00f, 1.00f },  // bored
    { 0.40f, 0.25f, 0.30f, 1.00f },  // smug
    { 0.80f, 0.50f, 0.50f, 0.85f },  // pain
    { 0.25f, 0.05f, 0.50f, 0.90f },  // focused: fighting stance
};

// What each animation says about the character's mood. Animations not listed leave the face to the context
// (talking, Link nearby, enemies in a fight).
struct AnimMood {
    const char* name;
    Mood mood;
    f32 pulse; // > 0: the mood strengthens and eases with the animation's loop (effort beats)
};

const AnimMood kAnimMoods[] = {
    // Kokiri
    { "object_os_anime/gKokiriLiftingRockAnim", kStrain, 0.35f },
    { "object_os_anime/gKokiriPunchingAnim", kAngry, 0.3f },
    { "object_os_anime/gKokiriLaughingAnim", kJoy, 0.0f },
    { "object_os_anime/gKokiriBackflipAnim", kJoy, 0.0f },
    { "object_os_anime/gKokiriCuttingGrassAnim", kStrain, 0.25f },
    { "object_os_anime/gKokiriSittingDiggingAnim", kStrain, 0.2f },
    { "object_os_anime/gKokiriWipingForeheadAnim", kTired, 0.0f },
    { "object_os_anime/gKokiriStandingApprehensiveAnim", kWorried, 0.0f },
    { "object_os_anime/gKokiriSittingHeadOnHandAnim", kBored, 0.0f },
    { "object_os_anime/gKokiriStandingHandOnChestAnim", kSmug, 0.0f },
    { "object_os_anime/gKokiriStandingHandsOnHipsAnim", kSmug, 0.0f },
    { "object_os_anime/gKokiriSittingCrossedArmsLegsAnim", kBored, 0.0f },
    { "object_os_anime/gKokiriRecliningLeaningBackAnim", kHappy, 0.0f },
    { "object_os_anime/gKokiriSittingArmsUpAnim", kHappy, 0.0f },
    { "object_os_anime/gKokiriBlockingAnim", kAngry, 0.0f },
    // Mido
    { "object_md/gMidoAngryHeadTurnAnim", kAngry, 0.0f },
    { "object_md/gMidoAnnoyedPointedHeadIdle1Anim", kAngry, 0.0f },
    { "object_md/gMidoAnnoyedPointedHeadIdle2Anim", kAngry, 0.0f },
    { "object_md/gMidoHaltAnim", kAngry, 0.0f },
    { "object_md/gMidoHandsOnHipsIdleAnim", kSmug, 0.0f },
    { "object_md/gMidoSlamAnim", kAngry, 0.4f },
    // Saria
    { "object_sa/gSariaHandsOnFaceAnim", kSad, 0.0f },
    { "object_sa/gSariaPlayingOcarinaAnim", kHappy, 0.0f },
    { "object_sa/gSariaStandHandsOnHipsAnim", kSmug, 0.0f },
    // Hylians, Kakariko, market
    { "object_human/gHumanThinkingAnim", kBored, 0.0f },
    { "object_human/gHumanLookingAroundAnim", kWorried, 0.0f },
    { "object_cs/gGraveyardKidSwingStickUpAnim", kAngry, 0.3f },
    { "object_js/gCarpetMerchantSlappingKneeAnim", kJoy, 0.0f },
    { "object_mm/gRunningManExcitedAnim", kJoy, 0.0f },
    { "object_mm/gRunningManHappyAnim", kHappy, 0.0f },
    { "object_mm/gRunningManSprintAnim", kStrain, 0.2f },
    { "object_mm/gRunningManSitWaitAnim", kBored, 0.0f },
    { "object_ms/gBeanSalesmanEatingAnim", kHappy, 0.0f },
    { "object_bg/gChuGirlNoddingOffAnim", kSleepy, 0.0f },
    { "object_bg/gChuGirlWakeUpAnim", kSurprise, 0.0f },
    { "object_ani/gRoofManKnockbackAnim", kPain, 0.0f },
    { "object_ani/gRoofManGettingUpAfterKnockbackAnim", kTired, 0.0f },
    { "object_tk/gDampeDigAnim", kStrain, 0.3f },
    { "object_ps/gPoeSellerSwingStickAnim", kAngry, 0.2f },
    { "object_ta/gTalonSitSleepingAnim", kAsleep, 0.0f },
    { "object_ta/gTalonSleepAnim", kAsleep, 0.0f },
    { "object_ta/gTalonSitWakeUpAnim", kSurprise, 0.0f },
    { "object_ta/gTalonWakeUpAnim", kSurprise, 0.0f },
    { "object_ta/gTalonSitHandsUpAnim", kJoy, 0.0f },
    { "object_ta/gTalonRunAnim", kWorried, 0.0f },
    { "object_ma1/gMalonChildSingAnim", kHappy, 0.0f },
    { "object_ma1/gMalonChildRaiseHandsAnim", kJoy, 0.0f },
    { "object_ma2/gMalonAdultSingAnim", kHappy, 0.0f },
    { "object_fu/gWindmillManPlayAndMoveHeadAnim", kAngry, 0.0f },
    { "object_fu/gWindmillManPlayStillAnim", kAngry, 0.0f },
    { "object_sd/gEnHeishiDyingGuardTalkAnim", kPain, 0.0f },
    { "object_sd/gEnHeishiDyingGuardDieAnim", kPain, 0.0f },
    // Gorons, Zoras, Gerudo
    { "object_du/gDaruniaHitBreastAnim", kSmug, 0.0f },
    { "object_du/gDaruniaWrongSongAnim", kAngry, 0.0f },
    { "object_du/gDaruniaDancingLoop1Anim", kJoy, 0.0f },
    { "object_du/gDaruniaDancingLoop2Anim", kJoy, 0.0f },
    { "object_du/gDaruniaDancingLoop3Anim", kJoy, 0.0f },
    { "object_du/gDaruniaDancingLoop4Anim", kJoy, 0.0f },
    { "object_kz/gKzMweepAnim", kSad, 0.0f },
    { "object_zo/gZoraHandsOnHipsTappingFootAnim", kBored, 0.0f },
    { "object_ru1/gRutoChildSquirmAnim", kAngry, 0.0f },
    { "object_ru1/gRutoChildShutterAnim", kFear, 0.0f },
    { "object_ru1/gRutoChildSeesSapphireAnim", kSurprise, 0.0f },
    { "object_ru1/gRutoChildWaitHandsOnHipsAnim", kSmug, 0.0f },
    { "object_ru2/gAdultRutoCrossingArmsAnim", kSmug, 0.0f },
    { "object_ru2/gAdultRutoIdleHandsOnHipsAnim", kSmug, 0.0f },
    { "object_ge1/gGerudoWhiteClapAnim", kHappy, 0.0f },
    { "object_ge1/gGerudoWhiteDismissiveAnim", kSmug, 0.0f },
    { "object_nb/gNabooruOnAllFoursAnim", kTired, 0.0f },
    { "object_nb/gNabooruKneeingAtCrawlspaceAnim", kStrain, 0.0f },
    // Villains and bosses with faces
    { "object_gndd/gYoungGanondorfLaughLoopAnim", kSmug, 0.0f },
    { "object_gndd/gYoungGanondorfLaughStartAnim", kSmug, 0.0f },
    { "object_gndd/gYoungGanondorfArmsCrossedAnim", kSmug, 0.0f },
    { "object_ganon_anime2/gGanondorfLaughWhileLeaningAnim", kSmug, 0.0f },
    { "object_ganon_anime2/gGanondorfYellLoopAnim", kAngry, 0.3f },
    { "object_ganon_anime2/gGanondorfYellStartAnim", kAngry, 0.0f },
    { "object_ganon_anime2/gGanondorfVomitLoopAnim", kPain, 0.0f },
    { "object_ganon_anime2/gGanondorfCollapseAnim", kPain, 0.0f },
    { "object_ganon_anime2/gGanondorfDefeatedLoopAnim", kPain, 0.0f },
    { "object_ganon_anime1/gGanondorfDamageAnim", kPain, 0.0f },
    { "object_ganon_anime1/gGanondorfDownedAnim", kPain, 0.0f },
    { "object_ganon_anime3/gGanondorfFloatingHeavyBreathingAnim", kTired, 0.3f },
    { "object_skj/gSkullKidFightingStanceAnim", kFocused, 0.0f },
    { "object_skj/gSkullKidHitAnim", kPain, 0.0f },
    { "object_anubice/gAnubiceLaughingAnim", kSmug, 0.0f },
    { "object_sk2/gStalfosLaughAnim", kSmug, 0.0f },
    { "object_rd/gGibdoRedeadSobbingAnim", kSad, 0.0f },
    { "object_rd/gGibdoRedeadWipingTearsAnim", kSad, 0.0f },
    { "object_po_sisters/gPoeSistersMegCryAnim", kSad, 0.0f },
    { "object_zf/gZfCryingAnim", kSad, 0.0f },
    { "object_shopnuts/gBusinessScrubNervousIdleAnim", kWorried, 0.0f },
    { "object_shopnuts/gBusinessScrubNervousTransitionAnim", kWorried, 0.0f },
    { "object_dns/gDntJijiCowerAnim", kFear, 0.0f },
    { "object_dekunuts/gDekuNutsGaspAnim", kSurprise, 0.0f },
    { "object_dekunuts/gDekuNutsDamageAnim", kPain, 0.0f },
};

bool Enabled() {
    return CVarGetInteger(CVAR_MODERN_ANIM_ENABLED, 1) != 0 && CVarGetInteger(CVAR_MODERN_ANIM_NPC_FACES, 1) != 0;
}

const std::unordered_map<std::string_view, const AnimMood*>& MoodTable() {
    static const auto table = [] {
        std::unordered_map<std::string_view, const AnimMood*> t;
        for (const AnimMood& entry : kAnimMoods) {
            t[entry.name] = &entry;
        }
        return t;
    }();
    return table;
}

const AnimMood* MoodForAnimation(const void* animation) {
    if (animation == nullptr) {
        return nullptr;
    }
    // SoH animations are resource paths such as "__OTR__objects/object_md/gMidoHaltAnim"
    static constexpr std::string_view kPrefix = "__OTR__objects/";
    const char* path = (const char*)animation;
    if (strncmp(path, kPrefix.data(), kPrefix.size()) != 0) {
        return nullptr;
    }
    auto it = MoodTable().find(std::string_view(path + kPrefix.size()));
    return it != MoodTable().end() ? it->second : nullptr;
}

// Own random generator: the game's must not be touched
struct Rng {
    u32 s;
    f32 Next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s & 0xFFFFFF) / 16777215.0f;
    }
    f32 Range(f32 a, f32 b) {
        return a + (b - a) * Next();
    }
};

struct FaceState {
    u32 seenFrame = 0;
    Rng rng;
    Expression shown = kExpressions[kNeutral];
    Mood mood = kNeutral;
    // Blinks
    s32 blinkTimer = 0;
    s32 blinkPhase = -1;
    bool doubleBlink = false;
    // Gaze
    f32 lookWeight = 0.0f;
    Vec3f lookTarget = { 0.0f, 0.0f, 0.0f };
    bool hasLookTarget = false;
    f32 saccade[2] = { 0.0f, 0.0f };
    f32 saccadeGoal[2] = { 0.0f, 0.0f };
    s32 saccadeTimer = 0;
    // Context
    s32 surpriseFrames = 0;
    s32 framesSinceLinkNear = 1000;
    NpcFaceParams params = {};
};

std::unordered_map<Actor*, FaceState> sFaces;
std::vector<SkelAnime*> sSkeletons;
u32 sFrame = 1;

// Blink curve: frames into the blink -> lid closure (close fast, open slower)
constexpr f32 kBlinkCurve[] = { 0.55f, 1.0f, 0.75f, 0.4f, 0.15f };
constexpr s32 kBlinkFrames = ARRAY_COUNT(kBlinkCurve);

bool HasFace(Actor* actor) {
    switch (actor->category) {
        case ACTORCAT_NPC:
        case ACTORCAT_ENEMY:
        case ACTORCAT_BOSS:
        case ACTORCAT_PROP: // a few characters live here
            return true;
        default:
            return false;
    }
}

f32 Approach(f32 cur, f32 target, f32 rate) {
    return cur + (target - cur) * rate;
}

void UpdateFace(PlayState* play, Actor* actor, SkelAnime* skel) {
    FaceState& face = sFaces[actor];
    if (face.seenFrame == 0) {
        face.rng.s = 0x9E3779B9u ^ (u32)(uintptr_t)actor ^ ((u32)actor->id << 16) ^ (u32)actor->params;
        face.blinkTimer = (s32)face.rng.Range(10.0f, 60.0f);
    }
    face.seenFrame = sFrame;

    Player* player = GET_PLAYER(play);
    Vec3f linkHead = player->bodyPartsPos[PLAYER_BODYPART_HEAD];
    Vec3f eyes = actor->focus.pos;
    f32 dist = Math_Vec3f_DistXYZ(&eyes, &linkHead);
    s16 yawToLink = Math_Vec3f_Yaw(&actor->world.pos, &player->actor.world.pos);
    s16 facing = ABS((s16)(yawToLink - actor->shape.rot.y));
    bool talking = play->msgCtx.msgMode != MSGMODE_NONE && player->talkActor == actor;
    bool linkNear = dist < 220.0f;
    bool linkInView = dist < 500.0f && facing < 0x5000;

    // ---- Mood: the animation first, then what is going on around the character ----
    const AnimMood* animMood = skel != nullptr ? MoodForAnimation(skel->animation) : nullptr;
    Mood mood = animMood != nullptr ? animMood->mood : kNeutral;
    f32 intensity = 1.0f;
    if (animMood != nullptr && animMood->pulse > 0.0f && skel->animLength > 1.0f) {
        // Effort comes in beats with the motion: strongest at the peak of each loop
        f32 phase = skel->curFrame / skel->animLength;
        intensity = 1.0f - animMood->pulse + animMood->pulse * (0.5f - 0.5f * cosf(phase * 2.0f * (f32)M_PI));
    }
    if (animMood == nullptr) {
        if (actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS) {
            mood = linkInView ? kFocused : kNeutral;
        } else if (talking) {
            mood = kHappy;
            intensity = 0.6f;
        } else if (linkNear && Player_GetMeleeWeaponHeld(player) != 0) {
            mood = kWorried;
            intensity = 0.6f;
        }
    }
    // Noticing Link after a while away
    if (linkNear && face.framesSinceLinkNear > 100 && animMood == nullptr && actor->category == ACTORCAT_NPC) {
        face.surpriseFrames = 12;
    }
    face.framesSinceLinkNear = linkNear ? 0 : face.framesSinceLinkNear + 1;
    if (face.surpriseFrames > 0) {
        face.surpriseFrames--;
        mood = kSurprise;
        intensity = 0.7f;
    }
    // Taking a hit (the game flashes the actor)
    if (actor->colorFilterTimer > 0 && (actor->category == ACTORCAT_ENEMY || actor->category == ACTORCAT_BOSS)) {
        mood = kPain;
        intensity = 1.0f;
    }
    // Test hook (tools/anim-test): show one mood on every face
    s32 forced = CVarGetInteger("gSettings.ModernAnimation.NpcFaceForceMood", -1);
    if (forced >= 0 && forced < kNumMoods) {
        mood = (Mood)forced;
        intensity = 1.0f;
    }
    face.mood = mood;

    Expression goal = kExpressions[kNeutral];
    const Expression& target = kExpressions[mood];
    goal.upperLid += (target.upperLid - goal.upperLid) * intensity;
    goal.lowerLid += (target.lowerLid - goal.lowerLid) * intensity;
    goal.tilt += (target.tilt - goal.tilt) * intensity;
    goal.pupil += (target.pupil - goal.pupil) * intensity;
    // Faces change over a few frames, faster into surprise and pain
    f32 rate = (mood == kSurprise || mood == kPain) ? 0.6f : 0.25f;
    face.shown.upperLid = Approach(face.shown.upperLid, goal.upperLid, rate);
    face.shown.lowerLid = Approach(face.shown.lowerLid, goal.lowerLid, rate);
    face.shown.tilt = Approach(face.shown.tilt, goal.tilt, rate);
    face.shown.pupil = Approach(face.shown.pupil, goal.pupil, rate);

    // ---- Gaze ----
    bool lookAtLink = talking || (linkInView && actor->category != ACTORCAT_PROP);
    Vec3f lookGoal = linkHead;
    f32 weightGoal = lookAtLink ? 1.0f : 0.0f;
    // Characters busy with something look at their work, not at Link, unless he talks to them
    if (animMood != nullptr && animMood->pulse > 0.0f && !talking) {
        weightGoal = 0.0f;
    }
    bool bigShift = false;
    if (weightGoal > 0.0f) {
        if (face.hasLookTarget && Math_Vec3f_DistXYZ(&face.lookTarget, &lookGoal) > 60.0f) {
            bigShift = true;
        }
        face.lookTarget = lookGoal;
        face.hasLookTarget = true;
    }
    if ((weightGoal > 0.5f) != (face.lookWeight > 0.5f)) {
        bigShift = true;
    }
    // Eyes move fast (saccades), not smoothly like heads
    face.lookWeight = Approach(face.lookWeight, weightGoal, 0.7f);

    // Small saccades around the target, more often while idle
    if (--face.saccadeTimer <= 0) {
        f32 range = lookAtLink ? 0.08f : 0.3f;
        face.saccadeGoal[0] = face.rng.Range(-range, range);
        face.saccadeGoal[1] = face.rng.Range(-range, range) * 0.5f;
        face.saccadeTimer = (s32)face.rng.Range(lookAtLink ? 12.0f : 8.0f, lookAtLink ? 40.0f : 30.0f);
        if (!lookAtLink && face.rng.Next() < 0.3f) {
            bigShift = true;
        }
    }
    face.saccade[0] = Approach(face.saccade[0], face.saccadeGoal[0], 0.7f);
    face.saccade[1] = Approach(face.saccade[1], face.saccadeGoal[1], 0.7f);

    // ---- Blinks: every 2-5 s, sometimes double, often with a big eye movement ----
    f32 blink = 0.0f;
    if (face.blinkPhase >= 0) {
        blink = kBlinkCurve[face.blinkPhase];
        if (++face.blinkPhase >= kBlinkFrames) {
            face.blinkPhase = face.doubleBlink ? 1 : -1;
            face.doubleBlink = false;
        }
    } else if (--face.blinkTimer <= 0 || (bigShift && face.rng.Next() < 0.6f && face.blinkTimer < 50)) {
        face.blinkPhase = 0;
        face.doubleBlink = face.rng.Next() < 0.12f;
        // Tired and sleepy characters blink slowly and more often
        bool drowsy = mood == kTired || mood == kSleepy || mood == kBored;
        face.blinkTimer = (s32)face.rng.Range(drowsy ? 25.0f : 40.0f, drowsy ? 60.0f : 100.0f);
    }

    NpcFaceParams& p = face.params;
    p.lookTarget[0] = face.lookTarget.x;
    p.lookTarget[1] = face.lookTarget.y;
    p.lookTarget[2] = face.lookTarget.z;
    // Eyes lead the head only partly: full weight sends the iris to the corner of the eye
    p.lookWeight = face.hasLookTarget ? face.lookWeight * 0.6f : 0.0f;
    p.saccade[0] = face.saccade[0];
    p.saccade[1] = face.saccade[1];
    p.upperLid = std::clamp(face.shown.upperLid + (1.0f - face.shown.upperLid) * blink, 0.0f, 1.0f);
    p.lowerLid = std::clamp(face.shown.lowerLid, 0.0f, 1.0f);
    p.tilt = std::clamp(face.shown.tilt, -1.0f, 1.0f);
    p.pupil = face.shown.pupil;
    p.flags = G_FACEPARAMS_ACTIVE;
}

// Once per frame, before drawing: work out every face from this frame's (vanilla) game state
void UpdateAll() {
    PlayState* play = gPlayState;
    if (play == nullptr || !Enabled()) {
        sSkeletons.clear();
        return;
    }
    // Each actor's main skeleton: the largest one that lives inside the actor's instance
    struct Range {
        uintptr_t begin, end;
        Actor* actor;
    };
    std::vector<Range> ranges;
    for (s32 cat = 0; cat < ACTORCAT_MAX; cat++) {
        for (Actor* a = play->actorCtx.actorLists[cat].head; a != nullptr; a = a->next) {
            if (HasFace(a)) {
                uintptr_t begin = (uintptr_t)a;
                ranges.push_back({ begin, begin + ActorDB::Instance->RetrieveEntry(a->id).entry.instanceSize, a });
            }
        }
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.begin < b.begin; });
    std::unordered_map<Actor*, SkelAnime*> mainSkel;
    for (SkelAnime* skel : sSkeletons) {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), (uintptr_t)skel,
                                   [](uintptr_t p, const Range& r) { return p < r.begin; });
        if (it == ranges.begin()) {
            continue;
        }
        --it;
        if ((uintptr_t)skel >= it->end) {
            continue;
        }
        SkelAnime*& best = mainSkel[it->actor];
        if (best == nullptr || skel->limbCount > best->limbCount) {
            best = skel;
        }
    }
    sSkeletons.clear();
    for (auto& [actor, skel] : mainSkel) {
        UpdateFace(play, actor, skel);
    }
    std::erase_if(sFaces, [](const auto& entry) { return sFrame - entry.second.seenFrame > 30; });
    sFrame++;
}

void ForgetActor(Actor* actor) {
    sFaces.erase(actor);
}

void RegisterNpcFace() {
    COND_HOOK(OnPlayDrawBegin, true, UpdateAll);
    COND_HOOK(OnActorDestroy, true, [](void* actor) { ForgetActor((Actor*)actor); });
    COND_HOOK(ShouldActorDestroy, true, [](void* actor, bool* result) { ForgetActor((Actor*)actor); });
    COND_HOOK(OnPlayDestroy, true, [] {
        sFaces.clear();
        sSkeletons.clear();
    });
}

} // namespace

static RegisterShipInitFunc initFunc(RegisterNpcFace);

static_assert(sizeof(NpcFaceParams) == sizeof(GfxFaceParams), "NpcFaceParams must match the renderer's GfxFaceParams");

// The renderer draws lids, gaze and the new iris on eye textures from these parameters. Kokiri and others fade
// in on the translucent list, so both lists get them; null resets to vanilla eyes after the actor.
// Outside the anonymous namespace: OPEN_DISPS declares functions it calls
static void EmitFaceParams(PlayState* play, const NpcFaceParams* params) {
    GfxFaceParams* copy = nullptr;
    if (params != nullptr) {
        copy = (GfxFaceParams*)Graph_Alloc(play->state.gfxCtx, sizeof(GfxFaceParams));
        memcpy(copy, params, sizeof(GfxFaceParams));
    }
    OPEN_DISPS(play->state.gfxCtx);
    gSPSetFaceParams(POLY_OPA_DISP++, copy);
    gSPSetFaceParams(POLY_XLU_DISP++, copy);
    CLOSE_DISPS(play->state.gfxCtx);
}

extern "C" {

void NpcFace_NoteSkeleton(SkelAnime* skelAnime) {
    sSkeletons.push_back(skelAnime);
}

void NpcFace_BeginActorDraw(PlayState* play, Actor* actor) {
    if (sFaces.empty()) {
        return;
    }
    auto it = sFaces.find(actor);
    if (it != sFaces.end() && it->second.seenFrame + 1 >= sFrame) {
        EmitFaceParams(play, &it->second.params);
    }
}

void NpcFace_EndActorDraw(PlayState* play, Actor* actor) {
    if (sFaces.empty()) {
        return;
    }
    auto it = sFaces.find(actor);
    if (it != sFaces.end() && it->second.seenFrame + 1 >= sFrame) {
        EmitFaceParams(play, nullptr);
    }
}

const NpcFaceParams* NpcFace_Get(Actor* actor, s32* mood) {
    auto it = sFaces.find(actor);
    if (it == sFaces.end()) {
        return nullptr;
    }
    *mood = it->second.mood;
    return &it->second.params;
}

const char* NpcFace_MoodName(s32 mood) {
    return mood >= 0 && mood < kNumMoods ? kMoodNames[mood] : "?";
}

} // extern "C"
