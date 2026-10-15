#pragma once

// In-memory form of an .lmdl player model and the small matrix helpers the skinning uses.
//
// .lmdl layout (little endian), written by tools/player-model/export.py:
//   char[4] "LMDL", u32 version (1 or 2)
//   u32 numVerts, numTris, numBones, numMorphs
//   f32 rootYScale                       hip height of the model / hip height of vanilla child Link
//   per bone:   str name, i32 parent, f32[16] bind (column-major, model units), f32[3] jointPos
//   per vertex: f32[3] pos, s8[3] normal, u8 pad, f32[2] uv, u8[4] bone, u8[4] weight (sum 255)
//   u32[3 * numTris] indices
//   u32 numMaterials; per material: str texture, u32 firstTri, u32 numTris
//   version 2 adds, per morph: str name, u32 count, then count * (u32 vertex, f32[3] delta)
//   and then u32 numTags; per tag: str name, u32 count, u32[count] vertex indices
// str = u32 length + bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace PlayerModel {

// Child Link's skeleton limbs, by array index (the limb callbacks receive index + 1).
enum Limb {
    kLimbRoot = 0,
    kLimbWaist = 1,
    kLimbRightThigh = 3,
    kLimbRightShin = 4,
    kLimbRightFoot = 5,
    kLimbLeftThigh = 6,
    kLimbLeftShin = 7,
    kLimbLeftFoot = 8,
    kLimbUpper = 9,
    kLimbHead = 10,
    kLimbHat = 11,
    kLimbCollar = 12,
    kLimbLeftShoulder = 13,
    kLimbLeftForearm = 14,
    kLimbLeftHand = 15,
    kLimbRightShoulder = 16,
    kLimbRightForearm = 17,
    kLimbRightHand = 18,
    kLimbSheath = 19,
    kLimbTorso = 20,
    kNumLimbs = 21,
};

// The base color is an HD texture addressed as a nominal 32x32 RGBA32 image; the texture
// coordinates are scaled so that tile shift 4 maps the full image onto [0, 1].
constexpr int kNominalTexSize = 32;
constexpr int kTexShift = 4;
constexpr float kTexCoordScale = 16384.0f;

// Column-major 4x4: m[col][row], matching MtxF.
struct Mat4 {
    float m[4][4];
};

Mat4 Identity();
Mat4 Mul(const Mat4& a, const Mat4& b);
Mat4 Inverse(const Mat4& a);
void TransformPoint(const Mat4& a, const float in[3], float out[3]);
void TransformDir(const Mat4& a, const float in[3], float out[3]);

struct Bone {
    std::string name;
    int parent = -1;
    Mat4 bind;
    Mat4 invBind;
    float jointPos[3];
};

struct Vertex {
    float pos[3];
    float nrm[3];
    int16_t tc[2];
    uint8_t bone[4];
    uint8_t weight[4];
};

struct Morph {
    std::string name;
    std::vector<uint32_t> verts;
    std::vector<float> deltas; // 3 per entry
};

struct Tag {
    std::string name;
    std::vector<uint32_t> verts;
};

struct Material {
    std::string texture; // resource path of the base color, e.g. textures/player-model/child-link/basecolor
    uint32_t firstTri;
    uint32_t numTris;
};

// A run of triangles whose vertices fit in one vertex load. Chunks never span materials.
struct Chunk {
    uint32_t material;
    uint32_t firstVert; // into chunkVerts
    uint32_t numVerts;
    uint32_t firstTri; // into chunkTris (in triangles)
    uint32_t numTris;
};

struct Model {
    float rootYScale = 1.0f;
    std::vector<Bone> bones;
    std::vector<Vertex> verts;
    std::vector<uint32_t> tris;
    std::vector<Morph> morphs;
    std::vector<Tag> tags;
    std::vector<Material> materials;

    std::vector<Chunk> chunks;
    std::vector<uint32_t> chunkVerts; // model vertex for each loaded vertex
    std::vector<uint8_t> chunkTris;   // chunk-local indices, 3 per triangle

    int FindBone(const char* name) const;
    int FindMorph(const char* name) const;
    const Tag* FindTag(const char* name) const;
};

bool ParseModel(const char* data, size_t size, Model& out, std::string& error);

// Splits the triangle list into vertex loads of at most maxVerts vertices, growing each load
// across neighbouring triangles so vertices are shared.
void BuildChunks(Model& model, int maxVerts);

} // namespace PlayerModel
