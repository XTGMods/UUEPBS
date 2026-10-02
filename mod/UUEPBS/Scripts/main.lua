-- UUEPBS (Universal Unreal Engine Player Body Sliders) - Lua half
--
-- Finds the character to edit, reads the bone hierarchy (and morph target names) of its
-- skeletal meshes and hands everything to native\UUEPBS.dll through two small text files
-- (the DLL edits the final pose every frame and owns the slider window and presets).
-- Morph target weights come back from the DLL and are applied here with SetMorphTarget.
-- The DLL is a plain Windows DLL mapped with package.loadlib, not a UE4SS C++ mod,
-- so it does not have to match the UE4SS version.
--
-- Performance notes: everything here runs on the game thread, so the script avoids
-- whole-object-array searches (FindAllOf). Each character's meshes are read from that
-- character alone, one character per tick, and only when it is new or changed. Between
-- those it only checks that the objects it already holds are still valid, and it only
-- rewrites the bridge file when something actually changed.

local VERSION = "2.6.2"
local TAG = "[UUEPBS] "
local Config = require("config")

local function say(fmt, ...)
    print(TAG .. string.format(fmt, ...) .. "\n")
end

-- Tabs and line breaks cannot appear inside a bridge field.
local function clean(text)
    return (tostring(text or ""):gsub("[\t\r\n]", " "))
end

local function chatter(fmt, ...)
    if Config.Verbose then
        say(fmt, ...)
    end
end

---------------------------------------------------------------------------
-- Paths
---------------------------------------------------------------------------
local function script_path()
    local info = debug and debug.getinfo and debug.getinfo(1, "S")
    local source = info and info.source or ""
    if source:sub(1, 1) == "@" then
        return (source:sub(2):gsub("/", "\\"))
    end
    return nil
end

local function parent_dir(path)
    return path and path:match("^(.*)\\[^\\]+\\?$") or nil
end

local function split_path(path)
    local parts = {}
    for part in path:gmatch("[^\\]+") do
        parts[#parts + 1] = part
    end
    return parts
end

local scriptFile = script_path()
if not scriptFile then
    say("cannot work out where this script lives; nothing to do")
    return
end
local scriptDir = parent_dir(scriptFile)
local modDir = parent_dir(scriptDir)
local modsDir = parent_dir(modDir) or modDir
local nativeDir = modDir .. "\\native"
local dllPath = nativeDir .. "\\UUEPBS.dll"
local inFile = nativeDir .. "\\bridge_in.txt"
local outFile = nativeDir .. "\\bridge_out.txt"
-- The mod was called XTGBodySlider before 2.1; its game profiles are picked up once.
local LEGACY_MOD_FOLDER = "XTGBodySlider"

-- <Game>\<Project>\Binaries\Win64\ue4ss\Mods\UUEPBS\Scripts\main.lua
local projectName, win64Dir = "", nil
do
    local parts = split_path(scriptDir)
    for i = #parts - 1, 1, -1 do
        if parts[i]:lower() == "binaries" then
            projectName = parts[i - 1] or ""
            win64Dir = table.concat(parts, "\\", 1, math.min(i + 1, #parts))
            break
        end
    end
    if not win64Dir then
        win64Dir = scriptDir
        for _ = 1, 4 do
            win64Dir = parent_dir(win64Dir) or win64Dir
        end
    end
end

local presetFolder = (Config.PresetFolder or "") ~= "" and Config.PresetFolder
    or (win64Dir .. "\\" .. ((Config.PresetFolderName or "") ~= "" and Config.PresetFolderName or "UUEPBS Presets"))
local skinFolder = (Config.SkinFolder or "") ~= "" and Config.SkinFolder
    or (win64Dir .. "\\" .. ((Config.SkinFolderName or "") ~= "" and Config.SkinFolderName or "UUEPBS Skins"))

local function read_text(path)
    local f = io.open(path, "rb")
    if not f then
        return nil
    end
    local text = f:read("a")
    f:close()
    return text
end

local function file_exists(path)
    local f = io.open(path, "rb")
    if f then
        f:close()
        return true
    end
    return false
end

-- Game profile: Scripts\GameProfiles\<project folder>.json
-- Created automatically the first time the mod runs in a game (from an XTGBodySlider install's
-- profile if there is one, else a built-in profile for games we already know, else Config.Default),
-- then read on every start. Edit it freely.
local json = require("uuepbs_json")
local profileDir = scriptDir .. "\\GameProfiles"
local profileKey = projectName ~= "" and projectName or "UnknownGame"
local profileFileName = profileKey:gsub('[<>:"/\\|?*]', "_") .. ".json"
local profileFile = profileDir .. "\\" .. profileFileName
local legacyProfileFile = modsDir .. "\\" .. LEGACY_MOD_FOLDER .. "\\Scripts\\GameProfiles\\" .. profileFileName
local PROFILE_ORDER = { "Project", "Game", "Target", "TargetClassPath", "PrimaryComponent", "IncludeComponents", "ExcludeComponents",
    "SameSkeletonOnly", "WorldMeshSearch", "BodyGroups", "MorphGroups", "ExcludeMorphs", "Hook", "_help" }
local PROFILE_HELP = "Target: actor class to edit (empty = the pawn you control). TargetClassPath: optional full path of that class. "
    .. "PrimaryComponent: mesh whose bones are listed (empty = the Character's Mesh). Include/ExcludeComponents: mesh names. "
    .. "SameSkeletonOnly: also edit other meshes on the actor that share the primary mesh's skeleton. "
    .. "BodyGroups: remap Simplified Panel sliders to this game's bones, e.g. {\"Thighs\": [\"Hip_L\", \"Hip_R\"], \"Waist\": [\"Spine1_M\"]} "
    .. "(real bone names from the Detailed Panel, * wildcards allowed; [] hides a slider; new names add sliders). "
    .. "MorphGroups: Simplified Panel sliders for morph targets, e.g. {\"Breasts\": [\"BreastSize*\"]} (names from Detailed Panel > Morphs). "
    .. "ExcludeMorphs: morph names to hide, e.g. [\"*_corrective*\"]. "
    .. "WorldMeshSearch: true = find meshes by searching every skeletal mesh in the world (slower; only for games where "
    .. "the log says the direct mesh lookup misses meshes)."

-- Games that ship with a ready-made profile (used only when their file does not exist yet).
local BuiltInProfiles = {
    Roku3 = {
        Game = "GON - God Of Nothing",
        Target = "BP_Rokuv3_C",
        TargetClassPath = "/Game/ROKUv3/BP_Rokuv3.BP_Rokuv3_C",
        PrimaryComponent = "CharacterMesh0",
    },
}

local Profile = { name = profileKey }
local profileStatus = ""
local pendingProfileText = nil -- a new profile file still to be written
local needProfileFolder = false

local function apply_profile_data(data)
    for k, v in pairs(data) do
        if type(k) == "string" and k:sub(1, 1) ~= "_" then
            Profile[k] = v
        end
    end
end

do
    for k, v in pairs(Config.Default or {}) do
        Profile[k] = v
    end
    local text = read_text(profileFile)
    local legacyText = not text and read_text(legacyProfileFile) or nil
    if text then
        local data, err = json.decode(text)
        if type(data) == "table" then
            apply_profile_data(data)
            profileStatus = "loaded"
        else
            profileStatus = "error: " .. tostring(err)
        end
    elseif legacyText and type(json.decode(legacyText)) == "table" then
        -- Carried over verbatim (comments, Hook pins and all) from the XTGBodySlider install.
        apply_profile_data(json.decode(legacyText))
        pendingProfileText = legacyText
        profileStatus = "migrated"
    else
        -- First run in this game: built-in profile, else an old config.lua "Profiles" entry, else defaults.
        local seed = BuiltInProfiles[projectName] or (Config.Profiles or {})[projectName] or {}
        for k, v in pairs(seed) do
            Profile[k] = v
        end
        local out = { Project = profileKey, _help = PROFILE_HELP }
        for _, k in ipairs(PROFILE_ORDER) do
            if k ~= "Project" and k ~= "_help" and Profile[k] ~= nil then
                out[k] = Profile[k]
            end
        end
        if out.Game == nil then
            out.Game = ""
        end
        out.BodyGroups = json.empty_object
        out.MorphGroups = json.empty_object
        out.ExcludeMorphs = {}
        pendingProfileText = json.encode_profile(out, PROFILE_ORDER)
        profileStatus = "new"
    end
    Profile.name = profileKey
end

local stateDirty = true -- bridge_in.txt needs rewriting

-- Writes a newly created profile. The GameProfiles folder ships with the mod; if it was
-- deleted, the DLL is asked to recreate it (Lua cannot make folders) and this retries.
local function flush_profile()
    if not pendingProfileText then
        return
    end
    local f = io.open(profileFile, "wb")
    if f then
        f:write(pendingProfileText)
        f:close()
        pendingProfileText = nil
        if needProfileFolder then
            needProfileFolder = false
            stateDirty = true
        end
        say("%s game profile %s", profileStatus == "migrated" and "copied the XTGBodySlider" or "created", profileFile)
    elseif not needProfileFolder then
        needProfileFolder = true
        stateDirty = true
    end
end

flush_profile()

---------------------------------------------------------------------------
-- Load the DLL
---------------------------------------------------------------------------
if not file_exists(dllPath) then
    say("%s is missing - build it (see README) and put it in the native folder", dllPath)
    return
end
if type(package) ~= "table" or type(package.loadlib) ~= "function" then
    say("this UE4SS build has no package.loadlib; the DLL cannot be loaded")
    return
end

-- Leftovers from the last run must not be mistaken for fresh data.
os.remove(outFile)
os.remove(inFile)

do
    local ok, err = package.loadlib(dllPath, "*")
    if not ok then
        say("could not load %s: %s", dllPath, tostring(err))
        return
    end
end

local engineVersion = ""
pcall(function()
    engineVersion = string.format("%d.%d", UnrealVersion:GetMajor(), UnrealVersion:GetMinor())
end)

say("v%s loaded (game folder '%s', UE %s)", VERSION, projectName, engineVersion ~= "" and engineVersion or "?")
if profileStatus:sub(1, 6) == "error:" then
    say("GameProfiles\\%s could not be read (%s); using defaults and leaving the file as it is", profileFileName, profileStatus:sub(8))
elseif profileStatus == "loaded" or profileStatus == "migrated" then
    say("game profile: GameProfiles\\%s%s", profileFileName, (Profile.Target or "") ~= "" and (" (target " .. Profile.Target .. ")") or " (target: the pawn you control)")
end
do
    -- An enabled XTGBodySlider would hook the same function and edit the pose twice.
    local legacyOn = file_exists(modsDir .. "\\" .. LEGACY_MOD_FOLDER .. "\\enabled.txt")
    local modsList = read_text(modsDir .. "\\mods.txt") or ""
    for line in modsList:gmatch("[^\r\n]+") do
        local name, flag = line:match("^%s*([^:;%s]+)%s*:%s*(%d)")
        if name == LEGACY_MOD_FOLDER and flag == "1" then
            legacyOn = true
        end
    end
    if legacyOn and file_exists(modsDir .. "\\" .. LEGACY_MOD_FOLDER .. "\\Scripts\\main.lua") then
        say("XTGBodySlider (the old name of this mod) is still enabled - disable or delete Mods\\%s, both would edit the same pose", LEGACY_MOD_FOLDER)
    end
end

---------------------------------------------------------------------------
-- Object helpers
---------------------------------------------------------------------------
local function alive(obj)
    local ok, valid = pcall(function()
        return obj ~= nil and obj:IsValid()
    end)
    return ok and valid == true
end

local function address_num(obj)
    local ok, a = pcall(function()
        return obj:GetAddress()
    end)
    return ok and a or nil
end

local function address_of(obj)
    return string.format("%X", obj:GetAddress())
end

local function same_object(a, b)
    if a == nil or b == nil then
        return a == b
    end
    local x, y = address_num(a), address_num(b)
    return x ~= nil and x == y
end

local function name_of(obj)
    local ok, name = pcall(function()
        return obj:GetFName():ToString()
    end)
    return ok and name or "?"
end

local function is_default_object(obj)
    local ok, full = pcall(function()
        return obj:GetFullName()
    end)
    return not ok or full:find("Default__", 1, true) ~= nil
end

local function set_of(list)
    local s = {}
    for _, v in ipairs(list or {}) do
        s[string.lower(v)] = true
    end
    return s
end

-- TArray elements come over as RemoteUnrealParam (get() returns the object); plain objects pass through.
local function unwrap(elem)
    local v = elem
    pcall(function()
        if elem ~= nil and elem.get then
            v = elem:get()
        end
    end)
    return v
end

-- Calls fn(object) for every element of a TArray (UE4SS: ForEach) or of a plain Lua list.
local function each_of(list, fn)
    local ok, method = pcall(function()
        return list.ForEach
    end)
    if ok and method then
        list:ForEach(function(_, elem)
            fn(unwrap(elem))
        end)
    elseif type(list) == "table" then
        for _, elem in ipairs(list) do
            fn(unwrap(elem))
        end
    end
end

-- Engine classes by path, looked up once. nil when the class is not there: StaticFindObject hands
-- back an invalid object then, which must never reach K2_GetComponentsByClass.
local classCache = {}
local function engine_class(path)
    local c = classCache[path]
    if c == nil then
        local ok, found = pcall(StaticFindObject, path)
        if ok and alive(found) then
            c = found
            classCache[path] = c
        end
    end
    return c
end

local includeSet = set_of(Profile.IncludeComponents)
local excludeSet = set_of(Profile.ExcludeComponents)

-- "Skeleton /Game/A/B.B" -> "/Game/A/B.B"
local function object_path(obj)
    local full = obj:GetFullName()
    return full:match("^%S+%s+(.+)$") or full
end

local function mesh_asset(comp)
    local mesh
    pcall(function()
        mesh = comp:GetSkinnedAsset() -- UE 5.1+
    end)
    if not alive(mesh) then
        pcall(function()
            mesh = comp.SkeletalMeshAsset -- UE 5.1+ property
        end)
    end
    if not alive(mesh) then
        pcall(function()
            mesh = comp.SkeletalMesh -- UE4 / UE 5.0
        end)
    end
    return alive(mesh) and mesh or nil
end

-- Address of the component's mesh asset from its properties (a plain read, no engine call).
local function mesh_prop(comp)
    local mesh
    pcall(function()
        mesh = comp.SkeletalMeshAsset -- UE 5.1+
    end)
    if mesh == nil then
        pcall(function()
            mesh = comp.SkeletalMesh -- UE4 / UE 5.0
        end)
    end
    return mesh ~= nil and address_num(mesh) or nil
end

local function mesh_path(comp)
    local mesh = mesh_asset(comp)
    local ok, path = pcall(function()
        return mesh and object_path(mesh) or nil
    end)
    return ok and path or nil
end

local function skeleton_path(comp)
    local mesh = mesh_asset(comp)
    local ok, path = pcall(function()
        local skeleton = mesh and mesh.Skeleton
        return alive(skeleton) and object_path(skeleton) or nil
    end)
    return ok and path or nil
end

-- The local PlayerController is looked up once and then reused: FindAllOf walks the whole
-- object array, which is far too slow to do every few seconds on the game thread.
local cachedController = nil

local function player_controller()
    if cachedController and alive(cachedController) then
        return cachedController
    end
    cachedController = nil
    local fallback
    pcall(function()
        for _, pc in ipairs(FindAllOf("PlayerController") or {}) do
            if alive(pc) and not is_default_object(pc) then
                if alive(pc.Pawn) then
                    cachedController = pc
                    return
                end
                fallback = fallback or pc
            end
        end
    end)
    cachedController = cachedController or fallback
    return cachedController
end

local function player_pawn()
    local pc = player_controller()
    if not pc then
        return nil
    end
    local pawn
    pcall(function()
        pawn = pc.Pawn
    end)
    return alive(pawn) and pawn or nil
end

---------------------------------------------------------------------------
-- Bone hierarchy (cached per mesh asset)
---------------------------------------------------------------------------
local hierarchyCache = {}

local function number_or_nil(v)
    return type(v) == "number" and v or nil
end

-- Parent-relative reference pose, used by the DLL to measure left/right mirroring.
local function read_reference(comp, count)
    local numbers = {}
    local ok = pcall(function()
        for i = 0, count - 1 do
            local t = comp:GetRefPoseTransform(i)
            local r, p, s = t.Rotation, t.Translation, t.Scale3D
            local values = { r.X, r.Y, r.Z, r.W, p.X, p.Y, p.Z, s.X, s.Y, s.Z }
            for k = 1, 10 do
                local v = number_or_nil(values[k])
                if not v then
                    error("not a number")
                end
                numbers[#numbers + 1] = string.format("%.7g", v)
            end
        end
    end)
    if ok and #numbers == count * 10 then
        return table.concat(numbers, " ")
    end
    return nil
end

-- Skeletons on disk (native\skeleton_cache.txt): reading a 400-bone skeleton takes over a thousand
-- engine calls (names, parents, reference pose) - 15-30 ms the first time a mesh is seen, every session.
-- What was read is kept per mesh asset and reused in later sessions after a two-bone spot check (a game
-- update that changes a skeleton under the same path and bone count is read again). New entries are
-- appended; a later entry for the same mesh wins. Entries: "skel <key>", "names ...", "parents ...",
-- "ref <numbers>" / "ref -" (not available) / "ref ?" (not read yet).
local SKELETON_FILE = nativeDir .. "\\skeleton_cache.txt"
local SKELETON_HEADER = "UUEPBS skeletons 1"
local SKELETON_MAX_BYTES = 16 * 1024 * 1024
local diskSkeletons = nil -- key -> { names, parents, reference (string | false = not available | nil = not read) }
local skeletonFileOk = true

local function load_disk_skeletons()
    diskSkeletons = {}
    local text = read_text(SKELETON_FILE)
    if not text then
        return
    end
    if #text > SKELETON_MAX_BYTES or text:sub(1, #SKELETON_HEADER) ~= SKELETON_HEADER then
        os.remove(SKELETON_FILE) -- too big, or an older format: start over
        return
    end
    local entry, count = nil, 0
    for line in text:gmatch("[^\n]+") do
        local kind, rest = line:match("^(%a+)\t(.*)$")
        if kind == "skel" then
            entry = { names = nil, parents = nil, reference = nil }
            diskSkeletons[rest] = entry
            count = count + 1
        elseif entry and kind == "names" then
            entry.names = rest
        elseif entry and kind == "parents" then
            entry.parents = rest
        elseif entry and kind == "ref" then
            if rest == "-" then
                entry.reference = false
            elseif rest ~= "?" then
                entry.reference = rest
            end
        end
    end
    chatter("%d skeleton(s) known from earlier sessions", count)
end

local function save_disk_skeleton(key, h)
    if not skeletonFileOk or key:sub(1, 1) == "?" then
        return
    end
    local ref = h.reference or (h.referenceTried and "-" or "?")
    local body = "skel\t" .. key .. "\nnames\t" .. h.names .. "\nparents\t" .. h.parents .. "\nref\t" .. ref .. "\n"
    if body:find("\r", 1, true) or select(2, body:gsub("\n", "")) ~= 4 then
        return -- a name with a line break would break the file
    end
    local fresh = not file_exists(SKELETON_FILE)
    local f = io.open(SKELETON_FILE, "ab")
    if not f then
        skeletonFileOk = false
        return
    end
    if fresh then
        f:write(SKELETON_HEADER, "\n")
    end
    f:write(body)
    f:close()
end

-- A skeleton from disk is used only if the mesh's first and last bone still have the same names.
local function disk_skeleton(comp, key, count)
    if not diskSkeletons then
        load_disk_skeletons()
    end
    local d = diskSkeletons[key]
    if not d or not d.names or not d.parents then
        return nil
    end
    local first, last = d.names:match("^([^\t]*)"), d.names:match("([^\t]*)$")
    local ok, same = pcall(function()
        return comp:GetBoneName(0):ToString() == first and comp:GetBoneName(count - 1):ToString() == last
    end)
    if not (ok and same) then
        diskSkeletons[key] = nil
        chatter("skeleton of %s changed since it was saved; reading it again", key)
        return nil
    end
    return {
        count = count,
        names = d.names,
        parents = d.parents,
        reference = d.reference or nil,
        referenceTried = d.reference ~= nil,
    }
end

local function read_hierarchy(comp, wantReference)
    local count = comp:GetNumBones()
    if not count or count <= 0 then
        return nil
    end
    local key = (mesh_path(comp) or "?") .. "#" .. count
    local cached = hierarchyCache[key]
    if cached and (cached.reference ~= nil or not wantReference or cached.referenceTried) then
        return cached
    end

    local changed = false
    if not cached then
        cached = key:sub(1, 1) ~= "?" and disk_skeleton(comp, key, count) or nil
    end
    if not cached then
        local names, fnames, index = {}, {}, {}
        for i = 0, count - 1 do
            local fname = comp:GetBoneName(i)
            local name = fname:ToString()
            names[i + 1] = name
            fnames[i + 1] = fname
            index[string.lower(name)] = i
        end
        local parents = {}
        for i = 1, count do
            local parent = comp:GetParentBone(fnames[i]):ToString()
            local p = index[string.lower(parent)]
            parents[i] = p ~= nil and p or -1
        end
        cached = {
            count = count,
            names = table.concat(names, "\t"),
            parents = table.concat(parents, "\t"),
        }
        changed = true
    end
    hierarchyCache[key] = cached
    if wantReference and not cached.referenceTried then
        cached.referenceTried = true
        cached.reference = read_reference(comp, count)
        changed = true
        chatter("reference pose %s", cached.reference and "read" or "not available (mirroring will be measured in game)")
    end
    if changed then
        save_disk_skeleton(key, cached)
    end
    return cached
end

---------------------------------------------------------------------------
-- Morph targets (cached per mesh asset)
---------------------------------------------------------------------------
local MAX_MORPHS = 1024
local morphNameCache = {}

-- Names of the mesh asset's morph targets, tab separated, or nil when it has none.
local function read_morph_names(comp)
    local mesh = mesh_asset(comp)
    if not mesh then
        return nil
    end
    local key = mesh_path(comp) or tostring(address_num(mesh))
    local cached = morphNameCache[key]
    if cached ~= nil then
        return cached or nil
    end
    local names, seenName = {}, {}
    pcall(function()
        local list = mesh.MorphTargets
        if list == nil then
            return
        end
        local function add(elem)
            local m = elem
            if type(elem) ~= "nil" then
                pcall(function()
                    if elem.get then
                        m = elem:get() -- UE4SS hands TArray elements over as RemoteUnrealParam
                    end
                end)
            end
            if #names < MAX_MORPHS and alive(m) then
                local n = clean(name_of(m))
                if n ~= "" and n ~= "?" and n ~= "None" and not seenName[string.lower(n)] then
                    seenName[string.lower(n)] = true
                    names[#names + 1] = n
                end
            end
        end
        if list.ForEach then
            list:ForEach(function(_, elem)
                add(elem)
            end)
        else
            for i = 1, math.min(#list, MAX_MORPHS) do
                add(list[i])
            end
        end
    end)
    local text = #names > 0 and table.concat(names, "\t") or false
    morphNameCache[key] = text
    if text then
        chatter("%d morph target(s) on %s", #names, key)
    end
    return text or nil
end

local function morph_set(text)
    if not text then
        return nil
    end
    local set = {}
    for name in text:gmatch("[^\t]+") do
        set[string.lower(name)] = name
    end
    return set
end

---------------------------------------------------------------------------
-- Target selection
---------------------------------------------------------------------------
-- The window edits one character at a time (the target). Every other character that has
-- been picked and edited before keeps its sliders: the DLL lists those ("keep") and they
-- are scanned too. Characters that were never edited are not touched at all.
-- Actor keys: "player" for the default character (the pawn you control, or the profile's
-- Target), so its sliders follow respawns and level changes; otherwise the actor's address.
local PLAYER_KEY = "player"
local MAX_KEPT = math.max(0, tonumber(Config.MaxEditedCharacters) or 16)
local MAX_MESHES = math.max(8, math.min(64, tonumber(Config.MaxTrackedMeshes) or 64)) -- the DLL tracks at most 64

local pickedId = nil -- address chosen in the window's picker (nil = automatic)
local target = nil -- current actor
local targetInfo = { id = "", label = "", key = PLAYER_KEY }
local rigs = {} -- list of { comp, address, label, primary, hierarchy, actorKey, ownerLabel, morphs, morphSet }
local candidates = {}
local candidatesText = ""
local knownActors = {} -- actor key -> { actor, full, label } for characters that have been the target
local keptOrder, keptSet = {}, {} -- characters with edits, from the DLL ("keep" lines)
local keptInRange = {} -- actor key -> near the player at the last scan (kept NPCs)
local goneKeys = {} -- kept characters that no longer exist (reported until the DLL drops them)

local function actor_label(actor, pawn)
    local label = name_of(actor)
    if pawn and same_object(actor, pawn) then
        label = label .. " (player)"
    end
    return label
end

local function full_name(obj)
    local ok, full = pcall(function()
        return obj:GetFullName()
    end)
    return ok and full or nil
end

local profileTarget = nil -- last actor of the profile's Target class

local function find_profile_target()
    for _, a in ipairs(FindAllOf(Profile.Target) or {}) do
        if alive(a) and not is_default_object(a) then
            profileTarget = a
            return a
        end
    end
    profileTarget = nil
    return nil
end

local function default_target()
    if Profile.Target and Profile.Target ~= "" then
        -- Keep the one we found while it lives; only search again when it is gone.
        if profileTarget and alive(profileTarget) then
            return profileTarget
        end
        return find_profile_target()
    end
    return player_pawn()
end

-- The default character without any object search (nil when it is not known right now).
local function cheap_default()
    if Profile.Target and Profile.Target ~= "" then
        return (profileTarget and alive(profileTarget)) and profileTarget or nil
    end
    return player_pawn()
end

local function key_of(actor, default)
    if default and same_object(actor, default) then
        return PLAYER_KEY
    end
    return address_of(actor)
end

-- Filled in further down (they need helpers defined later).
local late = {}
-- Characters seen through the new-object notification: the picker lists these while the window
-- stays open instead of walking the whole object array every few seconds (that walk takes long
-- enough in big games to be felt). The full search runs when the window opens and on Refresh.
local liveCharacters = {} -- { obj, full }
local liveCharactersSeen = false
-- The live list may have missed characters loaded with a level, so the first listing after a start or a
-- reload walks the object array; later ones (opening the window or the list) only re-sort what is known.
local liveStale = true
local USE_LIVE_LIST = #(Config.CandidateClasses or {}) == 1 and (Config.CandidateClasses or {})[1] == "Character"

local function note_character(obj)
    if #liveCharacters >= 4096 then
        local keep = {}
        for _, e in ipairs(liveCharacters) do
            if alive(e.obj) then
                keep[#keep + 1] = e
            end
        end
        liveCharacters = keep
    end
    if #liveCharacters < 4096 then
        liveCharacters[#liveCharacters + 1] = { obj = obj }
    end
end

-- Lists characters for the picker: the player first, then the nearest ones. `full` walks the object
-- array; otherwise the characters noticed since the last full walk are used.
local function refresh_candidates(full)
    local pool, seen = {}, {}
    local pawn = player_pawn()
    local function add(actor)
        if not alive(actor) or is_default_object(actor) then
            return
        end
        local id = address_of(actor)
        if id and not seen[id] then
            seen[id] = true
            pool[#pool + 1] = actor
        end
    end
    if pawn then
        add(pawn)
    end
    if Profile.Target and Profile.Target ~= "" then
        if full or not USE_LIVE_LIST then
            for _, a in ipairs(FindAllOf(Profile.Target) or {}) do
                add(a)
            end
        elseif profileTarget then
            add(profileTarget)
        end
    end
    if full or not USE_LIVE_LIST or not liveCharactersSeen then
        local fresh = {}
        for _, class in ipairs(Config.CandidateClasses or {}) do
            for _, a in ipairs(FindAllOf(class) or {}) do
                add(a)
                fresh[#fresh + 1] = { obj = a }
            end
        end
        if USE_LIVE_LIST then
            liveCharacters, liveCharactersSeen = fresh, true
        end
    else
        local keep = {}
        for _, e in ipairs(liveCharacters) do
            if alive(e.obj) then
                keep[#keep + 1] = e
                add(e.obj)
            end
        end
        liveCharacters = keep
    end
    -- characters with sliders, even if the lists above missed them
    for key in pairs(keptSet) do
        local k = knownActors[key]
        if k and alive(k.actor) and full_name(k.actor) == k.full then
            add(k.actor)
        end
    end
    if full and USE_LIVE_LIST then
        liveStale = false
    end
    -- nearest first (the list is capped)
    local origin = pawn and alive(pawn) and late.location and late.location(pawn) or nil
    local entries = {}
    for _, a in ipairs(pool) do
        local d = math.huge
        if pawn and same_object(a, pawn) then
            d = -1
        elseif origin then
            local p = late.location(a)
            if p then
                d = (p[1] - origin[1]) ^ 2 + (p[2] - origin[2]) ^ 2 + (p[3] - origin[3]) ^ 2
            end
        end
        entries[#entries + 1] = { actor = a, d = d, n = #entries }
    end
    table.sort(entries, function(x, y)
        if x.d ~= y.d then
            return x.d < y.d
        end
        return x.n < y.n
    end)
    -- Capped at MaxCandidates, but characters with sliders (kept) are always listed, wherever they are.
    local list, plain = {}, 0
    local cap = Config.MaxCandidates or 40
    local default = cheap_default()
    for _, e in ipairs(entries) do
        local key = key_of(e.actor, default)
        local edited = keptSet[key] ~= nil
        if not edited then
            plain = plain + 1
        end
        if edited or plain <= cap then
            local label = actor_label(e.actor, pawn)
        -- show who a character is recognised as when that differs from its object name
            if late.identity and not (default and same_object(e.actor, default)) then
                local ok, ident = pcall(late.identity, e.actor)
                if ok and ident and ident ~= name_of(e.actor) and not ident:find("@", 1, true) then
                    label = label .. "  = " .. ident
                end
            end
            -- distance from the player, whole metres (only in the list; the label stays a name)
            local dist = (e.d >= 0 and e.d < math.huge) and string.format("   %d m", math.floor(math.sqrt(e.d) / 100 + 0.5)) or ""
            list[#list + 1] = { id = address_of(e.actor), label = label, actor = e.actor, key = key, dist = dist }
        end
    end
    candidates = list
    local parts = {}
    for _, c in ipairs(list) do
        parts[#parts + 1] = c.id .. "\t" .. c.label .. c.dist .. "\t" .. c.key
    end
    local text = table.concat(parts, "\n")
    if text ~= candidatesText then
        candidatesText = text
        stateDirty = true
    end
end

local function resolve_target()
    if pickedId then
        for _, c in ipairs(candidates) do
            if c.id == pickedId and alive(c.actor) then
                return c.actor
            end
        end
        refresh_candidates(true)
        for _, c in ipairs(candidates) do
            if c.id == pickedId and alive(c.actor) then
                return c.actor
            end
        end
        say("picked character is gone, back to automatic")
        pickedId = nil
    end
    return default_target()
end

-- The actor behind a kept character's key, or nil (and reported gone) when it no longer exists.
local function resolve_kept(key, default, pawn)
    if key == PLAYER_KEY then
        return default, default and actor_label(default, pawn) or nil
    end
    local k = knownActors[key]
    if not k then
        -- Not seen as the target by this script (e.g. the mod was reloaded): find it by address.
        for _, c in ipairs(candidates) do
            if c.id == key and alive(c.actor) then
                k = { actor = c.actor, full = full_name(c.actor), label = c.label }
                knownActors[key] = k
                break
            end
        end
    end
    -- Same address but another object (the memory was reused) counts as gone too.
    if k and alive(k.actor) and full_name(k.actor) == k.full then
        return k.actor, k.label
    end
    knownActors[key] = nil
    if not goneKeys[key] then
        goneKeys[key] = true
        stateDirty = true
        say("%s is gone; its sliders are dropped", k and k.label or key)
    end
    return nil
end

---------------------------------------------------------------------------
-- Remembered NPCs
---------------------------------------------------------------------------
-- An NPC's identity survives reloads (its address does not): the actor's name without the
-- instance number ("Anca_243" -> "Anca"), or for actors spawned without a name, class@face mesh.
-- The DLL saves each edited NPC's sliders under its identity and lists them ("remember"); new
-- characters are checked against that list and reported ("npc") so their sliders come back.
local REMEMBER = Config.RememberCharacters ~= false
local RANGE = tonumber(Config.CharacterRange) or 5000 -- cm; 0 = any distance
local remembered = {} -- identity -> true (from the DLL)
local rememberedText = ""
local sighted = {} -- actor key -> { actor, full, identity, label } remembered NPCs that are loaded
local newCharacters = {} -- { obj, due } from NotifyOnNewObject, checked once they are set up
local identityCache = {} -- actor full name -> identity (full names are unique among live objects; addresses get reused)
local MAX_SAME = math.max(1, tonumber(Config.MaxSameIdentity) or 2)
local sameWarned = {}

-- Character names in Unreal:
--   * placed in a map: the name saved with the level, a short number (Gatekeeper_church2_277,
--     Anca_243). Stable across loads, and different numbers are different people.
--   * spawned at run time: the engine makes up a number counting down from 2147483647
--     (Gatekeeper_church2_2147465868). The rest of the name is only the template it was spawned
--     from, often shared by a whole squad.
-- A spawned character usually still points at the placed record it was spawned for (Dawnwalker:
-- the "Stub" component -> DogwoodAIStub "Gatekeeper_church2_277"). find_link() looks for such a
-- reference: an object property on the actor or one of its components whose value is a placed
-- object with the same name. Where it is found is remembered per class, so it is searched once.
local function split_number(name)
    return tostring(name or ""):match("^(.-)_(%d+)$")
end

local function is_runtime_number(num)
    return num ~= nil and #num >= 7
end

-- Strips a run-time instance number (7+ digits). Short numbers are kept.
local function stem(name)
    local base, num = split_number(name)
    if is_runtime_number(num) then
        return base
    end
    return tostring(name or "")
end

local function safe_id(text)
    local id = tostring(text):gsub('[<>:"/\\|?*%c]', "_")
    return id:sub(1, 80)
end

local LINK_SEARCH = Config.IdentityLinkSearch ~= false
local linkByClass = {} -- actor class -> { holder = component name or "", prop = name } | number of failed searches
local objectPropsByClass = {} -- class full name -> game-declared object property names
local linkFailLimit = 3

local function components_of(actor)
    local out = {}
    local cls = engine_class("/Script/Engine.ActorComponent")
    if cls then
        pcall(function()
            each_of(actor:K2_GetComponentsByClass(cls), function(c)
                out[#out + 1] = c
            end)
        end)
    end
    return out
end

-- Object properties declared by the game (engine base classes are skipped: they never hold such a link).
local function object_props(obj)
    local cls
    pcall(function()
        cls = obj:GetClass()
    end)
    local key = cls and full_name(cls)
    if not key then
        return {}
    end
    local cached = objectPropsByClass[key]
    if cached then
        return cached
    end
    local names = {}
    pcall(function()
        local s, depth = cls, 0
        while s and depth < 16 do
            local path = full_name(s) or ""
            if path == "" or path:find(" /Script/Engine.", 1, true) or path:find(" /Script/CoreUObject.", 1, true) then
                break
            end
            s:ForEachProperty(function(prop)
                local kind = ""
                pcall(function()
                    kind = prop:GetClass():GetFName():ToString()
                end)
                if kind == "ObjectProperty" then
                    names[#names + 1] = prop:GetFName():ToString()
                end
            end)
            local ok, super = pcall(function()
                return s:GetSuperStruct()
            end)
            s = ok and super and (super.IsValid == nil or super:IsValid()) and super or nil
            depth = depth + 1
        end
    end)
    objectPropsByClass[key] = names
    return names
end

-- The placed object `value` names the same character as base ("Gatekeeper_church2").
local function placed_twin(value, base)
    if value == nil or not alive(value) then
        return nil
    end
    local n = name_of(value)
    local b, num = split_number(n)
    if b and not is_runtime_number(num) and b:lower() == base:lower() then
        return n
    end
    return nil
end

local function read_prop(holder, prop)
    local ok, v = pcall(function()
        return holder[prop]
    end)
    return ok and v or nil
end

-- Returns the placed name a spawned character belongs to, and whether its class is known to have one.
local function find_link(actor, cls, base)
    local known = linkByClass[cls]
    if type(known) == "table" then
        local holder = actor
        if known.holder ~= "" then
            holder = nil
            for _, c in ipairs(components_of(actor)) do
                if name_of(c) == known.holder then
                    holder = c
                    break
                end
            end
        end
        return holder and placed_twin(read_prop(holder, known.prop), base) or nil, true
    end
    if not LINK_SEARCH or (known or 0) >= linkFailLimit then
        return nil, false
    end
    local holders = { { "", actor } }
    for _, c in ipairs(components_of(actor)) do
        holders[#holders + 1] = { name_of(c), c }
    end
    for _, h in ipairs(holders) do
        for _, prop in ipairs(object_props(h[2])) do
            local twin = placed_twin(read_prop(h[2], prop), base)
            if twin then
                linkByClass[cls] = { holder = h[1], prop = prop }
                say("spawned %s characters are recognised through %s%s (%s is %s)", cls, h[1] ~= "" and (h[1] .. ".") or "", prop,
                    name_of(actor), twin)
                return twin, true
            end
        end
    end
    linkByClass[cls] = (known or 0) + 1
    return nil, false
end

local function class_name(actor)
    local cls = "?"
    pcall(function()
        cls = name_of(actor:GetClass())
    end)
    return cls
end

local function identity_of(actor)
    local full = full_name(actor)
    local cached = full and identityCache[full]
    if cached ~= nil then
        return cached or nil
    end
    local cls = class_name(actor)
    local raw = name_of(actor)
    -- "BP_NonPlayerCharacter_C" or "BP_NonPlayerCharacter_C_12": the engine's default name, not a person's
    local generic = raw == cls or (raw:sub(1, #cls + 1) == cls .. "_" and raw:sub(#cls + 2):match("^%d+$") ~= nil)
    local id
    local strong = true -- only identities that cannot change later are cached
    if not generic and raw ~= "" and raw ~= "?" then
        local base, num = split_number(raw)
        if is_runtime_number(num) then
            -- spawned: the placed record it belongs to, else the template name ("Esme")
            local twin, classHasLink = find_link(actor, cls, base)
            if twin then
                id = safe_id(twin)
            else
                id = safe_id(base)
                strong = not classHasLink -- its link may just not be set yet
            end
        else
            id = safe_id(raw) -- placed: "Anca_243", "Gatekeeper_church2_277"
        end
    else
        -- spawned without a name: its class plus the face/head mesh it wears (else its main mesh)
        local mesh
        local skel = engine_class("/Script/Engine.SkeletalMeshComponent")
        pcall(function()
            each_of(skel and actor:K2_GetComponentsByClass(skel) or {}, function(c)
                local n = name_of(c):lower()
                if not mesh and (n:find("face", 1, true) or n:find("head", 1, true)) then
                    local m = mesh_asset(c)
                    mesh = m and name_of(m) or nil
                end
            end)
        end)
        if not mesh then
            -- no face mesh (yet): fall back to the main mesh, but look again next time
            strong = false
            pcall(function()
                local m = alive(actor.Mesh) and mesh_asset(actor.Mesh)
                mesh = m and name_of(m) or nil
            end)
        end
        id = mesh and safe_id(stem(cls) .. "@" .. mesh) or nil
    end
    if full and id and strong then
        identityCache[full] = id
    end
    return id
end

-- The stored wrapper still points at the object it was taken from (not a destroyed one, and
-- not a new object that got the same address). Checked before calling anything on it.
local function still(obj, full)
    return full ~= nil and alive(obj) and full_name(obj) == full
end

local function location_of(actor)
    local ok, v = pcall(function()
        local l = actor:K2_GetActorLocation()
        return { l.X, l.Y, l.Z }
    end)
    return ok and type(v) == "table" and type(v[1]) == "number" and v or nil
end

late.identity, late.location = identity_of, location_of

-- NPCs are only sculpted while they are this close to the player (CharacterRange in config.lua).
-- An NPC already in range only counts as out of it 10% further away, so one standing right at the
-- edge (or walking around on it) doesn't flip in and out, causing a rescan each time.
local function in_range(actor, origin, full, wasNear)
    if RANGE <= 0 or not origin then
        return true
    end
    if full and not still(actor, full) then
        return false
    end
    local p = location_of(actor)
    if not p then
        return true
    end
    local dx, dy, dz = p[1] - origin[1], p[2] - origin[2], p[3] - origin[3]
    local r = wasNear and RANGE * 1.1 or RANGE
    return dx * dx + dy * dy + dz * dz <= r * r
end

local function player_origin()
    local p = cheap_default()
    return p and alive(p) and location_of(p) or nil
end

-- Is this a remembered character? Then report it (once per appearance).
local function check_character(actor)
    if not alive(actor) or is_default_object(actor) then
        return
    end
    local default = cheap_default()
    if default and same_object(actor, default) then
        return -- the player has its own session file
    end
    local id = identity_of(actor)
    if not id or not remembered[id] then
        return
    end
    local key = address_of(actor)
    if sighted[key] then
        return
    end
    -- Several loaded characters with the same identity: only the first few get the sliders.
    local same = 0
    for _, e in pairs(sighted) do
        if e.identity == id then
            same = same + 1
        end
    end
    if same >= MAX_SAME then
        if not sameWarned[id] then
            sameWarned[id] = true
            say("more than %d loaded characters are recognised as %s; the others are left alone (MaxSameIdentity in config.lua)", MAX_SAME, id)
        end
        return
    end
    local label = actor_label(actor, nil)
    sighted[key] = { actor = actor, full = full_name(actor), identity = id, label = label }
    knownActors[key] = knownActors[key] or { actor = actor, full = sighted[key].full, label = label }
    stateDirty = true
    chatter("remembered character %s found (%s)", label, id)
end

local function check_new_characters(now)
    if not REMEMBER or next(remembered) == nil then
        newCharacters = {}
        return
    end
    local keep = {}
    for _, n in ipairs(newCharacters) do
        if now >= n.due then
            pcall(check_character, n.obj)
        else
            keep[#keep + 1] = n
        end
    end
    newCharacters = keep
    for key, e in pairs(sighted) do
        if not alive(e.actor) or full_name(e.actor) ~= e.full then
            sighted[key] = nil
            stateDirty = true
        end
    end
end

-- Characters already loaded (script start, or the remembered list changed): the picker's list.
local function sweep_characters()
    if not REMEMBER or next(remembered) == nil then
        return
    end
    if USE_LIVE_LIST and liveCharactersSeen then
        for _, e in ipairs(liveCharacters) do
            pcall(check_character, e.obj)
        end
    else
        for _, c in ipairs(candidates) do
            pcall(check_character, c.actor)
        end
    end
end

---------------------------------------------------------------------------
-- Scanning
---------------------------------------------------------------------------
-- Every character is looked at on its own ("collected"), at most one per tick: an NPC walking into
-- range costs a small lookup on that NPC, not a rescan of everyone. (Up to 2.5 every change was a
-- full rescan that walked every skeletal mesh in the world - about 1,000 in a Dawnwalker town -
-- and rebuilt every character, felt as a hitch each time an edited NPC came or went.)
--
-- A character's meshes come from the actor itself (K2_GetComponentsByClass lists every mesh it owns,
-- also those created by another of its components, e.g. Dawnwalker's Appearance slots) plus the
-- meshes of actors attached to it (outfits some games and mods spawn as their own actor). The old
-- world search (every skeletal mesh, matched by owner and attachment) is the fallback: for a
-- character the direct lookup finds no mesh on, when the profile asks for it ("WorldMeshSearch"),
-- or for the rest of the session when a one-time check finds a mesh the direct lookup missed.
local scanCount = 0 -- sent to the DLL, so a re-collect always reaches it (re-tracks stale meshes)
local rigsText = ""
local rigsChanged = false -- the sculpted meshes changed: morph weights need applying again
local make_plan, watch_characters, settle_due, process_queue, scan_status
do
    -- Games often finish assembling a character after it appears (outfit actors attached later,
    -- placeholder meshes swapped for the real ones), so a new character is checked again a few times
    -- while it settles. A check costs a few property reads; the character is only collected again
    -- when one of its meshes changed or it has more or fewer of them.
    local SETTLE_MS = { 1000, 3000, 7000, 15000, 30000 }
    local CHECK_STEP = 3 -- settle step (7 s) at which the direct lookup is compared once with the world search
    local worldSearch = Profile.WorldMeshSearch == true -- every collect uses the world search
    local lookupChecked = worldSearch
    -- Once the direct lookup has found meshes on some character, it works in this game: a character it
    -- then finds no meshes on simply has none right now (still being dressed, or being torn down - Dawnwalker
    -- unloads an NPC's meshes a few seconds before the NPC goes), and a world search would find nothing
    -- usable either, for ~50 ms.
    local directProven = false

    local chars = {} -- actor key -> collected character { actor, addr, full, label, rigs, how, count, ms }
    local plan, planByKey, plannedAddr = {}, {}, {} -- characters wanted (target first, then kept NPCs nearest first)
    local keptActors = {} -- actor key -> actor, kept characters in the plan
    local queue, queued = {}, {} -- actor keys waiting to be collected
    local settles = {} -- actor key -> { due = { times }, step } for characters still settling
    local seenActor = {} -- actor key -> "address|full name" it was last collected for (a new one settles)
    local lastReport = {} -- actor key -> scan report last logged
    local slowSaid = {} -- actor key -> ms of the last slow scan shown
    local leftOutText = ""
    local keptOverflow = 0
    local fullPass = nil -- { t0, ms, chars, meshes } while a full rescan is being worked through
    local stats = { collects = 0, ms = 0, label = "", meshes = 0, how = "", world = 0, worldMs = 0 }

    local function rebuild_rigs_text()
        local lines = {}
        for _, r in ipairs(rigs) do
            lines[#lines + 1] = "rig\t" .. r.address .. "\t" .. clean(r.label) .. "\t" .. (r.primary and "1" or "0") .. "\t" .. clean(r.ownerLabel) .. "\t" .. r.actorKey
            lines[#lines + 1] = "bones\t" .. r.hierarchy.names
            lines[#lines + 1] = "parents\t" .. r.hierarchy.parents
            if r.primary and r.hierarchy.reference then
                lines[#lines + 1] = "ref\t" .. r.hierarchy.reference
            end
            if r.morphs then
                lines[#lines + 1] = "morphs\t" .. r.morphs
            end
        end
        rigsText = table.concat(lines, "\n")
    end

    -- The world search: finds the skeletal meshes of every listed character in one pass over the
    -- object array: meshes they own, plus meshes of other actors attached to them.
    local function collect_meshes(entries)
        local byOwner = {}
        for _, e in ipairs(entries) do
            e.addr = address_num(e.actor)
            e.comps, e.attachedFrom = {}, {}
            if e.addr then
                byOwner[e.addr] = e
            end
        end
        local function owner_of_chain(start)
            local ok, found = pcall(function()
                local parent = start
                for _ = 1, 8 do
                    if not alive(parent) then
                        return nil
                    end
                    local e = byOwner[address_num(parent:GetOuter())]
                    if e then
                        return e
                    end
                    parent = parent:GetAttachParent()
                end
                return nil
            end)
            return ok and found or nil
        end
        local attachedActors = {} -- outer address -> entry or false (the walk is done once per actor)
        local function attached_entry(outer, comp)
            local key = address_num(outer)
            local known = attachedActors[key]
            if known == nil then
                local root
                pcall(function()
                    root = outer.RootComponent
                end)
                if alive(root) then
                    local ok, parent = pcall(function()
                        return root:GetAttachParent()
                    end)
                    known = ok and owner_of_chain(parent) or false
                else
                    local ok, parent = pcall(function()
                        return comp:GetAttachParent()
                    end)
                    return ok and owner_of_chain(parent) or nil
                end
                attachedActors[key] = known
            end
            return known or nil
        end
        local t0 = os.clock()
        for _, comp in ipairs(FindAllOf("SkeletalMeshComponent") or {}) do
            local ok, outer = pcall(function()
                return comp:GetOuter()
            end)
            if ok and outer ~= nil then
                local outerAddr = address_num(outer)
                local e = outerAddr and byOwner[outerAddr]
                if e then
                    if alive(comp) then
                        e.comps[#e.comps + 1] = comp
                    end
                elseif outerAddr then
                    local a = attached_entry(outer, comp)
                    if a and alive(comp) and not is_default_object(comp) then
                        a.comps[#a.comps + 1] = comp
                        a.attachedFrom[comp] = name_of(outer)
                    end
                end
            end
        end
        stats.world = stats.world + 1
        stats.worldMs = (os.clock() - t0) * 1000.0
    end

    -- Actors attached to this one (outfit / costume actors). GetAttachedActors fills its out array:
    -- UE4 has (OutActors, bResetArray), UE5 adds bRecursivelyIncludeAttachedActors; the form that
    -- works is remembered, and if neither does the call is not tried again.
    local attachForm = nil -- 2, 3 or false
    local function attached_actors(actor)
        if attachForm == false then
            return {}
        end
        local out = {}
        local function take(list)
            pcall(each_of, list, function(a)
                out[#out + 1] = a
            end)
        end
        local function try(form)
            local arr = {}
            local ok, ret = pcall(function()
                if form == 2 then
                    return actor:GetAttachedActors(arr, true)
                end
                return actor:GetAttachedActors(arr, true, false)
            end)
            if ok then
                take(arr)
                if #out == 0 and ret ~= nil then
                    take(ret)
                end
            end
            return ok
        end
        if attachForm then
            try(attachForm)
        elseif try(2) then
            attachForm = 2
        elseif try(3) then
            attachForm = 3
        else
            attachForm = false
            chatter("GetAttachedActors is not usable here; attached outfit actors are only found by the world search")
        end
        return out
    end

    -- The direct lookup: the actor's own skeletal meshes plus those of the actors attached to it.
    -- Returns nil when the actor cannot be asked (the world search is used instead).
    local function direct_meshes(e)
        local skel = engine_class("/Script/Engine.SkeletalMeshComponent")
        if not skel then
            return nil
        end
        local comps, from, seenAddr = {}, {}, {}
        local function add(c, owner)
            local a = alive(c) and address_num(c)
            if a and not seenAddr[a] then
                seenAddr[a] = true
                comps[#comps + 1] = c
                if owner then
                    from[c] = owner
                end
            end
        end
        local ok = pcall(function()
            each_of(e.actor:K2_GetComponentsByClass(skel), function(c)
                add(c)
            end)
        end)
        if not ok then
            return nil
        end
        local self = address_num(e.actor)
        for _, a in ipairs(attached_actors(e.actor)) do
            local addr = alive(a) and address_num(a)
            -- another character being sculpted (a rider, a carried NPC) keeps its own meshes
            if addr and addr ~= self and not plannedAddr[addr] then
                local owner = name_of(a)
                pcall(function()
                    each_of(a:K2_GetComponentsByClass(skel), function(c)
                        add(c, owner)
                    end)
                end)
            end
        end
        return comps, from
    end

    local function has_bones(comps)
        for _, comp in ipairs(comps) do
            local ok, n = pcall(function()
                return comp:GetNumBones()
            end)
            if ok and type(n) == "number" and n > 0 then
                return true
            end
        end
        return false
    end

    -- Picks the character's main mesh and the meshes to edit; adds them to `out`.
    local function build_rigs(e, report, out)
        local actor, comps = e.actor, e.comps
        local tag = e.kept and " [kept]" or ""
        local primary
        local wanted = (Profile.PrimaryComponent or ""):lower()
        local meshAddress
        pcall(function()
            local m = actor.Mesh
            if alive(m) then
                meshAddress = address_num(m)
            end
        end)
        local best = -1
        for _, comp in ipairs(comps) do
            local label = name_of(comp):lower()
            local bones = 0
            pcall(function()
                bones = comp:GetNumBones() or 0
            end)
            if wanted ~= "" and label == wanted then
                primary = comp
                break
            end
            if wanted == "" and meshAddress and address_num(comp) == meshAddress and bones > 0 then
                primary = comp
                break
            end
            if bones > best then
                best, primary = bones, comp
            end
        end
        if not primary then
            report[#report + 1] = string.format("%s%s has no skeletal mesh", e.label, tag)
            return 0
        end
        local primarySkeleton = skeleton_path(primary)
        local driven = 0
        for _, comp in ipairs(comps) do
            local label = name_of(comp)
            local key = label:lower()
            local isPrimary = comp == primary
            local compSkeleton = skeleton_path(comp)
            local use = isPrimary or includeSet[key]
            if not use and not excludeSet[key] then
                use = not Profile.SameSkeletonOnly or (primarySkeleton ~= nil and compSkeleton == primarySkeleton)
            end
            if excludeSet[key] and not isPrimary then
                use = false
            end
            local why = use and "edited" or (excludeSet[key] and "excluded in profile" or "other skeleton")
            local morphCount = 0
            if use then
                local ok, h = pcall(read_hierarchy, comp, isPrimary)
                if ok and h then
                    local okm, morphs = pcall(read_morph_names, comp)
                    morphs = okm and morphs or nil
                    out[#out + 1] = { comp = comp, address = address_of(comp), label = label, primary = isPrimary, hierarchy = h,
                        morphs = morphs, morphSet = morph_set(morphs), actorKey = e.key, ownerLabel = e.label,
                        full = full_name(comp), meshAddr = mesh_prop(comp) }
                    driven = driven + 1
                    if morphs then
                        morphCount = select(2, morphs:gsub("\t", "")) + 1
                    end
                elseif ok then
                    why = "skipped (no mesh assigned / 0 bones)"
                else
                    why = "could not read bones: " .. tostring(h)
                end
            end
            -- One line per mesh so a log shows exactly what was (not) picked up.
            local bones, visible, asset = 0, "?", "?"
            pcall(function() bones = comp:GetNumBones() or 0 end)
            pcall(function() visible = comp:IsVisible() and "visible" or "hidden" end)
            pcall(function() asset = (mesh_path(comp) or "no mesh"):match("([^/.]+)$") or "?" end)
            report[#report + 1] = string.format("  mesh %s%s: %s, %d bones%s, %s, skeleton %s -> %s%s", label,
                e.attachedFrom[comp] and (" (attached from " .. e.attachedFrom[comp] .. ")") or "", asset, bones,
                morphCount > 0 and (", " .. morphCount .. " morphs") or "", visible,
                (compSkeleton or "?"):match("([^/.]+)$") or "?", why, isPrimary and " (primary)" or "")
        end
        report[#report + 1] = string.format("%s%s: %d mesh(es) driven", e.label, tag, driven)
        return driven
    end

    -- The meshes sent to the DLL, from every collected character in plan order (target first, then
    -- kept NPCs nearest first). The DLL tracks at most MAX_MESHES meshes: a kept character either fits
    -- whole or is left out (half a character would look broken), farthest first.
    local function assemble()
        rigs = {}
        local leftOut = {}
        for _, e in ipairs(plan) do
            local ch = chars[e.key]
            if ch then
                if e.kept and #rigs + #ch.rigs > MAX_MESHES then
                    leftOut[#leftOut + 1] = e.label
                else
                    table.move(ch.rigs, 1, #ch.rigs, #rigs + 1, rigs)
                end
            end
        end
        local text = table.concat(leftOut, ", ")
        if text ~= leftOutText then
            leftOutText = text
            if text ~= "" then
                say("mesh limit (%d) reached; left alone for now: %s", MAX_MESHES, text)
            end
        end
        rebuild_rigs_text()
        stateDirty = true
        rigsChanged = true
    end

    local function queue_key(key)
        if queued[key] or not planByKey[key] then
            return
        end
        queued[key] = true
        if key == targetInfo.key then
            table.insert(queue, 1, key) -- the character being edited goes first
        else
            queue[#queue + 1] = key
        end
    end

    -- The per-mesh lines are written for the character being edited; for other characters only the
    -- summary line (printing to the UE4SS console costs ~0.4 ms a line - a dozen lines per NPC arriving
    -- cost as much as reading the NPC). Verbose shows everything. UUEPBS.log lists every tracked mesh anyway.
    local function log_report(key, report, brief)
        local text = table.concat(report, "\n")
        if text ~= lastReport[key] or Config.Verbose then
            lastReport[key] = text
            local first = (brief and not Config.Verbose) and #report or 1
            for i = first, #report do
                say("%s", report[i])
            end
        end
    end

    -- Builds a collected character from e.comps / e.attachedFrom (filled by either lookup).
    local function finish_collect(e, how, t0)
        local built, report = {}, {}
        build_rigs(e, report, built)
        if #built == 0 and #e.comps > 0 and chars[e.key] and #chars[e.key].rigs > 0 and not has_bones(e.comps) then
            -- its meshes were unloaded (Dawnwalker does that a few seconds before an NPC goes)
            report = { string.format("%s: meshes unloaded, let go", e.label) }
        end
        scanCount = scanCount + 1
        local ms = (os.clock() - t0) * 1000.0
        chars[e.key] = { actor = e.actor, addr = e.addr, full = e.full, label = e.label, rigs = built, how = how, count = #e.comps, ms = ms }
        -- A character seen for the first time (or a new actor under the same key, e.g. after a reload)
        -- is checked again a few times while the game finishes it.
        local who = tostring(e.addr) .. "|" .. tostring(e.full)
        if seenActor[e.key] ~= who then
            if e.key == PLAYER_KEY and seenActor[e.key] ~= nil then
                liveStale = true -- a reload: the next listing walks the object array once
            end
            seenActor[e.key] = who
            local now = os.clock() * 1000.0
            local due = {}
            for i, d in ipairs(SETTLE_MS) do
                due[i] = now + d
            end
            settles[e.key] = { due = due, step = 0 }
        end
        log_report(e.key == PLAYER_KEY and "" or e.key, report, e.key ~= targetInfo.key)
        stats.collects = stats.collects + 1
        stats.ms, stats.label, stats.meshes, stats.how = ms, e.label, #e.comps, how
        -- Slow scans are shown (once per character, again only if one gets much slower); the rest with Verbose.
        if ms > 5 and ms > 2 * (slowSaid[e.key] or 0) then
            slowSaid[e.key] = ms
            say("scan of %s: %d mesh(es) in %.1f ms (%s)", e.label, #e.comps, ms, how)
        else
            chatter("scan of %s: %d mesh(es) in %.1f ms (%s)", e.label, #e.comps, ms, how)
        end
        if fullPass then
            fullPass.ms, fullPass.chars, fullPass.meshes = fullPass.ms + ms, fullPass.chars + 1, fullPass.meshes + #e.comps
        end
    end

    -- Collects the next waiting character (with the world search on: every waiting one, in one pass).
    function process_queue()
        if #queue == 0 then
            return false
        end
        local batch, replan = {}, false
        repeat
            local key = table.remove(queue, 1)
            queued[key] = nil
            local e = planByKey[key]
            if e and still(e.actor, e.full) then
                batch[#batch + 1] = e
            elseif e then
                replan = true -- gone since it was planned
            end
        until #queue == 0 or (#batch > 0 and not worldSearch)
        if #batch == 0 then
            return replan
        end
        local t0 = os.clock()
        local viaWorld = {}
        for _, e in ipairs(batch) do
            e.addr = address_num(e.actor)
            local comps, from
            if not worldSearch then
                comps, from = direct_meshes(e)
            end
            local withBones = comps and has_bones(comps)
            if comps and (withBones or directProven) then
                directProven = directProven or withBones
                e.comps, e.attachedFrom = comps, from
                finish_collect(e, "direct", t0)
            else
                e.directMiss = not worldSearch and (comps == nil and "the lookup failed" or "it lists no mesh") or nil
                viaWorld[#viaWorld + 1] = e
            end
        end
        if #viaWorld > 0 then
            collect_meshes(viaWorld)
            for _, e in ipairs(viaWorld) do
                -- The direct lookup came up empty on a character the world search finds meshes on (not
                -- just one that isn't dressed yet): it doesn't work in this game, so stop paying for both.
                if e.directMiss and not worldSearch and has_bones(e.comps) then
                    worldSearch, lookupChecked = true, true
                    say("the direct mesh lookup found no meshes on %s (%s) but the world search does; using the world search from now on. "
                        .. "Set \"WorldMeshSearch\": true in GameProfiles\\%s to skip the direct lookup.", e.label, e.directMiss, profileFileName)
                end
                finish_collect(e, "world search", t0)
            end
        end
        assemble()
        if #queue == 0 and fullPass then
            local f = fullPass
            fullPass = nil
            local text = string.format("full rescan: %d character(s), %d mesh(es), %.1f ms of work", f.chars, f.meshes, f.ms)
            if f.ms > 5 then
                say("%s", text)
            else
                chatter("%s", text)
            end
        end
        return replan
    end

    -- Works out who is sculpted: the target, then kept characters (NPCs only while near the player),
    -- nearest first. No object-array search here. Characters no longer wanted are dropped at once;
    -- new ones are queued. `full` (Refresh, ubs rescan, reloads) collects everyone again.
    function make_plan(full)
        local actor = resolve_target()
        target = actor
        local pawn = player_pawn()
        local default = cheap_default()
        if not default and not pickedId then
            default = actor -- the automatic target is the default character
        end
        local entries = {}
        if actor then
            local key = (pickedId == nil or (default and same_object(actor, default))) and PLAYER_KEY or address_of(actor)
            targetInfo = { id = pickedId or "auto", label = actor_label(actor, pawn), key = key,
                identity = (key ~= PLAYER_KEY and REMEMBER) and (identity_of(actor) or "") or "" }
            if key ~= PLAYER_KEY then
                knownActors[key] = { actor = actor, full = full_name(actor), label = targetInfo.label }
            end
            entries[1] = { actor = actor, key = key, label = targetInfo.label }
        else
            targetInfo = { id = pickedId or "auto", label = "", key = pickedId or PLAYER_KEY }
        end
        local origin = player_origin()
        local keptInRangeBefore = keptInRange
        keptInRange = {}
        local keptList = {}
        for _, key in ipairs(keptOrder) do
            if key ~= targetInfo.key and not goneKeys[key] then
                local a, label = resolve_kept(key, default, pawn)
                if a and not (actor and same_object(a, actor)) then
                    local near = key == PLAYER_KEY or in_range(a, origin, nil, keptInRangeBefore[key])
                    keptInRange[key] = near
                    if near then
                        local d = 0
                        if key ~= PLAYER_KEY and origin then
                            local p = location_of(a)
                            d = p and ((p[1] - origin[1]) ^ 2 + (p[2] - origin[2]) ^ 2 + (p[3] - origin[3]) ^ 2) or math.huge
                        end
                        keptList[#keptList + 1] = { actor = a, key = key, label = label or key, kept = true, dist = key == PLAYER_KEY and -1 or d }
                    end
                end
            end
        end
        table.sort(keptList, function(x, y)
            if x.dist ~= y.dist then
                return x.dist < y.dist
            end
            return x.key < y.key
        end)
        keptActors = {}
        local overflow = math.max(0, #keptList - MAX_KEPT)
        if overflow ~= keptOverflow then
            keptOverflow = overflow
            if overflow > 0 then
                say("more than %d edited characters nearby; the farthest are left alone (MaxEditedCharacters in config.lua)", MAX_KEPT)
            end
        end
        for i, e in ipairs(keptList) do
            if i > MAX_KEPT then
                break
            end
            entries[#entries + 1] = e
            keptActors[e.key] = e.actor
        end
        plan, planByKey, plannedAddr = entries, {}, {}
        for _, e in ipairs(entries) do
            e.addr, e.full = address_num(e.actor), full_name(e.actor)
            planByKey[e.key] = e
            if e.addr then
                plannedAddr[e.addr] = true
            end
        end
        local dropped = false
        for key, ch in pairs(chars) do
            local e = planByKey[key]
            if not e or ch.addr ~= e.addr or ch.full ~= e.full then
                chars[key] = nil
                dropped = true
                chatter("%s: meshes let go", ch.label)
            end
        end
        local waiting = queued
        queue, queued = {}, {}
        for _, e in ipairs(entries) do
            if full or waiting[e.key] or not chars[e.key] then
                queued[e.key] = true
                queue[#queue + 1] = e.key
            end
        end
        if full then
            scanCount = scanCount + 1 -- reaches the DLL even if nothing changed (re-tracks stale meshes)
            fullPass = #queue > 0 and { ms = 0, chars = 0, meshes = 0 } or nil
        end
        if dropped or (#entries == 0 and #rigs > 0) then
            assemble()
        end
        if #entries == 0 then
            chatter("no character found")
        end
        stateDirty = true
    end

    -- Cheap check of one collected character at a settle step: did a mesh change, or did it gain or
    -- lose one? Characters found by the world search are collected again instead.
    local function settle_changed(key)
        local ch, e = chars[key], planByKey[key]
        if not ch or not e then
            return false
        end
        if ch.how ~= "direct" then
            return true
        end
        for _, r in ipairs(ch.rigs) do
            if not still(r.comp, r.full) or r.meshAddr ~= mesh_prop(r.comp) then
                return true
            end
        end
        local comps = direct_meshes(e)
        return comps == nil or #comps ~= ch.count
    end

    -- Once per session: does the direct lookup find every mesh the world search finds? If not (a game
    -- that hangs outfit meshes on the character in a way the direct lookup cannot see), the world
    -- search is used from then on.
    local function check_lookup(key)
        -- Only a character the direct lookup found meshes on can be compared (not e.g. a main menu
        -- pawn without any); otherwise the check waits for the next new player character.
        local ch, e = chars[key], planByKey[key]
        if not ch or not e or ch.how ~= "direct" or #ch.rigs == 0 then
            return
        end
        local direct = direct_meshes(e)
        if not direct then
            return
        end
        lookupChecked = true
        local have = {}
        for _, c in ipairs(direct) do
            have[address_num(c)] = true
        end
        local probe = { actor = e.actor, key = e.key, label = e.label }
        collect_meshes({ probe })
        local missing = {}
        for _, c in ipairs(probe.comps) do
            if not have[address_num(c)] then
                missing[#missing + 1] = name_of(c) .. (probe.attachedFrom[c] and (" (attached from " .. probe.attachedFrom[c] .. ")") or "")
            end
        end
        if #missing > 0 then
            worldSearch = true
            say("the direct mesh lookup missed %d mesh(es) of %s that the world search finds (%s); using the world search from now on. "
                .. "Set \"WorldMeshSearch\": true in GameProfiles\\%s to skip this check.", #missing, e.label, table.concat(missing, ", "), profileFileName)
            for _, p in ipairs(plan) do
                queue_key(p.key)
            end
        else
            say("mesh lookup check: all %d mesh(es) of %s are found without the world search", #direct, e.label)
        end
    end

    -- For "ubs status": how meshes are found and what the last character scan cost.
    function scan_status()
        return string.format("mesh lookup: %s; %d character scan(s), last: %s, %d mesh(es) in %.1f ms (%s); %d world search(es)%s%s",
            worldSearch and "world search" or (lookupChecked and "direct (checked)" or "direct"), stats.collects,
            stats.label ~= "" and stats.label or "-", stats.meshes, stats.ms, stats.how ~= "" and stats.how or "-", stats.world,
            stats.world > 0 and string.format(", last %.1f ms", stats.worldMs) or "", #queue > 0 and string.format("; %d waiting", #queue) or "")
    end

    -- Settle steps that are due: each character still settling is checked, and collected again if it changed.
    function settle_due(t)
        for key, s in pairs(settles) do
            local due = s.due[s.step + 1]
            if due and t >= due then
                s.step = s.step + 1
                if chars[key] and settle_changed(key) then
                    queue_key(key)
                end
                if key == targetInfo.key then
                    if s.step == CHECK_STEP and not lookupChecked then
                        check_lookup(key)
                    end
                    -- Backstop for characters the new-object notification missed (e.g. loaded with the
                    -- level): after a reload, re-list loaded characters twice and look for remembered ones.
                    if REMEMBER and next(remembered) ~= nil and (s.step == 2 or s.step == 4) then
                        refresh_candidates(true)
                        sweep_characters()
                    end
                end
                if s.step >= #SETTLE_MS then
                    settles[key] = nil
                end
            end
        end
    end

    -- Cheap checks between plans (validity checks and property reads, no object-array searches).
    -- A character whose mesh went away or got another asset is queued for collecting again; returns
    -- true when the plan itself needs redoing (target or kept characters changed, moved in or out of range).
    local profileMisses = 0
    function watch_characters()
        for key, ch in pairs(chars) do
            if not still(ch.actor, ch.full) then
                return true
            end
            for _, r in ipairs(ch.rigs) do
                -- gone, or the game put another mesh asset on it (e.g. a placeholder replaced by the real outfit)
                if not still(r.comp, r.full) or r.meshAddr ~= mesh_prop(r.comp) then
                    queue_key(key)
                    break
                end
            end
        end
        if target and not alive(target) then
            return true
        end
        -- A kept NPC walked into or out of range.
        if RANGE > 0 and next(keptInRange) ~= nil then
            local origin = player_origin()
            for key, near in pairs(keptInRange) do
                local k = knownActors[key]
                if key ~= PLAYER_KEY and k then
                    if not still(k.actor, k.full) then
                        return true
                    end
                    if in_range(k.actor, origin, k.full, near) ~= near then
                        return true
                    end
                end
            end
        end
        for key, a in pairs(keptActors) do
            if not alive(a) then
                return true
            end
            if key == PLAYER_KEY then
                local d = cheap_default()
                if d and not same_object(d, a) then
                    return true -- the player's pawn changed while someone else is being edited
                end
            end
        end
        if pickedId then
            return target == nil
        end
        if Profile.Target and Profile.Target ~= "" then
            if target and same_object(target, profileTarget) then
                return false
            end
            if profileTarget and alive(profileTarget) then
                return true -- e.g. just switched back to automatic
            end
            -- No character yet (menus, loading): search, but back off while nothing turns up.
            profileMisses = profileMisses + 1
            if profileMisses % math.min(8, 2 ^ math.min(3, profileMisses // 4)) ~= 0 then
                return false
            end
            local found = find_profile_target() ~= nil
            if found then
                profileMisses = 0
            end
            return found
        end
        local pawn = player_pawn()
        if (pawn == nil) ~= (target == nil) then
            return true
        end
        return pawn ~= nil and not same_object(pawn, target)
    end
end

---------------------------------------------------------------------------
-- Applying morph weights
---------------------------------------------------------------------------
-- morphWanted: what the DLL asks for, per character (actor key -> folded name -> { name, weight }).
-- A morph in there overrides the game's value; one that leaves it gets the value it had before
-- we touched it.
local morphWanted = {}
local morphDirty = false -- morphWanted or the tracked meshes changed since the last apply
local morphApplied = {} -- "<component address>|<folded name>" -> { comp, name, weight, original }
local morphFights = {} -- folded name -> times something else overwrote our value
local animatedMorphs = {} -- folded name -> display name (reported to the DLL)
local animatedText = ""

local function get_morph(comp, name)
    local ok, v = pcall(function()
        return comp:GetMorphTarget(FName(name))
    end)
    return ok and type(v) == "number" and v or nil
end

-- removeZero: a 0 weight drops the override so the game's animation drives the morph again.
local function set_morph(comp, name, weight, removeZero)
    return pcall(function()
        comp:SetMorphTarget(FName(name), weight, removeZero and true or false)
    end)
end

local function apply_morphs()
    morphDirty = false
    local seenSlot = {}
    for _, r in ipairs(rigs) do
        local wanted = morphWanted[r.actorKey]
        if r.morphSet and wanted and alive(r.comp) then
            for key, want in pairs(wanted) do
                local name = r.morphSet[key]
                if name then
                    local slot = r.address .. "|" .. key
                    seenSlot[slot] = true
                    local a = morphApplied[slot]
                    if not a then
                        a = { comp = r.comp, name = name, original = get_morph(r.comp, name) or 0 }
                        morphApplied[slot] = a
                    end
                    a.comp, a.full = r.comp, r.full -- the wrapper from the latest scan
                    if a.weight ~= want.weight and set_morph(r.comp, name, want.weight, false) then
                        a.weight = want.weight
                    end
                end
            end
        end
    end
    -- Morphs that are no longer wanted (or whose mesh went away): give them back to the game.
    for slot, a in pairs(morphApplied) do
        if not seenSlot[slot] then
            if still(a.comp, a.full) then
                set_morph(a.comp, a.name, a.original, a.original == 0)
            end
            morphApplied[slot] = nil
        end
    end
end

-- Something else (animation curves, the game's own code) may keep writing a morph we set.
-- Put ours back and, after a few times, tell the DLL so the window can mark the morph.
local function watch_morphs()
    local changed = false
    for _, a in pairs(morphApplied) do
        if a.weight ~= nil and still(a.comp, a.full) then
            local v = get_morph(a.comp, a.name)
            if v ~= nil and math.abs(v - a.weight) > 1e-3 then
                set_morph(a.comp, a.name, a.weight, false)
                local key = string.lower(a.name)
                morphFights[key] = (morphFights[key] or 0) + 1
                if morphFights[key] == 3 and not animatedMorphs[key] then
                    animatedMorphs[key] = a.name
                    changed = true
                    say("morph %s keeps being set by the game; the slider is re-applied but may flicker or not stick", a.name)
                end
            end
        end
    end
    if changed then
        local names = {}
        for _, n in pairs(animatedMorphs) do
            names[#names + 1] = clean(n)
        end
        table.sort(names)
        animatedText = table.concat(names, "\t")
        stateDirty = true
    end
end

---------------------------------------------------------------------------
-- Bridge files
---------------------------------------------------------------------------
local session = string.format("%d-%d", os.time(), math.random(100000, 999999))
local seq = 0
local lastWritten = nil
local pendingCommands = {} -- { id, verb, arg }
local nextCommandId = 1
local awaitingReply = {}

local function build_state()
    local lines = {}
    local function add(s)
        if s ~= "" then
            lines[#lines + 1] = s
        end
    end
    add(table.concat({
        "setup",
        "version=" .. VERSION,
        "presets=" .. clean(presetFolder),
        "skins=" .. clean(skinFolder),
        "skin=" .. clean(Config.Skin or ""),
        "restore=" .. (Config.RestoreLastSession and "1" or "0"),
        "startup=" .. clean(Config.StartupPreset or ""),
        "topmost=" .. (Config.KeepWindowOnTop and "1" or "0"),
        "scale=" .. tostring(Config.WindowScale or 1),
        "font=" .. tostring(Config.FontSize or 19),
        "renderer=" .. clean(Config.Renderer or "cpu"),
        "fps=" .. tostring(Config.WindowFps or 30),
        "key=" .. clean(Config.MenuKey or "F6"),
        "refresh_key=" .. clean(Config.RefreshKey == nil and "F7" or Config.RefreshKey),
        "game=" .. clean(projectName),
        "profile=" .. clean(Profile.name),
        "mkdir=" .. (needProfileFolder and clean(profileDir) or ""),
        "profile_file=" .. clean(profileFile),
        "engine=" .. clean(engineVersion),
    }, "\t"))
    add("scan\t" .. scanCount)
    add("target\t" .. clean(targetInfo.id) .. "\t" .. clean(targetInfo.label) .. "\t" .. clean(targetInfo.key) .. "\t" .. clean(targetInfo.identity or ""))
    for key, e in pairs(sighted) do
        add("npc\t" .. clean(key) .. "\t" .. clean(e.identity) .. "\t" .. clean(e.label))
    end
    for _, c in ipairs(candidates) do
        add("cand\t" .. c.id .. "\t" .. clean(c.label .. (c.dist or "")) .. "\t" .. clean(c.key or c.id))
    end
    for key in pairs(goneKeys) do
        add("gone\t" .. clean(key))
    end
    add(rigsText)
    if animatedText ~= "" then
        add("manim\t" .. animatedText)
    end
    for _, c in ipairs(pendingCommands) do
        add("cmd\t" .. c.id .. "\t" .. clean(c.verb) .. "\t" .. clean(c.arg))
    end
    return table.concat(lines, "\n")
end

-- Rewrites bridge_in.txt only when something changed since the last write.
local function write_state()
    if not stateDirty then
        return
    end
    local body = build_state()
    if body == lastWritten then
        stateDirty = false
        return
    end
    local f = io.open(inFile, "wb")
    if not f then
        return -- the DLL has it open; stay dirty and try again next tick
    end
    seq = seq + 1
    f:write(string.format("UBS1 %s %d\n", session, seq), body, string.format("\n#end %d\n", seq))
    f:close()
    lastWritten = body
    stateDirty = false
end

local seen = { rescan = 0, pick = 0, refresh = 0, relist = 0, session = nil }
local hookState, hookText = "waiting", ""
local windowOpen = false
local lastDllText = nil
local pendingScan, pendingCandidates = false, false -- noticed by the fast morph loop, handled by tick()
local pendingRelist = false -- re-sort the character list from what is known (opening the window or the list)
local pendingFull = false -- the DLL asked for a rescan (Refresh, F7, a stale mesh): collect everyone again
local rememberedChanged = false

-- Reads bridge_out.txt. Rescan / character list requests are left in pendingScan (pendingFull)
-- and pendingCandidates; new morph weights set morphDirty.
local function read_dll_state()
    local f = io.open(outFile, "rb")
    if not f then
        return
    end
    local text = f:read("a")
    f:close()
    if not text or text == lastDllText or not text:find("\n#end\n", 1, true) then
        return
    end
    lastDllText = text
    local wantScan, wantCandidates = false, false
    local wanted, sawMorphs = {}, false
    local keep, keepList = {}, {}
    local rememberList = {}
    local dllSession = text:match("^UBS1 (%S+)")
    if dllSession ~= seen.session then
        -- New DLL session: take its counters as the baseline.
        seen.session = dllSession
        seen.rescan, seen.pick, seen.refresh, seen.relist = -1, -1, -1, -1
    end
    local mine = false -- ack/replies are only ours once the DLL has seen this script's session
    local open = false
    for line in text:gmatch("[^\n]+") do
        local kind, a, b = line:match("^(%w+)\t?([^\t]*)\t?(.*)$")
        if kind == "for" then
            mine = a == session
        elseif kind == "ack" and mine then
            local ack = tonumber(a) or 0
            local keep = {}
            for _, c in ipairs(pendingCommands) do
                if c.id > ack then
                    keep[#keep + 1] = c
                end
            end
            if #keep ~= #pendingCommands then
                pendingCommands = keep
                stateDirty = true
            end
        elseif kind == "rescan" then
            local n = tonumber(a) or 0
            if seen.rescan >= 0 and n ~= seen.rescan then
                wantScan = true
                pendingFull = true
            end
            seen.rescan = n
        elseif kind == "pick" then
            local n = tonumber(a) or 0
            if seen.pick >= 0 and n ~= seen.pick then
                pickedId = (b ~= "" and b ~= "auto") and b or nil
                wantScan, wantCandidates = true, true
            end
            seen.pick = n
        elseif kind == "refresh" then
            local n = tonumber(a) or 0
            if seen.refresh >= 0 and n ~= seen.refresh then
                wantCandidates = true
            end
            seen.refresh = n
        elseif kind == "relist" then
            local n = tonumber(a) or 0
            if seen.relist >= 0 and n ~= seen.relist then
                pendingRelist = true
            end
            seen.relist = n
        elseif kind == "ui" then
            open = a == "1"
        elseif kind == "hook" then
            if a ~= hookState or b ~= hookText then
                if a == "failed" then
                    say("pose hook: %s (see native\\UUEPBS.log)", b)
                elseif a == "live" and hookState ~= "live" then
                    say("%s", b)
                end
                hookState, hookText = a, b
            end
        elseif kind == "morph" then
            sawMorphs = true
        elseif kind == "mw" then
            local weight, key = b:match("^([^\t]*)\t?(.*)$")
            local w = tonumber(weight)
            if key == nil or key == "" then
                key = targetInfo.key
            end
            if a ~= "" and w then
                wanted[key] = wanted[key] or {}
                wanted[key][string.lower(a)] = { name = a, weight = w }
            end
        elseif kind == "remember" then
            if a ~= "" then
                rememberList[#rememberList + 1] = a
            end
        elseif kind == "keep" then
            if a ~= "" and not keep[a] then
                keep[a] = b
                keepList[#keepList + 1] = a
            end
        elseif kind == "reply" and mine then
            local id = tonumber(a)
            if id and awaitingReply[id] then
                awaitingReply[id] = nil
                for part in (b .. "\\n"):gmatch("(.-)\\n") do
                    if part ~= "" then
                        print(TAG .. part .. "\n")
                    end
                end
            end
        end
    end
    if open and not windowOpen then
        -- the picker is about to be looked at: sorted by the current distances
        if liveStale or not USE_LIVE_LIST then
            wantCandidates = true
        else
            pendingRelist = true
        end
    end
    windowOpen = open
    if sawMorphs then
        morphWanted = wanted
        morphDirty = true
    end
    -- Characters with edits: a change in the list means meshes to pick up or let go.
    local rtext = table.concat(rememberList, "\n")
    if rtext ~= rememberedText then
        rememberedText = rtext
        remembered = {}
        for _, id in ipairs(rememberList) do
            remembered[id] = true
        end
        rememberedChanged = true
    end
    if table.concat(keepList, "\n") ~= table.concat(keptOrder, "\n") then
        keptOrder, keptSet = keepList, keep
        wantScan = true
    end
    for key in pairs(goneKeys) do
        if not keep[key] then
            goneKeys[key] = nil -- the DLL dropped it
            stateDirty = true
        end
    end
    pendingScan = pendingScan or wantScan
    pendingCandidates = pendingCandidates or wantCandidates
end

local function send_command(verb, arg, echo)
    local id = nextCommandId
    nextCommandId = nextCommandId + 1
    pendingCommands[#pendingCommands + 1] = { id = id, verb = verb, arg = arg or "" }
    if echo then
        awaitingReply[id] = true
    end
    stateDirty = true
    write_state()
    return id
end

---------------------------------------------------------------------------
-- Scheduling (everything that touches UObjects runs on the game thread)
---------------------------------------------------------------------------
local POLL_MS = math.max(100, tonumber(Config.PollIntervalMs) or 400)
local WATCH_MS = math.max(POLL_MS, tonumber(Config.WatchIntervalMs) or 3000)
local PICKER_MS = 5000 -- character list refresh while the window is open

local function now_ms()
    return os.clock() * 1000.0
end

local scanDue = 0 -- when the plan is next worked out (see make_plan)
local scanFull = false -- ... and everyone collected again
local candidatesDue = 0 -- once at start; afterwards only on request or while the window is open
local candidatesFull = true -- the next listing walks the object array (start, window opened, Refresh, reloads)
local watchDue = 0

local function schedule_scan(delayMs, full)
    local due = now_ms() + (delayMs or 0)
    if scanDue == nil or due < scanDue then
        scanDue = due
    end
    scanFull = scanFull or full == true
end

local function tick()
    local ok, err = pcall(function()
        if pendingProfileText then
            flush_profile()
        end
        read_dll_state()
        local wantScan, wantCandidates = pendingScan, pendingCandidates
        pendingScan, pendingCandidates = false, false
        if rememberedChanged then
            rememberedChanged = false
            sweep_characters()
        end
        check_new_characters(now_ms())
        if wantCandidates then
            candidatesDue, candidatesFull = 0, true
        elseif pendingRelist then
            candidatesDue = 0
        end
        pendingRelist = false
        if wantScan then
            schedule_scan(0, pendingFull)
            pendingFull = false
        end
        local t = now_ms()
        if candidatesDue and t >= candidatesDue then
            refresh_candidates(candidatesFull)
            candidatesFull = false
            candidatesDue = windowOpen and (t + PICKER_MS) or nil
        elseif candidatesDue == nil and windowOpen then
            candidatesDue = t + PICKER_MS
        end
        if scanDue and t >= scanDue then
            scanDue = nil
            local full = scanFull
            scanFull = false
            make_plan(full)
            watchDue = t + WATCH_MS
        elseif t >= watchDue then
            watchDue = t + WATCH_MS
            if watch_characters() then
                schedule_scan(300)
            end
        end
        settle_due(t)
        -- One character per tick (the one being edited first), so a crowd arriving at once is
        -- spread over several ticks instead of landing in one frame.
        if process_queue() then
            schedule_scan(0)
        end
        if rigsChanged then
            rigsChanged = false
            morphDirty = true -- new or re-found meshes get the morph weights too
        end
        if morphDirty then
            apply_morphs()
        elseif next(morphApplied) ~= nil then
            watch_morphs()
        end
        write_state()
    end)
    if not ok then
        say("tick error: %s", tostring(err))
    end
end

-- While the window is open, morph slider drags are picked up every MorphPollMs instead of
-- waiting for the next PollIntervalMs tick. It only reads one small file, and does nothing
-- at all while the window is closed.
local MORPH_MS = tonumber(Config.MorphPollMs) or 100
local function morph_tick()
    if not windowOpen then
        return
    end
    local ok, err = pcall(function()
        read_dll_state()
        if morphDirty then
            apply_morphs()
        end
    end)
    if not ok then
        say("morph tick error: %s", tostring(err))
    end
end

local function every(ms, fn)
    if type(LoopInGameThreadWithDelay) == "function" then
        LoopInGameThreadWithDelay(ms, fn)
    else
        LoopAsync(ms, function()
            ExecuteInGameThread(fn)
            return false
        end)
    end
end

every(POLL_MS, tick)
if MORPH_MS > 0 and MORPH_MS < POLL_MS then
    every(math.max(33, MORPH_MS), morph_tick)
end

local function world_changed()
    cachedController = nil
    liveStale = true
    schedule_scan(1000, true)
    candidatesDue, candidatesFull = 0, true
end

if Profile.TargetClassPath and Profile.TargetClassPath ~= "" then
    pcall(function()
        NotifyOnNewObject(Profile.TargetClassPath, function()
            schedule_scan(1500)
            if windowOpen then
                candidatesDue = 0
            end
            return false
        end)
    end)
end

pcall(function()
    RegisterHook("/Script/Engine.PlayerController:ClientRestart", world_changed)
end)

-- New characters (reloads, new areas): checked against the remembered NPCs a moment later,
-- once the game has named and dressed them. Only queued while there is anything remembered.
-- Every new character is also added to the picker's list (see note_character).
pcall(function()
    NotifyOnNewObject("/Script/Engine.Character", function(obj)
        note_character(obj)
        if REMEMBER and next(remembered) ~= nil and #newCharacters < 512 then
            newCharacters[#newCharacters + 1] = { obj = obj, due = now_ms() + 2000 }
        end
        return false
    end)
end)

---------------------------------------------------------------------------
-- Console: uuepbs <command>  (ubs for short)
---------------------------------------------------------------------------
local function rest_of(params, first)
    local parts = {}
    for i = first, #params do
        parts[#parts + 1] = params[i]
    end
    return table.concat(parts, " ")
end

local usage = "uuepbs (or ubs) ui | status | rescan | list | load <name> | save <name> | on | off | reset [all] | diag | skin [name] | morph <name> [weight]"

local function console(_, params, out)
    local verb = string.lower(params[1] or "")
    local function reply(text)
        if out and out.Log then
            out:Log(text)
        end
        print(TAG .. text .. "\n")
    end
    if verb == "" or verb == "ui" then
        send_command("ui", "", false)
    elseif verb == "rescan" then
        cachedController = nil
        candidatesDue, candidatesFull = 0, true
        schedule_scan(0, true)
        reply("rescanning")
    elseif verb == "status" then
        reply(string.format("script v%s, game '%s', profile '%s', character '%s', %d other edited character(s), %d mesh(es), hook %s", VERSION,
            projectName, Profile.name, targetInfo.label, math.max(0, #keptOrder - (keptSet[targetInfo.key] and 1 or 0)), #rigs, hookState))
        reply(scan_status())
        send_command("status", "", true)
    elseif verb == "load" or verb == "save" or verb == "skin" or verb == "morph" or verb == "reset" then
        send_command(verb, rest_of(params, 2), true)
    elseif verb == "list" or verb == "on" or verb == "off" or verb == "diag" then
        send_command(verb, "", true)
    else
        reply(usage)
    end
    return true
end

RegisterConsoleCommandHandler("uuepbs", console)
RegisterConsoleCommandHandler("ubs", console)

write_state()
say("ready - press %s in game to open the sliders", tostring(Config.MenuKey or "F6"))
