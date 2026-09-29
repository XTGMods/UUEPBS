"""Turns FModel's SK_Roku_v3_Skeleton.json export into roku_ref.txt for the tests."""
import json
import sys

src = sys.argv[1] if len(sys.argv) > 1 else "SK_Roku_v3_Skeleton.json"
data = json.load(open(src, encoding="utf-8"))
skel = next(x for x in data if x["Type"] == "Skeleton")
info = skel["ReferenceSkeleton"]["FinalRefBoneInfo"]
pose = skel["ReferenceSkeleton"]["FinalRefBonePose"]
with open("roku_ref.txt", "w") as out:
    for bone, p in zip(info, pose):
        r, t, s = p["Rotation"], p["Translation"], p["Scale3D"]
        out.write("%s %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n" % (
            bone["Name"], bone["ParentIndex"], r["X"], r["Y"], r["Z"], r["W"],
            t["X"], t["Y"], t["Z"], s["X"], s["Y"], s["Z"]))
print("wrote roku_ref.txt with", len(info), "bones")
