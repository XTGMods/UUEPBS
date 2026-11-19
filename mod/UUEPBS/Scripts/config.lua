-- UUEPBS settings (Universal Unreal Engine Player Body Sliders)
return {
    -- Opens / closes the slider window while the game (or the window) has focus.
    -- F1-F24, Insert, Home, End, PageUp, PageDown, Delete, Pause, ScrollLock, Numpad0-9, a letter or digit.
    MenuKey = "F6",

    -- Rescans the character and retries the pose hook (same as the window's Refresh button).
    -- Handy in games that finish setting the character up late. "" turns it off.
    RefreshKey = "F7",

    -- Load the next / previous saved preset (alphabetical, wraps around) onto the character picked in
    -- the window (the player unless you picked someone else), without opening the window. Each character
    -- keeps its own place in the list. Same key names as MenuKey; "" turns a key off.
    -- Also available as console commands: ubs next / ubs prev.
    NextPresetKey = "",
    PrevPresetKey = "",

    -- Hold to compare: while this key is down the game shows the character's original body,
    -- and your sliders come back when you let go (nothing is changed or saved). The window's
    -- Before / After switch does the same with a click. "" turns the key off.
    CompareKey = "F8",

    -- Presets go to <Win64>\<PresetFolderName> (the folder that holds the game exe).
    -- Set PresetFolder to an absolute path to use somewhere else instead.
    -- Presets from an older "RBS Presets" folder next to it are copied over the first time.
    PresetFolderName = "UUEPBS Presets",
    PresetFolder = "",

    -- Re-apply the sliders you had last time (stored as "_last_session.json").
    RestoreLastSession = true,
    -- Or always start from a named preset instead (file name without .json). Empty = off.
    StartupPreset = "",

    -- How the slider window is drawn:
    --   "cpu"  (default) software rendering + plain Windows drawing. Uses no Direct3D/DXGI at all,
    --          so ReShade, DLSS/FSR frame generation, Steam/RivaTuner overlays etc. never touch it.
    --   "gpu"  Direct3D 11. Slightly smoother on very high-DPI screens, but those tools
    --          may hook this window too. Falls back to "cpu" if Direct3D cannot start.
    -- Takes effect the next time the game starts.
    Renderer = "cpu",
    -- Frame rate cap for the window while you drag sliders (it idles at ~4 fps otherwise and
    -- skips frames that would look the same). Lower = less CPU taken from the game. 15-60.
    WindowFps = 30,

    -- Window skin: the name of a folder in <Win64>\<SkinFolderName> ("" = built-in look).
    -- A skin picked in the window (Status tab) or with "ubs skin <name>" wins over this.
    -- See README.txt in that folder; an example skin is written there the first time.
    Skin = "",
    SkinFolderName = "UUEPBS Skins",
    SkinFolder = "", -- absolute path to keep skins somewhere else

    KeepWindowOnTop = true,
    WindowScale = 1.0, -- multiplies the Windows DPI scale
    FontSize = 22,     -- text size in the window (pixels before DPI scaling)

    -- Classes listed in the window's Character picker, nearest first. The world is searched when the
    -- window opens and on Refresh; while it stays open, newly spawned characters are added as they appear.
    CandidateClasses = { "Character" },
    MaxCandidates = 40,
    -- Readable names in the Character list and mesh labels ("Eve" instead of BP_Eve_C_2147482611 or
    -- 00_EVE_01_Body): the game's own name for the character when it has one, else the game profile's
    -- "Names" table, else the engine name cleaned up. Saved sliders and presets don't depend on it.
    -- false = show the engine's object names.
    FriendlyNames = true,
    -- Characters you picked and edited keep their sliders while you edit someone else
    -- (characters you never edited are left alone). This caps how many are kept at once.
    MaxEditedCharacters = 16,
    -- NPCs you edit are remembered by name (or class + face mesh) and get their last sliders back
    -- by themselves whenever they are loaded near the player again (after a reload, in a new area).
    -- Files: <presets folder>\_characters\<name>.json. Release / Reset all on an NPC forgets it.
    RememberCharacters = true,
    -- Characters spawned at run time (names ending in a long number, e.g. Gatekeeper_church2_2147465868)
    -- are recognised through the placed record they point at, when the game has one (The Blood of
    -- Dawnwalker: the "Stub" component -> Gatekeeper_church2_277). The search runs once per class.
    -- false = recognise them by name only (all spawned "Gatekeeper_church2" guards would share one).
    IdentityLinkSearch = true,
    -- NPCs are only sculpted while they are this close to the player, in cm (5000 = 50 m). 0 = any distance.
    CharacterRange = 5000,
    -- Character Switch Watcher: for games where you switch between party members while the game keeps one
    -- player character and swaps its looks (Clair Obscur: Expedition 33 and many other JRPGs). Without it,
    -- the player's sliders stay on whoever you switch to. With it, each party member keeps their own
    -- sliders: the mod recognises who you're playing by their face mesh (else head, else main mesh) and
    -- swaps the sliders when that changes. Saved in <presets folder>\_characters\player@<face mesh>.json.
    -- The follower version of a party member (Expedition 33: BP_Pawn_AICompanion_Lune_C walking behind you)
    -- has the same face mesh and shares those sliders: edit Lune while playing her and her follower matches.
    -- The first character it sees keeps the sliders you already had. Leave off for other games.
    CharacterSwitchWatcher = false,
    -- At most this many loaded characters get the sliders of one remembered NPC at a time
    -- (two NPCs can only share a name or face by accident).
    MaxSameIdentity = 2,
    -- Total skeletal meshes sculpted at once, all characters together (each character has several:
    -- body, hands, outfit pieces...). Characters that would not fit whole are left alone, farthest first.
    -- 8-64; the DLL never tracks more than 64.
    MaxTrackedMeshes = 64,
    -- Scene copies: cutscenes, inventory / equipment screens and other menus often show the character
    -- through an actor of their own that uses the same meshes. Those copies get the same sliders, so the
    -- body doesn't snap back to its original sizes there. A copy has to use the character's main (body)
    -- mesh and share at least two of its sculpted meshes (one when the character only has one), and must
    -- not be an AI-controlled NPC sharing a single mesh. (Shared eyes, lashes and hair alone don't count:
    -- in many games every NPC wears those.) "ubs copies" searches for copies now and shows what was found.
    -- false = only the characters themselves are sculpted.
    SceneCopies = true,
    -- "outfit" (the rule above) or "mesh": one shared mesh is enough, for games whose cutscene / menu
    -- copies only reuse the body. Careful: NPCs that share the body mesh with the character then match too.
    SceneCopyMatch = "outfit",
    -- true = also look at every new skeletal mesh component and scene capture the moment the engine makes it
    -- (finds menu / cutscene copies a little sooner). Off by default: UE4SS runs that Lua on whatever thread
    -- creates the object, and games that stream levels on the async loading thread (Stellar Blade) crash.
    -- Like any setting here it can be set per game in GameProfiles\<game>.json "Features".
    SceneCopyWatchNew = false,

    -- Character Scale > Camera: moves the third-person camera of the character you play (its spring arm /
    -- camera boom, else its camera component, else its eye height) by the Camera sliders, e.g. up with
    -- Height. "ubs camera" shows what was found. false = the game's camera is never touched.
    CameraControl = true,
    -- How often (ms) the camera offset is checked and written again while a character has Camera sliders.
    -- Nothing runs at this rate when no character has them.
    CameraPollMs = 50,

    -- Collision follows Height: a character's collision capsule (half height and radius) grows and
    -- shrinks with its Height slider, so a tall character no longer walks through low ceilings and walls
    -- and a small one no longer floats. The feet stay on the floor (the actor moves up by what the capsule
    -- grew and the mesh down by as much), crouching keeps working, and MaxStepHeight grows along.
    -- Every character with a capsule (ACharacter or a capsule root). "ubs collision" shows the sizes.
    -- The switch in the window (Character Scale > Overall) and "ubs collision on|off" are remembered in
    -- <presets folder>\_collision.txt and win over this. false = off until switched on; "never" = the
    -- capsule is never touched and the switch is hidden.
    CollisionFollow = true,
    CollisionRadius = true,      -- false = only the height follows (the capsule keeps the game's width)
    CollisionStepHeight = true,  -- false = keep the game's step height
    CollisionPollMs = 50,        -- how often the capsule is checked (crouching changes it); idle when nothing to do

    -- How often the script talks to the DLL (console commands, picker, rescans).
    PollIntervalMs = 600,
    -- How often it checks that the characters and their meshes are still the same.
    -- Only cheap validity checks run here; a character is read again only when it changed.
    WatchIntervalMs = 5000,
    -- While the window is open, morph target sliders are applied this often (ms) so dragging
    -- feels live. 0 = only every PollIntervalMs. Nothing runs at this rate while it is closed.
    MorphPollMs = 100,

    -- Character portraits for the window's banner and "Change character" grid. While the window is open,
    -- characters without a picture get one taken in the game (the game's own portrait when the game
    -- profile names it in "PortraitTexture", else a head shot), saved in "UUEPBS Presets\_portraits".
    -- Your own PNG/JPG there, named after the character, always wins. false = only your own images.
    AutoPortraits = true,

    -- Saving a preset takes a whole-body picture of the character for the preset browser, saved in
    -- "UUEPBS Presets\_library\<preset>.auto.png". Your own PNG/JPG there named after the preset wins.
    -- The game profile's "ThumbnailDistance" (cm) / "ThumbnailFov" tune the framing. false = no pictures.
    PresetThumbnails = true,

    -- Looks tab (skin tone, sweat, hair colour): recolours the outfit's own material slots through
    -- dynamic material instances. false = the tab's sliders do nothing in the game.
    Looks = true,

    -- Mesh deformers (Deformer Graphs, UE 5.2+) that some games draw characters with. A dual quaternion
    -- skinning deformer (Afterparty) cannot show scaled bones: Head size, Hand size, Height... tear the
    -- mesh apart. "auto" switches a character's deformers off while its sliders scale bones and puts
    -- them back afterwards (whatever else they did, e.g. jiggle, is missing meanwhile). "keep" never
    -- touches them.
    MeshDeformers = "auto",

    -- Jiggle physics (breasts, butt, genitals, tails, hair) runs in the game's animation BEFORE the
    -- sliders, so it bounces and collides at the original sizes. For characters whose sliders scale bones:
    --   "follow" scales the physics shapes with the bones (Physics Asset bodies and joints used by Rigid
    --            Body nodes, AnimDynamics, Kawaii Physics), relative to the whole body
    --   "off"    switches the physics nodes off (Alpha 0) and pauses cloth while the character is edited
    --   "keep"   leaves the game's physics alone
    -- The window's choice (Character Scale > Overall) is remembered in _physics.txt and wins over this.
    -- Console: "ubs physics [follow|off|keep]".
    PhysicsMode = "follow",

    -- Auto Ground: longer legs sink the feet into the floor (the pelvis stays where the animation put
    -- it), shorter legs make them float. With this on, the player's mesh is moved up or down by what
    -- the sliders moved the lowest foot bones, worked out from the reference pose and the sliders in
    -- _last_session.json. Visual only: the collision capsule follows Height alone (CollisionFollow).
    -- Console: "ubs ground" (status), "ubs ground on|off", "ubs ground <cm>" (extra, this session).
    -- The switch in the window (Character Scale > Overall) and "ubs ground on|off" are remembered in
    -- <presets folder>\_auto_ground.txt and win over this; delete that file to go back to this setting.
    AutoGround = true,
    -- "dll": done inside the pose by the DLL every frame - the body is lifted by what the foot that sank
    --        most sank, and every other leg is bent with IK so its foot stays where the game's foot IK put
    --        it (stairs, slopes). Works for every edited character, not just the player. Needs the DLL
    --        built with Auto Ground.
    -- "lua": the older way - the player's mesh component is moved from here (one offset for the whole
    --        body, so on a step the higher foot may float). The settings below marked (lua) are for it.
    AutoGroundMode = "dll",
    AutoGroundLegIK = true,  -- (dll) bend the other legs back onto the ground; false = lift only
    AutoGroundExtraCm = 0, -- added to the computed lift (+ up, - down) to fine-tune per game
    AutoGroundMaxCm = 40,  -- the lift never goes beyond this, either way
    -- (lua) Follow the live pose (stairs, slopes, bent knees): the lift is worked out from where the bones
    -- are on each frame, not from the standing reference pose. One mesh offset cannot fix both feet,
    -- so on a step the higher foot may float slightly. false = the fixed reference pose lift only.
    AutoGroundFollowPose = true,
    AutoGroundIntervalMs = 16,  -- how often the live lift is updated (16 = about every frame at 60 fps)
    AutoGroundSmoothing = 0,    -- 0 = none; 0.5 = the lift moves half way to the new value each update (less jitter, a little lag)

    -- Hand Contact: the game's animations put the hands where the game's body is. After body edits a
    -- hand that rests on the body (a self-touch, a hand on the hip or chest, arms at the sides) floats or
    -- sinks in. With this on, in every animation, a hand, elbow or foot that is close to the body in the
    -- game's own pose is carried along with the edited body part it rests on (moved, turned and scaled
    -- with it), the arm or leg is bent with IK to reach it, and the fingers open or close around the new
    -- shape. Hands and feet away from the body are not touched. Done by the DLL inside the pose, for every edited character.
    -- Console: "ubs hands" (what each character plays and where each hand rests; also in UUEPBS.log).
    -- Easier: the window's Character Scale > Animation section has the same settings with sliders, and what you
    -- set there (remembered in <presets folder>\_hand_contact.txt) wins over these until you press "Use config.lua".
    HandContact = true,
    HandContactNear = 0.25,  -- fully corrected within this distance of a body bone (x the arm's length)
    HandContactFar = 0.45,   -- not corrected beyond this distance (x the arm's length); in between it fades
    HandContactStrength = 1, -- 0..1, how much of the correction is used
    -- When the edited body puts the spot out of the arm's reach (longer torso, bigger body part):
    HandContactShoulderTurn = 30, -- the shoulder (collar bone) may turn this many degrees toward it
    HandContactArmStretch = 1.2,  -- then the arm may grow up to this factor to reach (1 = never)
    HandContactFingers = true,    -- fingers follow the touched part's new shape (open around a thicker part)
    HandContactFeet = true,       -- feet and knees resting on the body (crossed legs, a foot on a knee) follow it too
    -- Only while the character plays an animation (montage) whose name contains one of these words,
    -- comma separated, e.g. "Touch, Masturbat". "" = whenever a hand is close to the body.
    -- "ubs hands" shows the names of the animations playing.
    HandContactAnims = "",

    Verbose = false,

    -- Starting values for a new game profile. The first time the mod runs in a game it writes
    -- Scripts\GameProfiles\<project folder>.json from these (the project folder is the one that
    -- contains Binaries, e.g. "Roku3" for GON). Edit that JSON to set up the game; this block is
    -- only used again for games that do not have a profile file yet.
    Default = {
        -- Actor class to edit (e.g. "BP_Hero_C"). Empty = whatever pawn the player controls.
        Target = "",
        -- Optional full class path of Target (lets the script react the moment one spawns).
        TargetClassPath = "",
        -- Component whose bones are listed. Empty = the Character's "Mesh" (CharacterMesh0),
        -- or the skeletal mesh with the most bones.
        PrimaryComponent = "",
        -- Other skeletal meshes on the same actor are edited too when they use the same skeleton
        -- as the primary one (clothing, modular body parts). Force components in or out by name:
        IncludeComponents = {},
        ExcludeComponents = {},
        SameSkeletonOnly = true,
    },
}
