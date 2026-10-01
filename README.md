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
  It rescans the character and its meshes, relists the characters, and immediately retries to pose the character.
* **Character** picker at the top: *Automatic* picks the game profile's character, or otherwise the pawn you control. The list also shows every `Character` loaded in the level.
  The list is only built while the window is open (and every few seconds while it stays open). **Refresh** rescans.
* **Body** tab: curated sliders for the head, neck, rib cage, waist, hips, shoulders, breasts, butt, thighs, calves, arms and so on.
  A slider only appears when the skeleton has matching bones. Hover a slider's name to see which bones it drives.
  Left/right pairs and twist/roll/jiggle helper bones move together. **XYZ** splits the size into Length (X, along the bone), Width (Y) and Depth (Z).
  Scale sliders run from 0.1× to 10× on a log scale. **Ctrl + mouse wheel** nudges a slider by 0.01, and **Ctrl + Shift + wheel** by 0.001.
  Ctrl+click any slider to type a value.
* **Bones** tab: every bone of the primary mesh, with search, "edited only" and mirror left/right.
  * **Scale**, **Rotate** (roll/pitch/yaw in degrees, pivoting on the joint) and **Move** (cm), all in the bone's own axes (X runs along the bone).
  * **Children** mode:
    * *Scale children too* is the engine's behaviour.
    * *Keep children size* makes children follow the new shape without growing.
    * *This bone only* changes nothing below the bone.
  * **Mirror to other side** gives the opposite bone the true mirror image. The per-bone axis conversion is measured from the skeleton's reference pose, or from the first animated frame if the game doesn't expose the reference pose.
* **Presets** tab: save, load (or double-click), delete and open the folder. Presets are JSON files in **`<Win64>\UUEPBS Presets`**.
  The current sliders auto-save to `_last_session.json` and come back next time.
  Bone names aren't case-sensitive, and bones a skeleton doesn't have are skipped, so one preset can be shared between characters on the same rig.
* **Status** tab: hook details, the Lua link, mirroring, tracked meshes, **Rescan**, and **Write diagnostics**, which dumps what the hook finder sees into the log for bug reports.

UE4SS console: `uuepbs` (toggle window), `uuepbs status`, `uuepbs rescan`, `uuepbs list`, `uuepbs load <name>`, `uuepbs save <name>`, `uuepbs on|off`, `uuepbs reset`, `uuepbs diag`.
`ubs …` is the short form.
### Preset format
```json
{
  "format": "UUEPBS preset",
  "version": 2,
  "bones": {
    "boob_l":  { "length": 1.25, "width": 1.25, "depth": 1.25, "children": "scale_too" },
    "thigh_l": { "length": 1,    "width": 1.12, "depth": 1.12, "children": "keep_size" },
    "head":    { "length": 1, "width": 1, "depth": 1, "rotate": [0, 0, 10], "move": [0, 0, 1.5], "children": "scale_too" }
  }
}
```
* You should generally name your UUEPBS with the following convention `Game Name - Preset Name - Author(optional)`
  * Example: `GON God of Nothing - XXTB Roku - XTGMods.JSON`
* `length`/`width`/`depth` are X/Y/Z scale. `rotate` is in degrees and `move` in cm, both optional.
* `children` is `scale_too`, `keep_size` or `this_bone_only`.
* `"scale": 1.2` and `"scale": [1, 1.2, 1.2]` shortcuts work.
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
