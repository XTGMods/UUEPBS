-- UUEPBS Auto Ground: keeps the player's feet on the floor when the sliders change leg length.
--
-- The DLL edits the final pose, so longer legs push the feet below the floor (the pelvis stays at
-- the same height) and shorter legs leave them floating. This module reads the player's sliders
-- from <presets>\_last_session.json, works out from the reference pose how far the lowest foot
-- bones move up or down with those sliders, and moves the player's mesh component the other way
-- (RelativeLocation Z), so the whole body stands on the floor again.
--
-- Terrain: the game's foot IK puts the *unedited* feet on the ground (stairs, slopes), and how far
-- the edit sinks a foot depends on how its leg is bent. With AutoGroundFollowPose the lift is worked
-- out every frame from the live bone positions (GetSocketLocation) instead of the reference pose:
-- the foot that sank most is put back on the ground. One mesh offset cannot fix both feet, so on a
-- step the higher foot may float slightly.
--
-- Model of the DLL's edit (an approximation: rotate sliders are ignored):
--   length / width / depth scale the bone along its X / Y / Z, and its children's offsets with it;
--   children "scale_too" also inherit the scale, "keep_size" keep their own size,
--   "this_bone_only" leaves everything below the bone as it was. move = cm along the bone's X, Y, Z.

local M = {}

local IGNORE_WORDS = { "ik", "attach", "weapon", "socket", "prop", "camera", "root", "twist", "skirt", "cloth", "ribbon", "cape" }
local CONTACT_BAND = 12 -- cm above the lowest foot bone that still count as touching the floor

local function split_tabs(text)
    local out = {}
    for part in (text .. "\t"):gmatch("([^\t]*)\t") do
        out[#out + 1] = part
    end
    return out
end

local function quat_rotate(q, v)
    -- v' = v + 2w(u x v) + 2 u x (u x v), u = (q.x, q.y, q.z)
    local qx, qy, qz, qw = q[1], q[2], q[3], q[4]
    local vx, vy, vz = v[1], v[2], v[3]
    local cx = qy * vz - qz * vy
    local cy = qz * vx - qx * vz
    local cz = qx * vy - qy * vx
    local ccx = qy * cz - qz * cy
    local ccy = qz * cx - qx * cz
    local ccz = qx * cy - qy * cx
    return { vx + 2 * (qw * cx + ccx), vy + 2 * (qw * cy + ccy), vz + 2 * (qw * cz + ccz) }
end

-- a * b (apply b first, then a), as UE composes a child's local rotation under its parent's.
local function quat_mul(a, b)
    local ax, ay, az, aw = a[1], a[2], a[3], a[4]
    local bx, by, bz, bw = b[1], b[2], b[3], b[4]
    return {
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    }
end

local function ignored(name)
    local n = name:lower()
    for _, w in ipairs(IGNORE_WORDS) do
        -- whole word only: "ik_foot_l", "foot_ik", "root"; not "kick" or "rootpart"
        if n == w or n:find("^" .. w .. "[_%.%s]") or n:find("[_%.%s]" .. w .. "$") or n:find("[_%.%s]" .. w .. "[_%.%s]") then
            return true
        end
    end
    return false
end

-- Skeleton from a rig's hierarchy (names, parents, reference). nil when the reference pose is missing.
local function parse_skeleton(h)
    if not h or type(h.reference) ~= "string" then
        return nil
    end
    local names, parents = split_tabs(h.names), split_tabs(h.parents)
    local nums = {}
    for v in h.reference:gmatch("%S+") do
        nums[#nums + 1] = tonumber(v) or 0
    end
    local count = #names
    if #parents ~= count or #nums ~= count * 10 then
        return nil
    end
    local sk = { count = count, names = names, parents = {}, q = {}, t = {} }
    for i = 1, count do
        sk.parents[i] = (tonumber(parents[i]) or -1) + 1 -- 1-based, 0 = no parent
        local b = (i - 1) * 10
        sk.q[i] = { nums[b + 1], nums[b + 2], nums[b + 3], nums[b + 4] }
        sk.t[i] = { nums[b + 5], nums[b + 6], nums[b + 7] }
    end
    return sk
end

local function vec3(v, default)
    if type(v) ~= "table" then
        return { default, default, default }
    end
    return {
        tonumber(v[1] or v.x or v.X) or default,
        tonumber(v[2] or v.y or v.Y) or default,
        tonumber(v[3] or v.z or v.Z) or default,
    }
end

-- Component-space positions of every bone with the given sliders (bone name lowercased -> slider).
local function pose_positions(sk, sliders)
    local pos, rot, total, inherited, kidsPos, mode = {}, {}, {}, {}, {}, {}
    local one = { 1, 1, 1 }
    for i = 1, sk.count do
        local s = sliders[sk.names[i]:lower()]
        local own = s and { tonumber(s.length) or 1, tonumber(s.width) or 1, tonumber(s.depth) or 1 } or one
        local move = s and s.move and vec3(s.move, 0) or nil
        mode[i] = s and s.children or "keep_size"
        local p = sk.parents[i]
        local base, r, inh
        if p == 0 then
            base, r, inh = sk.t[i], sk.q[i], one
        else
            -- offset to this bone: scaled by the parent's edit unless the parent was "this bone only"
            local pscale = mode[p] == "this_bone_only" and inherited[p] or total[p]
            local t = sk.t[i]
            local d = quat_rotate(rot[p], { t[1] * pscale[1], t[2] * pscale[2], t[3] * pscale[3] })
            local pp = kidsPos[p]
            base = { pp[1] + d[1], pp[2] + d[2], pp[3] + d[3] }
            r = quat_mul(rot[p], sk.q[i])
            if mode[p] == "scale_too" then
                inh = total[p]
            elseif mode[p] == "this_bone_only" then
                inh = inherited[p]
            else
                inh = one
            end
        end
        local here = base
        if move then
            local m = quat_rotate(r, move)
            here = { base[1] + m[1], base[2] + m[2], base[3] + m[3] }
        end
        pos[i], rot[i], inherited[i] = here, r, inh
        total[i] = { inh[1] * own[1], inh[2] * own[2], inh[3] * own[3] }
        kidsPos[i] = mode[i] == "this_bone_only" and base or here
    end
    return pos
end

local FOOT_WORDS = { "foot", "ball", "toe", "ankle", "heel" }
local LEG_WORDS = { "thigh", "upleg", "upperleg", "calf", "lowerleg", "shin", "knee", "leg", "datui", "xiaotui", "momo" }

local function has_word(name, words)
    local n = name:lower()
    for _, w in ipairs(words) do
        if n:find(w, 1, true) then
            return true
        end
    end
    return false
end

-- The bones that touch the floor in the reference pose: feet, balls and toes that hang below a leg
-- bone. Weapon / camera / interaction points (children of the root at the origin), IK targets
-- (they never move with the sliders) and skirt or cloth bones (they can hang lower than the feet)
-- are left out. Falls back to any foot-named bone, then to the lowest bones of the skeleton.
local function contact_bones(sk, refPos)
    local underLeg = {}
    for i = 1, sk.count do
        local p = sk.parents[i]
        underLeg[i] = p ~= 0 and (underLeg[p] or has_word(sk.names[p], LEG_WORDS))
    end
    local function pick(test)
        local list, minZ = {}, math.huge
        for i = 1, sk.count do
            if sk.parents[i] ~= 0 and not ignored(sk.names[i]) and test(i) then
                list[#list + 1] = i
                minZ = math.min(minZ, refPos[i][3])
            end
        end
        local out = {}
        for _, i in ipairs(list) do
            if refPos[i][3] <= minZ + CONTACT_BAND then
                out[#out + 1] = i
            end
        end
        return out
    end
    local out = pick(function(i)
        return underLeg[i] and has_word(sk.names[i], FOOT_WORDS)
    end)
    if #out == 0 then
        out = pick(function(i)
            return has_word(sk.names[i], FOOT_WORDS)
        end)
    end
    if #out == 0 then
        out = pick(function(i)
            return underLeg[i]
        end)
    end
    return out
end


-- How far (cm, mesh space) the body has to go up so the lowest foot bone is where it was,
-- in the reference pose. Used until the live pose can be read (and if it never can).
local function compute_lift(sk, sliders)
    if not sk.ref then
        sk.ref = pose_positions(sk, {})
        sk.contact = contact_bones(sk, sk.ref)
    end
    if #sk.contact == 0 then
        return 0
    end
    local pos = pose_positions(sk, sliders)
    local refMin, newMin = math.huge, math.huge
    for _, i in ipairs(sk.contact) do
        refMin = math.min(refMin, sk.ref[i][3])
        newMin = math.min(newMin, pos[i][3])
    end
    return refMin - newMin
end

local function dist(a, b)
    local x, y, z = a[1] - b[1], a[2] - b[2], a[3] - b[3]
    return math.sqrt(x * x + y * y + z * z)
end

-- Live-pose plan for the sliders: the chain from the hips down to each contact bone, split into
-- segments (parent bone -> child bone). The sliders stretch each segment by a factor k (its length
-- with the sliders / its reference length). The game's IK put the unedited feet on the ground, so
-- how far a foot sank on this frame is the sum, down its chain, of how much each segment's vertical
-- part grew: on a bent leg (stairs, slopes, walking) that is less than on a straight one.
local function build_plan(sk, sliders)
    if not sk.ref then
        compute_lift(sk, {})
    end
    local pos = pose_positions(sk, sliders)
    local segs, segIndex, chains, bones, boneSet = {}, {}, {}, {}, {}
    local function need(i)
        if not boneSet[i] then
            boneSet[i] = true
            bones[#bones + 1] = i
        end
    end
    local best = nil
    for _, c in ipairs(sk.contact) do
        local chain = {}
        local i = c
        while sk.parents[i] ~= 0 and sk.parents[sk.parents[i]] ~= 0 do -- stop below the root
            local p = sk.parents[i]
            local id = segIndex[i]
            if id == nil then
                local l0 = dist(sk.ref[i], sk.ref[p])
                local k = l0 > 0.5 and dist(pos[i], pos[p]) / l0 or 1
                if math.abs(k - 1) > 1e-4 then
                    segs[#segs + 1] = { child = i, parent = p, k = k, l0 = l0 }
                    id = #segs
                    need(i)
                    need(p)
                    if not best or math.abs(k - 1) * l0 > math.abs(segs[best].k - 1) * segs[best].l0 then
                        best = id
                    end
                else
                    id = false
                end
                segIndex[i] = id
            end
            if id then
                chain[#chain + 1] = id
            end
            i = p
        end
        chains[#chains + 1] = chain
    end
    return { segs = segs, chains = chains, bones = bones, probe = best }
end

---------------------------------------------------------------------------

-- opts: json, read_text, say, alive, still, address_of, fname, sessionFile, playerKey,
--       enabled, extraCm, maxCm, followPose, smoothing
function M.new(opts)
    local self = {
        enabled = opts.enabled ~= false,
        followPose = opts.followPose ~= false,
        smoothing = math.max(0, math.min(0.95, tonumber(opts.smoothing) or 0)),
        extra = tonumber(opts.extraCm) or 0,
        max = math.abs(tonumber(opts.maxCm) or 40),
        lift = 0, -- the lift in effect (cm, extra included)
        computed = 0, -- reference pose lift from the sliders alone (cm, mesh space)
        live = nil, -- last live-pose lift (cm, world), nil = not available
        sessionText = nil,
        sliders = {},
        skeletonKey = nil,
        skeleton = nil,
        plan = nil,
        primary = nil, -- rig of the player's primary mesh
        roots = {}, -- address -> rig: the player's meshes not attached to another one of them
        edited = nil, -- does GetSocketLocation see the DLL's edit? (true / false / nil = not known yet)
        votes = 0,
        liveFails = 0,
        applied = {}, -- component address -> { comp, full, base = {x,y,z}, last = {x,y,z}, fights }
        warned = {},
        note = "waiting for the player's mesh",
    }

    local function get_rel(comp)
        local ok, v = pcall(function()
            local l = comp.RelativeLocation
            return { l.X, l.Y, l.Z }
        end)
        return ok and type(v) == "table" and type(v[3]) == "number" and v or nil
    end

    local function get_scale_z(comp)
        local ok, z = pcall(function()
            return comp.RelativeScale3D.Z
        end)
        return ok and type(z) == "number" and z ~= 0 and z or 1
    end

    local function set_rel(comp, v)
        local ok = pcall(function()
            comp:K2_SetRelativeLocation({ X = v[1], Y = v[2], Z = v[3] }, false, {}, false)
        end)
        return ok
    end

    local function differs(a, b)
        return math.abs(a[1] - b[1]) > 0.01 or math.abs(a[2] - b[2]) > 0.01 or math.abs(a[3] - b[3]) > 0.01
    end

    local function restore(addr)
        local a = self.applied[addr]
        if a and opts.still(a.comp, a.full) then
            local cur = get_rel(a.comp)
            -- only put the base back if the game has not moved the mesh itself since
            if cur and not differs(cur, a.last) then
                set_rel(a.comp, a.base)
            end
        end
        self.applied[addr] = nil
    end

    function self.restore_all()
        for addr in pairs(self.applied) do
            restore(addr)
        end
    end

    local function read_sliders()
        local text = opts.read_text(opts.sessionFile)
        if text == nil or text == self.sessionText then
            return false
        end
        self.sessionText = text
        local data = opts.json.decode(text)
        local sliders = {}
        if type(data) == "table" and type(data.bones) == "table" then
            for name, s in pairs(data.bones) do
                if type(name) == "string" and type(s) == "table" then
                    sliders[name:lower()] = s
                end
            end
        end
        self.sliders = sliders
        return true
    end

    -- Moves the root meshes so the body is `lift` cm higher than where the game put it.
    local function apply(lift)
        for addr in pairs(self.applied) do
            if not self.roots[addr] or lift == 0 then
                restore(addr)
            end
        end
        if lift == 0 then
            return 0
        end
        local count = 0
        for addr, r in pairs(self.roots) do
            local cur = opts.still(r.comp, r.full) and get_rel(r.comp) or nil
            if cur then
                local a = self.applied[addr]
                if not a then
                    a = { comp = r.comp, full = r.full, base = cur, last = cur, fights = 0 }
                    self.applied[addr] = a
                elseif differs(cur, a.last) then
                    -- the game moved the mesh (crouching, a cutscene...): that is the new base
                    a.base = cur
                    a.fights = a.fights + 1
                    if a.fights == 20 and not self.warned[addr] then
                        self.warned[addr] = true
                        opts.say("auto ground: the game keeps moving %s itself; the lift is re-applied but may jitter", r.label)
                    end
                end
                a.comp, a.full = r.comp, r.full
                local want = { a.base[1], a.base[2], a.base[3] + lift }
                if differs(cur, want) then
                    if set_rel(r.comp, want) then
                        a.last = get_rel(r.comp) or want
                    end
                else
                    a.last = cur
                end
                count = count + 1
            end
        end
        return count
    end

    local function clamp(lift)
        lift = lift + self.extra
        if lift > self.max then
            lift = self.max
        elseif lift < -self.max then
            lift = -self.max
        end
        if math.abs(lift) < 0.05 then
            lift = 0
        end
        return lift
    end

    -- The live-pose lift (cm, world) from this frame's bone positions, or nil when they cannot be read.
    local function live_lift()
        local plan, sk, r = self.plan, self.skeleton, self.primary
        if not (plan and sk and r) or not opts.still(r.comp, r.full) then
            return nil
        end
        if #plan.segs == 0 then
            return 0
        end
        local at = {}
        for _, i in ipairs(plan.bones) do
            local ok, v = pcall(function()
                local l = r.comp:GetSocketLocation(opts.fname(sk.names[i]))
                return { l.X, l.Y, l.Z }
            end)
            if not ok or type(v) ~= "table" or type(v[3]) ~= "number" then
                return nil
            end
            at[i] = v
        end
        -- Does the engine hand back the pose with the DLL's edit in it, or the one before? Told apart by
        -- the most stretched segment: its length now is either k times the reference length or about it.
        if self.edited == nil or self.votes < 10 then
            local s = plan.segs[plan.probe]
            local len = dist(at[s.child], at[s.parent]) / get_scale_z(r.comp)
            local guess = math.abs(len - s.k * s.l0) < math.abs(len - s.l0)
            if guess == self.edited then
                self.votes = self.votes + 1
            else
                self.edited, self.votes = guess, 0
            end
        end
        local lift = nil
        for _, chain in ipairs(plan.chains) do
            local sank = 0
            for _, id in ipairs(chain) do
                local s = plan.segs[id]
                local dz = at[s.child][3] - at[s.parent][3]
                -- (edited height - original height) of this segment
                local grow = self.edited and (dz - dz / s.k) or (dz * (s.k - 1))
                sank = sank - grow
            end
            if lift == nil or sank > lift then
                lift = sank -- the foot that sank most stands on the ground; the other may float a little
            end
        end
        return lift or 0
    end

    -- Every frame or so (AutoGroundIntervalMs): follow the live pose.
    function self.fast()
        if not self.enabled or not self.followPose or not self.plan then
            return
        end
        local live = live_lift()
        if live == nil then
            self.liveFails = self.liveFails + 1
            if self.liveFails == 30 and not self.warned.live then
                self.warned.live = true
                opts.say("auto ground: bone positions cannot be read; using the reference pose lift (not terrain aware)")
            end
            self.live = nil
            return
        end
        self.liveFails = 0
        if self.smoothing > 0 and self.live ~= nil then
            live = self.live + (live - self.live) * (1 - self.smoothing)
        end
        self.live = live
        self.lift = clamp(live)
        local n = apply(self.lift)
        self.note = string.format("following the pose (%s), %d mesh(es) moved", self.edited and "edited pose" or "game pose", n)
    end

    -- rigs: the meshes sent to the DLL this scan (see main.lua). Runs on the slow tick.
    function self.tick(rigs)
        if not self.enabled then
            if next(self.applied) then
                self.restore_all()
            end
            return
        end
        local slidersChanged = read_sliders()

        -- The player's meshes, and those of them that are not attached to another one of them
        -- (moving those moves the rest: a head attached to the body follows the body).
        local mine, byAddr, primary = {}, {}, nil
        for _, r in ipairs(rigs) do
            if r.actorKey == opts.playerKey and opts.still(r.comp, r.full) then
                mine[#mine + 1] = r
                byAddr[r.address] = true
                if r.primary then
                    primary = r
                end
            end
        end
        self.primary = primary
        if not primary then
            self.roots, self.plan = {}, nil
            self.note = "waiting for the player's mesh"
            return
        end
        local roots = {}
        for _, r in ipairs(mine) do
            local ok, parent = pcall(function()
                return r.comp:GetAttachParent()
            end)
            local parentAddr = ok and parent and opts.alive(parent) and opts.address_of(parent) or nil
            if not (parentAddr and byAddr[parentAddr]) then
                roots[r.address] = r
            end
        end
        self.roots = roots

        local key = primary.address .. "#" .. tostring(primary.hierarchy and primary.hierarchy.count)
        if key ~= self.skeletonKey then
            self.skeletonKey = key
            self.skeleton = parse_skeleton(primary.hierarchy)
            self.edited, self.votes = nil, 0
            slidersChanged = true
            if not self.skeleton and not self.warned.noref then
                self.warned.noref = true
                opts.say("auto ground: no reference pose for %s; feet cannot be measured", primary.label)
            end
        end
        if not self.skeleton then
            self.plan = nil
            self.note = "no reference pose"
            return
        end
        if slidersChanged then
            local ok, lift = pcall(compute_lift, self.skeleton, self.sliders)
            self.computed = ok and lift or 0
            local okp, plan = pcall(build_plan, self.skeleton, self.sliders)
            self.plan = okp and plan or nil
            if not ok or not okp then
                opts.say("auto ground: %s", tostring(not ok and lift or plan))
            end
            self.edited, self.votes = nil, 0
        end
        -- Following the live pose: fast() does the work. Otherwise (or until it works) the reference lift.
        if self.followPose and self.live ~= nil then
            return
        end
        self.lift = clamp(self.computed * get_scale_z(primary.comp))
        local n = apply(self.lift)
        self.note = self.lift == 0 and "nothing to correct" or string.format("reference pose lift, %d mesh(es) moved", n)
    end

    function self.status()
        return string.format("auto ground %s: lift %.1f cm (live %s, reference %.1f, extra %.1f, max %.1f), %s, sliders from %s",
            self.enabled and "on" or "off", self.lift, self.live and string.format("%.1f", self.live) or "-", self.computed,
            self.extra, self.max, self.note, opts.sessionFile)
    end

    function self.set_enabled(on)
        self.enabled = on
        if not on then
            self.restore_all()
            self.lift, self.live = 0, nil
        else
            self.sessionText = nil -- recompute
            self.skeletonKey = nil
        end
    end

    function self.set_extra(cm)
        self.extra = cm
    end

    return self
end

-- For testing outside the game.
M._compute_lift = compute_lift
M._parse_skeleton = parse_skeleton
M._build_plan = build_plan

return M
