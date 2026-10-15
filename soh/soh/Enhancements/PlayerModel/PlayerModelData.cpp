#include "PlayerModelData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PlayerModel {

Mat4 Identity() {
    Mat4 r{};
    for (int i = 0; i < 4; i++) {
        r.m[i][i] = 1.0f;
    }
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            r.m[c][row] = a.m[0][row] * b.m[c][0] + a.m[1][row] * b.m[c][1] + a.m[2][row] * b.m[c][2] +
                          a.m[3][row] * b.m[c][3];
        }
    }
    return r;
}

Mat4 Inverse(const Mat4& in) {
    // Gauss-Jordan on the row-major transpose view.
    float a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r][c] = in.m[c][r];
            a[r][c + 4] = (r == c) ? 1.0f : 0.0f;
        }
    }
    for (int col = 0; col < 4; col++) {
        int pivot = col;
        for (int r = col + 1; r < 4; r++) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        if (std::fabs(a[pivot][col]) < 1e-12f) {
            return Identity();
        }
        if (pivot != col) {
            for (int c = 0; c < 8; c++) {
                std::swap(a[pivot][c], a[col][c]);
            }
        }
        const float inv = 1.0f / a[col][col];
        for (int c = 0; c < 8; c++) {
            a[col][c] *= inv;
        }
        for (int r = 0; r < 4; r++) {
            if (r != col && a[r][col] != 0.0f) {
                const float f = a[r][col];
                for (int c = 0; c < 8; c++) {
                    a[r][c] -= f * a[col][c];
                }
            }
        }
    }
    Mat4 out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.m[c][r] = a[r][c + 4];
        }
    }
    return out;
}

void TransformPoint(const Mat4& a, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = a.m[0][r] * in[0] + a.m[1][r] * in[1] + a.m[2][r] * in[2] + a.m[3][r];
    }
}

void TransformDir(const Mat4& a, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++) {
        out[r] = a.m[0][r] * in[0] + a.m[1][r] * in[1] + a.m[2][r] * in[2];
    }
}

namespace {

class Reader {
  public:
    Reader(const char* data, size_t size) : mData(data), mSize(size) {
    }

    bool Read(void* dst, size_t n) {
        if (mPos + n > mSize) {
            mFailed = true;
            memset(dst, 0, n);
            return false;
        }
        memcpy(dst, mData + mPos, n);
        mPos += n;
        return true;
    }

    uint32_t U32() {
        uint32_t v;
        Read(&v, 4);
        return v;
    }

    int32_t I32() {
        int32_t v;
        Read(&v, 4);
        return v;
    }

    float F32() {
        float v;
        Read(&v, 4);
        return v;
    }

    std::string Str() {
        uint32_t n = U32();
        if (n > 4096 || mPos + n > mSize) {
            mFailed = true;
            return {};
        }
        std::string s(mData + mPos, n);
        mPos += n;
        return s;
    }

    bool AtEnd() const {
        return mPos >= mSize;
    }
    bool Failed() const {
        return mFailed;
    }

  private:
    const char* mData;
    size_t mSize;
    size_t mPos = 0;
    bool mFailed = false;
};

} // namespace

bool ParseModel(const char* data, size_t size, Model& out, std::string& error) {
    Reader r(data, size);
    char magic[4];
    r.Read(magic, 4);
    if (memcmp(magic, "LMDL", 4) != 0) {
        error = "bad magic";
        return false;
    }
    const uint32_t version = r.U32();
    if (version < 1 || version > 2) {
        error = "unsupported version " + std::to_string(version);
        return false;
    }
    const uint32_t numVerts = r.U32();
    const uint32_t numTris = r.U32();
    const uint32_t numBones = r.U32();
    const uint32_t numMorphs = r.U32();
    if (numBones < kNumLimbs || numBones > 255 || numVerts > 1000000 || numTris > 2000000 || numMorphs > 256) {
        error = "bad counts";
        return false;
    }
    out.rootYScale = r.F32();

    out.bones.resize(numBones);
    for (Bone& b : out.bones) {
        b.name = r.Str();
        b.parent = r.I32();
        r.Read(&b.bind.m[0][0], sizeof(float) * 16);
        r.Read(b.jointPos, sizeof(float) * 3);
        b.invBind = Inverse(b.bind);
    }
    for (size_t i = kNumLimbs; i < numBones; i++) {
        if (out.bones[i].parent < 0 || out.bones[i].parent >= (int)i) {
            error = "extra bone " + out.bones[i].name + " has a bad parent";
            return false;
        }
    }

    out.verts.resize(numVerts);
    for (Vertex& v : out.verts) {
        int8_t n[4];
        float uv[2];
        r.Read(v.pos, sizeof(float) * 3);
        r.Read(n, 4);
        r.Read(uv, sizeof(float) * 2);
        r.Read(v.bone, 4);
        r.Read(v.weight, 4);
        for (int k = 0; k < 3; k++) {
            v.nrm[k] = n[k] / 127.0f;
        }
        v.tc[0] = (int16_t)std::lround(std::clamp(uv[0], -1.9f, 1.9f) * kTexCoordScale);
        v.tc[1] = (int16_t)std::lround(std::clamp(1.0f - uv[1], -1.9f, 1.9f) * kTexCoordScale);
        for (int k = 0; k < 4; k++) {
            if (v.weight[k] != 0 && v.bone[k] >= numBones) {
                error = "vertex bone index out of range";
                return false;
            }
        }
    }

    out.tris.resize(numTris * 3);
    r.Read(out.tris.data(), out.tris.size() * sizeof(uint32_t));
    for (uint32_t idx : out.tris) {
        if (idx >= numVerts) {
            error = "triangle index out of range";
            return false;
        }
    }

    const uint32_t numMaterials = r.U32();
    if (numMaterials == 0 || numMaterials > 16) {
        error = "bad material count";
        return false;
    }
    out.materials.resize(numMaterials);
    for (Material& m : out.materials) {
        m.texture = r.Str();
        m.firstTri = r.U32();
        m.numTris = r.U32();
        if ((uint64_t)m.firstTri + m.numTris > numTris) {
            error = "material range out of bounds";
            return false;
        }
    }

    if (version >= 2) {
        out.morphs.resize(numMorphs);
        for (Morph& m : out.morphs) {
            m.name = r.Str();
            const uint32_t count = r.U32();
            if (count > numVerts) {
                error = "morph too large";
                return false;
            }
            m.verts.resize(count);
            m.deltas.resize(count * 3);
            for (uint32_t k = 0; k < count; k++) {
                m.verts[k] = r.U32();
                r.Read(&m.deltas[k * 3], sizeof(float) * 3);
                if (m.verts[k] >= numVerts) {
                    error = "morph vertex out of range";
                    return false;
                }
            }
        }
        const uint32_t numTags = r.U32();
        if (numTags > 256) {
            error = "bad tag count";
            return false;
        }
        out.tags.resize(numTags);
        for (Tag& t : out.tags) {
            t.name = r.Str();
            const uint32_t count = r.U32();
            if (count > numVerts) {
                error = "tag too large";
                return false;
            }
            t.verts.resize(count);
            r.Read(t.verts.data(), count * sizeof(uint32_t));
            for (uint32_t v : t.verts) {
                if (v >= numVerts) {
                    error = "tag vertex out of range";
                    return false;
                }
            }
        }
    }

    if (r.Failed()) {
        error = "file is truncated";
        return false;
    }
    return true;
}

void BuildChunks(Model& model, int maxVerts) {
    const size_t numTris = model.tris.size() / 3;
    const size_t numVerts = model.verts.size();

    // vertex -> triangles
    std::vector<uint32_t> start(numVerts + 1, 0);
    for (uint32_t idx : model.tris) {
        start[idx + 1]++;
    }
    for (size_t i = 0; i < numVerts; i++) {
        start[i + 1] += start[i];
    }
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    std::vector<uint32_t> adj(model.tris.size());
    for (size_t t = 0; t < numTris; t++) {
        for (int k = 0; k < 3; k++) {
            adj[fill[model.tris[t * 3 + k]]++] = (uint32_t)t;
        }
    }

    // Triangles outside every material range are never drawn.
    std::vector<uint8_t> used(numTris, 1);
    std::vector<int> slot(numVerts, -1);
    std::vector<uint32_t> loaded;
    std::vector<uint32_t> frontier;

    model.chunks.clear();
    model.chunkVerts.clear();
    model.chunkTris.clear();

    for (uint32_t mat = 0; mat < model.materials.size(); mat++) {
    const Material& material = model.materials[mat];
    for (uint32_t t = material.firstTri; t < material.firstTri + material.numTris; t++) {
        used[t] = 0;
    }
    size_t seed = material.firstTri;
    const size_t seedEnd = (size_t)material.firstTri + material.numTris;
    while (true) {
        while (seed < seedEnd && used[seed]) {
            seed++;
        }
        if (seed >= seedEnd) {
            break;
        }
        Chunk chunk{};
        chunk.material = mat;
        chunk.firstVert = (uint32_t)model.chunkVerts.size();
        chunk.firstTri = (uint32_t)(model.chunkTris.size() / 3);
        loaded.clear();
        frontier.clear();
        frontier.push_back((uint32_t)seed);

        auto newVertsFor = [&](uint32_t t) {
            int n = 0;
            for (int k = 0; k < 3; k++) {
                n += slot[model.tris[t * 3 + k]] < 0;
            }
            return n;
        };

        while (!frontier.empty()) {
            // Take the frontier triangle that needs the fewest new vertices.
            size_t best = frontier.size();
            int bestNew = 4;
            for (size_t i = 0; i < frontier.size(); i++) {
                if (used[frontier[i]]) {
                    continue;
                }
                int n = newVertsFor(frontier[i]);
                if (n < bestNew) {
                    bestNew = n;
                    best = i;
                    if (n == 0) {
                        break;
                    }
                }
            }
            if (best == frontier.size()) {
                break;
            }
            const uint32_t t = frontier[best];
            frontier[best] = frontier.back();
            frontier.pop_back();
            if ((int)loaded.size() + bestNew > maxVerts) {
                continue;
            }
            used[t] = 1;
            for (int k = 0; k < 3; k++) {
                const uint32_t v = model.tris[t * 3 + k];
                if (slot[v] < 0) {
                    slot[v] = (int)loaded.size();
                    loaded.push_back(v);
                    for (uint32_t a = start[v]; a < start[v + 1]; a++) {
                        if (!used[adj[a]]) {
                            frontier.push_back(adj[a]);
                        }
                    }
                }
                model.chunkTris.push_back((uint8_t)slot[v]);
            }
            chunk.numTris++;
            // Keep the frontier bounded; stale entries are skipped above.
            if (frontier.size() > 512) {
                frontier.erase(std::remove_if(frontier.begin(), frontier.end(), [&](uint32_t f) { return used[f]; }),
                               frontier.end());
            }
        }

        for (uint32_t v : loaded) {
            slot[v] = -1;
            model.chunkVerts.push_back(v);
        }
        chunk.numVerts = (uint32_t)loaded.size();
        model.chunks.push_back(chunk);
    }
    // Neighbours from another material were never marked usable, so they stay out of this one.
    for (uint32_t t = material.firstTri; t < material.firstTri + material.numTris; t++) {
        used[t] = 1;
    }
    }
}

int Model::FindBone(const char* name) const {
    for (size_t i = 0; i < bones.size(); i++) {
        if (bones[i].name == name) {
            return (int)i;
        }
    }
    return -1;
}

int Model::FindMorph(const char* name) const {
    for (size_t i = 0; i < morphs.size(); i++) {
        if (morphs[i].name == name) {
            return (int)i;
        }
    }
    return -1;
}

const Tag* Model::FindTag(const char* name) const {
    for (const Tag& t : tags) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace PlayerModel
