// Rigging a model that came without joints: the figure's skeleton fitted
// into it, and its surface bound to the bones.
#include "model/Model3D.h"

#include <QObject>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

// The joints, in the order rigJointNames() gives them.
enum Joint {
    Hips, Spine, Chest, Neck, Head, HeadTop,
    ShoulderL, ElbowL, WristL, HandTipL,
    ShoulderR, ElbowR, WristR, HandTipR,
    HipL, KneeL, AnkleL, ToeL,
    HipR, KneeR, AnkleR, ToeR,
    JointCount
};

// The figure's bones, in its own order: where each starts, which joint it
// points at, and which way it points before anything turns it.
struct Bone {
    const char* name;
    int parent;
    int joint, toward;
    QVector3D restDir;
    float restZ = 0.0f;   // the figure's arms hang a little out from the body
};

const Bone kBones[] = {
    {"hips", -1, Hips, Spine, {0, 1, 0}},
    {"spine", 0, Spine, Chest, {0, 1, 0}},
    {"chest", 1, Chest, Neck, {0, 1, 0}},
    {"neck", 2, Neck, Head, {0, 1, 0}},
    {"head", 3, Head, HeadTop, {0, 1, 0}},
    {"upperarm.L", 2, ShoulderL, ElbowL, {0, -1, 0}, 9.0f},
    {"forearm.L", 5, ElbowL, WristL, {0, -1, 0}},
    {"hand.L", 6, WristL, HandTipL, {0, -1, 0}},
    {"upperarm.R", 2, ShoulderR, ElbowR, {0, -1, 0}, -9.0f},
    {"forearm.R", 8, ElbowR, WristR, {0, -1, 0}},
    {"hand.R", 9, WristR, HandTipR, {0, -1, 0}},
    {"thigh.L", 0, HipL, KneeL, {0, -1, 0}},
    {"shin.L", 11, KneeL, AnkleL, {0, -1, 0}},
    {"foot.L", 12, AnkleL, ToeL, {0.0f, -0.33f, 0.94f}},
    {"thigh.R", 0, HipR, KneeR, {0, -1, 0}},
    {"shin.R", 14, KneeR, AnkleR, {0, -1, 0}},
    {"foot.R", 15, AnkleR, ToeR, {0.0f, -0.33f, 0.94f}},
};
constexpr int kBoneCount = int(sizeof(kBones) / sizeof(kBones[0]));

float segmentDistance(const QVector3D& p, const QVector3D& a, const QVector3D& b)
{
    const QVector3D ab = b - a;
    const float len2 = QVector3D::dotProduct(ab, ab);
    const float t = len2 > 1e-12f ? std::clamp(QVector3D::dotProduct(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    return (p - (a + ab * t)).length();
}

QVector3D centroid(const std::vector<QVector3D>& pts, const QVector3D& fallback)
{
    if (pts.empty())
        return fallback;
    QVector3D sum;
    for (const QVector3D& p : pts)
        sum += p;
    return sum / float(pts.size());
}

float median(std::vector<float> v)
{
    if (v.empty())
        return 0.0f;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

} // namespace

QStringList Model3D::rigJointNames()
{
    return {QObject::tr("Hips"), QObject::tr("Waist"), QObject::tr("Chest"), QObject::tr("Neck"), QObject::tr("Head"), QObject::tr("Top of the head"),
            QObject::tr("Left shoulder"), QObject::tr("Left elbow"), QObject::tr("Left wrist"), QObject::tr("Left fingertips"),
            QObject::tr("Right shoulder"), QObject::tr("Right elbow"), QObject::tr("Right wrist"), QObject::tr("Right fingertips"),
            QObject::tr("Left hip"), QObject::tr("Left knee"), QObject::tr("Left ankle"), QObject::tr("Left toes"),
            QObject::tr("Right hip"), QObject::tr("Right knee"), QObject::tr("Right ankle"), QObject::tr("Right toes")};
}

std::vector<std::pair<int, int>> Model3D::rigBones()
{
    std::vector<std::pair<int, int>> out;
    for (const Bone& b : kBones)
        out.emplace_back(b.joint, b.toward);
    // The hips to each leg, and the chest to each shoulder, which no bone joins by itself.
    out.insert(out.end(), {{Hips, HipL}, {Hips, HipR}, {Chest, ShoulderL}, {Chest, ShoulderR}});
    return out;
}

int Model3D::rigMirror(int joint)
{
    if (joint >= ShoulderL && joint <= HandTipL)
        return joint + 4;
    if (joint >= ShoulderR && joint <= HandTipR)
        return joint - 4;
    if (joint >= HipL && joint <= ToeL)
        return joint + 4;
    if (joint >= HipR && joint <= ToeR)
        return joint - 4;
    return joint;
}

bool Model3D::facesAway(const std::vector<QVector3D>& j) const
{
    if (j.size() != JointCount)
        return false;
    return (j[ToeL].z() - j[AnkleL].z()) + (j[ToeR].z() - j[AnkleR].z()) < 0.0f;
}

bool Model3D::fitRig(std::vector<QVector3D>* out, QString* why) const
{
    auto fail = [&](const QString& text) {
        if (why)
            *why = text;
        return false;
    };
    // Every point where it stands, thinned out on a large model.
    std::vector<QVector3D> pts;
    size_t total = 0;
    const std::vector<Part> parts = posed();
    for (const Part& p : parts)
        total += p.positions.size();
    const size_t step = std::max<size_t>(1, total / 120000);
    size_t k = 0;
    for (const Part& p : parts) {
        for (const QVector3D& v : p.positions) {
            if (k++ % step == 0)
                pts.push_back(v);
        }
    }
    QVector3D lo, hi;
    if (pts.size() < 200 || !bounds(&lo, &hi))
        return fail(QObject::tr("The model has too few points to find a person in"));
    const float H = hi.y() - lo.y();
    if (H <= 0.0f || hi.z() - lo.z() > H * 1.05f || hi.x() - lo.x() > H * 2.2f)
        return fail(QObject::tr("Rigging needs a person standing upright, head up"));
    auto at = [&](double f) { return float(lo.y() + f * H); };
    auto slice = [&](float y0, float y1, const std::function<bool(const QVector3D&)>& keep) {
        std::vector<QVector3D> s;
        for (const QVector3D& p : pts) {
            if (p.y() >= y0 && p.y() <= y1 && keep(p))
                s.push_back(p);
        }
        return s;
    };
    auto any = [](const QVector3D&) { return true; };
    // A slice about a height, made thicker until it has points in it: a model
    // with few points along a long limb has none in a thin one.
    auto around = [&](double f, double half, const std::function<bool(const QVector3D&)>& keep) {
        std::vector<QVector3D> s;
        for (int widen = 0; widen < 5 && s.size() < 6; ++widen, half *= 2.0)
            s = slice(at(f - half), at(f + half), keep);
        return s;
    };

    // The middle of the body, from the hips.
    const std::vector<QVector3D> hipsBand = slice(at(0.45), at(0.55), any);
    if (hipsBand.size() < 20)
        return fail(QObject::tr("Rigging needs a person standing upright, head up"));
    std::vector<float> xs, zs;
    for (const QVector3D& p : hipsBand) {
        xs.push_back(p.x());
        zs.push_back(p.z());
    }
    const float cx = median(xs), cz = median(zs);
    // How wide the chest is: the run of points round the middle with no gap in it.
    std::vector<float> chestX;
    for (const QVector3D& p : slice(at(0.66), at(0.74), any))
        chestX.push_back(p.x());
    std::sort(chestX.begin(), chestX.end());
    float left = cx, right = cx;
    {
        const auto mid = std::lower_bound(chestX.begin(), chestX.end(), cx);
        if (mid != chestX.end()) {
            auto it = mid;
            while (std::next(it) != chestX.end() && *std::next(it) - *it < 0.012f * H)
                ++it;
            right = *it;
            it = mid;
            while (it != chestX.begin() && *it - *std::prev(it) < 0.012f * H)
                --it;
            left = *it;
        }
    }
    const float w = std::max(0.04f * H, (right - left) / 2.0f);

    // The legs, each on its own side, and from the toes which way it faces.
    QVector3D hip[2], knee[2], ankle[2], toe[2];
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? 1.0f : -1.0f;
        auto mine = [&](const QVector3D& p) { return s * (p.x() - cx) > 0.005f * H && std::abs(p.x() - cx) < 0.25f * H; };
        const QVector3D guess(cx + s * 0.1f * H, 0, cz);
        knee[side] = centroid(around(0.275, 0.015, mine), guess + QVector3D(0, at(0.275), 0));
        ankle[side] = centroid(around(0.0625, 0.0125, mine), guess + QVector3D(0, at(0.06), 0));
        hip[side] = centroid(around(0.455, 0.015, mine), guess + QVector3D(0, at(0.49), 0));
        hip[side].setY(at(0.49));
        ankle[side].setY(std::max(ankle[side].y(), at(0.045)));
        const std::vector<QVector3D> sole = slice(lo.y(), at(0.05), mine);
        if (sole.size() < 5)
            return fail(QObject::tr("Could not find two feet: rigging needs a person standing on two legs"));
        float far = -1.0f;
        for (const QVector3D& p : sole) {
            const float d = std::hypot(p.x() - ankle[side].x(), p.z() - ankle[side].z());
            if (d > far) {
                far = d;
                toe[side] = p;
            }
        }
    }
    const bool away = (toe[0].z() - ankle[0].z()) + (toe[1].z() - ankle[1].z()) < 0.0f;
    // Facing +Z, a person's left hand is on the +X side.
    const int leftSide = away ? 1 : 0;

    // Up the middle: hips, waist, chest, neck and head, each the middle of its slice.
    auto central = [&](float half) { return [cx, half](const QVector3D& p) { return std::abs(p.x() - cx) < half; }; };
    std::vector<QVector3D> j(JointCount);
    j[Hips] = centroid(slice(at(0.52), at(0.54), central(w)), QVector3D(cx, 0, cz));
    j[Hips].setY(at(0.53));
    j[Spine] = centroid(slice(at(0.60), at(0.62), central(0.8f * w)), j[Hips]);
    j[Spine].setY(at(0.61));
    j[Chest] = centroid(slice(at(0.70), at(0.72), central(0.8f * w)), j[Spine]);
    j[Chest].setY(at(0.71));
    j[Neck] = centroid(slice(at(0.83), at(0.85), central(0.05f * H)), j[Chest]);
    j[Neck].setY(at(0.84));
    j[Head] = centroid(slice(at(0.865), at(0.885), central(0.07f * H)), j[Neck]);
    j[Head].setY(at(0.875));
    j[HeadTop] = QVector3D(j[Head].x(), hi.y(), j[Head].z());

    // What is body at each height (the trunk, or below the hips the two
    // legs), so that what stands clear of it can be taken for arms: the
    // clusters of points across each band, split where there is a gap.
    const int bands = 60;
    std::vector<std::vector<std::pair<float, float>>> body(bands);   // x ranges that are body
    auto bandOf = [&](float y) { return std::clamp(int((y - lo.y()) / H * bands), 0, bands - 1); };
    {
        std::vector<std::vector<float>> xsAt(bands);
        for (const QVector3D& p : pts)
            xsAt[size_t(bandOf(p.y()))].push_back(p.x());
        for (int b = 0; b < bands; ++b) {
            std::vector<float>& xs = xsAt[size_t(b)];
            if (xs.empty())
                continue;
            std::sort(xs.begin(), xs.end());
            std::vector<std::pair<float, float>> clusters{{xs.front(), xs.front()}};
            for (float x : xs) {
                if (x - clusters.back().second > 0.012f * H)
                    clusters.push_back({x, x});
                else
                    clusters.back().second = x;
            }
            // The cluster round the middle, or else the nearest on each side (two legs apart).
            int middle = -1, leftNear = -1, rightNear = -1;
            for (int c = 0; c < int(clusters.size()); ++c) {
                if (clusters[size_t(c)].first <= cx && clusters[size_t(c)].second >= cx)
                    middle = c;
                else if (clusters[size_t(c)].second < cx)
                    leftNear = c;
                else if (rightNear < 0)
                    rightNear = c;
            }
            if (middle >= 0) {
                body[size_t(b)].push_back(clusters[size_t(middle)]);
            } else {
                if (leftNear >= 0)
                    body[size_t(b)].push_back(clusters[size_t(leftNear)]);
                if (rightNear >= 0)
                    body[size_t(b)].push_back(clusters[size_t(rightNear)]);
            }
        }
    }
    auto inBody = [&](const QVector3D& p) {
        for (const auto& [l, r] : body[size_t(bandOf(p.y()))]) {
            if (p.x() >= l - 0.004f * H && p.x() <= r + 0.004f * H)
                return true;
        }
        return false;
    };
    // The trunk's half width between the hips and the shoulders. Where an arm
    // touches the body a band measures both, so a narrow one is taken, not the widest.
    std::vector<float> trunk;
    for (int b = bandOf(at(0.60)); b <= bandOf(at(0.78)); ++b) {
        for (const auto& [l, r] : body[size_t(b)]) {
            if (l <= cx && r >= cx && r - l > 0.04f * H)
                trunk.push_back(std::max(cx - l, r - cx));
        }
    }
    const float chestHalf = trunk.empty() ? w : median(trunk);
    const float shoulderHalf = chestHalf * 1.25f;
    // How wide the hips are: legs stay within it, hands reach past it.
    float hipHalf = 0.0f;
    for (int b = bandOf(at(0.42)); b <= bandOf(at(0.56)); ++b) {
        for (const auto& [l, r] : body[size_t(b)])
            hipHalf = std::max(hipHalf, std::max(std::abs(cx - l), std::abs(r - cx)));
    }

    // The arms: from the shoulder out to the hand furthest from it, centred along the way.
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? 1.0f : -1.0f;
        const bool isLeft = side == leftSide;
        const int shoulderJ = isLeft ? ShoulderL : ShoulderR;
        std::vector<QVector3D> shoulderRing;
        const QVector3D shoulderAt(cx + s * shoulderHalf, at(0.79), j[Chest].z());
        for (const QVector3D& p : pts) {
            if (std::abs(p.y() - shoulderAt.y()) < 0.02f * H && std::abs(p.x() - shoulderAt.x()) < 0.02f * H)
                shoulderRing.push_back(p);
        }
        const QVector3D shoulder(shoulderAt.x(), shoulderAt.y(), centroid(shoulderRing, shoulderAt).z());
        std::vector<QVector3D> arm;
        for (const QVector3D& p : pts) {
            if (s * (p.x() - cx) <= 0.0f || p.y() < at(0.30))
                continue;
            const bool clear = !inBody(p) || (p.y() > at(0.62) && s * (p.x() - cx) > shoulderHalf + 0.02f * H);
            // Below the shoulders, only what reaches out past the hips: never a thigh.
            const bool outside = p.y() > at(0.70) || s * (p.x() - cx) > hipHalf + 0.01f * H;
            if (clear && outside)
                arm.push_back(p);
        }
        QVector3D tip, elbow, wrist;
        if (arm.size() < 30) {
            // Arms down against the body: the usual places, straight down.
            elbow = QVector3D(cx + s * (shoulderHalf + 0.01f * H), at(0.63), shoulder.z());
            wrist = QVector3D(cx + s * (shoulderHalf + 0.02f * H), at(0.49), shoulder.z());
            tip = QVector3D(cx + s * (shoulderHalf + 0.02f * H), at(0.39), shoulder.z());
        } else {
            float far = -1.0f;
            for (const QVector3D& p : arm) {
                const float d = (p - shoulder).length();
                if (d > far) {
                    far = d;
                    tip = p;
                }
            }
            const QVector3D u = (tip - shoulder).normalized();
            const float len = (tip - shoulder).length();
            // The middle of the arm across it, at the elbow and the wrist.
            auto across = [&](float t) {
                const QVector3D p0 = shoulder + u * (t * len);
                std::vector<QVector3D> ring;
                for (const QVector3D& p : arm) {
                    const QVector3D d = p - p0;
                    const float along = QVector3D::dotProduct(d, u);
                    if (std::abs(along) < 0.015f * H && (d - u * along).length() < 0.1f * H)
                        ring.push_back(p);
                }
                return ring.size() >= 10 ? centroid(ring, p0) : p0;
            };
            elbow = across(0.42f);
            wrist = across(0.76f);
        }
        j[shoulderJ] = shoulder;
        j[shoulderJ + 1] = elbow;
        j[shoulderJ + 2] = wrist;
        j[shoulderJ + 3] = tip;
        const int hipJ = isLeft ? HipL : HipR;
        j[hipJ] = hip[side];
        j[hipJ + 1] = knee[side];
        j[hipJ + 2] = ankle[side];
        j[hipJ + 3] = toe[side];
    }
    *out = j;
    return true;
}

bool Model3D::bindRig(const std::vector<QVector3D>& J)
{
    if (J.size() != JointCount || !canRig())
        return false;
    // Everything into one space, where it stands now: the skin starts from there.
    const std::vector<QMatrix4x4> world = worldTransforms();
    std::vector<ModelMesh> baked;
    for (const ModelInstance& inst : instances) {
        if (inst.mesh < 0 || inst.mesh >= int(meshes.size()))
            continue;
        ModelMesh m = meshes[size_t(inst.mesh)];
        if (inst.node >= 0 && inst.node < int(world.size())) {
            const QMatrix4x4& t = world[size_t(inst.node)];
            for (QVector3D& p : m.positions)
                p = t.map(p);
            for (QVector3D& n : m.normals)
                n = t.mapVector(n).normalized();
        }
        baked.push_back(std::move(m));
    }
    if (baked.empty())
        return false;

    // The skeleton: the figure's bones, each turned to lie along this model's
    // limb. Worked out facing +Z from the hips, then turned round if it faces away.
    const QQuaternion facing = facesAway(J) ? QQuaternion::fromAxisAndAngle(0, 1, 0, 180) : QQuaternion();
    std::vector<QVector3D> q(J.size());
    for (size_t i = 0; i < J.size(); ++i)
        q[i] = facing.inverted().rotatedVector(J[i] - J[Hips]);
    std::vector<ModelNode> skeleton(kBoneCount);
    std::vector<QQuaternion> bound(kBoneCount);   // each bone's turn as the model stands
    std::vector<float> length(kBoneCount);
    std::vector<QMatrix4x4> inverseBind(kBoneCount);
    for (int b = 0; b < kBoneCount; ++b) {
        const Bone& bone = kBones[b];
        const QQuaternion parentTurn = bone.parent >= 0 ? bound[size_t(bone.parent)] : QQuaternion();
        const QQuaternion rest = QQuaternion::fromAxisAndAngle(0, 0, 1, bone.restZ);
        ModelNode& n = skeleton[size_t(b)];
        n.name = QLatin1String(bone.name);
        n.parent = bone.parent;
        if (bone.parent < 0) {
            n.translation = J[Hips];
            n.rotation = facing * rest;
        } else if (kBones[bone.parent].toward == bone.joint) {
            n.translation = kBones[bone.parent].restDir.normalized() * length[size_t(bone.parent)];
            n.rotation = rest;
        } else {
            n.translation = parentTurn.inverted().rotatedVector(q[size_t(bone.joint)] - q[size_t(kBones[bone.parent].joint)]);
            n.rotation = rest;
        }
        // The turn that lays the bone along the limb.
        const QQuaternion before = parentTurn * rest;
        const QVector3D natural = before.rotatedVector(bone.restDir.normalized());
        QVector3D wanted = q[size_t(bone.toward)] - q[size_t(bone.joint)];
        length[size_t(b)] = wanted.length();
        wanted = wanted.lengthSquared() > 1e-12f ? wanted.normalized() : natural;
        const QQuaternion lay = QQuaternion::rotationTo(natural, wanted);
        bound[size_t(b)] = (lay * before).normalized();
        n.pose = (before.inverted() * lay * before).normalized();
        QMatrix4x4 m;
        m.translate(J[Hips]);
        m.rotate(facing);
        m.translate(q[size_t(bone.joint)]);
        m.rotate(bound[size_t(b)]);
        inverseBind[size_t(b)] = m.inverted();
    }

    // Which bones each point follows: the nearest, counted in each bone's own
    // thickness so that a thin arm does not take the side of a broad chest,
    // and blended between two where they are nearly as near.
    QVector3D lo, hi;
    bounds(&lo, &hi);
    const float H = std::max(1e-6f, hi.y() - lo.y());
    std::vector<float> radius(kBoneCount, 0.05f * H);
    {
        std::vector<std::vector<float>> near(kBoneCount);
        size_t k = 0;
        for (const ModelMesh& m : baked) {
            for (const QVector3D& p : m.positions) {
                if (k++ % 7)
                    continue;
                int best = 0;
                float bestD = std::numeric_limits<float>::max();
                for (int b = 0; b < kBoneCount; ++b) {
                    const float d = segmentDistance(p, J[size_t(kBones[b].joint)], J[size_t(kBones[b].toward)]);
                    if (d < bestD) {
                        bestD = d;
                        best = b;
                    }
                }
                near[size_t(best)].push_back(bestD);
            }
        }
        for (int b = 0; b < kBoneCount; ++b) {
            if (near[size_t(b)].size() >= 8)
                radius[size_t(b)] = std::max(0.012f * H, median(near[size_t(b)]));
        }
    }
    // Near a joint a point shares in the bone on the other side of it, half and
    // half right at the joint and less further off, so that a bent knee or elbow
    // bends smoothly instead of folding with a lip.
    std::vector<int> childOf(kBoneCount, -1);
    for (int b = 0; b < kBoneCount; ++b) {
        for (int c = 0; c < kBoneCount; ++c) {
            if (kBones[c].parent == b && kBones[c].joint == kBones[b].toward)
                childOf[size_t(b)] = c;
        }
    }
    std::vector<float> boneLength(kBoneCount);
    for (int b = 0; b < kBoneCount; ++b)
        boneLength[size_t(b)] = (J[size_t(kBones[b].toward)] - J[size_t(kBones[b].joint)]).length();
    auto blendSpan = [&](int a, int b) {
        const float shorter = std::min(boneLength[size_t(a)], boneLength[size_t(b)]);
        return std::clamp(0.3f * shorter, 0.6f * radius[size_t(a)], 2.0f * radius[size_t(a)]);
    };
    auto smooth = [](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    for (ModelMesh& m : baked) {
        m.joints.assign(m.positions.size(), {0, 0, 0, 0});
        m.weights.assign(m.positions.size(), {1.0f, 0.0f, 0.0f, 0.0f});
        for (size_t v = 0; v < m.positions.size(); ++v) {
            const QVector3D& p = m.positions[v];
            int best = 0;
            float bestD = std::numeric_limits<float>::max();
            for (int b = 0; b < kBoneCount; ++b) {
                const float d = segmentDistance(p, J[size_t(kBones[b].joint)], J[size_t(kBones[b].toward)]) / radius[size_t(b)];
                if (d < bestD) {
                    bestD = d;
                    best = b;
                }
            }
            // Past the end of its bone a point belongs to the next one along the
            // limb, however thick this one is: below the knee is shin, not thigh.
            {
                const QVector3D from = J[size_t(kBones[best].joint)], seg = J[size_t(kBones[best].toward)] - from;
                const float len2 = QVector3D::dotProduct(seg, seg);
                const float t = len2 > 1e-12f ? QVector3D::dotProduct(p - from, seg) / len2 : 0.5f;
                if (t > 1.0f && childOf[size_t(best)] >= 0)
                    best = childOf[size_t(best)];
                else if (t < 0.0f && kBones[best].parent >= 0)
                    best = kBones[best].parent;
            }
            // How far along its bone it lies, from each end.
            const QVector3D a = J[size_t(kBones[best].joint)];
            const QVector3D ab = J[size_t(kBones[best].toward)] - a;
            const float len = boneLength[size_t(best)];
            const float along = len > 1e-9f ? std::clamp(QVector3D::dotProduct(p - a, ab) / len, 0.0f, len) : 0.0f;
            int other = -1;
            float share = 0.0f;
            const int parent = kBones[best].parent;
            if (parent >= 0) {
                const float span = blendSpan(best, parent);
                if (along < span) {
                    other = parent;
                    share = 0.5f * (1.0f - smooth(along / span));
                }
            }
            const int child = childOf[size_t(best)];
            if (child >= 0) {
                const float span = blendSpan(best, child);
                if (len - along < span) {
                    const float s2 = 0.5f * (1.0f - smooth((len - along) / span));
                    if (s2 > share) {
                        other = child;
                        share = s2;
                    }
                }
            }
            // And round a shoulder or a hip, where a limb leaves the trunk: the trunk's points
            // nearest the joint share in the limb, as the limb's share in the trunk.
            for (int c = 0; c < kBoneCount; ++c) {
                if (kBones[c].parent != best || c == child)
                    continue;
                const float span = 1.3f * blendSpan(c, best);
                const float d = (p - J[size_t(kBones[c].joint)]).length();
                if (d < span) {
                    const float s3 = 0.5f * (1.0f - smooth(d / span));
                    if (s3 > share) {
                        other = c;
                        share = s3;
                    }
                }
            }
            m.joints[v] = {best, other >= 0 ? other : best, best, best};
            m.weights[v] = {1.0f - share, other >= 0 ? share : 0.0f, 0.0f, 0.0f};
        }
    }

    // Put together: one skin over everything, and the figure's handles.
    ModelSkin skin;
    for (int b = 0; b < kBoneCount; ++b) {
        skin.joints.push_back(b);
        skin.inverseBind.push_back(inverseBind[size_t(b)]);
    }
    nodes = std::move(skeleton);
    skins = {skin};
    meshes = std::move(baked);
    instances.clear();
    for (size_t i = 0; i < meshes.size(); ++i)
        instances.push_back({int(i), -1, 0});
    auto tip = [&](int bone, int joint) { return inverseBind[size_t(bone)].map(J[size_t(joint)]); };
    handles = {{0, {}, -1, true}, {2, {}, 1, false}, {3, {}, 2, false}, {4, {}, 3, false}, {4, tip(4, HeadTop), 4, false}};
    for (int side = 0; side < 2; ++side) {
        const int arm = side == 0 ? 5 : 8, leg = side == 0 ? 11 : 14;
        const int handTip = side == 0 ? HandTipL : HandTipR, toeJ = side == 0 ? ToeL : ToeR;
        handles.push_back({arm + 1, {}, arm, false});
        handles.push_back({arm + 2, {}, arm + 1, false});
        handles.push_back({arm + 2, tip(arm + 2, handTip), arm + 2, false});
        handles.push_back({leg + 1, {}, leg, false});
        handles.push_back({leg + 2, {}, leg + 1, false});
        handles.push_back({leg + 2, tip(leg + 2, toeJ), leg + 2, false});
    }
    humanoid = true;
    rig = J;
    m_poseScale = std::max(0.01f, (J[Hips].y() - lo.y()) / 0.95f);
    m_floor = lo.y();
    return true;
}
