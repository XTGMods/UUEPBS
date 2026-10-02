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
    ("2.6.2: no distances in the list",
     "parts[#parts + 1] = c.id .. \"\\t\" .. c.label .. c.dist .. \"\\t\" .. c.key", "parts[#parts + 1] = c.id .. \"\\t\" .. c.label .. \"\\t\" .. c.key"),
    ("2.6.2: edited characters capped like the rest",
     "        if not edited then\n            plain = plain + 1\n        end\n        if edited or plain <= cap then",
     "        plain = plain + 1\n        if plain <= cap then"),
    ("2.6.2: kept characters not added to the list",
     "        if k and alive(k.actor) and full_name(k.actor) == k.full then\n            add(k.actor)",
     "        if false then\n            add(k.actor)"),
    ("2.6.2: relist request ignored",
     "            if seen.relist >= 0 and n ~= seen.relist then\n                pendingRelist = true",
     "            if false then\n                pendingRelist = true"),
    ("2.6.2: opening the window always walks the object array",
     "        if liveStale or not USE_LIVE_LIST then", "        if true then"),
    ("2.6.2: a reload doesn't mark the list stale",
     "                liveStale = true -- a reload", "                local _ = true -- a reload"),
    ("2.7: watcher on by default",
     "    return Config.CharacterSwitchWatcher == true", "    return Config.CharacterSwitchWatcher ~= false"),
    ("2.7: watcher never reports",
     "    if switch_on() and Switch.who then\n        add(", "    if false then\n        add("),
    ("2.7: face found by component name only",
     "if name_of(c):lower():find(word, 1, true) or an:lower():find(word, 1, true) then", "if name_of(c):lower():find(word, 1, true) then"),
    ("2.7: head before face",
     "for _, word in ipairs({ \"face\", \"head\" }) do", "for _, word in ipairs({ \"head\", \"face\" }) do"),
    ("2.7: face component not watched",
     "if switch_on() and Switch.comp and chars[PLAYER_KEY] and", "if false and"),
    ("2.7: no name in the window",
     "                label = label .. \" - \" .. switch_display(Switch.who)", "                label = label"),
    ("2.7: player@ entries treated as NPCs",
     "            if a:sub(1, 7) == \"player@\" then\n                if switch_on()", "            if false then\n                if switch_on()"),
    ("2.7: followers not reported",
     "    if switch_on() then\n        for key, l in pairs(Switch.links) do", "    if false then\n        for key, l in pairs(Switch.links) do"),
    ("2.7: linked follower also a remembered NPC",
     "    if Switch.links[address_of(actor)] then\n        return -- a party member's follower", "    if false then\n        return -- a party member's follower"),
    ("2.7: saved party files ignored",
     "                if switch_on() and #a > 7 then\n                    Switch.add(a:sub(8))", "                if false then\n                    Switch.add(a:sub(8))"),
    ("2.7: no look through loaded characters for a new party member",
     "    Switch.party[who] = true\n    for _, e in ipairs(liveCharacters) do", "    Switch.party[who] = true\n    for _, e in ipairs({}) do"),
    ("2.7: everyone looked at in one tick",
     "        if done < Switch.CHECKS_PER_TICK and now >= q.due then", "        if now >= q.due then"),
    ("2.7: gone followers kept",
     "        if not still(l.actor, l.full) then\n            Switch.links[key] = nil", "        if false then\n            Switch.links[key] = nil"),
    ("2.7: anyone with a face linked",
     "    if not who or not Switch.party[who] then", "    if not who then"),
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
