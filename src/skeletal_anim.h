// Skeletal animation: plays a glTF clip on skinned imported meshes. Each frame the pose is evaluated and, when it changed,
// the vertices are skinned on the CPU (on the job system) into the renderer's deformable mesh, so both backends draw the
// same result, shadows included, with no shader changes. Vertex color and alpha pass through, so material markers survive.
// TBD_ANIM_TIME=<seconds> holds every clip at one moment, for deterministic captures.
// Skinning is dual quaternion (volume kept through bends and twists: wrists, elbows, shoulders do not pinch or candy-wrap);
// a vertex bound to a joint whose matrix carries scale falls back to linear blending. TBD_SKIN_LBS=1: linear for all, to compare.
#pragma once
#include "platform/platform.h"

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/jobs.h"
#include "mesh_import.h"
#include "ragdoll.h"

namespace tbd {

struct SkinnedPartRange {
    uint32_t firstIndex = 0, indexCount = 0;
};

class SkeletalAnimator {
public:
    // Welds `flat` (the file's vertices, tinted), makes its deformable mesh and starts it on `clipPath`. Returns
    // kInvalidMesh, drawing nothing new, when the model has no usable skin or the clip can't be read; the entity then stays
    // in its bind pose through the ordinary path. `ranges` gets one index range per model part.
    MeshId Add(Renderer& renderer, const std::string& clipPath, const ImportedModel& model, const std::vector<Vertex>& flat,
               std::vector<SkinnedPartRange>& ranges) {
        ranges.clear();
        const AnimClip* clip = Clip(clipPath);
        if (!clip) return kInvalidMesh;
        if (model.joints.empty() || model.vertexSkin.size() != flat.size() || model.parts.empty() ||
            model.parts.back().first + model.parts.back().count != flat.size()) {
            Log("Animation: the model has no usable skin; staying in its bind pose");
            return kInvalidMesh;
        }
        auto inst = std::make_unique<Instance>();
        inst->model = &model;
        inst->clip = clip;
        inst->tracks.reserve(model.joints.size());
        size_t matched = 0;
        for (const SkinJoint& j : model.joints) {
            const auto it = clip->joints.find(j.name);
            inst->tracks.push_back(it == clip->joints.end() ? nullptr : &it->second);
            matched += it != clip->joints.end();
        }
        if (matched == 0) {
            Log("Animation: the clip shares no joint names with the model; staying in its bind pose");
            return kInvalidMesh;
        }
        inst->palette.resize(model.joints.size() * 16);
        inst->world.resize(model.joints.size() * 16);

        // Weld within each part, so a part stays one contiguous index range.
        std::vector<uint32_t> indices;
        indices.reserve(flat.size());
        inst->bind.reserve(flat.size() / 3);
        inst->skin.reserve(flat.size() / 3);
        struct Key {
            Vertex v;
            VertexSkin s;
            bool operator==(const Key& o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
        };
        static_assert(sizeof(Key) == sizeof(Vertex) + sizeof(VertexSkin), "Key is compared as bytes");
        struct KeyHash {
            size_t operator()(const Key& k) const {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
                uint64_t h = 14695981039346656037ull;
                for (size_t i = 0; i < sizeof(Key); ++i) h = (h ^ p[i]) * 1099511628211ull;
                return (size_t)h;
            }
        };
        for (const ImportedPart& part : model.parts) {
            SkinnedPartRange range;
            range.firstIndex = (uint32_t)indices.size();
            range.indexCount = (uint32_t)part.count;
            std::unordered_map<Key, uint32_t, KeyHash> seen;
            seen.reserve(part.count / 3);
            for (size_t i = part.first; i < part.first + part.count; ++i) {
                Key key;
                std::memset(&key, 0, sizeof(key));
                key.v = flat[i];
                key.s = model.vertexSkin[i];
                const auto found = seen.try_emplace(key, (uint32_t)inst->bind.size());
                if (found.second) {
                    inst->bind.push_back(flat[i]);
                    inst->skin.push_back(model.vertexSkin[i]);
                }
                indices.push_back(found.first->second);
            }
            ranges.push_back(range);
        }
        const MeshId id = renderer.CreateDeformableMesh(inst->bind.data(), (uint32_t)inst->bind.size(), indices.data(), (uint32_t)indices.size());
        if (id == kInvalidMesh) return kInvalidMesh;
        inst->mesh = id;
        Log("Animation: %zu vertices (%zu flat), %zu joints, %zu driven by the clip", inst->bind.size(), flat.size(), model.joints.size(), matched);
        instances_.push_back(std::move(inst));
        return id;
    }

    // The farthest any joint's translation track strays from its first key, in meters (root motion, a jump's pelvis).
    float ClipReach(const std::string& path) {
        const AnimClip* clip = Clip(path);
        if (!clip) return 0.0f;
        float reach = 0.0f;
        for (const auto& [name, tracks] : clip->joints) {
            const std::vector<float>& v = tracks.translation.values;
            for (size_t k = 0; k + 3 <= v.size(); k += 3) {
                const float x = v[k] - v[0], y = v[k + 1] - v[1], z = v[k + 2] - v[2];
                reach = std::max(reach, std::sqrt(x * x + y * y + z * z));
            }
        }
        return reach;
    }

    // Play mode: drives this mesh from several clips at once instead of its own. Times and weights come per frame through
    // SetBlendPose. `stripJoint`'s horizontal travel is removed (root motion: the capsule moves him, not the clip).
    bool SetBlendClips(MeshId mesh, const std::vector<std::string>& paths, const std::string& stripJoint) {
        Instance* inst = Find(mesh);
        if (!inst) return false;
        inst->layers.clear();
        for (const std::string& path : paths) {
            const AnimClip* clip = Clip(path);
            if (!clip) { inst->layers.clear(); return false; }
            BlendLayer layer;
            layer.tracks.reserve(inst->model->joints.size());
            for (const SkinJoint& j : inst->model->joints) {
                const auto it = clip->joints.find(j.name);
                layer.tracks.push_back(it == clip->joints.end() ? nullptr : &it->second);
            }
            inst->layers.push_back(std::move(layer));
        }
        inst->stripJoint = -1;
        for (size_t j = 0; j < inst->model->joints.size(); ++j)
            if (inst->model->joints[j].name == stripJoint) inst->stripJoint = (int)j;
        return true;
    }
    // `times` and `weights` hold one value per clip given to SetBlendClips. Weights should sum to one.
    void SetBlendPose(MeshId mesh, const float* times, const float* weights) {
        if (Instance* inst = Find(mesh))
            for (size_t i = 0; i < inst->layers.size(); ++i) { inst->layers[i].time = times[i]; inst->layers[i].weight = weights[i]; }
    }
    void ClearBlend(MeshId mesh) { if (Instance* inst = Find(mesh)) inst->layers.clear(); }

    // Ragdoll: the skeleton at rest (joint worlds in the mesh's own space), the pose the clips give this frame, and a pose that
    // replaces it on the skin. The clips keep running underneath, so the muscles always have something to aim for.
    bool Skeleton(MeshId mesh, RagdollSkeleton& out) const {
        const Instance* inst = FindConst(mesh);
        if (!inst) return false;
        const std::vector<SkinJoint>& joints = inst->model->joints;
        out.names.clear(); out.parent.clear();
        out.bind.assign(joints.size() * 16, 0.0f);
        for (size_t j = 0; j < joints.size(); ++j) {
            const SkinJoint& joint = joints[j];
            out.names.push_back(joint.name);
            out.parent.push_back(joint.parent);
            const meshimport_detail::M4 local = meshimport_detail::FromTRS(joint.t, joint.q, joint.s);
            Mul(joint.parent >= 0 ? &out.bind[(size_t)joint.parent * 16] : joint.rootParent, local.m, &out.bind[j * 16]);
        }
        return !joints.empty();
    }
    // The clips' pose from the last Update (joint count * 16 floats), or null for an unknown mesh.
    const std::vector<float>* AnimatedPose(MeshId mesh) const {
        const Instance* inst = FindConst(mesh);
        return !inst ? nullptr : inst->override.empty() ? &inst->world : &inst->target;
    }
    // `worlds` must hold one matrix per joint. Applied from the next Update.
    bool SetPoseOverride(MeshId mesh, const std::vector<float>& worlds) {
        Instance* inst = Find(mesh);
        if (!inst || worlds.size() != inst->world.size()) return false;
        inst->override = worlds;
        return true;
    }
    void ClearPoseOverride(MeshId mesh) {
        if (Instance* inst = Find(mesh)) { inst->override.clear(); inst->target.clear(); }
    }
    float ClipDuration(const std::string& path) { const AnimClip* clip = Clip(path); return clip ? clip->duration : 0.0f; }

    void Forget(MeshId mesh) {
        for (size_t i = 0; i < instances_.size(); ++i)
            if (instances_[i]->mesh == mesh) {
                instances_.erase(instances_.begin() + (std::ptrdiff_t)i);
                return;
            }
    }
    void Clear() { instances_.clear(); }
    bool Animates(MeshId mesh) const {
        for (const std::unique_ptr<Instance>& inst : instances_)
            if (inst->mesh == mesh) return true;
        return false;
    }

    // Between BeginFrame and the first draw.
    void Update(Renderer& renderer, JobSystem& jobs, float dt) {
        if (instances_.empty()) return;
        static const float fixedTime = [] {
            char v[32] = {};
            return tbd::platform::GetEnvA("TBD_ANIM_TIME", v, sizeof(v)) > 0 ? (float)std::atof(v) : -1.0f;
        }();
        time_ += dt;
        const float now = fixedTime >= 0.0f ? fixedTime : time_;
        jobs.ParallelFor(instances_.size(), 1, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) instances_[i]->EvaluatePose(now);
        });
        // One parallel round over slices of all the meshes: waking the workers per mesh cost visible frame spikes.
        constexpr size_t kSliceVertices = 8192;
        slices_.clear();
        for (const std::unique_ptr<Instance>& inst : instances_) {
            // The same pose (a held clip, a held key): the mesh already has it. TBD_ANIM_RESKIN=1 skins anyway, to measure
            // what a moving character costs.
            static const bool alwaysSkin = tbd::platform::GetEnvA("TBD_ANIM_RESKIN", nullptr, 0) > 0;
            // Skinned once more after the last change: motion vectors compare the newest vertices with the ones before.
            inst->heldFrames = inst->shown == inst->palette ? inst->heldFrames + 1 : 0;
            if (!alwaysSkin && inst->heldFrames >= 2) continue;
            Vertex* out = renderer.DeformableVertices(inst->mesh);
            if (!out) continue;
            inst->shown = inst->palette;
            for (size_t begin = 0; begin < inst->bind.size(); begin += kSliceVertices)
                slices_.push_back({inst.get(), out, begin, std::min(begin + kSliceVertices, inst->bind.size())});
        }
        jobs.ParallelFor(slices_.size(), 1, [this](size_t begin, size_t end) {
            for (size_t k = begin; k < end; ++k) slices_[k].instance->Skin(slices_[k].out, slices_[k].begin, slices_[k].end);
        });
        renderer.CommitDeformables();
    }

private:
    static void Mul(const float* a, const float* b, float* r) {
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
                r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] + a[12 + row] * b[c * 4 + 3];
    }

    // Finds the keys around `time` and the blend between them.
    static void Locate(const AnimTrack& track, float time, size_t& a, size_t& b, float& f) {
        const std::vector<float>& t = track.times;
        if (time <= t.front() || t.size() == 1) { a = b = 0; f = 0; return; }
        if (time >= t.back()) { a = b = t.size() - 1; f = 0; return; }
        b = (size_t)(std::upper_bound(t.begin(), t.end(), time) - t.begin());
        a = b - 1;
        f = track.step ? 0.0f : (time - t[a]) / std::max(t[b] - t[a], 1e-6f);
    }
    static void SampleVec3(const AnimTrack& track, float time, float out[3]) {
        if (track.times.empty()) return;
        size_t a, b;
        float f;
        Locate(track, time, a, b, f);
        for (int c = 0; c < 3; ++c) out[c] = track.values[a * 3 + c] + (track.values[b * 3 + c] - track.values[a * 3 + c]) * f;
    }
    static void SampleQuat(const AnimTrack& track, float time, float out[4]) {
        if (track.times.empty()) return;
        size_t a, b;
        float f;
        Locate(track, time, a, b, f);
        const float* qa = &track.values[a * 4];
        const float* qb = &track.values[b * 4];
        const float sign = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0 ? -1.0f : 1.0f; // the short way round
        float len = 0;
        for (int c = 0; c < 4; ++c) {
            out[c] = qa[c] + (qb[c] * sign - qa[c]) * f;
            len += out[c] * out[c];
        }
        len = std::sqrt(std::max(len, 1e-12f));
        for (int c = 0; c < 4; ++c) out[c] /= len;
    }

    struct BlendLayer {
        std::vector<const AnimJointTracks*> tracks; // per joint; nullptr = rest pose
        float time = 0.0f, weight = 0.0f;
    };

    struct Instance {
        std::vector<BlendLayer> layers; // non-empty: these drive the pose, not `clip`
        int stripJoint = -1;
        MeshId mesh = kInvalidMesh;
        const ImportedModel* model = nullptr;
        const AnimClip* clip = nullptr;
        std::vector<const AnimJointTracks*> tracks; // per joint; nullptr = rest pose
        std::vector<float> world, palette;          // 16 floats per joint
        std::vector<float> dq;                      // 8 per joint: rotation quaternion (x y z w), then its dual part
        std::vector<uint8_t> rigid;                 // per joint: the palette is rotation + translation (dual quaternion usable)
        std::vector<float> override, target;        // ragdoll: the pose to show instead, and the clips' pose it replaced
        std::vector<float> shown;                   // the palette the mesh's vertices were last skinned with
        int heldFrames = 0;                         // frames in a row with that same palette
        std::vector<Vertex> bind;
        std::vector<VertexSkin> skin;

        // Clips made on another skeleton (the Unreal retargets in export/anim/unreal) carry that skeleton's bone offsets as
        // constant translation keys: played as is they move the elbow and wrist joints ~12 cm off this mesh's bones and drag the
        // skin into a thin, cranked wrist. Below the pelvis (two joints down from a skin root) a translation track that never
        // moves is only a bone length, so the mesh's own rest offset is kept; moving tracks (root motion, the pelvis, a jaw) play.
        bool ClipTranslation(size_t j, const AnimTrack& tr) const {
            if (tr.values.empty()) return false;
            if (!tr.still) return true;
            int depth = 0;
            for (int p = model->joints[j].parent; p >= 0 && depth < 2; p = model->joints[(size_t)p].parent) ++depth;
            return depth < 2;
        }
        // Keeping this mesh's offsets must not change where the clip points the bones: the clip's elbow sits 27 degrees below
        // the upper arm's rest axis, so the upper arm is turned by the rotation C that takes the mesh's child offset (summed over
        // the children whose offset was replaced) onto the clip's, and each child is turned back by C's inverse so its own
        // world rotation is what the clip says. Local rotation played: inverse(C of the parent) * clip rotation * C.
        std::vector<std::vector<uint16_t>> children;
        void BoneAim(const std::vector<const AnimJointTracks*>& tr, size_t j, float c[4]) const {
            c[0] = c[1] = c[2] = 0; c[3] = 1;
            if (j >= children.size()) return;
            float a[3] = {}, b[3] = {};
            for (uint16_t k : children[j]) {
                const AnimJointTracks* t = tr[k];
                if (!t || t->translation.values.size() < 3 || ClipTranslation(k, t->translation)) continue;
                const SkinJoint& kj = model->joints[k];
                for (int i = 0; i < 3; ++i) { a[i] += kj.t[i]; b[i] += t->translation.values[(size_t)i]; }
            }
            const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (la < 1e-4f || lb < 1e-4f) return;
            for (int i = 0; i < 3; ++i) { a[i] /= la; b[i] /= lb; }
            const float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            if (d > 0.99996f || d < -0.9f) return; // under half a degree, or nearly opposite (not a bone length: leave it)
            float q[4] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 1 + d};
            const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (int i = 0; i < 4; ++i) c[i] = q[i] / l;
        }
        static void QMul(const float a[4], const float b[4], float r[4]) {
            const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
            const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
            const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
            const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
            r[0] = x; r[1] = y; r[2] = z; r[3] = w;
        }
        void AimBones(const std::vector<const AnimJointTracks*>& tr, size_t j, float q[4]) {
            if (children.size() != model->joints.size()) {
                children.assign(model->joints.size(), {});
                for (size_t k = 0; k < model->joints.size(); ++k)
                    if (model->joints[k].parent >= 0) children[(size_t)model->joints[k].parent].push_back((uint16_t)k);
            }
            float cj[4], cp[4] = {0, 0, 0, 1}, t[4];
            BoneAim(tr, j, cj);
            if (model->joints[j].parent >= 0) BoneAim(tr, (size_t)model->joints[j].parent, cp);
            const float cpi[4] = {-cp[0], -cp[1], -cp[2], cp[3]};
            QMul(cpi, q, t);
            QMul(t, cj, q);
        }

        // The weighted sum of the layers for one joint: translation and scale add, rotations are nlerped on one hemisphere.
        void BlendJoint(size_t j, const SkinJoint& joint, float t[3], float q[4], float s[3]) {
            float bt[3] = {}, bq[4] = {}, bs[3] = {}, total = 0.0f;
            for (const BlendLayer& layer : layers) {
                if (layer.weight <= 0.0f) continue;
                float lt[3] = {t[0], t[1], t[2]}, lq[4] = {q[0], q[1], q[2], q[3]}, ls[3] = {s[0], s[1], s[2]};
                if (const AnimJointTracks* tr = layer.tracks[j]) {
                    if (ClipTranslation(j, tr->translation)) SampleVec3(tr->translation, layer.time, lt);
                    SampleQuat(tr->rotation, layer.time, lq);
                    SampleVec3(tr->scale, layer.time, ls);
                    AimBones(layer.tracks, j, lq);
                    if ((int)j == stripJoint && !tr->translation.values.empty()) { lt[0] = tr->translation.values[0]; lt[2] = tr->translation.values[2]; }
                }
                const float sign = total > 0.0f && bq[0] * lq[0] + bq[1] * lq[1] + bq[2] * lq[2] + bq[3] * lq[3] < 0.0f ? -1.0f : 1.0f;
                for (int c = 0; c < 3; ++c) { bt[c] += lt[c] * layer.weight; bs[c] += ls[c] * layer.weight; }
                for (int c = 0; c < 4; ++c) bq[c] += lq[c] * layer.weight * sign;
                total += layer.weight;
            }
            if (total <= 0.0f) return;
            const float len = std::sqrt(std::max(bq[0] * bq[0] + bq[1] * bq[1] + bq[2] * bq[2] + bq[3] * bq[3], 1e-12f));
            for (int c = 0; c < 3; ++c) { t[c] = bt[c] / total; s[c] = bs[c] / total; }
            for (int c = 0; c < 4; ++c) q[c] = bq[c] / len;
        }

        void EvaluatePose(float now) {
            const float time = clip->duration > 0.0f ? std::fmod(now, clip->duration) : 0.0f;
            const std::vector<SkinJoint>& joints = model->joints;
            for (size_t j = 0; j < joints.size(); ++j) {
                const SkinJoint& joint = joints[j];
                float t[3] = {joint.t[0], joint.t[1], joint.t[2]}, q[4] = {joint.q[0], joint.q[1], joint.q[2], joint.q[3]}, s[3] = {joint.s[0], joint.s[1], joint.s[2]};
                if (!layers.empty()) BlendJoint(j, joint, t, q, s);
                else if (const AnimJointTracks* tr = tracks[j]) {
                    if (ClipTranslation(j, tr->translation)) SampleVec3(tr->translation, time, t);
                    SampleQuat(tr->rotation, time, q);
                    SampleVec3(tr->scale, time, s);
                    AimBones(tracks, j, q);
                }
                const meshimport_detail::M4 local = meshimport_detail::FromTRS(t, q, s);
                Mul(joint.parent >= 0 ? &world[(size_t)joint.parent * 16] : joint.rootParent, local.m, &world[j * 16]);
                Mul(&world[j * 16], joint.inverseBind, &palette[j * 16]);
            }
            if (!override.empty() && override.size() == world.size()) {
                target = world;
                world = override;
                for (size_t j = 0; j < joints.size(); ++j) Mul(&world[j * 16], joints[j].inverseBind, &palette[j * 16]);
            }
            BuildDualQuats();
        }

        // Each palette matrix as a unit dual quaternion, when it is rigid (unit, orthogonal columns, no mirror).
        void BuildDualQuats() {
            const size_t n = palette.size() / 16;
            dq.resize(n * 8);
            rigid.assign(n, 0);
            for (size_t j = 0; j < n; ++j) {
                const float* m = &palette[j * 16]; // column-major: m[c * 4 + row]
                const float c0 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2], c1 = m[4] * m[4] + m[5] * m[5] + m[6] * m[6], c2 = m[8] * m[8] + m[9] * m[9] + m[10] * m[10];
                const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
                if (std::fabs(c0 - 1.0f) > 2e-3f || std::fabs(c1 - 1.0f) > 2e-3f || std::fabs(c2 - 1.0f) > 2e-3f || det < 0.5f) continue;
                // rotation -> quaternion (r = row, c = column: R(r, c) = m[c * 4 + r])
                const float r00 = m[0], r11 = m[5], r22 = m[10], tr = r00 + r11 + r22;
                float q[4];
                if (tr > 0) { const float S = std::sqrt(tr + 1.0f) * 2; q[3] = 0.25f * S; q[0] = (m[6] - m[9]) / S; q[1] = (m[8] - m[2]) / S; q[2] = (m[1] - m[4]) / S; }
                else if (r00 > r11 && r00 > r22) { const float S = std::sqrt(1.0f + r00 - r11 - r22) * 2; q[3] = (m[6] - m[9]) / S; q[0] = 0.25f * S; q[1] = (m[4] + m[1]) / S; q[2] = (m[8] + m[2]) / S; }
                else if (r11 > r22) { const float S = std::sqrt(1.0f + r11 - r00 - r22) * 2; q[3] = (m[8] - m[2]) / S; q[0] = (m[4] + m[1]) / S; q[1] = 0.25f * S; q[2] = (m[9] + m[6]) / S; }
                else { const float S = std::sqrt(1.0f + r22 - r00 - r11) * 2; q[3] = (m[1] - m[4]) / S; q[0] = (m[8] + m[2]) / S; q[1] = (m[9] + m[6]) / S; q[2] = 0.25f * S; }
                const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                for (float& x : q) x /= ql;
                const float t[3] = {m[12], m[13], m[14]};
                float* o = &dq[j * 8];
                o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[3];
                // dual = 0.5 * (t, 0) * q
                o[4] = 0.5f * (t[0] * q[3] + t[1] * q[2] - t[2] * q[1]);
                o[5] = 0.5f * (-t[0] * q[2] + t[1] * q[3] + t[2] * q[0]);
                o[6] = 0.5f * (t[0] * q[1] - t[1] * q[0] + t[2] * q[3]);
                o[7] = 0.5f * (-t[0] * q[0] - t[1] * q[1] - t[2] * q[2]);
                rigid[j] = 1;
            }
            if (!dqLogged) { dqLogged = true; size_t r = 0; for (uint8_t x : rigid) r += x; Log("Skinning: dual quaternion on %zu of %zu joints (the rest blend linearly)", r, n); }
        }
        bool dqLogged = false;

        // Writes each vertex whole: the destination is uncached memory that held another pose.
        void Skin(Vertex* out, size_t begin, size_t end) const {
            for (size_t i = begin; i < end; ++i) {
                const VertexSkin& vs = skin[i];
                Vertex v = bind[i];
                static const bool linearOnly = tbd::platform::GetEnvA("TBD_SKIN_LBS", nullptr, 0) > 0;
                bool useDq = !linearOnly && !dq.empty();
                for (int k = 0; k < 4 && useDq; ++k) if (vs.weights[k] > 0.0f && (vs.joints[k] >= rigid.size() || !rigid[vs.joints[k]])) useDq = false;
                if (useDq && vs.weights[0] + vs.weights[1] + vs.weights[2] + vs.weights[3] > 0.0f) {
                    float r[4] = {}, d[4] = {};
                    const float* first = nullptr;
                    for (int k = 0; k < 4; ++k) {
                        const float w = vs.weights[k];
                        if (w <= 0.0f) continue;
                        const float* q = &dq[(size_t)vs.joints[k] * 8];
                        if (!first) first = q;
                        const float sw = first[0] * q[0] + first[1] * q[1] + first[2] * q[2] + first[3] * q[3] < 0.0f ? -w : w; // shortest path
                        for (int c = 0; c < 4; ++c) { r[c] += q[c] * sw; d[c] += q[4 + c] * sw; }
                    }
                    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
                    if (len > 1e-8f) {
                        for (int c = 0; c < 4; ++c) { r[c] /= len; d[c] /= len; }
                        const auto rotate = [&](float x, float y, float z, float out3[3]) { // v + 2 * cross(q, cross(q, v) + w v)
                            const float cx = r[1] * z - r[2] * y + r[3] * x, cy = r[2] * x - r[0] * z + r[3] * y, cz = r[0] * y - r[1] * x + r[3] * z;
                            out3[0] = x + 2.0f * (r[1] * cz - r[2] * cy);
                            out3[1] = y + 2.0f * (r[2] * cx - r[0] * cz);
                            out3[2] = z + 2.0f * (r[0] * cy - r[1] * cx);
                        };
                        const Vertex& b = bind[i];
                        float p3[3], n3[3];
                        rotate(b.px, b.py, b.pz, p3);
                        // translation = 2 * (w_r * d_xyz - w_d * r_xyz + cross(r_xyz, d_xyz))
                        const float tx = 2.0f * (r[3] * d[0] - d[3] * r[0] + r[1] * d[2] - r[2] * d[1]);
                        const float ty = 2.0f * (r[3] * d[1] - d[3] * r[1] + r[2] * d[0] - r[0] * d[2]);
                        const float tz = 2.0f * (r[3] * d[2] - d[3] * r[2] + r[0] * d[1] - r[1] * d[0]);
                        v.px = p3[0] + tx; v.py = p3[1] + ty; v.pz = p3[2] + tz;
                        if (b.nx != 0.0f || b.ny != 0.0f || b.nz != 0.0f) { rotate(b.nx, b.ny, b.nz, n3); v.nx = n3[0]; v.ny = n3[1]; v.nz = n3[2]; }
                        out[i] = v;
                        continue;
                    }
                }
                if (vs.weights[0] + vs.weights[1] + vs.weights[2] + vs.weights[3] > 0.0f) {
                    float m[12] = {}; // columns 0..3, xyz: only the affine part matters
                    for (int k = 0; k < 4; ++k) {
                        const float w = vs.weights[k];
                        if (w <= 0.0f) continue;
                        const float* p = &palette[(size_t)vs.joints[k] * 16];
                        for (int c = 0; c < 4; ++c)
                            for (int row = 0; row < 3; ++row) m[c * 3 + row] += p[c * 4 + row] * w;
                    }
                    const Vertex& b = bind[i];
                    v.px = m[0] * b.px + m[3] * b.py + m[6] * b.pz + m[9];
                    v.py = m[1] * b.px + m[4] * b.py + m[7] * b.pz + m[10];
                    v.pz = m[2] * b.px + m[5] * b.py + m[8] * b.pz + m[11];
                    const float nx = m[0] * b.nx + m[3] * b.ny + m[6] * b.nz;
                    const float ny = m[1] * b.nx + m[4] * b.ny + m[7] * b.nz;
                    const float nz = m[2] * b.nx + m[5] * b.ny + m[8] * b.nz;
                    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-12f) { v.nx = nx / len; v.ny = ny / len; v.nz = nz / len; } // zero normals mean "unlit": kept
                }
                out[i] = v;
            }
        }
    };

    const Instance* FindConst(MeshId mesh) const {
        for (const std::unique_ptr<Instance>& inst : instances_)
            if (inst->mesh == mesh) return inst.get();
        return nullptr;
    }
    Instance* Find(MeshId mesh) {
        for (const std::unique_ptr<Instance>& inst : instances_)
            if (inst->mesh == mesh) return inst.get();
        return nullptr;
    }

    // Clips stay loaded for the program's life; a file that can't be read is logged once.
    const AnimClip* Clip(const std::string& path) {
        const auto it = clips_.find(path);
        if (it != clips_.end()) return it->second.get();
        std::unique_ptr<AnimClip>& slot = clips_[path];
        std::vector<Vertex> unused;
        ImportedModel file;
        ImportOptions options;
        options.normalizeToUnitBox = false;
        std::string error;
        if (!ImportMesh(path, unused, &error, options, &file) || file.clips.empty()) {
            Log("Animation: cannot use clip %s: %s", path.c_str(), error.empty() ? "no animation in the file" : error.c_str());
            return nullptr;
        }
        slot = std::make_unique<AnimClip>(std::move(file.clips[0]));
        return slot.get();
    }

    std::vector<std::unique_ptr<Instance>> instances_;
    struct Slice {
        const Instance* instance;
        Vertex* out;
        size_t begin, end;
    };
    std::vector<Slice> slices_; // reused, so the frame doesn't allocate
    std::unordered_map<std::string, std::unique_ptr<AnimClip>> clips_;
    float time_ = 0.0f;
};

} // namespace tbd
