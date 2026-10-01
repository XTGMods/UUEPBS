# WARNING:
<p><b>Use UUEPBS at your own discretion! This is solely made for single player games, I AM NOT responsible for any damage or bans caused to users attempting to install UUEPBS on multiplayer/competitive games or any games with online elements for that matter.</b></p>

# About:
Universal Unreal Engine Player Body Sliders (UUEPBS) is a near universal player character body slider mod for Unreal Engine based games, leveraging the powerful UE4/5SS tool. UUEPBS gives players the freedom to customise their character's body proportions, such as hips, spine, shoulders, arms, legs etc. With capabilities such as saving and loading user-defined UUEPBS presets, as well as being able to change NPC's body proportions and save their UUEPBS presets.


https://github.com/user-attachments/assets/4e4372bc-8a64-4255-b8ae-a33645ce187e
### V2.6.1 - Changelogs
* Added full support for player and NPC that works simultaneously
* Improved performance
* Changed scanning method to drop full world rescan for actors
* Renamed Body and Bones Tab to Simplified and Detailed Panel
* Added UI Customisability

## Tested and confirmed working games so far
* GON God of Nothing
* Wuchang Fallen Feathers
* Beast of Reincarnation
* Clair Obscur Expedition 33
* The Killing Antidote
* Stellar Blade
* VanySlash
* Mortal Shell 2
* Lies of P
* Dawn Break
* Lunar Eclipse
## Confirmed incompatible games
* Ninja Gaiden 2 Black - Hybrid propietary/UE game engine

## Installing
1. Install UE4SS for the game as usual, use experimental-latest releases.
2. Extract UUEPBS folder to `Game-Name\<Win64>\ue4ss\Mods`, where Game-Name is the name of the game located inside its root folder.

## Using it

* **F6** opens and closes the window while the game or the window has focus. `MenuKey` in `config.lua` changes the key.
  The window is a separate top-level window, so it works with borderless or windowed fullscreen. With *exclusive* fullscreen, switch the game to borderless.
* **F7** (`RefreshKey` in `config.lua`, `""` turns it off) does the same as the **Refresh** button, in game or in the window.
  It rescans the character and its meshes, relists the characters, and immediately retries a pose hook that was refused, instead of waiting out the retry delay.
* **Character** picker at the top: *Automatic* picks the game profile's character, or otherwise the pawn you control. The list also shows every `Character` loaded in the level.
  The list is only built while the window is open (and every few seconds while it stays open). **Refresh** rescans all actors within the current level.
* **Simplified Panel** (called the Body tab before 2.2): curated sliders for the head, neck, rib cage, waist, hips, shoulders, breasts, butt, thighs, calves, arms and so on.
  A slider only appears when the skeleton has matching bones. Hover a slider's name to see which bones it drives.
  Left/right pairs and twist/roll/jiggle helper bones move together. **XYZ** splits the size into Length (X, along the bone), Width (Y) and Depth (Z).
  Scale sliders run from 0.1× to 10× on a log scale. **Ctrl + mouse wheel** nudges a slider by 0.01, and **Ctrl + Shift + wheel** by 0.001.
  Ctrl+click any slider to type a value.
  Morph groups from the game profile (`MorphGroups`, see below) appear here as extra sliders.
* **Detailed Panel** (called the Bones tab before 2.2). When the meshes have morph targets it has two sub-tabs, **Bones** and **Morphs**.
  * **Bones:** every bone of the primary mesh, with search, "edited only" and mirror left/right.
    * **Scale**, **Rotate** (roll/pitch/yaw in degrees, pivoting on the joint) and **Move** (cm), all in the bone's own axes (X runs along the bone).
    * **Children** mode:
      * *Scale children too* is the engine's behaviour.
      * *Keep children size* makes children follow the new shape without growing.
      * *This bone only* changes nothing below the bone.
    * **Mirror to other side** gives the opposite bone the true mirror image. The per-bone axis conversion is measured from the skeleton's reference pose, or from the first animated frame if the game doesn't expose the reference pose.
      The Status tab shows which one was used.
  * **Morphs:** every morph target on the tracked meshes, with search and "edited only". Sliders run 0–1; Ctrl+click types any value from −2 to 2.
    * A morph you move **replaces** the game's own value, even at 0. **Reset** gives it back, and the value the game had before comes back.
    * *Mirror left/right morphs* moves `…_L`/`…_R` twins together.
    * A **!** next to a name means the game keeps setting that morph itself (an animation or its own code). UUEPBS puts your value back each time it notices, but it may flicker or not stick.
* **Presets** tab: save, load (or double-click), delete and open the folder. Presets are JSON files in **`<Win64>\UUEPBS Presets`** and hold bones and morphs.
  The current sliders auto-save to `_last_session.json` and come back next time.
  Bone and morph names aren't case-sensitive, and names a character doesn't have are skipped, so one preset can be shared between characters on the same rig.
* **Status** tab: hook details, the Lua link, mirroring, morph counts, the **window skin** picker, tracked meshes, **Rescan**, and **Write diagnostics**, which dumps what the hook finder sees into the log for bug reports.

UE4SS console: `uuepbs` (toggle window), `uuepbs status`, `uuepbs rescan`, `uuepbs list`, `uuepbs load <name>`, `uuepbs save <name>`, `uuepbs on|off`, `uuepbs reset`, `uuepbs diag`,
`uuepbs skin` (list skins), `uuepbs skin <name>` (switch; `Default` = built-in look), `uuepbs morph <name> <weight>` (set a morph; without a weight it goes back to the game).
`ubs …` is the short form.

### Window skins

Skins live in **`<Win64>\UUEPBS Skins\<name>\`**: a `skin.json` plus PNG or JPG images. The folder is created the first time the window opens, with a `README.txt` that lists every key and the example skin **Example - Midnight Rose**.
Pick a skin in **Status > Window skin** or with `ubs skin <name>`; the choice is remembered. `Skin` in `config.lua` sets the one used until you pick another.

* **What a skin can change:** every colour (accent, text, window, panels, frames, buttons, tabs, headers, scrollbars; any Dear ImGui colour by name), sizes (rounding, padding, spacing, borders, font scale), a TTF/OTF font, and images for the
  window background, the title strip, a logo in place of the title, panels, buttons (normal/hover/pressed), slider track/fill/grab, checkboxes and tabs, plus icons for the tabs, buttons and each Simplified Panel slider (`"group.Thighs"`).
* **What it can't change:** the layout. Skins are looks only.
* **Images** can stretch, cover, contain, centre, tile, or nine-slice (corners keep their size, so one small frame image fits any button). Anything a skin leaves out keeps the built-in look, and an image that can't be loaded falls back to the normal widget; the Status tab lists the problem.
* **Editing:** the window reloads the active skin as soon as you save `skin.json` or one of its images, so you can tweak it with the game running. Start by copying the example folder.
* Limits: PNG or JPG, at most 4096×4096 and 32 MB per file (bigger than 2048 is scaled down); files must be inside the skin's folder. Images are decoded inside the game process, so only use skins from people you trust.
* With the CPU renderer, large images are sampled once and reused, so a skinned window costs about the same per frame as the built-in look; only the first frame after a skin loads or a tab opens is slower.

### Performance settings (`config.lua`)
* `WindowFps` (default 30): frame cap for the slider window while you drag sliders. 15–60.
* `PollIntervalMs` (default 600): how often the script exchanges messages with the DLL (console commands, picker, rescan requests).
* `MorphPollMs` (default 100): while the window is open, morph slider changes are applied this often, so dragging feels live. Nothing runs at this rate while the window is closed. `0` = only every `PollIntervalMs`.
* `WatchIntervalMs` (default 5000): how often the script checks that the character and its meshes are still valid. Only cheap validity checks run on this timer.
  A full scan (which does walk the object list once) only happens when something changed: a new pawn, a destroyed mesh, a level change, F7 or **Refresh**.
* After the pose hook first goes in, the mod rescans the character once by itself a few seconds later, so games that set the character up late no longer need F7.

### Game profiles (set up automatically)

The first time the mod runs in a game, it writes a small profile for that game to **`Scripts\GameProfiles\<project>.json`**, then reads it on every start.
`<project>` is the Unreal project folder, the one that contains `Binaries`. For GON it is `Roku3`, and for Wuchang it is that game's project folder.
Install the mod in as many games as you like; each one gets its own file.

```json
{
  "Project": "Roku3",
  "Game": "GON - God Of Nothing",
  "Target": "BP_Rokuv3_C",
  "TargetClassPath": "/Game/ROKUv3/BP_Rokuv3.BP_Rokuv3_C",
  "PrimaryComponent": "CharacterMesh0",
  "IncludeComponents": [],
  "ExcludeComponents": [],
  "SameSkeletonOnly": true
}
```
* A new game's profile starts from `Default` in `config.lua`, which means: edit the pawn you control, list its `Mesh`, and include meshes on the same skeleton.
  GON ships with the profile above.
* **`Target`**: an actor class (the UE4SS object dump or Live View shows it), or `""` for the pawn you control.
* **`PrimaryComponent`**: the mesh whose bones are listed. **`Include/ExcludeComponents`**: mesh names to force in or out.
* Changes take effect the next time the game starts.
  * If the file has a mistake, the console shows the line and column, the mod uses the defaults, and your file isn't overwritten.
  * Delete a profile to have it regenerated.
* Profiles from an older `config.lua` (`Profiles = { ... }`), or from an XTGBodySlider install next to this one, are carried over automatically the first time.

**Morph targets in the profile** (both optional, applied while the game runs):
```json
"MorphGroups": {
  "Breasts": ["BreastSize*"],
  "Belly":   { "Morphs": ["Belly", "Tummy_Round"], "Section": "Body shape", "Hint": "Belly size" }
},
"ExcludeMorphs": ["*_corrective*", "Viseme_*"]
```
* **`MorphGroups`** adds sliders to the Simplified Panel. Each drives every morph whose name matches (names from Detailed Panel > Morphs; `*` = anything, `#` = digits, not case-sensitive).
* **`ExcludeMorphs`** hides morphs from the window, e.g. corrective shapes and face visemes. Hidden morphs are never touched.

### Simplified Panel mapping (bone dictionary)

The Simplified Panel sliders (Thighs, Waist, Breasts, Biceps…) find their bones through **`Scripts\BoneDictionary.json`**.
You can edit it, and the window picks up changes within a couple of seconds. A copy is built into the DLL, so a missing or broken file never breaks anything.

* **Supported out of the box:** UE4/UE5 mannequin, MetaHuman (including `upperarm_bicep`, `latissimus` and `clavicle_pec` helpers), Mixamo, Daz Genesis, Blender Rigify and Auto-Rig Pro (`thigh_stretch.l`, `.x` centre bones), Character Creator, VRoid, 3ds Max Biped and **Advanced Skeleton** (Wuchang: `Hip_L` = thigh, `Shoulder_L` = upper arm, `Root_M`, `SDtui`/`SDabi` helpers).
  Category prefixes like BoR's `MOT_`/`SUP_`/`PRG_` are stripped, and some pinyin and romaji words are included (`datui`, `dabi`, `xiaotui`, `pigu`, `momo`, `shiri`…).
* **Muscles section:** biceps, triceps, deltoids, pecs, lats, traps, serratus, abs, obliques, quads, hamstrings and calf muscles. It appears only when the rig has such helper bones.
* **How matching works:** bone names are cleaned (prefixes, the left/right marker and `_M`-style suffixes removed), then compared with each group's patterns (`*` = anything, `#` = digits, `=Name` = the exact original name).
  A group's first matching pattern wins, and both sides are taken together. Twist, roll, stretch and "thicker" helpers of limbs come along automatically.
  Rig **Styles** (such as Advanced Skeleton) are detected from the bone names and re-point the groups whose names mean something different in that rig.
* The **Detailed Panel** shows each bone's group next to its name, e.g. `Hip_L [Thighs]`, and the **Status** tab shows the detected rig style.

**Per-game remapping, no dictionary editing needed.** Add `BodyGroups` to the game's `Scripts\GameProfiles\<project>.json`, using real bone names from the Detailed Panel:
```json
"BodyGroups": {
  "Thighs": ["Hip_L", "Hip_R", "SDtui*"],
  "Waist":  ["Spine1_M"],
  "Eyes":   [],
  "Tail":   { "Bones": ["tail_01"], "Section": "Extras", "Children": "scale_too" }
}
```
* **Overriding a slider:** a listed group replaces the automatic match for that slider.
* **Hiding a slider:** `[]` hides it.
* **Adding a slider:** a new name adds a slider. It can take `Section`, `Hint` and `Children` (`scale_too`, `keep_size` or `this_bone_only`).
* **When it applies:** changes apply while the game runs. If the JSON has a mistake, the Status tab says so and the automatic mapping is used.

### Pinning the pose hook for a game

The hook finder normally works everything out by itself. When it succeeds, the log (`native\UUEPBS.log`) prints a ready-made line like:
```
"Hook": { "Slot": 330, "Buffers": "0x610", "ReadIndex": "0x658" }
```
Paste it into the game's `Scripts\GameProfiles\<project>.json` to pin those values. The search is then skipped, and only the pinned values are checked against the live character.
Game profiles can be shared, so this works as a per-game hook database, and a profile shared by someone else fixes a game whose automatic search fails.
If a game update moves things, the check fails, the log says so, and you delete the line to search again.

### Preset format
```json
{
  "format": "UUEPBS preset",
  "version": 3,
  "bones": {
    "boob_l":  { "length": 1.25, "width": 1.25, "depth": 1.25, "children": "scale_too" },
    "thigh_l": { "length": 1,    "width": 1.12, "depth": 1.12, "children": "keep_size" },
    "head":    { "length": 1, "width": 1, "depth": 1, "rotate": [0, 0, 10], "move": [0, 0, 1.5], "children": "scale_too" }
  },
  "morphs": { "BreastSize": 0.8, "Belly": 0 }
}
```
* `length`/`width`/`depth` are X/Y/Z scale. `rotate` is in degrees and `move` in cm, both optional.
* `morphs` (optional) holds morph weights. A listed morph replaces the game's value, even at 0. Loading a preset without `morphs` gives every morph back to the game. Presets without morphs are still written as version 2, so older builds read them unchanged.
* `children` is `scale_too`, `keep_size` or `this_bone_only`.
* `"scale": 1.2` and `"scale": [1, 1.2, 1.2]` shortcuts work. Older `.rbs` files still load.
### Limits
* The change is visual only. Collision capsules, physics bodies and hit boxes keep their original size.
* <b>UUEPBS presets are generally not interchangeable between games, that is to say a preset made for Stellar Blade will not work with a preset made for Wuchang, and that won't work with a preset made for Expedition 33. This is simply because every UE game developer uses different practices and character rigs for the player. And thus there is no possibility to generalise the naming scheme of body models.</b>
* Generally, it should work for most UE4.2x–5.x game that runs UE4SS Lua mods and animates characters with skeletal meshes, however, there maybe edge cases where if the game uses its own custom UE build, it may not work until a new config for it is made.
  The following aren't supported, so do not even try it:
  * games with anti-cheat (don't try online games);
  * heavily modified engines;
  * characters deformed by morph targets or vertex animation instead of bones.
## Tips & Tricks
* If the simplified Body tab only manages to show very few bones, then you need to use the detailed Bones tab.
* If UUEPBS is not having any effects on your character upon initial loading of the game, press F7 to rescan and refresh the player character.
* Share your UUEPBS with fellow gamers!
## Credits
* Claude
* Uses [Dear ImGui](https://github.com/ocornut/imgui)
* UUEPBS links third-party code under this license.
(It runs alongside UE4SS but contains and links no UE4SS code.)

=== Dear ImGui (https://github.com/ocornut/imgui) ===
The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
