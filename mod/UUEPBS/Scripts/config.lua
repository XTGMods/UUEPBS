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

    KeepWindowOnTop = true,
    WindowScale = 1.0, -- multiplies the Windows DPI scale
    FontSize = 19,     -- text size in the window (pixels before DPI scaling)

    -- Classes listed in the window's Character picker. The list is only built while the
    -- window is open (searching for actors is expensive).
    CandidateClasses = { "Character" },
    MaxCandidates = 40,

    -- How often the script talks to the DLL (console commands, picker, rescans).
    PollIntervalMs = 400,
    -- How often it checks that the character and its meshes are still the same.
    -- Only cheap validity checks run here; a full scan happens when something changed.
    WatchIntervalMs = 3000,

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
