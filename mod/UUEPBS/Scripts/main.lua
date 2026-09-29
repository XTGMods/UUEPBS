-- UUEPBS (Universal Unreal Engine Player Body Sliders) - Lua half
--
-- Finds the character to edit, reads the bone hierarchy of its skeletal meshes and
-- hands everything to native\UUEPBS.dll through two small text files
-- (the DLL edits the final pose every frame and owns the slider window and presets).
-- The DLL is a plain Windows DLL mapped with package.loadlib, not a UE4SS C++ mod,
-- so it does not have to match the UE4SS version.
--
-- Performance notes: everything here runs on the game thread, so the script avoids
-- whole-object-array searches (FindAllOf) outside of real scans. Between scans it only
-- checks that the objects it already holds are still valid, and it only rewrites the
-- bridge file when something actually changed.

local VERSION = "2.1.0"
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
    "SameSkeletonOnly", "BodyGroups", "Hook", "_help" }
local PROFILE_HELP = "Target: actor class to edit (empty = the pawn you control). TargetClassPath: optional full path of that class. "
    .. "PrimaryComponent: mesh whose bones are listed (empty = the Character's Mesh). Include/ExcludeComponents: mesh names. "
    .. "SameSkeletonOnly: also edit other meshes on the actor that share the primary mesh's skeleton. "
    .. "BodyGroups: remap Body tab sliders to this game's bones, e.g. {\"Thighs\": [\"Hip_L\", \"Hip_R\"], \"Waist\": [\"Spine1_M\"]} "
    .. "(real bone names from the Bones tab, * wildcards allowed; [] hides a slider; new names add sliders)."

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
        hierarchyCache[key] = cached
    end
    if wantReference and not cached.referenceTried then
        cached.referenceTried = true
        cached.reference = read_reference(comp, count)
        chatter("reference pose %s", cached.reference and "read" or "not available (mirroring will be measured in game)")
    end
    return cached
end

---------------------------------------------------------------------------
-- Target selection
---------------------------------------------------------------------------
local pickedId = nil -- address chosen in the window's picker (nil = automatic)
local target = nil -- current actor
local targetInfo = { id = "", label = "" }
local rigs = {} -- list of { comp, address, label, primary, hierarchy }
local candidates = {}
local candidatesText = ""

local function actor_label(actor, pawn)
    local label = name_of(actor)
    if pawn and same_object(actor, pawn) then
        label = label .. " (player)"
    end
    return label
end

-- Walks the object array once per listed class, so it only runs when the picker needs it.
local function refresh_candidates()
    local list, seen = {}, {}
    local pawn = player_pawn()
    local function add(actor)
        if #list >= (Config.MaxCandidates or 40) or not alive(actor) then
            return
        end
        local id = address_of(actor)
        if not seen[id] and not is_default_object(actor) then
            seen[id] = true
            list[#list + 1] = { id = id, label = actor_label(actor, pawn), actor = actor }
        end
    end
    if pawn then
        add(pawn)
    end
    if Profile.Target and Profile.Target ~= "" then
        for _, a in ipairs(FindAllOf(Profile.Target) or {}) do
            add(a)
        end
    end
    for _, class in ipairs(Config.CandidateClasses or {}) do
        for _, a in ipairs(FindAllOf(class) or {}) do
            add(a)
        end
    end
    candidates = list
    local parts = {}
    for _, c in ipairs(list) do
        parts[#parts + 1] = c.id .. "\t" .. c.label
    end
    local text = table.concat(parts, "\n")
    if text ~= candidatesText then
        candidatesText = text
        stateDirty = true
    end
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

local function resolve_target()
    if pickedId then
        for _, c in ipairs(candidates) do
            if c.id == pickedId and alive(c.actor) then
                return c.actor
            end
        end
        refresh_candidates()
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

---------------------------------------------------------------------------
-- Scanning
---------------------------------------------------------------------------
local scanCount = 0
local lastScanReport = nil
local rigsText = ""

local function rebuild_rigs_text()
    local lines = {}
    for _, r in ipairs(rigs) do
        lines[#lines + 1] = "rig\t" .. r.address .. "\t" .. clean(r.label) .. "\t" .. (r.primary and "1" or "0") .. "\t" .. clean(targetInfo.label)
        lines[#lines + 1] = "bones\t" .. r.hierarchy.names
        lines[#lines + 1] = "parents\t" .. r.hierarchy.parents
        if r.primary and r.hierarchy.reference then
            lines[#lines + 1] = "ref\t" .. r.hierarchy.reference
        end
    end
    rigsText = table.concat(lines, "\n")
end

local function scan()
    scanCount = scanCount + 1 -- sent to the DLL, so a rescan always reaches it (re-tracks stale meshes)
    stateDirty = true
    local actor = resolve_target()
    target = actor
    rigs = {}
    if not actor then
        targetInfo = { id = pickedId or "auto", label = "" }
        rebuild_rigs_text()
        chatter("no character found")
        return
    end
    local pawn = player_pawn()
    targetInfo = { id = pickedId or "auto", label = actor_label(actor, pawn) }
    local owner = address_num(actor)

    -- Meshes owned by the actor, plus meshes of other actors attached to it
    -- (costume / outfit mods often spawn their own actor and attach it to the character).
    -- The attachment walk is done once per owning actor, not once per component.
    local attachedActors = {}
    local function chain_reaches_target(start)
        local ok, found = pcall(function()
            local parent = start
            for _ = 1, 8 do
                if not alive(parent) then
                    return false
                end
                if address_num(parent:GetOuter()) == owner then
                    return true
                end
                parent = parent:GetAttachParent()
            end
            return false
        end)
        return ok and found == true
    end
    local function actor_attached(outer, comp)
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
                known = ok and chain_reaches_target(parent) or false
            else
                local ok, parent = pcall(function()
                    return comp:GetAttachParent()
                end)
                return ok and chain_reaches_target(parent) or false
            end
            attachedActors[key] = known
        end
        return known
    end

    local comps, attachedFrom = {}, {}
    for _, comp in ipairs(FindAllOf("SkeletalMeshComponent") or {}) do
        local ok, outer = pcall(function()
            return comp:GetOuter()
        end)
        if ok and outer ~= nil then
            local outerAddr = address_num(outer)
            if outerAddr == owner then
                if alive(comp) then
                    comps[#comps + 1] = comp
                end
            elseif outerAddr and actor_attached(outer, comp) and alive(comp) and not is_default_object(comp) then
                comps[#comps + 1] = comp
                attachedFrom[comp] = name_of(outer)
            end
        end
    end

    -- Primary: named component, else the Character's Mesh, else the most bones.
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
        rebuild_rigs_text()
        chatter("%s has no skeletal mesh", targetInfo.label)
        return
    end
    local primarySkeleton = skeleton_path(primary)

    local report = {}
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
        if use then
            local ok, h = pcall(read_hierarchy, comp, isPrimary)
            if ok and h then
                rigs[#rigs + 1] = { comp = comp, address = address_of(comp), label = label, primary = isPrimary, hierarchy = h }
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
        report[#report + 1] = string.format("  mesh %s%s: %s, %d bones, %s, skeleton %s -> %s%s", label,
            attachedFrom[comp] and (" (attached from " .. attachedFrom[comp] .. ")") or "", asset, bones, visible,
            (compSkeleton or "?"):match("([^/.]+)$") or "?", why, isPrimary and " (primary)" or "")
    end
    report[#report + 1] = string.format("%s: %d mesh(es) driven", targetInfo.label, #rigs)
    -- Only log a scan when its outcome differs from the last one (rescans are often no-ops).
    local reportText = table.concat(report, "\n")
    if reportText ~= lastScanReport or Config.Verbose then
        lastScanReport = reportText
        for _, line in ipairs(report) do
            say("%s", line)
        end
    end
    rebuild_rigs_text()
end

-- Cheap check between scans: same character, same meshes, all still alive?
-- Only validity checks and property reads here - no object-array searches.
local profileMisses = 0
local function needs_rescan()
    if target and not alive(target) then
        return true
    end
    for _, r in ipairs(rigs) do
        if not alive(r.comp) then
            return true
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
    add("target\t" .. clean(targetInfo.id) .. "\t" .. clean(targetInfo.label))
    for _, c in ipairs(candidates) do
        add("cand\t" .. c.id .. "\t" .. clean(c.label))
    end
    add(rigsText)
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

local seen = { rescan = 0, pick = 0, refresh = 0, session = nil }
local hookState, hookText = "waiting", ""
local windowOpen = false
local lastDllText = nil

-- Returns wantScan, wantCandidates (the DLL asked for a rescan / new character list).
local function read_dll_state()
    local f = io.open(outFile, "rb")
    if not f then
        return false, false
    end
    local text = f:read("a")
    f:close()
    if not text or text == lastDllText or not text:find("\n#end\n", 1, true) then
        return false, false
    end
    lastDllText = text
    local wantScan, wantCandidates = false, false
    local dllSession = text:match("^UBS1 (%S+)")
    if dllSession ~= seen.session then
        -- New DLL session: take its counters as the baseline.
        seen.session = dllSession
        seen.rescan, seen.pick, seen.refresh = -1, -1, -1
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
        wantCandidates = true -- the picker is about to be looked at
    end
    windowOpen = open
    return wantScan, wantCandidates
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

local scanDue = 0
local candidatesDue = 0 -- once at start; afterwards only on request or while the window is open
local watchDue = 0

local function schedule_scan(delayMs)
    local due = now_ms() + (delayMs or 0)
    if scanDue == nil or due < scanDue then
        scanDue = due
    end
end

local function tick()
    local ok, err = pcall(function()
        if pendingProfileText then
            flush_profile()
        end
        local wantScan, wantCandidates = read_dll_state()
        if wantCandidates then
            candidatesDue = 0
        end
        if wantScan then
            schedule_scan(0)
        end
        local t = now_ms()
        if candidatesDue and t >= candidatesDue then
            refresh_candidates()
            candidatesDue = windowOpen and (t + PICKER_MS) or nil
        elseif candidatesDue == nil and windowOpen then
            candidatesDue = t + PICKER_MS
        end
        if scanDue and t >= scanDue then
            scanDue = nil
            scan()
            watchDue = t + WATCH_MS
        elseif t >= watchDue then
            watchDue = t + WATCH_MS
            if needs_rescan() then
                schedule_scan(300)
            end
        end
        write_state()
    end)
    if not ok then
        say("tick error: %s", tostring(err))
    end
end

if type(LoopInGameThreadWithDelay) == "function" then
    LoopInGameThreadWithDelay(POLL_MS, tick)
else
    LoopAsync(POLL_MS, function()
        ExecuteInGameThread(tick)
        return false
    end)
end

local function world_changed()
    cachedController = nil
    schedule_scan(1000)
    candidatesDue = 0
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

local usage = "uuepbs (or ubs) ui | status | rescan | list | load <name> | save <name> | on | off | reset | diag"

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
        candidatesDue = 0
        schedule_scan(0)
        reply("rescanning")
    elseif verb == "status" then
        reply(string.format("script v%s, game '%s', profile '%s', character '%s', %d mesh(es), hook %s", VERSION, projectName, Profile.name,
            targetInfo.label, #rigs, hookState))
        send_command("status", "", true)
    elseif verb == "load" or verb == "save" then
        send_command(verb, rest_of(params, 2), true)
    elseif verb == "list" or verb == "on" or verb == "off" or verb == "reset" or verb == "diag" then
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
