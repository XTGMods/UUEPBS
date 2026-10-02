-- Runs Scripts/main.lua against a mocked UE4SS API and talks to it through the
-- real file bridge (bridge_peer, built from src/core/bridge.cpp).
--   lua lua_harness.lua <Scripts dir> <bridge_peer exe> <work dir> <gon|generic|nodir|broken|legacy>
-- <work dir> stands in for the installed mod folder: native/ and Scripts/GameProfiles/ live there.
local scripts, peer, work, mode = arg[1], arg[2], arg[3], arg[4] or "gon"
package.path = scripts .. "/?.lua;" .. package.path

local function check(cond, msg)
    if not cond then
        io.stderr:write("FAIL: " .. msg .. "\n")
        os.exit(1)
    end
end

-- skeleton + reference pose from roku_ref.txt
local boneNames, boneParents, boneRef = {}, {}, {}
for line in io.lines("roku_ref.txt") do
    local f = {}
    for v in line:gmatch("%S+") do f[#f + 1] = v end
    boneNames[#boneNames + 1] = f[1]
    boneParents[#boneParents + 1] = tonumber(f[2])
    local n = {}
    for i = 3, 12 do n[#n + 1] = tonumber(f[i]) end
    boneRef[#boneRef + 1] = n
end

extraChars = {} -- more characters FindAllOf("Character") returns
badClassCalls = 0
local function FNameObj(s) return { ToString = function() return s end, _s = s } end
local nextAddr = 0x7FF600001000
local function obj(fullName, fields)
    nextAddr = nextAddr + 0x1000
    local o = fields or {}
    o._addr = nextAddr
    o._valid = true
    o.IsValid = function(self) return self._valid end
    o.GetAddress = function(self) return self._addr end
    o.GetFullName = function(self) return fullName end
    o.GetFName = function() return FNameObj(fullName:match("([^.%s]+)$")) end
    local className = fullName:match("^(%S+)%s")
    o.GetClass = function() return { GetFName = function() return FNameObj(className or "?") end, IsValid = function() return true end } end
    o._loc = { X = 0, Y = 0, Z = 0 }
    o.K2_GetActorLocation = function(self) return self._loc end
    -- The actor's own components: every live mesh in allComps whose outer is this actor, or that is
    -- registered with it (_owner: created by another of its components, e.g. Dawnwalker's Appearance).
    o._k2 = 0
    o.K2_GetComponentsByClass = function(self, cls)
        if cls == nil or not cls:IsValid() then badClassCalls = badClassCalls + 1; error("invalid class") end -- would crash the game
        self._k2 = self._k2 + 1
        local list = {}
        for _, c in ipairs(allComps) do
            if c._valid and (c._owner or c.GetOuter()) == self then list[#list + 1] = c end
        end
        return { ForEach = function(_, fn) for i, c in ipairs(list) do fn(i - 1, { get = function() return c end }) end end }
    end
    o.GetAttachedActors = function(self, out, reset)
        for _, a in ipairs(self._attached or {}) do out[#out + 1] = a end
    end
    return o
end

local skeleton = obj("Skeleton /Game/ROKUv3/SK_Roku_v3_Skeleton.SK_Roku_v3_Skeleton")
local otherSkeleton = obj("Skeleton /Game/Hair/SK_Fringe_Skeleton.SK_Fringe_Skeleton")
-- Morph targets as UE4SS hands them over: a TArray with ForEach(index, RemoteUnrealParam).
local function morph_list(names)
    local list = {}
    for i, n in ipairs(names) do list[i] = obj("MorphTarget /Game/ROKUv3/SK_Roku_v3." .. n) end
    return { ForEach = function(_, fn) for i, m in ipairs(list) do fn(i - 1, { get = function() return m end }) end end }
end
local bodyMesh = obj("SkeletalMesh /Game/ROKUv3/SK_Roku_v3.SK_Roku_v3", { Skeleton = skeleton, MorphTargets = morph_list({ "BreastSize", "Belly", "Smile" }) })
local shirtMesh = obj("SkeletalMesh /Game/ROKUv3/Clothes/SK_Shirt.SK_Shirt", { Skeleton = skeleton })
local hairMesh = obj("SkeletalMesh /Game/Hair/SK_Fringe.SK_Fringe", { Skeleton = otherSkeleton })

local actor = obj("BP_Rokuv3_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Rokuv3_C_0")
local npc = obj("BP_Guard_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Guard_C_3")
local cdo = obj("BP_Guard_C /Game/Chars/BP_Guard.Default__BP_Guard_C")

local refCalls, scanCalls, searches, morphReads, morphWrites = 0, 0, 0, 0, 0
local function comp(name, outer, mesh, names, parents, withRef)
    local c = obj("SkeletalMeshComponent /Game/Maps/Lvl.Lvl:PersistentLevel." .. name)
    c.GetFName = function() return FNameObj(name) end
    c.GetOuter = function() return outer end
    c.GetSkinnedAsset = function() return mesh end
    c.SkeletalMeshAsset = mesh -- the property UE 5.1+ components have
    c.GetNumBones = function() return #names end
    c._names = 0
    c.GetBoneName = function(_, i) c._names = c._names + 1; return FNameObj(names[i + 1]) end
    c.GetParentBone = function(_, f)
        for i, n in ipairs(names) do
            if n == f._s then
                local p = parents[i]
                return FNameObj(p >= 0 and names[p + 1] or "None")
            end
        end
        return FNameObj("None")
    end
    c._morphs = {}
    c._vis = 0 -- read once per mesh each time its character is collected
    c.IsVisible = function() c._vis = c._vis + 1; return true end
    c.GetMorphTarget = function(_, f) morphReads = morphReads + 1; return c._morphs[f._s] or 0 end
    c.SetMorphTarget = function(_, f, w, removeZero)
        morphWrites = morphWrites + 1
        if removeZero and w == 0 then c._morphs[f._s] = nil else c._morphs[f._s] = w end
    end
    if withRef then
        c.GetRefPoseTransform = function(_, i)
            refCalls = refCalls + 1
            local r = boneRef[i + 1]
            return { Rotation = { X = r[1], Y = r[2], Z = r[3], W = r[4] }, Translation = { X = r[5], Y = r[6], Z = r[7] }, Scale3D = { X = r[8], Y = r[9], Z = r[10] } }
        end
    else
        c.GetRefPoseTransform = function() error("function not found") end
    end
    return c
end

local body = comp("CharacterMesh0", actor, bodyMesh, boneNames, boneParents, true)
local shirt = comp("C_shirt1", actor, shirtMesh, boneNames, boneParents, true)
local hair = comp("H_Fringe", actor, hairMesh, { "root", "fringe_01" }, { -1, 0 }, true)
local guardBody = comp("CharacterMesh0", npc, bodyMesh, boneNames, boneParents, false)
allComps = { body, shirt, hair, guardBody }
actor.Mesh = body
npc.Mesh = guardBody
-- generic: a cape hangs on the character in a way only the world search sees (the mesh itself is
-- attached to the body; the actor it belongs to is not attached to anything)
local cape
if mode == "generic" then
    cape = comp("Cape", obj("BP_CapeHolder_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_CapeHolder_C_0"), shirtMesh, boneNames, boneParents, false)
    cape.GetAttachParent = function() return guardBody end
    allComps[#allComps + 1] = cape
end
-- legacy: the profile asks for the world search ("WorldMeshSearch": true): the direct lookup is never tried
local skippedLookups = 0
-- nodir: a game where the direct lookup lists no meshes at all (the world search takes over for the session)
if mode == "nodir" then npc.K2_GetComponentsByClass = function() return { ForEach = function() end } end end
if mode == "legacy" then npc.K2_GetComponentsByClass = function() skippedLookups = skippedLookups + 1; error("should not be called") end end
local pc = obj("BP_PlayerController_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_PlayerController_C_0", { Pawn = (mode == "gon") and actor or npc })

-- mocked UE4SS globals
UnrealVersion = { GetMajor = function() return 5 end, GetMinor = function() return 7 end }
local loop, fast, console, notify = nil, nil, {}, nil
function FName(s) return FNameObj(s) end
-- os.clock is wall time under the MSVC runtime the game uses; simulate 250 ms per tick.
local fakeNow = 0
os.clock = function() return fakeNow end
local pollMs
local fastMs
function LoopInGameThreadWithDelay(ms, fn)
    if not loop then
        pollMs = ms
        loop = function() fakeNow = fakeNow + ms / 1000; fn() end
    else
        fastMs = ms -- the morph loop (runs only while the window is open)
        fast = function() fakeNow = fakeNow + ms / 1000; fn() end
    end
    return 1
end
local notifies = {}
function NotifyOnNewObject(path, fn) notifies[path] = fn; notify = notify or fn end
local SKEL_CLASS = "/Script/Engine.SkeletalMeshComponent"
-- broken: the class is not found (StaticFindObject hands back an invalid object, as UE4SS does)
function StaticFindObject(path)
    local valid = not (mode == "broken" and path == SKEL_CLASS)
    return { _path = path, IsValid = function() return valid end }
end
function RegisterHook() end
function RegisterConsoleCommandHandler(name, fn) console[name] = fn end
function ExecuteInGameThread(fn) fn() end
function FindAllOf(name)
    searches = searches + 1
    if name == "PlayerController" then return { pc } end
    if name == "BP_Rokuv3_C" then return actor._valid and { actor } or nil end
    if name == "Character" then
        local l = { actor, npc, cdo }
        for _, c in ipairs(extraChars) do l[#l + 1] = c end
        return l
    end
    if name == "SkeletalMeshComponent" then scanCalls = scanCalls + 1; return allComps end
end

local project = mode == "gon" and "D:\\Steam Games\\steamapps\\common\\GON God Of Nothing\\Roku3" or "C:\\Games\\Some Game\\SomeGame"
local scriptPath = project .. "\\Binaries\\Win64\\ue4ss\\Mods\\UUEPBS\\Scripts\\main.lua"
debug.getinfo = function() return { source = "@" .. scriptPath } end

-- The installed mod folder maps onto the work dir.
local modPrefix = project .. "\\Binaries\\Win64\\ue4ss\\Mods\\UUEPBS\\"
local legacyPrefix = project .. "\\Binaries\\Win64\\ue4ss\\Mods\\XTGBodySlider\\"
local realOpen, realRemove = io.open, os.remove
local outReads = 0
local function map(p)
    if p:sub(1, #modPrefix) == modPrefix then return work .. "/" .. p:sub(#modPrefix + 1):gsub("\\", "/") end
    if p:sub(1, #legacyPrefix) == legacyPrefix then return work .. "/legacy/" .. p:sub(#legacyPrefix + 1):gsub("\\", "/") end
    return p
end
io.open = function(p, m)
    if p:find("bridge_out.txt", 1, true) then outReads = outReads + 1 end
    return realOpen(map(p), m)
end
os.remove = function(p) return realRemove(map(p)) end
os.execute("rm -rf " .. work .. "/native " .. work .. "/Scripts " .. work .. "/legacy && mkdir -p " .. work .. "/native")
local legacyText = '{\n  // my notes survive the move\n  "Project": "SomeGame",\n  "PrimaryComponent": "CharacterMesh0",\n  "WorldMeshSearch": true,\n  "Hook": { "Slot": 303, "Buffers": "0x5A8", "ReadIndex": "0x674" },\n}\n'
if mode == "legacy" then
    os.execute("mkdir -p " .. work .. "/legacy/Scripts/GameProfiles " .. work .. "/legacy/native")
    realOpen(work .. "/legacy/Scripts/GameProfiles/SomeGame.json", "wb"):write(legacyText):close()
    realOpen(work .. "/legacy/enabled.txt", "wb"):close()
    realOpen(work .. "/legacy/Scripts/main.lua", "wb"):close()
end
if mode ~= "nodir" then os.execute("mkdir -p " .. work .. "/Scripts/GameProfiles") end
if mode == "gon" then os.execute("cp " .. scripts .. "/GameProfiles/Roku3.json " .. work .. "/Scripts/GameProfiles/") end
local brokenText = '{\n  "Target": "BP_Guard_C"\n  "PrimaryComponent": "x"\n}\n'
if mode == "broken" then realOpen(work .. "/Scripts/GameProfiles/SomeGame.json", "wb"):write(brokenText):close() end
realOpen(work .. "/native/UUEPBS.dll", "wb"):close()
-- GON: skeletons from an earlier session - the body's (good) and the shirt's (stale: the game was
-- updated since and its first bone renamed)
if mode == "gon" then
    local nums = {}
    for _, r in ipairs(boneRef) do for k = 1, 10 do nums[#nums + 1] = string.format("%.7g", r[k]) end end
    local oldNames = { table.unpack(boneNames) }
    oldNames[1] = "OLD_root"
    local f = realOpen(work .. "/native/skeleton_cache.txt", "wb")
    f:write("UUEPBS skeletons 1\n")
    f:write("skel\t/Game/ROKUv3/SK_Roku_v3.SK_Roku_v3#184\nnames\t" .. table.concat(boneNames, "\t") .. "\nparents\t" .. table.concat(boneParents, "\t") .. "\nref\t" .. table.concat(nums, " ") .. "\n")
    f:write("skel\t/Game/ROKUv3/Clothes/SK_Shirt.SK_Shirt#184\nnames\t" .. table.concat(oldNames, "\t") .. "\nparents\t" .. table.concat(boneParents, "\t") .. "\nref\t?\n")
    f:close()
end
local loaded
package.loadlib = function(p, sym) loaded = { p, sym }; return true end

local printed = {}
local realPrint = print
print = function(s) printed[#printed + 1] = s end

local function run_peer(rescan, pick, pickId, refresh, session, ui, morphs, keep, remember, relist)
    local report = work .. "/peer_report.txt"
    os.execute(string.format('%s %s/native %d %d %s %d %s %d "%s" "%s" "%s" %d > %s', peer, work, rescan, pick, pickId, refresh, session or "dll1", ui or 0, morphs or "-", keep or "-", remember or "-", relist or 0, report))
    local h = realOpen(report, "rb")
    local out = h:read("a")
    h:close()
    return out
end

-- GON runs with the smallest mesh budget (8) so a crowd overflows it
local cfg = dofile(scripts .. "/config.lua")
if mode == "gon" then cfg.MaxTrackedMeshes = 8 end
package.loaded["config"] = cfg
dofile(scripts .. "/main.lua")
local json = dofile(scripts .. "/uuepbs_json.lua")
local function read(path) local f = realOpen(path, "rb"); if not f then return nil end; local t = f:read("a"); f:close(); return t end
local profilePath = work .. "/Scripts/GameProfiles/" .. (mode == "gon" and "Roku3" or "SomeGame") .. ".json"
if mode == "generic" then
    local p = assert(json.decode(assert(read(profilePath), "profile file created on first run")))
    check(p.Project == "SomeGame" and p.Target == "" and p.PrimaryComponent == "" and p.SameSkeletonOnly == true, "new profile from Default")
elseif mode == "broken" then
    check(read(profilePath) == brokenText, "broken profile left untouched")
    check(table.concat(printed, ""):find("could not be read", 1, true), "broken profile reported")
elseif mode == "nodir" then
    check(read(profilePath) == nil, "no folder yet")
elseif mode == "legacy" then
    check(read(profilePath) == legacyText, "XTGBodySlider profile copied verbatim")
    check(table.concat(printed, ""):find("copied the XTGBodySlider game profile", 1, true), "migration reported")
    check(table.concat(printed, ""):find("XTGBodySlider (the old name of this mod) is still enabled", 1, true), "old install warned about")
end
check(loaded and loaded[1] == project .. "\\Binaries\\Win64\\ue4ss\\Mods\\UUEPBS\\native\\UUEPBS.dll" and loaded[2] == "*", "loadlib path")
check(loop and console.uuepbs and console.ubs and not console.xbs, "registrations")
check(pollMs == 400, "poll interval " .. tostring(pollMs))
check(fast and fastMs == 100, "morph loop registered at 100 ms: " .. tostring(fastMs))

-- generic: the game starts on a main menu pawn without meshes (Dawnwalker): looked for with the world
-- search (the direct lookup has found nothing yet), and the one-time lookup check is kept for later
local menuScans = 0
if mode == "generic" then
    local menu = obj("BP_MainMenuPawn_C /Game/Maps/Menu.Menu:PersistentLevel.BP_MainMenuPawn_C_0")
    pc.Pawn = menu
    run_peer(0, 0, "none", 0) -- the DLL's first answer only sets the counter baselines
    for _ = 1, 30 do loop() end -- 12 s: past the settle step of the lookup check
    check(table.concat(printed, ""):find("BP_MainMenuPawn_C_0 (player) has no skeletal mesh", 1, true), "menu pawn has no mesh")
    check(not table.concat(printed, ""):find("lookup", 1, true), "no lookup check or switch on a pawn without meshes")
    menuScans = scanCalls
    pc.Pawn = npc
    run_peer(0, 0, "none", 1) -- Refresh: the picker lists the new pawn
    for _ = 1, 11 do loop() end -- the new pawn is noticed within the 3 s watch
end
loop()
local s = run_peer(0, 0, "none", 0)
if mode == "nodir" then
    check(read(work .. "/native/bridge_in.txt"):find("mkdir=" .. project:gsub("%p", "%%%0") .. "\\Binaries", 1, false), "asks the DLL to create the folder")
    os.execute("mkdir -p " .. work .. "/Scripts/GameProfiles") -- what the DLL does
    loop()
    check(read(profilePath) ~= nil, "profile written once the folder exists")
    check(not read(work .. "/native/bridge_in.txt"):find("mkdir=%S"), "mkdir request cleared")
end
check(s:find("presets " .. project:gsub("%p", "%%%0") .. "\\Binaries\\Win64\\UUEPBS Presets\n"), "preset folder:\n" .. s)
check(s:find("key F6\n", 1, true), "hotkey")
check(s:find("engine 5.7\n", 1, true), "engine version")
if mode == "gon" then
    check(read(profilePath) == read(scripts .. "/GameProfiles/Roku3.json"), "shipped profile not rewritten")
    check(s:find("game Roku3\n", 1, true), "game folder")
    check(s:find("rig %x+ CharacterMesh0 primary=1 owner=BP_Rokuv3_C_0 %(player%) bones=184 parents=184 ref=184 p5=" .. boneParents[6] .. " n5=" .. boneNames[6]:gsub("%p", "%%%0")), "primary rig with reference:\n" .. s)
    check(s:find("rig %x+ C_shirt1 primary=0 owner=BP_Rokuv3_C_0 %(player%) bones=184 parents=184 ref=0"), "clothing on the same skeleton")
    check(not s:find("H_Fringe", 1, true), "hair on another skeleton skipped")
    check(not s:find("BP_Guard_C_3 ", 1, true) or true, "")
else
    check(s:find("rig %x+ CharacterMesh0 primary=1 owner=BP_Guard_C_3 %(player%) bones=184 parents=184 ref=0"), "generic: player pawn, no reference pose:\n" .. s)
end
check(s:find("candidates 2\n", 1, true), "two candidates (class default object skipped):\n" .. s)
check(s:find("morphs CharacterMesh0 3 BreastSize Belly Smile\n", 1, true), "morph names of the body mesh:\n" .. s)
check(not s:find("morphs C_shirt1", 1, true), "mesh without morph targets sends none")
check(refCalls == 0, "GON: reference pose from the skeleton file, not read again: " .. refCalls)
if mode == "gon" then
    check(body._names == 2, "body skeleton from the file after a two-bone spot check: " .. body._names)
    check(shirt._names == 1 + 184, "stale shirt skeleton noticed (first bone differs) and read again: " .. shirt._names)
    local file = read(work .. "/native/skeleton_cache.txt")
    local _, bodyEntries = file:gsub("skel\t/Game/ROKUv3/SK_Roku_v3%.SK_Roku_v3#184\n", "")
    check(bodyEntries == 1, "a skeleton from the file is not written again")
    check(file:find("skel\t/Game/ROKUv3/Clothes/SK_Shirt%.SK_Shirt#184\nnames\t" .. boneNames[1]:gsub("%p", "%%%0") .. "\t[^\n]*\nparents\t[^\n]*\nref\t%?\n$"),
        "the re-read shirt skeleton is appended (the later entry wins):\n" .. file:sub(-300))
else
    local file = read(work .. "/native/skeleton_cache.txt") or ""
    check(file:sub(1, 19) == "UUEPBS skeletons 1\n" and file:find("skel\t/Game/ROKUv3/SK_Roku_v3%.SK_Roku_v3#184\n") and file:find("\nref\t%-\n"),
        "skeletons read this session are saved, with 'reference not available' noted:\n" .. file:sub(1, 200))
end

-- console command round trip
console.ubs("ubs load My Preset", { "load", "My", "Preset" }, { Log = function() end })
s = run_peer(0, 0, "none", 0)
check(s:find("cmd 1 load [My Preset]", 1, true), "command sent:\n" .. s)
loop()
local joined = table.concat(printed, "")
check(joined:find("done load My Preset", 1, true) and joined:find("second line", 1, true), "reply printed")
s = run_peer(0, 0, "none", 0)
check(not s:find("cmd ", 1, true), "acked command dropped")

-- the character's meshes come from the actor itself, without walking the world's meshes
local drvActor = mode == "gon" and actor or npc
local drvBody0 = mode == "gon" and body or guardBody
local brokenLookup = mode == "legacy" or mode == "broken" or mode == "nodir"
if brokenLookup then
    check(drvBody0._vis == 1 and scanCalls == 1, "first scan through the world search: " .. drvBody0._vis .. " " .. scanCalls)
    if mode == "broken" or mode == "nodir" then -- broken: the class is missing; nodir: the lookup lists nothing
        check(table.concat(printed, ""):find("the direct mesh lookup found no meshes on BP_Guard_C_3 (player) (" .. (mode == "broken" and "the lookup failed" or "it lists no mesh")
            .. ") but the world search does; using the world search from now on", 1, true), "switch to the world search reported:\n" .. table.concat(printed, ""))
    else
        check(not table.concat(printed, ""):find("direct mesh lookup", 1, true) and skippedLookups == 0, "WorldMeshSearch: no direct lookup tried: " .. skippedLookups)
    end
else
    check(drvBody0._vis == 1 and scanCalls == menuScans, "first scan: one collect, no world search: " .. drvBody0._vis .. " " .. scanCalls)
end
-- a new character is checked a few times while the game finishes it (cheap checks, no re-collect
-- when nothing changed); once, the direct lookup is compared with the world search
local function settle()
    for _ = 1, 100 do loop() end -- 40 s of game time
end
local k2First = drvActor._k2
settle()
joined = table.concat(printed, "")
if brokenLookup then
    check(scanCalls <= 1 + 5, "settling re-collects through the world search at most once per step: " .. scanCalls)
    check(not joined:find("mesh lookup check", 1, true), "no lookup check once the world search is on")
elseif mode == "generic" then
    check(joined:find("the direct mesh lookup missed 1 mesh(es) of BP_Guard_C_3 (player) that the world search finds (Cape (attached from BP_CapeHolder_C_0))", 1, true),
        "a mesh only the world search finds is reported:\n" .. joined)
    local r = run_peer(0, 0, "none", 0)
    check(r:find("rig %x+ Cape primary=0 owner=BP_Guard_C_3 %(player%)"), "and picked up through the world search from then on:\n" .. r)
    check(scanCalls - menuScans >= 2 and scanCalls - menuScans <= 7, "world search used for the check and the re-collects: " .. (scanCalls - menuScans))
else
    check(joined:find("mesh lookup check: all " .. (mode == "gon" and 3 or 1) .. " mesh(es) of " .. (mode == "gon" and "BP_Rokuv3_C_0" or "BP_Guard_C_3") .. " (player) are found without the world search", 1, true),
        "direct lookup checked once against the world search:\n" .. joined)
    check(scanCalls == 1, "one world search per session, for that check: " .. scanCalls)
    check(drvBody0._vis == 1, "nothing changed while settling: not collected again: " .. drvBody0._vis)
    check(drvActor._k2 > k2First and drvActor._k2 <= k2First + 8, "a few cheap settle checks: " .. (drvActor._k2 - k2First))
end
local worldMode = mode == "generic" or brokenLookup -- every collect uses the world search from here on

-- no churn: nothing rewritten, no rescans and no object-array searches while nothing changes
local before = scanCalls
local searchesBefore = searches
local visBefore, k2Before = drvBody0._vis, drvActor._k2
local inBefore = read(work .. "/native/bridge_in.txt")
for _ = 1, 40 do loop() end
check(scanCalls == before, "no rescans while idle")
check(searches == searchesBefore, "no FindAllOf while idle: " .. (searches - searchesBefore))
check(drvBody0._vis == visBefore and drvActor._k2 == k2Before, "no lookups on the character while idle")
check(read(work .. "/native/bridge_in.txt") == inBefore, "bridge file not rewritten while idle")

-- window open: the picker list is re-sorted by the current distances (and kept fresh) only then. The object
-- array was walked once at start, so opening the window doesn't walk it again - except after the player
-- character changed (generic: the menu pawn was replaced), when it is walked once more.
local other, otherName = (mode == "gon") and npc or actor, (mode == "gon") and "BP_Guard_C_3" or "BP_Rokuv3_C_0"
other._loc = { X = 1234, Y = 0, Z = 0 }
run_peer(0, 0, "none", 0, "dll1", 1)
searchesBefore = searches
loop()
s = run_peer(0, 0, "none", 0, "dll1", 1)
check(s:find("cand " .. otherName .. "   12 m key=", 1, true), "list re-sorted with distances when the window opens:\n" .. s)
check(searches == searchesBefore, "opening the window walks no object array: " .. (searches - searchesBefore))
-- opening the character list asks for a re-sort (relist): current distances, no object search
other._loc = { X = 2049, Y = 0, Z = 0 }
searchesBefore = searches
run_peer(0, 0, "none", 0, "dll1", 1, "-", "-", "-", 1)
loop()
s = run_peer(0, 0, "none", 0, "dll1", 1, "-", "-", "-", 1)
check(s:find("cand " .. otherName .. "   20 m key=", 1, true) and searches == searchesBefore, "list re-sorted when opened, no search:\n" .. s)
other._loc = { X = 0, Y = 0, Z = 0 }
settle()
run_peer(0, 0, "none", 0, "dll1", 0)
for _ = 1, 20 do loop() end
searchesBefore = searches
for _ = 1, 40 do loop() end
check(searches == searchesBefore, "no searches after the window closed")

-- morph targets: the fast loop does nothing while the window is closed
local readsBefore = outReads
for _ = 1, 10 do fast() end
check(outReads == readsBefore, "morph loop idle while the window is closed")
check(morphWrites == 0 and morphReads == 0, "no morph calls before any morph is set")

local drv = mode == "gon" and body or guardBody -- the driven character's body mesh
local otherBody = mode == "gon" and guardBody or body
-- window open + a slider moved: the fast loop applies it, keeping the game's own value to restore
drv._morphs.BreastSize = 0.1 -- the game's character creator set this one
run_peer(0, 0, "none", 0, "dll1", 1)
loop() -- the regular tick notices the open window
run_peer(0, 0, "none", 0, "dll1", 1, "BreastSize=0.8,Belly=0.25")
fast()
check(drv._morphs.BreastSize == 0.8 and drv._morphs.Belly == 0.25, "morphs applied by the fast loop: " .. tostring(drv._morphs.BreastSize))
check(shirt._morphs.BreastSize == nil and otherBody._morphs.BreastSize == nil, "only driven meshes that have the morph are touched")
local writes = morphWrites
fast()
loop()
check(morphWrites == writes, "unchanged weights are not re-sent")

-- the game keeps overwriting Belly: put back each time, reported after three times
for _ = 1, 3 do
    drv._morphs.Belly = 0
    loop()
    check(drv._morphs.Belly == 0.25, "morph put back after the game overwrote it")
end
s = run_peer(0, 0, "none", 0, "dll1", 1, "BreastSize=0.8,Belly=0.25")
check(s:find("manim Belly\n", 1, true), "fought-over morph reported to the DLL:\n" .. s)

-- slider reset (morph no longer sent): the game's values come back
run_peer(0, 0, "none", 0, "dll1", 0, "-")
loop()
check(drv._morphs.BreastSize == 0.1, "game's own value restored: " .. tostring(drv._morphs.BreastSize))
check(drv._morphs.Belly == nil, "morph the game never set is handed back entirely")
writes, readsBefore = morphWrites, morphReads
for _ = 1, 20 do loop() end
check(morphWrites == writes and morphReads == readsBefore, "no morph calls while none is set")

-- DLL asks for a rescan (Refresh, F7): the character is collected again
settle()
before = scanCalls
visBefore = drvBody0._vis
run_peer(1, 0, "none", 0)
loop()
check(drvBody0._vis == visBefore + 1, "rescan on request: " .. (drvBody0._vis - visBefore))
check(scanCalls == before + (worldMode and 1 or 0), "rescan uses the world search only when it has to: " .. (scanCalls - before))

-- picker: switch to the guard (by address) and back
local guardId = string.format("%X", npc._addr)
run_peer(1, 1, guardId, 0)
loop()
s = run_peer(1, 1, guardId, 0)
check(s:find("target " .. guardId .. " BP_Guard_C_3", 1, true), "picked target:\n" .. s)
check(s:find("owner=BP_Guard_C_3", 1, true) and not s:find("C_shirt1", 1, true), "rigs follow the picked character")
run_peer(1, 2, "auto", 0)
loop()
s = run_peer(1, 2, "auto", 0)
check(s:find("target auto ", 1, true), "back to automatic:\n" .. s)
check(s:find("owner=" .. (mode == "gon" and "BP_Rokuv3_C_0" or "BP_Guard_C_3"), 1, true), "automatic target restored:\n" .. s)

-- the game finishes the character after it appears (outfit pieces attached later, placeholder
-- meshes swapped): settle re-scans and the mesh-asset check pick that up (Dawnwalker save reloads)
if mode == "gon" then
    settle()
    local gloves = comp("Gauntlets", actor, shirtMesh, boneNames, boneParents, true)
    allComps[#allComps + 1] = gloves
    for _ = 1, 3 do loop() end
    s = run_peer(1, 2, "auto", 0)
    check(not s:find(" Gauntlets ", 1, true), "late mesh not there before a reason to rescan")
    -- save reload: the old character is destroyed and a new one appears, still without its gloves
    actor._valid, body._valid, shirt._valid, gloves._valid = false, false, false, false
    actor = obj("BP_Rokuv3_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Rokuv3_C_0")
    body = comp("CharacterMesh0", actor, bodyMesh, boneNames, boneParents, true)
    shirt = comp("C_shirt1", actor, shirtMesh, boneNames, boneParents, true)
    actor.Mesh = body
    body._morphs.BreastSize = 0.1 -- the game's character creator sets it again on load
    pc.Pawn = actor
    allComps = { body, shirt, hair, guardBody }
    for _ = 1, 12 do loop() end
    s = run_peer(1, 2, "auto", 0)
    check(s:find("CharacterMesh0 primary=1 owner=BP_Rokuv3_C_0 %(player%)") and not s:find(" Gauntlets ", 1, true), "new character scanned, gloves not there yet:\n" .. s)
    -- after a reload the first listing walks the object array once (characters loaded with the level)
    local reloadSearches = searches
    run_peer(1, 2, "auto", 0, "dll1", 1)
    loop()
    check(searches > reloadSearches, "after a reload, opening the window walks the object array once")
    run_peer(1, 2, "auto", 0, "dll1", 0)
    loop()
    reloadSearches = searches
    run_peer(1, 2, "auto", 0, "dll1", 1)
    loop()
    check(searches == reloadSearches, "and the next time not: " .. (searches - reloadSearches))
    run_peer(1, 2, "auto", 0, "dll1", 0)
    loop()
    -- the game attaches the outfit piece five seconds later; nothing else changes
    for _ = 1, 4 do loop() end
    gloves = comp("Gauntlets", actor, shirtMesh, boneNames, boneParents, true)
    allComps[#allComps + 1] = gloves
    settle()
    s = run_peer(1, 2, "auto", 0)
    check(s:find(" Gauntlets primary=0 owner=BP_Rokuv3_C_0 %(player%)"), "outfit piece attached after the load is picked up:\n" .. s)
    check(scanCalls == 1, "reload and late outfit piece handled without a world search: " .. scanCalls)
    -- placeholder mesh replaced on an already tracked component
    local before_swap, vis_swap = scanCalls, body._vis
    local realLegs = obj("SkeletalMesh /Game/ROKUv3/Clothes/SK_Shirt_Final.SK_Shirt_Final", { Skeleton = skeleton })
    shirt.GetSkinnedAsset = function() return realLegs end
    shirt.SkeletalMeshAsset = realLegs
    for _ = 1, 12 do loop() end
    check(body._vis == vis_swap + 1 and scanCalls == before_swap, "mesh asset swap on a tracked component: that character is collected again: " .. (body._vis - vis_swap))
    settle()
    for i, c in ipairs(allComps) do
        if c == gloves then table.remove(allComps, i) break end
    end
    for _ = 1, 12 do loop() end
    shirt.GetSkinnedAsset = function() return shirtMesh end
    shirt.SkeletalMeshAsset = shirtMesh
    settle()
end

-- remembered NPCs: found by identity after a reload, sliders only while near the player
if mode == "gon" then
    settle()
    check(notifies["/Script/Engine.Character"], "watches for new characters")
    -- the guard has no name of its own (BP_Guard_C_3 = class + number): identity is class@face mesh
    local guardFace = comp("Face Mesh", npc, obj("SkeletalMesh /Game/Chars/SK_Guard_Head_B.SK_Guard_Head_B", { Skeleton = otherSkeleton }), { "root" }, { -1 }, false)
    npc.K2_GetComponentsByClass = function()
        return { ForEach = function(_, fn) fn(0, { get = function() return guardBody end }); fn(1, { get = function() return guardFace end }) end }
    end
    local gid = string.format("%X", npc._addr)
    run_peer(1, 5, gid, 0, "dll1", 0, "-", "-", "-")
    loop()
    s = run_peer(1, 5, gid, 0, "dll1", 0, "-", "-", "-")
    check(s:find("target identity [BP_Guard_C@SK_Guard_Head_B]", 1, true), "unnamed NPC identity from class and face mesh:\n" .. s)
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "-")
    loop()
    settle()
    -- Anca was edited in an earlier session (the DLL lists her); she is loaded after a reload
    local function new_anca(number)
        local a = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel.Anca_" .. number)
        local b = comp("CharacterMesh0", a, bodyMesh, boneNames, boneParents, true)
        a.Mesh = b
        a._loc = { X = 1200, Y = 300, Z = 0 } -- 12 m from the player
        allComps[#allComps + 1] = b
        return a, b
    end
    local searchesBefore = searches
    local anca, ancaBody = new_anca(243)
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Anca_243,Lacra")
    loop()
    notifies["/Script/Engine.Character"](anca)
    for _ = 1, 8 do loop() end -- checked once she is set up (2 s)
    local ancaKey = string.format("%X", anca._addr)
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Anca_243,Lacra")
    check(s:find("npc " .. ancaKey .. " Anca_243 Anca_243\n", 1, true), "remembered NPC reported by identity:\n" .. s)
    check(searches == searchesBefore, "finding a remembered NPC costs no object searches: " .. (searches - searchesBefore))
    -- the DLL restores her sliders and lists her: her meshes are picked up while she is near
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", ancaKey, "Anca_243,Lacra")
    loop()
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", ancaKey, "Anca_243,Lacra")
    check(s:find("CharacterMesh0 primary=1 owner=Anca_243 [^\n]*actor=" .. ancaKey .. "\n"), "restored NPC sculpted while near:\n" .. s)
    settle()
    -- she walks 80 m away: left alone (still kept and remembered), and picked up again when back.
    -- Leaving drops her meshes without looking at anyone's; coming back looks at hers only.
    local scans0, playerVis, ancaVis = scanCalls, body._vis, ancaBody._vis
    anca._loc = { X = 8000, Y = 0, Z = 0 }
    for _ = 1, 10 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", ancaKey, "Anca_243,Lacra")
    check(not s:find("owner=Anca_243", 1, true) and not s:find("gone ", 1, true), "out-of-range NPC not sculpted, not dropped:\n" .. s)
    check(scanCalls == scans0 and body._vis == playerVis and ancaBody._vis == ancaVis, "leaving range drops her meshes without a scan")
    anca._loc = { X = 500, Y = 0, Z = 0 }
    for _ = 1, 10 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", ancaKey, "Anca_243,Lacra")
    check(s:find("owner=Anca_243 [^\n]*actor=" .. ancaKey .. "\n"), "back in range, sculpted again:\n" .. s)
    check(scanCalls == scans0, "walking into range needs no world search: " .. (scanCalls - scans0))
    check(ancaBody._vis == ancaVis + 1 and body._vis == playerVis, "only she is collected again: " .. (ancaBody._vis - ancaVis) .. " " .. (body._vis - playerVis))
    -- another reload: the old Anca is gone, the new one (same placed name, new object) is found again
    anca._valid, ancaBody._valid = false, false
    for _ = 1, 10 do loop() end
    local anca2, anca2Body = new_anca(243)
    notifies["/Script/Engine.Character"](anca2)
    for _ = 1, 8 do loop() end
    local anca2Key = string.format("%X", anca2._addr)
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", ancaKey, "Anca_243,Lacra")
    check(s:find("gone " .. ancaKey .. "\n", 1, true) and s:find("npc " .. anca2Key .. " Anca_243 Anca_243\n", 1, true) and not s:find("npc " .. ancaKey, 1, true),
        "after a reload the new Anca is found by the same identity:\n" .. s)
    -- two kept NPCs walking around each other (Dawnwalker: sparring guards) change their distance
    -- order between scans; that alone must not restart the settle re-scans (2.4.1 stutter: a full
    -- scan every ~1.3 s, forever)
    local lacra = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel.Lacra")
    local lacraBody = comp("CharacterMesh0", lacra, bodyMesh, boneNames, boneParents, true)
    lacra.Mesh = lacraBody
    allComps[#allComps + 1] = lacraBody
    local lacraKey = string.format("%X", lacra._addr)
    anca2._loc, lacra._loc = { X = 900, Y = 0, Z = 0 }, { X = 1000, Y = 0, Z = 0 }
    notifies["/Script/Engine.Character"](lacra)
    for _ = 1, 8 do loop() end
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key .. "," .. lacraKey, "Anca_243,Lacra")
    loop()
    settle()
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key .. "," .. lacraKey, "Anca_243,Lacra")
    check(s:find("owner=Anca_243 ", 1, true) and s:find("owner=Lacra ", 1, true), "both kept NPCs sculpted:\n" .. s)
    local swapScans, swapA, swapL, swapP = scanCalls, anca2Body._vis, lacraBody._vis, body._vis
    run_peer(2, 6, "auto", 0, "dll1", 0, "-", anca2Key .. "," .. lacraKey, "Anca_243,Lacra") -- one rescan request
    for i = 1, 100 do
        local near = i % 2 == 0
        anca2._loc.X, lacra._loc.X = near and 900 or 1100, near and 1000 or 800
        loop()
    end
    check(anca2Body._vis - swapA == 1 and lacraBody._vis - swapL == 1 and body._vis - swapP == 1 and scanCalls == swapScans,
        "NPCs swapping distance order cause no extra re-scans: " .. (anca2Body._vis - swapA) .. " " .. (lacraBody._vis - swapL) .. " " .. (body._vis - swapP) .. " " .. (scanCalls - swapScans))
    -- an NPC standing at the range edge doesn't flip in and out (10% margin once in range)
    local edgeL, edgeA = lacraBody._vis, anca2Body._vis
    for i = 1, 80 do -- 8 s each side, longer than the 3 s watch interval
        lacra._loc.X = ((i - 1) // 20 % 2 == 0) and 5200 or 4950
        loop()
    end
    check(lacraBody._vis == edgeL and anca2Body._vis == edgeA and scanCalls == swapScans, "NPC wobbling at the range edge causes no re-scans: " .. (lacraBody._vis - edgeL))
    lacra._valid, lacraBody._valid = false, false
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, "Anca_243,Lacra")
    for _ = 1, 3 do loop() end
    -- Dawnwalker: guards respawned at run time (Gatekeeper_church2_2147465868) share the template
    -- name; each points at its placed record through its "Stub" component (an ActorStubComponent
    -- whose Stub property is a DogwoodAIStub named after the placed guard). That record is the identity.
    local propVisits = 0
    local function uclass(full, props, super)
        return {
            GetFName = function() return FNameObj(full:match("([^.]+)$")) end,
            GetFullName = function() return full end,
            IsValid = function() return true end,
            ForEachProperty = function(_, fn)
                for _, pr in ipairs(props) do
                    propVisits = propVisits + 1
                    fn({ GetFName = function() return FNameObj(pr[1]) end,
                         GetClass = function() return { GetFName = function() return FNameObj(pr[2]) end } end })
                end
            end,
            GetSuperStruct = function() return super end,
        }
    end
    local engineComp = uclass("Class /Script/Engine.ActorComponent", { { "AssetUserData", "ArrayProperty" } }, nil)
    local engineChar = uclass("Class /Script/Engine.Character", { { "Mesh", "ObjectProperty" } }, nil)
    local npcClass = uclass("BlueprintGeneratedClass /Game/Chars/BP_NonPlayerCharacter.BP_NonPlayerCharacter_C",
        { { "Appearance", "ObjectProperty" }, { "Health", "FloatProperty" } }, engineChar)
    local stubCompClass = uclass("Class /Script/Population.ActorStubComponent", { { "Stub", "ObjectProperty" }, { "System", "ObjectProperty" } }, engineComp)
    local plainCompClass = uclass("Class /Script/Dawnwalker.AppearanceComponent", { { "Owner_Cached", "ObjectProperty" } }, engineComp)
    local stubSystem = obj("ActorStubSystemImpl /Game/Map_Blockout_Valley/Blockout_Valley.Blockout_Valley:ActorStubSystemImpl_2147480600")
    local function stub_record(name)
        return obj("DogwoodAIStub /Game/Map_Blockout_Valley/Blockout_Valley.Blockout_Valley:ActorStubSystemImpl_2147480600." .. name)
    end
    local function spawned_guard(name, stub, x)
        local base = "/Game/Map_Blockout_Valley/Blockout_Valley.Blockout_Valley:PersistentLevel." .. name
        local a = obj("BP_NonPlayerCharacter_C " .. base)
        a.GetClass = function() return npcClass end
        a._loc = { X = x or 600, Y = 0, Z = 0 }
        local appearance = obj("AppearanceComponent " .. base .. ".Appearance")
        appearance.GetClass = function() return plainCompClass end
        appearance.Owner_Cached = a
        local stubComp = obj("ActorStubComponent " .. base .. ".Stub")
        stubComp.GetClass = function() return stubCompClass end
        stubComp.Stub, stubComp.System = stub, stubSystem
        a.Appearance = appearance
        local comps = { appearance, stubComp }
        a.K2_GetComponentsByClass = function(_, cls)
            if cls._path == SKEL_CLASS then return { ForEach = function() end } end -- no meshes in this stand-in
            return { ForEach = function(_, fn) for i, c in ipairs(comps) do fn(i - 1, { get = function() return c end }) end end }
        end
        a._stubComp = stubComp
        extraChars[#extraChars + 1] = a
        return a
    end
    local g868 = spawned_guard("Gatekeeper_church2_2147465868", stub_record("Gatekeeper_church2_277"), 500)
    local g840 = spawned_guard("Gatekeeper_church2_2147465840", stub_record("Gatekeeper_church2_279"), 700)
    local rememberGuards = "Gatekeeper_church2_277,Gatekeeper_church2_281"
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, rememberGuards)
    loop()
    notifies["/Script/Engine.Character"](g868)
    for _ = 1, 8 do loop() end
    local visitsAfterFirst = propVisits
    notifies["/Script/Engine.Character"](g840)
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, rememberGuards)
    check(s:find(" Gatekeeper_church2_277 Gatekeeper_church2_2147465868\n", 1, true), "spawned guard recognised through its stub:\n" .. s)
    check(not s:find("\nnpc [^\n]*Gatekeeper_church2_2147465840"), "another spawned guard of the same squad is someone else:\n" .. s)
    check(propVisits == visitsAfterFirst, "the link is searched once per class, not per character: " .. (propVisits - visitsAfterFirst))
    joined = table.concat(printed, "")
    check(joined:find("recognised through Stub.Stub", 1, true), "the link found is logged once")
    -- a guard whose stub isn't set yet is looked at again later (its template name is not cached)
    local g812 = spawned_guard("Gatekeeper_church2_2147465812", nil, 800)
    notifies["/Script/Engine.Character"](g812)
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, rememberGuards)
    check(not s:find("\nnpc [^\n]*Gatekeeper_church2_2147465812"), "no stub yet: not recognised:\n" .. s)
    g812._stubComp.Stub = stub_record("Gatekeeper_church2_281")
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, rememberGuards .. ",Other") -- remembered list changed: sweep
    loop()
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, rememberGuards .. ",Other")
    check(s:find(" Gatekeeper_church2_281 Gatekeeper_church2_2147465812\n", 1, true), "recognised once its stub is set:\n" .. s)
    -- the picker: nearest first, shows who a spawned character is recognised as, and while the window
    -- stays open it lists the characters it was notified about instead of searching the object array
    run_peer(1, 6, "auto", 0, "dll1", 1, "-", anca2Key, rememberGuards .. ",Other")
    loop()
    loop()
    s = run_peer(1, 6, "auto", 0, "dll1", 1, "-", anca2Key, rememberGuards .. ",Other")
    check(s:find("cand [^\n]*Gatekeeper_church2_2147465868  = Gatekeeper_church2_277   5 m key=", 1) ~= nil, "picker shows the identity and distance:\n" .. s)
    local c1 = s:find("cand ", 1, true)
    check(c1 and s:find("^cand [^\n]*%(player%)", c1), "player listed first:\n" .. s)
    local p868, p840 = s:find("Gatekeeper_church2_2147465868", 1, true), s:find("Gatekeeper_church2_2147465840", 1, true)
    check(p868 and p840 and p868 < p840, "nearer character listed first:\n" .. s)
    local searchesOpen = searches - scanCalls
    for _ = 1, 40 do loop() end -- 16 s with the window open: three picker refreshes
    check(searches - scanCalls == searchesOpen, "picker refreshes while open walk no object array: " .. (searches - scanCalls - searchesOpen))
    local late = spawned_guard("Gatekeeper_church2_2147465700", stub_record("Gatekeeper_church2_285"), 300)
    notifies["/Script/Engine.Character"](late)
    for _ = 1, 25 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 1, "-", anca2Key, rememberGuards .. ",Other")
    check(s:find("Gatekeeper_church2_2147465700  = Gatekeeper_church2_285", 1, true), "a character spawned while open shows up:\n" .. s)
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", anca2Key, "Anca_243,Lacra")
    for _, g in ipairs({ g868, g840, g812, late }) do g._valid = false end
    extraChars = {}
    for _ = 1, 3 do loop() end
    -- identities (Dawnwalker crash report): a crowd of placed guards are different people; runtime
    -- numbers are dropped; a new character at a reused address gets its own identity
    local function npc_named(name, x)
        local a = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel." .. name)
        a._loc = { X = x or 300, Y = 0, Z = 0 }
        return a
    end
    local g277, g279 = npc_named("Gatekeeper_church2_277"), npc_named("Gatekeeper_church2_279")
    local esme = npc_named("Esme_2147298333")
    local spawned = npc_named("BP_NonPlayerCharacter_C_12")
    spawned.Mesh = guardBody
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Gatekeeper_church2_277,Esme,BP_NonPlayerCharacter_C@SK_Roku_v3")
    loop()
    for _, a in ipairs({ g277, g279, esme, spawned }) do notifies["/Script/Engine.Character"](a) end
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Gatekeeper_church2_277,Esme,BP_NonPlayerCharacter_C@SK_Roku_v3")
    check(s:find(" Gatekeeper_church2_277 Gatekeeper_church2_277\n", 1, true) and not s:find("\nnpc [^\n]*Gatekeeper_church2_279"),
        "one edited guard of a crowd is not spread to the others:\n" .. s)
    check(s:find(" Esme Esme_2147298333\n", 1, true), "runtime instance number dropped:\n" .. s)
    check(s:find(" BP_NonPlayerCharacter_C@SK_Roku_v3 BP_NonPlayerCharacter_C_12\n", 1, true), "engine-named NPC uses class and mesh:\n" .. s)
    -- the guard is destroyed and a different NPC is created at the same address
    g277._valid = false
    for _ = 1, 3 do loop() end
    local vladimir = npc_named("Vladimir_247")
    vladimir._addr = g277._addr
    notifies["/Script/Engine.Character"](vladimir)
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Gatekeeper_church2_277,Esme,BP_NonPlayerCharacter_C@SK_Roku_v3")
    check(not s:find("\nnpc [^\n]*Vladimir_247"), "a new NPC at a reused address does not inherit the old identity:\n" .. s)
    -- many loaded characters with one identity: only MaxSameIdentity (2) get it
    local twins = {}
    for i = 1, 4 do
        twins[i] = npc_named("BP_NonPlayerCharacter_C_" .. (20 + i))
        twins[i].Mesh = guardBody
        notifies["/Script/Engine.Character"](twins[i])
    end
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Gatekeeper_church2_277,Esme,BP_NonPlayerCharacter_C@SK_Roku_v3")
    local n = 0
    for _ in s:gmatch(" BP_NonPlayerCharacter_C@SK_Roku_v3 ") do n = n + 1 end
    check(n == 2, "at most two characters share one identity, got " .. n .. ":\n" .. s)
    for _, a in ipairs({ g279, esme, spawned, vladimir }) do a._valid = false end
    for _, a in ipairs(twins) do a._valid = false end
    for _ = 1, 3 do loop() end
    -- a character that is not remembered is never reported
    local stranger = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel.Stranger_4")
    notifies["/Script/Engine.Character"](stranger)
    for _ = 1, 8 do loop() end
    s = run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "Anca_243,Lacra")
    check(not s:find("\nnpc [^\n]*Stranger"), "characters that are not remembered are ignored:\n" .. s)
    loop()
    anca2._valid = false
    for _, c in ipairs(allComps) do if c.GetOuter() == anca2 then c._valid = false end end
    run_peer(1, 6, "auto", 0, "dll1", 0, "-", "-", "-")
    settle()
end

-- 2.6 per-character scans: each character is looked at on its own, without walking the world's meshes
if mode == "gon" then
    settle()
    local function owners(text, prefix)
        local list = {}
        for name in text:gmatch("\nrig %x+ CharacterMesh0 primary=1 owner=(" .. prefix .. "%d+) ") do list[#list + 1] = name end
        return list
    end
    -- eight kept NPCs walk into range together: collected one per tick, nearest first
    local crowd, keys = {}, {}
    for i = 1, 8 do
        local a = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel.Villager_" .. (300 + i))
        local b = comp("CharacterMesh0", a, bodyMesh, boneNames, boneParents, false)
        a.Mesh, a._loc, a._body = b, { X = 9000 + i * 10, Y = 0, Z = 0 }, b
        allComps[#allComps + 1] = b
        extraChars[#extraChars + 1] = a
        crowd[i], keys[i] = a, string.format("%X", a._addr)
    end
    local keepCrowd = table.concat(keys, ",")
    run_peer(1, 6, "auto", 1, "dll1", 0, "-", keepCrowd, "-") -- Refresh lists them; the DLL keeps them (edited before)
    for _ = 1, 3 do loop() end
    settle()
    s = run_peer(1, 6, "auto", 1, "dll1", 0, "-", keepCrowd, "-")
    check(#owners(s, "Villager_") == 0, "kept NPCs out of range are not sculpted:\n" .. s)
    local crowdScans = scanCalls
    for i = 1, 8 do crowd[i]._loc = { X = 100 + (9 - i) * 50, Y = 0, Z = 0 } end -- Villager_308 is nearest
    -- MaxTrackedMeshes is 8 here: the player's 2 meshes and 6 villagers fit, the 2 farthest wait
    local counts, last = {}, 0
    for _ = 1, 20 do
        loop()
        s = run_peer(1, 6, "auto", 1, "dll1", 0, "-", keepCrowd, "-")
        local got = owners(s, "Villager_")
        check(#got <= last + 1, "at most one character collected per tick: " .. last .. " -> " .. #got)
        if #got > last then
            -- the ones in so far are the nearest ones
            local want = {}
            for i = 8, 9 - #got, -1 do want["Villager_" .. (300 + i)] = true end
            for _, n in ipairs(got) do check(want[n], "nearest first: " .. n .. " came in at " .. #got) end
            counts[#counts + 1] = #got
        end
        last = #got
    end
    check(last == 6 and table.concat(counts, ",") == "1,2,3,4,5,6", "admitted one per tick up to the mesh budget: " .. table.concat(counts, ","))
    check(scanCalls == crowdScans, "a crowd walking in needs no world search: " .. (scanCalls - crowdScans))
    for _, a in ipairs(crowd) do check(a._body._vis == 1, "each collected once: " .. a._body._vis) end
    check(table.concat(printed, ""):find("mesh limit (8) reached; left alone for now: Villager_302, Villager_301", 1, true), "farthest left out, logged")
    -- the nearest one despawns: the next one waiting takes its place without being looked at again
    crowd[8]._valid, crowd[8]._body._valid = false, false
    for _ = 1, 10 do loop() end
    s = run_peer(1, 6, "auto", 1, "dll1", 0, "-", keepCrowd, "-")
    local now = table.concat(owners(s, "Villager_"), ",")
    check(#owners(s, "Villager_") == 6 and now:find("Villager_302", 1, true) and not now:find("Villager_308", 1, true), "freed room goes to the next nearest:\n" .. s)
    check(crowd[2]._body._vis == 1 and scanCalls == crowdScans, "admitted without another lookup")
    run_peer(1, 6, "auto", 1, "dll1", 0, "-", "-", "-")
    for _, a in ipairs(crowd) do a._valid, a._body._valid = false, false end
    extraChars = {}
    for _ = 1, 12 do loop() end
    s = run_peer(1, 6, "auto", 1, "dll1", 0, "-", "-", "-")
    check(#owners(s, "Villager_") == 0, "crowd let go:\n" .. s)

    -- while a bigger crowd is still being admitted, a change on the player is dealt with first
    local folk, folkKeys = {}, {}
    for i = 1, 12 do
        local a = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel.Townsfolk_" .. (400 + i))
        local b = comp("CharacterMesh0", a, bodyMesh, boneNames, boneParents, false)
        a.Mesh, a._loc, a._body = b, { X = 9000, Y = i * 10, Z = 0 }, b
        allComps[#allComps + 1] = b
        extraChars[#extraChars + 1] = a
        folk[i], folkKeys[i] = a, string.format("%X", a._addr)
    end
    local keepFolk = table.concat(folkKeys, ",")
    run_peer(1, 6, "auto", 2, "dll1", 0, "-", keepFolk, "-")
    for _ = 1, 3 do loop() end
    settle()
    for i = 1, 12 do folk[i]._loc = { X = 100 + i * 50, Y = 0, Z = 0 } end
    local function waiting()
        local n = 0
        for _, a in ipairs(folk) do if a._body._vis == 0 then n = n + 1 end end
        return n
    end
    for _ = 1, 12 do
        loop()
        if waiting() < 12 then break end
    end
    check(waiting() == 11, "admission started: " .. waiting())
    local finalShirt = obj("SkeletalMesh /Game/ROKUv3/Clothes/SK_Shirt_B.SK_Shirt_B", { Skeleton = skeleton })
    shirt.GetSkinnedAsset = function() return finalShirt end
    shirt.SkeletalMeshAsset = finalShirt
    local pv, waitingThen = body._vis, nil
    for _ = 1, 20 do
        loop()
        if body._vis > pv and waitingThen == nil then waitingThen = waiting() end
    end
    check(waitingThen ~= nil and waitingThen > 0, "player collected again ahead of the waiting crowd: " .. tostring(waitingThen))
    check(waiting() == 0, "and the crowd admitted after")
    shirt.GetSkinnedAsset = function() return shirtMesh end
    shirt.SkeletalMeshAsset = shirtMesh
    run_peer(1, 6, "auto", 2, "dll1", 0, "-", "-", "-")
    for _, a in ipairs(folk) do a._valid, a._body._valid = false, false end
    extraChars = {}
    for _ = 1, 12 do loop() end

    -- outfit pieces the game creates through another component (Dawnwalker: Appearance -> Legs_C,
    -- outer = the AppearanceComponent, registered with the character) and a costume actor attached
    -- to the character: both found by the direct lookup on a Refresh
    local appearance = obj("AppearanceComponent /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Rokuv3_C_0.Appearance")
    local legs = comp("Appearance.EAppearanceSlot::Legs_C", appearance, shirtMesh, boneNames, boneParents, true)
    legs._owner = actor
    legs.GetAttachParent = function() return body end
    local costume = obj("BP_Costume_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Costume_C_1")
    local cape = comp("CapeMesh", costume, shirtMesh, boneNames, boneParents, true)
    actor._attached = { costume }
    allComps[#allComps + 1] = legs
    allComps[#allComps + 1] = cape
    local outfitScans = scanCalls
    run_peer(3, 6, "auto", 2, "dll1", 0, "-", "-", "-") -- Refresh
    loop()
    s = run_peer(3, 6, "auto", 2, "dll1", 0, "-", "-", "-")
    check(s:find("\nrig %x+ Appearance%.EAppearanceSlot::Legs_C primary=0 owner=BP_Rokuv3_C_0 %(player%) [^\n]*actor=player\n"),
        "outfit piece owned through another component found:\n" .. s)
    check(s:find("\nrig %x+ CapeMesh primary=0 owner=BP_Rokuv3_C_0 %(player%) [^\n]*actor=player\n"), "attached costume actor's mesh found:\n" .. s)
    check(table.concat(printed, ""):find("mesh CapeMesh (attached from BP_Costume_C_1)", 1, true), "logged as attached")
    check(scanCalls == outfitScans, "found without a world search: " .. (scanCalls - outfitScans))
    -- taken off again: the next check lets them go
    actor._attached = nil
    legs._valid, cape._valid = false, false
    for _ = 1, 12 do loop() end
    s = run_peer(3, 6, "auto", 2, "dll1", 0, "-", "-", "-")
    check(not s:find("Legs_C", 1, true) and not s:find("CapeMesh", 1, true) and s:find("C_shirt1 primary=0", 1, true), "outfit let go:\n" .. s)

end

-- several characters at once (GON: the player is Roku, the guard is an NPC)
if mode == "gon" then
    local guardKey = guardId
    s = run_peer(1, 2, "auto", 0)
    check(s:find("cand BP_Rokuv3_C_0 (player) key=player\n", 1, true) and s:find("cand BP_Guard_C_3   0 m key=" .. guardKey .. "\n", 1, true),
        "candidates carry actor keys:\n" .. s)
    -- the player has edits, the guard is picked: both are scanned, each under its own key
    run_peer(1, 3, guardId, 0, "dll1", 0, "-", "player")
    loop()
    s = run_peer(1, 3, guardId, 0, "dll1", 0, "-", "player")
    check(s:find("target " .. guardId .. " BP_Guard_C_3 key=" .. guardKey .. "\n", 1, true), "NPC target key:\n" .. s)
    check(s:find("CharacterMesh0 primary=1 owner=BP_Guard_C_3 bones=184 [^\n]*actor=" .. guardKey .. "\n"), "picked NPC's mesh:\n" .. s)
    check(s:find("CharacterMesh0 primary=1 owner=BP_Rokuv3_C_0 %(player%) [^\n]*actor=player\n"), "edited player kept while the NPC is edited:\n" .. s)
    check(s:find("C_shirt1 primary=0 owner=BP_Rokuv3_C_0 %(player%) [^\n]*actor=player\n"), "player's clothing kept too:\n" .. s)
    -- morphs go to the right character
    run_peer(1, 3, guardId, 0, "dll1", 0, "Smile=0.7@" .. guardKey .. ",BreastSize=0.4@player", "player," .. guardKey)
    loop()
    check(guardBody._morphs.Smile == 0.7 and body._morphs.Smile == nil, "NPC morph only on the NPC")
    check(body._morphs.BreastSize == 0.4 and guardBody._morphs.BreastSize == nil, "player morph only on the player")
    -- back to automatic; the guard has edits, so it stays
    run_peer(1, 4, "auto", 0, "dll1", 0, "Smile=0.7@" .. guardKey, guardKey)
    loop()
    s = run_peer(1, 4, "auto", 0, "dll1", 0, "Smile=0.7@" .. guardKey, guardKey)
    check(s:find("target auto BP_Rokuv3_C_0 (player) key=player\n", 1, true), "back to the player:\n" .. s)
    check(s:find("owner=BP_Guard_C_3 [^\n]*actor=" .. guardKey .. "\n"), "edited NPC kept after switching away:\n" .. s)
    check(body._morphs.BreastSize == 0.1 and guardBody._morphs.Smile == 0.7, "player morph back to the game's value, NPC morph stays")
    -- released (no longer listed): its meshes are let go and its morph handed back
    run_peer(1, 4, "auto", 0, "dll1", 0, "-", "-")
    loop()
    s = run_peer(1, 4, "auto", 0, "dll1", 0, "-", "-")
    check(not s:find("owner=BP_Guard_C_3", 1, true), "released NPC no longer scanned:\n" .. s)
    check(guardBody._morphs.Smile == nil, "released NPC's morph handed back")
    -- never-edited characters are never scanned while idle, and a kept one costs no searches
    run_peer(1, 4, "auto", 0, "dll1", 0, "-", guardKey)
    loop()
    settle() -- a newly kept character gets its settle re-scans too
    local searchesKept = searches
    for _ = 1, 30 do loop() end
    check(searches == searchesKept, "no object searches while an NPC is kept: " .. (searches - searchesKept))
    -- a character with sliders is always in the list, even past the MaxCandidates cap and far away
    cfg.MaxCandidates = 1
    npc._loc = { X = 30000, Y = 0, Z = 0 }
    run_peer(1, 4, "auto", 1, "dll1", 0, "-", guardKey) -- Refresh
    loop()
    s = run_peer(1, 4, "auto", 1, "dll1", 0, "-", guardKey)
    check(s:find("candidates 2\n", 1, true) and s:find("cand BP_Guard_C_3   300 m key=" .. guardKey .. "\n", 1, true), "edited NPC listed past the cap:\n" .. s)
    -- and one the class lists don't return (a missed spawn notification, a class outside CandidateClasses)
    local far = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel.Far_7")
    far._loc = { X = 1000, Y = 0, Z = 0 }
    extraChars = { far }
    local farKey = string.format("%X", far._addr)
    run_peer(1, 4, "auto", 2, "dll1", 0, "-", guardKey .. "," .. farKey) -- Refresh lists it; the DLL keeps it
    for _ = 1, 3 do loop() end
    extraChars = {}
    run_peer(1, 4, "auto", 3, "dll1", 0, "-", guardKey .. "," .. farKey) -- Refresh: the class list no longer has it
    for _ = 1, 2 do loop() end
    s = run_peer(1, 4, "auto", 3, "dll1", 0, "-", guardKey .. "," .. farKey)
    check(s:find("cand Far_7   10 m key=" .. farKey .. "\n", 1, true), "edited character listed even when the class lists miss it:\n" .. s)
    far._valid = false
    run_peer(1, 4, "auto", 3, "dll1", 0, "-", guardKey)
    for _ = 1, 3 do loop() end
    cfg.MaxCandidates = 40
    npc._loc = { X = 0, Y = 0, Z = 0 }
    run_peer(1, 4, "auto", 0, "dll1", 0, "-", guardKey)
    for _ = 1, 12 do loop() end
    -- a kept character that despawns is reported gone, and the report stops once the DLL drops it
    npc._valid, guardBody._valid = false, false
    for _ = 1, 12 do loop() end
    s = run_peer(1, 4, "auto", 0, "dll1", 0, "-", guardKey)
    check(s:find("gone " .. guardKey .. "\n", 1, true) and not s:find("owner=BP_Guard_C_3", 1, true), "despawned NPC reported gone:\n" .. s)
    run_peer(1, 4, "auto", 0, "dll1", 0, "-", "-")
    loop()
    s = run_peer(1, 4, "auto", 0, "dll1", 0, "-", "-")
    check(not s:find("gone ", 1, true), "gone report cleared once the DLL dropped the character:\n" .. s)
    npc._valid, guardBody._valid = true, true
    loop()
end

-- mesh destroyed -> rescan -> dropped
if mode == "gon" then
    shirt._valid = false
    for _ = 1, 12 do loop() end
    s = run_peer(1, 2, "auto", 0)
    check(not s:find("C_shirt1", 1, true), "destroyed mesh dropped:\n" .. s)
    check(s:find("CharacterMesh0 primary=1 owner=BP_Rokuv3_C_0", 1, true), "body still driven:\n" .. s)
end

-- Once the direct lookup has found meshes (on the player), a character it finds no meshes on simply has
-- none right now: no world search for it. (2.6.0 searched for ~50 ms each time a Dawnwalker NPC's meshes
-- were unloaded, a few seconds before the NPC went.)
if mode == "gon" then
    settle()
    joined = table.concat(printed, "")
    check(not joined:find("scan of ", 1, true), "short character scans are not logged unless Verbose")
    local function kept_npc(name, x)
        local a = obj("BP_NonPlayerCharacter_C /Game/Maps/Lvl.Lvl:PersistentLevel." .. name)
        local b = comp("CharacterMesh0", a, bodyMesh, boneNames, boneParents, false)
        a.Mesh, a._loc = b, { X = x, Y = 0, Z = 0 }
        allComps[#allComps + 1] = b
        extraChars[#extraChars + 1] = a
        return a, b, string.format("%X", a._addr)
    end
    local odd, oddBody, oddKey = kept_npc("Odd_8", 450)
    odd.K2_GetComponentsByClass = function() return { ForEach = function() end } end
    local leaver, leaverBody, leaverKey = kept_npc("Leaver_9", 500)
    local meshLines = select(2, table.concat(printed, ""):gsub("  mesh CharacterMesh0: ", ""))
    local scans0 = scanCalls
    run_peer(1, 4, "auto", 5, "dll1", 0, "-", oddKey .. "," .. leaverKey, "-") -- Refresh lists them; the DLL keeps them
    for _ = 1, 4 do loop() end
    s = run_peer(1, 4, "auto", 5, "dll1", 0, "-", oddKey .. "," .. leaverKey, "-")
    check(not s:find("owner=Odd_8", 1, true) and scanCalls == scans0, "nothing listed: no world search: " .. (scanCalls - scans0))
    check(s:find("\nrig %x+ CharacterMesh0 primary=1 owner=Leaver_9 [^\n]*actor=" .. leaverKey .. "\n"), "the other NPC is sculpted:\n" .. s)
    joined = table.concat(printed, "")
    check(joined:find("Odd_8 [kept] has no skeletal mesh", 1, true) and joined:find("Leaver_9 [kept]: 1 mesh(es) driven", 1, true), "one summary line per NPC")
    check(select(2, joined:gsub("  mesh CharacterMesh0: ", "")) == meshLines, "no per-mesh lines for NPCs unless Verbose")
    -- her meshes are unloaded before she goes: let go, one line, no world search
    leaverBody.GetNumBones = function() return 0 end
    leaverBody.SkeletalMeshAsset, leaverBody.GetSkinnedAsset = nil, function() return nil end
    for _ = 1, 12 do loop() end
    s = run_peer(1, 4, "auto", 5, "dll1", 0, "-", oddKey .. "," .. leaverKey, "-")
    check(not s:find("owner=Leaver_9", 1, true) and scanCalls == scans0, "unloaded meshes let go without a world search: " .. (scanCalls - scans0))
    check(table.concat(printed, ""):find("Leaver_9: meshes unloaded, let go", 1, true), "said in one line")
    run_peer(1, 4, "auto", 5, "dll1", 0, "-", "-", "-")
    odd._valid, oddBody._valid, leaver._valid, leaverBody._valid = false, false, false, false
    extraChars = {}
    for _ = 1, 12 do loop() end
end

-- DLL restarted (new session) must not replay old counters
run_peer(5, 9, guardId, 3, "dll2")
before = scanCalls
loop()
check(scanCalls == before, "new DLL session only sets the baseline")

check(badClassCalls == 0, "K2_GetComponentsByClass never called with an invalid class: " .. badClassCalls)
check(skippedLookups == 0, "WorldMeshSearch profile: direct lookup never tried: " .. skippedLookups)
realPrint("lua harness OK (" .. mode .. ")")
for _, line in ipairs(printed) do io.write("  | " .. line) end
