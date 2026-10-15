#include "FaceRig.h"
#include "PlayerModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
}

namespace PlayerModel {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

// Blendshapes the face pass authors (tools/player-model/face.py).
enum Shape {
    kBlinkL, kBlinkR, kSquintL, kSquintR, kWideL, kWideR,
    kBrowUpL, kBrowUpR, kBrowDownL, kBrowDownR, kBrowInnerUpL, kBrowInnerUpR,
    kSmileL, kSmileR, kFrownL, kFrownR, kCheekL, kCheekR, kJawOpen, kPucker,
    kNumShapes,
};
const char* const kShapeNames[kNumShapes] = {
    "blinkL", "blinkR", "squintL", "squintR", "wideL", "wideR",
    "browUpL", "browUpR", "browDownL", "browDownR", "browInnerUpL", "browInnerUpR",
    "smileL", "smileR", "frownL", "frownR", "cheekL", "cheekR", "jawOpen", "pucker",
};

// A facial pose, symmetric unless both sides are set separately.
struct Face {
    float w[kNumShapes] = {};

    Face& Sym(Shape left, float v) {
        w[left] = v;
        w[left + 1] = v;
        return *this;
    }
    Face& Set(Shape s, float v) {
        w[s] = v;
        return *this;
    }
    void AddScaled(const Face& o, float k) {
        for (int i = 0; i < kNumShapes; i++) {
            w[i] += o.w[i] * k;
        }
    }
};

// Expression library. Each is a full face; they are layered by weight below.
Face Determined() {
    return Face().Sym(kBrowDownL, 0.7f).Sym(kSquintL, 0.3f).Sym(kFrownL, 0.15f);
}
Face Effort() { // sword swings: a short battle cry
    return Face().Sym(kBrowDownL, 1.0f).Sym(kSquintL, 0.6f).Set(kJawOpen, 0.55f).Sym(kCheekL, 0.3f);
}
Face Strain() { // climbing, lifting, pushing
    return Face().Sym(kBrowDownL, 0.5f).Sym(kSquintL, 0.65f).Set(kJawOpen, 0.12f).Sym(kFrownL, 0.35f).Sym(kCheekL, 0.25f);
}
Face Pain() {
    return Face().Sym(kBlinkL, 0.75f).Sym(kSquintL, 1.0f).Sym(kBrowDownL, 0.5f).Sym(kBrowInnerUpL, 0.7f)
        .Set(kJawOpen, 0.45f).Sym(kFrownL, 0.6f);
}
Face Joy() {
    return Face().Sym(kSmileL, 1.0f).Sym(kCheekL, 0.8f).Sym(kBrowUpL, 0.8f).Sym(kWideL, 0.3f).Set(kJawOpen, 0.4f);
}
Face Content() {
    return Face().Sym(kSmileL, 0.35f).Sym(kCheekL, 0.2f).Sym(kBrowUpL, 0.1f);
}
Face Fear() {
    return Face().Sym(kWideL, 1.0f).Sym(kBrowInnerUpL, 1.0f).Sym(kBrowUpL, 0.6f).Set(kJawOpen, 0.6f);
}
Face Surprise() {
    return Face().Sym(kWideL, 0.9f).Sym(kBrowUpL, 1.0f).Set(kJawOpen, 0.35f);
}
Face Worried() {
    return Face().Sym(kBrowInnerUpL, 0.8f).Sym(kFrownL, 0.35f).Sym(kSquintL, 0.1f);
}
Face Sad() {
    return Face().Sym(kBrowInnerUpL, 0.9f).Sym(kFrownL, 0.55f).Sym(kBlinkL, 0.2f);
}
Face Attentive() {
    return Face().Sym(kBrowUpL, 0.3f).Sym(kWideL, 0.2f);
}
Face HoldBreath() {
    return Face().Sym(kCheekL, 0.7f).Set(kPucker, 0.5f).Sym(kSquintL, 0.35f);
}
Face Ocarina() {
    return Face().Sym(kBlinkL, 0.5f).Sym(kCheekL, 0.5f).Set(kPucker, 0.7f).Sym(kBrowUpL, 0.2f);
}
Face HeatSquint() {
    return Face().Sym(kSquintL, 0.6f).Sym(kBrowDownL, 0.3f).Set(kJawOpen, 0.2f);
}
Face Yawn() {
    return Face().Set(kJawOpen, 1.0f).Sym(kBlinkL, 0.75f).Sym(kBrowInnerUpL, 0.5f).Sym(kBrowUpL, 0.3f);
}

struct Rng {
    uint32_t s = 0x9E3779B9u;
    float Next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s & 0xFFFFFF) / 16777215.0f;
    }
    float Range(float a, float b) {
        return a + (b - a) * Next();
    }
};

struct State {
    const Model* model = nullptr;
    int shapeMorph[kNumShapes];
    int eyeBone[2] = { -1, -1 }; // R, L
    int lidBone[2] = { -1, -1 };
    float eyeCentre[2][3];

    Rng rng;
    s32 lastFrame = -1;
    bool haveFrame = false;

    Face current;
    // blink
    int blinkTimer = 40;
    int blinkPhase = -1; // -1 idle, else frames into the blink
    bool doubleBlink = false;
    float blink = 0.0f;
    // gaze
    float gazeYaw = 0, gazePitch = 0;           // smoothed, radians
    float saccadeYaw = 0, saccadePitch = 0;     // offset around the target
    int saccadeTimer = 10;
    // scenario timers (frames)
    int painFrames = 0;
    int effortFrames = 0;
    int surpriseFrames = 0;
    int idleFrames = 0;
    int yawnFrames = 0;
    s8 lastInvincibility = 0;
    u32 lastState1 = 0;
    float lidAngle = 0.0f;
};

State sState;

void Setup(const Model& model) {
    sState = State();
    sState.model = &model;
    for (int i = 0; i < kNumShapes; i++) {
        sState.shapeMorph[i] = model.FindMorph(kShapeNames[i]);
    }
    const char* eyes[2] = { "EyeR", "EyeL" };
    const char* lids[2] = { "LidR", "LidL" };
    for (int i = 0; i < 2; i++) {
        sState.eyeBone[i] = model.FindBone(eyes[i]);
        sState.lidBone[i] = model.FindBone(lids[i]);
        if (sState.eyeBone[i] >= 0) {
            const Mat4& b = model.bones[sState.eyeBone[i]].bind;
            sState.eyeCentre[i][0] = b.m[3][0];
            sState.eyeCentre[i][1] = b.m[3][1];
            sState.eyeCentre[i][2] = b.m[3][2];
        }
    }
}

// Vanilla face textures the animation asked for, as an expression and an optional gaze.
void VanillaFace(s32 eyeIndex, s32 mouthIndex, Face& f, float& blinkFloor, float& yaw, float& pitch, bool& hasGaze) {
    switch (eyeIndex) {
        case 1: blinkFloor = 0.5f; break;                              // half closed
        case 2: blinkFloor = 1.0f; break;                              // closed
        case 3: yaw = 25 * kDeg; hasGaze = true; break;                // look left
        case 4: yaw = -25 * kDeg; hasGaze = true; break;               // look right
        case 5: f.AddScaled(Surprise(), 0.9f); break;                  // wide / shocked
        case 6: pitch = -15 * kDeg; hasGaze = true; break;             // look down
        case 7: blinkFloor = 1.0f; f.Sym(kSquintL, 0.5f); break;       // squeezed shut
        default: break;
    }
    switch (mouthIndex) {
        case 1: f.w[kJawOpen] = std::max(f.w[kJawOpen], 0.3f); break;
        case 2: f.w[kJawOpen] = std::max(f.w[kJawOpen], 0.8f); break;
        case 3: f.Sym(kSmileL, 0.8f); f.w[kJawOpen] = std::max(f.w[kJawOpen], 0.35f); break;
        default: break;
    }
}

// The story so far sets Link's resting mood.
Face StoryMood(PlayState* play) {
    Face f;
    const bool dekuTreeDead = Flags_GetEventChkInf(EVENTCHKINF_OBTAINED_KOKIRI_EMERALD_DEKU_TREE_DEAD);
    const bool metZelda = Flags_GetEventChkInf(EVENTCHKINF_OBTAINED_ZELDAS_LETTER);
    const bool hasOcarinaOfTime = Flags_GetEventChkInf(EVENTCHKINF_OBTAINED_OCARINA_OF_TIME);
    if (!dekuTreeDead) {
        f.AddScaled(Content(), 0.6f); // a carefree kid in the forest
    } else if (!metZelda) {
        // grieving the Great Deku Tree, more so at home in the forest
        const bool inForest = play->sceneNum == SCENE_KOKIRI_FOREST || play->sceneNum == SCENE_LOST_WOODS ||
                              play->sceneNum == SCENE_LINKS_HOUSE;
        f.AddScaled(Sad(), inForest ? 0.6f : 0.3f);
    } else if (!hasOcarinaOfTime) {
        f.AddScaled(Determined(), 0.3f); // on a mission from the princess
    } else {
        f.AddScaled(Determined(), 0.45f);
    }
    return f;
}

void StepBlink(float rateScale) {
    State& st = sState;
    if (st.blinkPhase < 0) {
        if (--st.blinkTimer <= 0) {
            st.blinkPhase = 0;
            st.doubleBlink = st.rng.Next() < 0.15f;
        }
    }
    // 20 Hz: close over 2 frames, hold 1, open over 2
    static const float kCurve[] = { 0.55f, 1.0f, 1.0f, 0.5f, 0.15f };
    if (st.blinkPhase >= 0) {
        st.blink = kCurve[st.blinkPhase];
        if (++st.blinkPhase >= (int)(sizeof(kCurve) / sizeof(kCurve[0]))) {
            if (st.doubleBlink) {
                st.doubleBlink = false;
                st.blinkPhase = 0;
            } else {
                st.blinkPhase = -1;
                st.blinkTimer = (int)(st.rng.Range(40.0f, 110.0f) / rateScale);
            }
        }
    } else {
        st.blink = 0.0f;
    }
}

bool ActorWorldPos(Actor* a, float out[3]) {
    if (a == nullptr) {
        return false;
    }
    out[0] = a->focus.pos.x;
    out[1] = a->focus.pos.y;
    out[2] = a->focus.pos.z;
    return true;
}

} // namespace

void FaceRig_Update(PlayState* play, Player* player, const Model& model, s32 eyeIndex, s32 mouthIndex,
                    const Mat4& base, Mat4* pose, float* morphWeights) {
    if (sState.model != &model) {
        Setup(model);
    }
    State& st = sState;

    const s32 frame = play != nullptr ? play->gameplayFrames : 0;
    const bool newFrame = !st.haveFrame || frame != st.lastFrame;
    st.lastFrame = frame;
    st.haveFrame = true;

    Face target;
    float blinkFloor = 0.0f;
    float animYaw = 0.0f, animPitch = 0.0f;
    bool animGaze = false;
    float blinkRate = 1.0f;
    float lookTarget[3];
    bool hasLook = false;
    float gazeSpread = 1.0f;

    VanillaFace(eyeIndex, mouthIndex, target, blinkFloor, animYaw, animPitch, animGaze);

    if (player != nullptr && play != nullptr) {
        const u32 s1 = player->stateFlags1, s2 = player->stateFlags2;
        const bool cutscene = (s1 & (PLAYER_STATE1_IN_CUTSCENE | PLAYER_STATE1_IN_ITEM_CS)) != 0;

        if (newFrame) {
            // Events that start a timed reaction.
            if (player->invincibilityTimer > 0 && st.lastInvincibility <= 0) {
                st.painFrames = 14;
            }
            if ((s1 & PLAYER_STATE1_DAMAGED) && !(st.lastState1 & PLAYER_STATE1_DAMAGED)) {
                st.painFrames = 14;
            }
            if (player->meleeWeaponState != 0) {
                st.effortFrames = 6;
            }
            if ((s1 & PLAYER_STATE1_FREEFALL) && player->actor.velocity.y < -12.0f) {
                st.surpriseFrames = 4;
            }
            st.lastInvincibility = player->invincibilityTimer;
            st.lastState1 = s1;

            const bool idle = player->actor.speedXZ < 0.5f && !(s1 & (PLAYER_STATE1_TALKING | PLAYER_STATE1_HOSTILE_LOCK_ON)) && !cutscene;
            st.idleFrames = idle ? st.idleFrames + 1 : 0;
            if (st.idleFrames > 20 * 20 && st.yawnFrames == 0 && st.rng.Next() < 0.004f) {
                st.yawnFrames = 45;
            }
            if (st.painFrames > 0) st.painFrames--;
            if (st.effortFrames > 0) st.effortFrames--;
            if (st.surpriseFrames > 0) st.surpriseFrames--;
            if (st.yawnFrames > 0) st.yawnFrames--;
        }

        if (!cutscene) {
            // Resting mood from the story, then the situation on top.
            target.AddScaled(StoryMood(play), 1.0f);

            const s16 health = gSaveContext.health;
            if (health > 0 && health <= 0x10) {
                target.AddScaled(Worried(), 0.8f);
                blinkRate *= 1.5f;
                // short breaths
                target.w[kJawOpen] += 0.12f + 0.08f * std::sin(frame * 0.4f);
            }
            if (s1 & PLAYER_STATE1_HOSTILE_LOCK_ON) {
                target.AddScaled(Determined(), 0.8f);
            }
            if (s1 & PLAYER_STATE1_SHIELDING) {
                target.AddScaled(Strain(), 0.5f);
            }
            if (s1 & PLAYER_STATE1_CHARGING_SPIN_ATTACK) {
                target.AddScaled(Effort(), 0.6f);
            }
            if (s1 & (PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_CLIMBING_LADDER |
                      PLAYER_STATE1_CARRYING_ACTOR)) {
                target.AddScaled(Strain(), 0.8f);
            }
            if (s1 & PLAYER_STATE1_GETTING_ITEM) {
                target.AddScaled(Joy(), 1.0f);
            }
            if (s2 & PLAYER_STATE2_UNDERWATER) {
                target.AddScaled(HoldBreath(), 1.0f);
                blinkRate *= 0.3f;
            }
            if (s2 & PLAYER_STATE2_OCARINA_PLAYING) {
                target.AddScaled(Ocarina(), 1.0f);
                blinkRate *= 0.5f;
            }
            if (s1 & PLAYER_STATE1_TALKING) {
                target.AddScaled(Attentive(), 0.8f);
                blinkRate *= 1.3f;
            }
            if ((play->sceneNum == SCENE_DEATH_MOUNTAIN_CRATER || play->sceneNum == SCENE_FIRE_TEMPLE) &&
                player->currentTunic != PLAYER_TUNIC_GORON) {
                target.AddScaled(HeatSquint(), 0.8f);
            }
            if (gSaveContext.nightFlag) {
                blinkRate *= 1.2f;
                target.Sym(kBlinkL, target.w[kBlinkL] + 0.1f);
            }
            if (st.effortFrames > 0) {
                target.AddScaled(Effort(), 1.0f);
            }
            if (st.surpriseFrames > 0 || ((s1 & PLAYER_STATE1_FREEFALL) && player->actor.velocity.y < -12.0f)) {
                target.AddScaled(Fear(), 0.9f);
            } else if (s1 & PLAYER_STATE1_JUMPING) {
                target.AddScaled(Surprise(), 0.3f);
            }
            if (st.yawnFrames > 0) {
                float k = std::sin((45 - st.yawnFrames) / 45.0f * kPi);
                target.AddScaled(Yawn(), k);
            }
            if (st.painFrames > 0 || (s1 & PLAYER_STATE1_DEAD)) {
                Face pain = Pain();
                target.AddScaled(pain, 1.2f);
            }

            // Where to look: lock-on / talk partner, Navi when she calls, else ahead.
            Actor* focus = player->focusActor;
            if (focus == nullptr && (s1 & PLAYER_STATE1_TALKING)) {
                focus = player->talkActor;
            }
            if (focus == nullptr && (s2 & PLAYER_STATE2_NAVI_ALERT)) {
                focus = player->naviActor;
            }
            if (focus == nullptr && player->talkActor != nullptr && player->talkActorDistance < 160.0f) {
                focus = player->talkActor; // glance at someone close by
            }
            hasLook = ActorWorldPos(focus, lookTarget);
            if (st.idleFrames > 100) {
                gazeSpread = 2.5f; // idle: let the eyes wander
            }
        }
    }

    if (newFrame) {
        StepBlink(blinkRate);
        if (--st.saccadeTimer <= 0) {
            st.saccadeTimer = (int)st.rng.Range(8.0f, 50.0f);
            st.saccadeYaw = st.rng.Range(-6.0f, 6.0f) * kDeg * gazeSpread;
            st.saccadePitch = st.rng.Range(-3.5f, 3.5f) * kDeg * gazeSpread;
        }
    }

    // Gaze target in the head's bind frame.
    float yaw = animYaw, pitch = animPitch;
    if (!animGaze && hasLook && st.eyeBone[0] >= 0) {
        const Mat4 headWorld = Mul(base, Mul(pose[kLimbHead], model.bones[kLimbHead].invBind));
        const Mat4 inv = Inverse(headWorld);
        float local[3];
        TransformPoint(inv, lookTarget, local);
        const float cx = 0.5f * (st.eyeCentre[0][0] + st.eyeCentre[1][0]);
        const float cy = 0.5f * (st.eyeCentre[0][1] + st.eyeCentre[1][1]);
        const float cz = 0.5f * (st.eyeCentre[0][2] + st.eyeCentre[1][2]);
        const float dx = local[0] - cx, dy = local[1] - cy, dz = local[2] - cz;
        if (dz > 1.0f) {
            yaw = std::atan2(dx, dz);
            pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
        }
    }
    yaw = std::clamp(yaw + st.saccadeYaw, -32 * kDeg, 32 * kDeg);
    pitch = std::clamp(pitch + st.saccadePitch, -22 * kDeg, 22 * kDeg);
    if (newFrame) {
        // Eyes move fast (saccades), but not in a single frame.
        st.gazeYaw += (yaw - st.gazeYaw) * 0.6f;
        st.gazePitch += (pitch - st.gazePitch) * 0.6f;
    }

    // Ease the expression so it never snaps.
    if (newFrame) {
        for (int i = 0; i < kNumShapes; i++) {
            const float t = std::clamp(target.w[i], 0.0f, 1.2f);
            const float rate = (st.painFrames > 0 || st.effortFrames > 0) ? 0.6f : 0.25f;
            st.current.w[i] += (t - st.current.w[i]) * rate;
        }
    }

    const float blink = std::max(st.blink, blinkFloor);
    for (int i = 0; i < kNumShapes; i++) {
        const int m = st.shapeMorph[i];
        if (m < 0) {
            continue;
        }
        float w = st.current.w[i];
        if (i == kBlinkL || i == kBlinkR) {
            w = std::max(w, blink);
        }
        morphWeights[m] = std::clamp(w, 0.0f, 1.0f);
    }

    // Eyeballs: aim. Lids: close over the eye, follow the gaze up and down a little.
    auto rotX = [](float a) {
        Mat4 r = Identity();
        r.m[1][1] = std::cos(a);
        r.m[1][2] = std::sin(a);
        r.m[2][1] = -std::sin(a);
        r.m[2][2] = std::cos(a);
        return r;
    };
    auto rotY = [](float a) {
        Mat4 r = Identity();
        r.m[0][0] = std::cos(a);
        r.m[0][2] = -std::sin(a);
        r.m[2][0] = std::sin(a);
        r.m[2][2] = std::cos(a);
        return r;
    };
    const Mat4 eyeRot = Mul(rotY(st.gazeYaw), rotX(-st.gazePitch));
    for (int side = 0; side < 2; side++) {
        const float lidClose = std::clamp(std::max(st.current.w[kBlinkR - side] , blink), 0.0f, 1.0f);
        const float squint = st.current.w[kSquintR - side];
        const float wide = st.current.w[kWideR - side];
        if (st.eyeBone[side] >= 0) {
            pose[st.eyeBone[side]] = Mul(pose[st.eyeBone[side]], eyeRot);
        }
        if (st.lidBone[side] >= 0) {
            float angle = 12.0f * kDeg                       // rest: lid just over the top of the iris
                          + lidClose * 62.0f * kDeg          // blink
                          + squint * 10.0f * kDeg - wide * 12.0f * kDeg - st.gazePitch * 0.6f;
            angle = std::clamp(angle, -6.0f * kDeg, 76.0f * kDeg);
            pose[st.lidBone[side]] = Mul(pose[st.lidBone[side]], rotX(angle));
        }
    }
}

} // namespace PlayerModel
