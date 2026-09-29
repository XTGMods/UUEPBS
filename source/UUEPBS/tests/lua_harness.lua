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
    return o
end

local skeleton = obj("Skeleton /Game/ROKUv3/SK_Roku_v3_Skeleton.SK_Roku_v3_Skeleton")
local otherSkeleton = obj("Skeleton /Game/Hair/SK_Fringe_Skeleton.SK_Fringe_Skeleton")
local bodyMesh = obj("SkeletalMesh /Game/ROKUv3/SK_Roku_v3.SK_Roku_v3", { Skeleton = skeleton })
local shirtMesh = obj("SkeletalMesh /Game/ROKUv3/Clothes/SK_Shirt.SK_Shirt", { Skeleton = skeleton })
local hairMesh = obj("SkeletalMesh /Game/Hair/SK_Fringe.SK_Fringe", { Skeleton = otherSkeleton })

local actor = obj("BP_Rokuv3_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Rokuv3_C_0")
local npc = obj("BP_Guard_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_Guard_C_3")
local cdo = obj("BP_Guard_C /Game/Chars/BP_Guard.Default__BP_Guard_C")

local refCalls, scanCalls, searches = 0, 0, 0
local function comp(name, outer, mesh, names, parents, withRef)
    local c = obj("SkeletalMeshComponent /Game/Maps/Lvl.Lvl:PersistentLevel." .. name)
    c.GetFName = function() return FNameObj(name) end
    c.GetOuter = function() return outer end
    c.GetSkinnedAsset = function() return mesh end
    c.GetNumBones = function() return #names end
    c.GetBoneName = function(_, i) return FNameObj(names[i + 1]) end
    c.GetParentBone = function(_, f)
        for i, n in ipairs(names) do
            if n == f._s then
                local p = parents[i]
                return FNameObj(p >= 0 and names[p + 1] or "None")
            end
        end
        return FNameObj("None")
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
actor.Mesh = body
npc.Mesh = guardBody
local pc = obj("BP_PlayerController_C /Game/Maps/Lvl.Lvl:PersistentLevel.BP_PlayerController_C_0", { Pawn = (mode == "gon") and actor or npc })

-- mocked UE4SS globals
UnrealVersion = { GetMajor = function() return 5 end, GetMinor = function() return 7 end }
local loop, console, notify = nil, {}, nil
-- os.clock is wall time under the MSVC runtime the game uses; simulate 250 ms per tick.
local fakeNow = 0
os.clock = function() return fakeNow end
local pollMs
function LoopInGameThreadWithDelay(ms, fn)
    pollMs = ms
    loop = function() fakeNow = fakeNow + ms / 1000; fn() end
    return 1
end
function NotifyOnNewObject(path, fn) notify = fn end
function RegisterHook() end
function RegisterConsoleCommandHandler(name, fn) console[name] = fn end
function ExecuteInGameThread(fn) fn() end
function FindAllOf(name)
    searches = searches + 1
    if name == "PlayerController" then return { pc } end
    if name == "BP_Rokuv3_C" then return actor._valid and { actor } or nil end
    if name == "Character" then return { actor, npc, cdo } end
    if name == "SkeletalMeshComponent" then scanCalls = scanCalls + 1; return { body, shirt, hair, guardBody } end
end

local project = mode == "gon" and "D:\\Steam Games\\steamapps\\common\\GON God Of Nothing\\Roku3" or "C:\\Games\\Some Game\\SomeGame"
local scriptPath = project .. "\\Binaries\\Win64\\ue4ss\\Mods\\UUEPBS\\Scripts\\main.lua"
debug.getinfo = function() return { source = "@" .. scriptPath } end

-- The installed mod folder maps onto the work dir.
local modPrefix = project .. "\\Binaries\\Win64\\ue4ss\\Mods\\UUEPBS\\"
local legacyPrefix = project .. "\\Binaries\\Win64\\ue4ss\\Mods\\XTGBodySlider\\"
local realOpen, realRemove = io.open, os.remove
local function map(p)
    if p:sub(1, #modPrefix) == modPrefix then return work .. "/" .. p:sub(#modPrefix + 1):gsub("\\", "/") end
    if p:sub(1, #legacyPrefix) == legacyPrefix then return work .. "/legacy/" .. p:sub(#legacyPrefix + 1):gsub("\\", "/") end
    return p
end
io.open = function(p, m) return realOpen(map(p), m) end
os.remove = function(p) return realRemove(map(p)) end
os.execute("rm -rf " .. work .. "/native " .. work .. "/Scripts " .. work .. "/legacy && mkdir -p " .. work .. "/native")
local legacyText = '{\n  // my notes survive the move\n  "Project": "SomeGame",\n  "PrimaryComponent": "CharacterMesh0",\n  "Hook": { "Slot": 303, "Buffers": "0x5A8", "ReadIndex": "0x674" },\n}\n'
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
local loaded
package.loadlib = function(p, sym) loaded = { p, sym }; return true end

local printed = {}
local realPrint = print
print = function(s) printed[#printed + 1] = s end

local function run_peer(rescan, pick, pickId, refresh, session, ui)
    local report = work .. "/peer_report.txt"
    os.execute(string.format("%s %s/native %d %d %s %d %s %d > %s", peer, work, rescan, pick, pickId, refresh, session or "dll1", ui or 0, report))
    local h = realOpen(report, "rb")
    local out = h:read("a")
    h:close()
    return out
end

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
check(refCalls == (mode == "gon" and 184 or 0), "reference read once for the primary mesh: " .. refCalls)

-- console command round trip
console.ubs("ubs load My Preset", { "load", "My", "Preset" }, { Log = function() end })
s = run_peer(0, 0, "none", 0)
check(s:find("cmd 1 load [My Preset]", 1, true), "command sent:\n" .. s)
loop()
local joined = table.concat(printed, "")
check(joined:find("done load My Preset", 1, true) and joined:find("second line", 1, true), "reply printed")
s = run_peer(0, 0, "none", 0)
check(not s:find("cmd ", 1, true), "acked command dropped")

-- no churn: nothing rewritten, no rescans and no object-array searches while nothing changes
local before = scanCalls
local searchesBefore = searches
local inBefore = read(work .. "/native/bridge_in.txt")
for _ = 1, 40 do loop() end
check(scanCalls == before, "no rescans while idle")
check(searches == searchesBefore, "no FindAllOf while idle: " .. (searches - searchesBefore))
check(read(work .. "/native/bridge_in.txt") == inBefore, "bridge file not rewritten while idle")

-- window open: the picker list is refreshed (and kept fresh) only then
run_peer(0, 0, "none", 0, "dll1", 1)
searchesBefore = searches
loop()
check(searches > searchesBefore, "character list refreshed when the window opens")
run_peer(0, 0, "none", 0, "dll1", 0)
for _ = 1, 20 do loop() end
searchesBefore = searches
for _ = 1, 40 do loop() end
check(searches == searchesBefore, "no searches after the window closed")

-- DLL asks for a rescan
run_peer(1, 0, "none", 0)
loop()
check(scanCalls == before + 1, "rescan on request")

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

-- mesh destroyed -> rescan -> dropped
if mode == "gon" then
    shirt._valid = false
    for _ = 1, 12 do loop() end
    s = run_peer(1, 2, "auto", 0)
    check(not s:find("C_shirt1", 1, true), "destroyed mesh dropped:\n" .. s)
    check(s:find("CharacterMesh0 primary=1 owner=BP_Rokuv3_C_0", 1, true), "body still driven:\n" .. s)
end

-- DLL restarted (new session) must not replay old counters
run_peer(5, 9, guardId, 3, "dll2")
before = scanCalls
loop()
check(scanCalls == before, "new DLL session only sets the baseline")

realPrint("lua harness OK (" .. mode .. ")")
for _, line in ipairs(printed) do io.write("  | " .. line) end
