-- UUEPBS settings (Universal Unreal Engine Player Body Sliders)
return {
    -- Opens / closes the slider window while the game (or the window) has focus.
    -- F1-F24, Insert, Home, End, PageUp, PageDown, Delete, Pause, ScrollLock, Numpad0-9, a letter or digit.
    MenuKey = "F6",

    -- Rescans the character and retries the pose hook (same as the window's Refresh button).
    -- Handy in games that finish setting the character up late. "" turns it off.
    RefreshKey = "F7",

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

    -- How often the script talks to the DLL (console commands, picker, rescans).
    PollIntervalMs = 600,
    -- How often it checks that the characters and their meshes are still the same.
    -- Only cheap validity checks run here; a character is read again only when it changed.
    WatchIntervalMs = 5000,
    -- While the window is open, morph target sliders are applied this often (ms) so dragging
    -- feels live. 0 = only every PollIntervalMs. Nothing runs at this rate while it is closed.
    MorphPollMs = 100,

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
