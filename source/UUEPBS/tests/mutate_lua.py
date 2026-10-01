"""Mutation check for the Lua harness: breaks one behaviour of Scripts/main.lua at a time and
expects lua_harness.lua to fail. Run from this folder:

    python mutate_lua.py ../../../mod/UUEPBS/Scripts ./bridge_peer /tmp/uuepbs [lua]

Prints CAUGHT / MISSED per mutation; PATTERN? means the code it edits has changed (update the entry).
Exit code = number of mutations missed or stale."""
import os, shutil, subprocess, sys, tempfile

scripts, peer, work = sys.argv[1], sys.argv[2], sys.argv[3]
lua = sys.argv[4] if len(sys.argv) > 4 else "lua"
base = open(os.path.join(scripts, "main.lua"), encoding="utf-8").read()

M = [
    ("all queued characters collected in one tick",
     "until #queue == 0 or (#batch > 0 and not worldSearch)", "until #queue == 0"),
    ("direct lookup never used",
     "            if not worldSearch then\n                comps, from = direct_meshes(e)\n            end",
     "            if false then\n                comps, from = direct_meshes(e)\n            end"),
    ("every plan collects everyone again",
     "if full or waiting[e.key] or not chars[e.key] then", "if true then"),
    ("kept NPCs farthest first",
     "            return x.dist < y.dist\n", "            return x.dist > y.dist\n"),
    ("attached actors ignored",
     "    for _, a in ipairs(attached_actors(e.actor)) do", "    for _, a in ipairs({}) do"),
    ("no world-search fallback",
     "            if comps and (withBones or directProven) then", "            if comps then"),
    ("range hysteresis off",
     "local r = wasNear and RANGE * 1.1 or RANGE", "local r = RANGE"),
    ("settle check ignores the mesh count",
     "    return comps == nil or #comps ~= ch.count", "    return comps == nil"),
    ("settle check re-collects every time",
     "    if ch.how ~= \"direct\" then\n            return true\n        end",
     "    if true then\n            return true\n        end"),
    ("no lookup check",
     "                if s.step == CHECK_STEP and not lookupChecked then", "                if false then"),
    ("lookup check never switches to the world search",
     "    if #missing > 0 then\n            worldSearch = true", "    if false then\n            worldSearch = true"),
    ("leaving range keeps the meshes",
     "        if not e or ch.addr ~= e.addr or ch.full ~= e.full then\n                chars[key] = nil",
     "        if e and (ch.addr ~= e.addr or ch.full ~= e.full) then\n                chars[key] = nil"),
    ("mesh swap not noticed between scans",
     "            if not still(r.comp, r.full) or r.meshAddr ~= mesh_prop(r.comp) then\n                    queue_key(key)",
     "            if not still(r.comp, r.full) then\n                    queue_key(key)"),
    ("budget not applied",
     "if e.kept and #rigs + #ch.rigs > MAX_MESHES then", "if false then"),
    ("player not sent again on a rescan request",
     "                pendingFull = true\n", "\n"),
    ("no session switch after a direct miss",
     "if e.directMiss and not worldSearch and has_bones(e.comps) then", "if false then"),
    ("invalid class object used",
     "        if ok and alive(found) then\n            c = found", "        if ok and found then\n            c = found"),
    ("target not first after a requeue",
     "        if key == targetInfo.key then\n            table.insert(queue, 1, key)", "        if false then\n            table.insert(queue, 1, key)"),
    ("WorldMeshSearch flag ignored",
     "local worldSearch = Profile.WorldMeshSearch == true", "local worldSearch = false"),
    ("2.6.1: skeleton file ignored",
     "        cached = key:sub(1, 1) ~= \"?\" and disk_skeleton(comp, key, count) or nil", "        cached = nil"),
    ("2.6.1: skeleton file trusted without the spot check",
     "    if not (ok and same) then\n        diskSkeletons[key] = nil", "    if false then\n        diskSkeletons[key] = nil"),
    ("2.6.1: skeletons not saved",
     "    if changed then\n        save_disk_skeleton(key, cached)", "    if false then\n        save_disk_skeleton(key, cached)"),
    ("2.6.1: empty lookup always falls back to the world search",
     "            if comps and (withBones or directProven) then", "            if comps and withBones then"),
    ("2.6.1: lookup check used up on a character without meshes",
     "        if not ch or not e or ch.how ~= \"direct\" or #ch.rigs == 0 then\n            return\n        end",
     "        lookupChecked = true\n        if not ch or not e or ch.how ~= \"direct\" or #ch.rigs == 0 then\n            return\n        end"),
    ("2.6.1: full mesh report for every NPC",
     "local first = (brief and not Config.Verbose) and #report or 1", "local first = 1"),
    ("2.6.1: unloaded meshes not summarised",
     "        if #built == 0 and #e.comps > 0 and chars[e.key]", "        if false and chars[e.key]"),
]

bad = 0
tmp = tempfile.mkdtemp(prefix="uuepbs_mut_")
try:
    for name, old, new in M:
        if base.count(old) != 1:
            print("PATTERN?", name)
            bad += 1
            continue
        target = os.path.join(tmp, "Scripts")
        shutil.rmtree(target, ignore_errors=True)
        shutil.copytree(scripts, target)
        with open(os.path.join(target, "main.lua"), "w", encoding="utf-8") as f:
            f.write(base.replace(old, new))
        caught = None
        for mode in ["gon", "generic", "nodir", "broken", "legacy"]:
            r = subprocess.run([lua, "lua_harness.lua", target, peer, work, mode], capture_output=True, text=True)
            if r.returncode != 0:
                caught = mode + ": " + ((r.stderr.strip().splitlines() or ["?"])[0])[:110]
                break
        print(("CAUGHT  " if caught else "MISSED  ") + name + ("  <- " + caught if caught else ""))
        bad += 0 if caught else 1
finally:
    shutil.rmtree(tmp, ignore_errors=True)
sys.exit(bad)
