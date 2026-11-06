# UUEPBS – Auto Ground & fixes

Changes on top of **UUEPBS 2.7.3** (XTGMods/UUEPBS, commit `7c54929`, MIT).
Based on v2.7.3 (commit `7c54929`). Tested in game on Phantom Blade Zero (UE 5.7).

- [English](#english)
- [한국어](#한국어)

---

## English

### Summary

| Type | Change |
|---|---|
| New feature | **Auto Ground**: lengthening or shortening the legs no longer sinks the feet into the floor or makes them float. |
| New feature | **Per-leg IK**: on stairs and slopes every foot stays where the game's foot IK put it. |
| New feature | Works for every edited character, including meshes that copy the body's pose (head, outfit). |
| Bug fix | Meshes that copy the main mesh's pose (Copy Pose / Leader Pose) got every slider **twice** (e.g. a shorter torso pulled the head mesh's neck away from the body). |
| Bug fix | After closing the slider window, **F6 did nothing** until you clicked another window and back into the game. |

### New: Auto Ground

**The problem.** The DLL edits the final pose, after the game's animation and foot IK. Longer calves or thighs push the feet below the floor, and shorter ones leave them floating, because the pelvis stays where the animation put it.

**How it works** (DLL, every frame, right after the sliders are applied – `RigSculptor::apply_ground`):

1. For each leg (thigh → calf → foot) it measures how far the edit moved the foot down: the game's pose against the edited pose, along component +Z.
2. It lifts the whole pose by the largest of those amounts, so the foot that sank most is back on the ground.
3. Every other foot now floats a little. A **two-bone IK** (`bend_leg`) bends that leg down to where the game had the foot. It keeps the knee's bend direction and the foot's rotation, and the bones hanging from each link follow it rigidly.
4. Only the character's **main mesh** (`CharacterMesh0`) measures the lift. The character's other meshes are handled as described below.

**The character's other meshes** (`decide_copy`). Every frame the main mesh publishes, for each bone, its position and scale before and after the edit. Each other mesh maps its bones onto the main mesh's bones by name and compares:

- **Matches the main mesh's state after the edit** → its pose is a copy (Copy Pose / Leader Pose). The bones it shares with the main mesh are left exactly as they are: no second edit and no second lift. Bones only that mesh has (facial bones, for example) still get their sliders.
- **Matches the state before the edit** → the mesh is animated on its own. It gets the edits and is grounded like the main mesh. A mesh without legs takes the main mesh's lift.
- **Matches neither** → it is left alone.

The decision only changes after three frames in a row agree, so it does not flicker.

**When it is off**, the result is bit-for-bit identical to the original DLL. A test checks this.

**Legs are found by bone name.** A leg is a foot or ankle bone whose parent is a calf, knee, lower-leg or shin bone, whose parent is a thigh, upper-leg or hip bone. IK, twist, roll, control and corrective bones are skipped. At most four legs are found.

### Lua fallback (`AutoGroundMode = "lua"`)

`Scripts/uuepbs_ground.lua` (new file) works without the new DLL. It reads the player's sliders from `_last_session.json`, measures the feet from the live bone positions (`GetSocketLocation`), and moves the player's mesh component (`RelativeLocation.Z`). It uses one offset for the whole body, so on a step the higher foot may float slightly. Use it only if the DLL cannot be rebuilt.

### Settings (`Scripts/config.lua`)

| Setting | Default | Meaning |
|---|---|---|
| `AutoGround` | `true` | Turns the feature on or off. |
| `AutoGroundMode` | `"dll"` | `"dll"`: lift and IK inside the pose (needs the new DLL). `"lua"`: the fallback described above. |
| `AutoGroundLegIK` | `true` | (dll) Bend the other legs back onto the ground. `false` = lift only. |
| `AutoGroundExtraCm` | `0` | Added to the lift (+ up, − down) for fine-tuning. |
| `AutoGroundMaxCm` | `40` | The lift never goes beyond this, either way. |
| `AutoGroundFollowPose` | `true` | (lua) Follow the live pose instead of the standing reference pose. |
| `AutoGroundIntervalMs` | `16` | (lua) How often the live lift is updated. |
| `AutoGroundSmoothing` | `0` | (lua) 0–0.95. Smooths the lift (less jitter, a little lag). |

The settings reach the DLL through new `setup` keys: `ground`, `ground_ik`, `ground_extra`, `ground_max`. An older `main.lua` sends none of them, so the feature stays off.

### Console

- `ubs ground` – show the Auto Ground settings.
- `ubs ground on` / `ubs ground off` – switch it on or off for this session.
- `ubs ground <cm>` – set the extra lift for this session.
- `ubs status` – now also prints the player's lift, the number of legs found per mesh, and a per-mesh report: main or other mesh, the copy decision, and the heights of the hip, neck and head bones before and after the edit.

### Bug fixes

1. **Sliders applied twice on meshes that copy the main mesh's pose.** The head mesh in Phantom Blade Zero copies the body's *final* pose, and UUEPBS then edited it again. Torso, neck and head sliders were therefore doubled on it (e.g. `spine_02` 0.807 became about 0.65, and `Head` 0.903 became about 0.82). The neck separated from the body. Such meshes are now detected as copies and their shared bones are not edited again.
   *Note:* the head and neck may look slightly larger than before, because they now get the slider value once, as intended. Presets tuned around the doubled values may need small adjustments.
2. **F6 did not reopen the window after closing it.** `hide()` hid the window first and then tried to give the focus back. By then Windows had already activated another window (for example the UE4SS console), so the call failed. The hotkey only works while the game is focused, so it was ignored. The focus is now handed to the game window *before* hiding, with a fallback to the game's `UnrealWindow` and one retry.

### Full slider audit (round 2)

`tests/test_audit.cpp` checks the whole slider surface on both characters in the cache: Hun (body 274, head 890, brow/lash 199) and QueenMarika (body 410, head 1115). It covers:

- every Simplified Panel group, with 12 kinds of edits (length, width, size, turn X/Y/Z, shift X/Y/Z, mixed);
- each of the 3 "Children" modes, in a standing pose and a stair-step pose, with ground on and off;
- every single bone, the way the Detailed Panel edits it;
- all groups at once;
- fast motion with the head mesh a frame behind the body.

That is about 8,000 cases. The check is that a copy-pose mesh ends up exactly like the same mesh edited correctly on its own, and that its shared bones sit exactly on the body's.

Problems it found, now fixed:

| # | Problem | Fix |
|---|---|---|
| 3 | **Turn-only sliders were doubled on the head mesh** (e.g. Head / Neck / Hands turned 15° became 30°). Copy detection compared only positions and scales, so a pure rotation gave it no evidence. | The main mesh also publishes rotations (`kPublishStride` 6 → 10). Detection uses them. |
| 4 | **"This bone only" / "Keep children size" on a shared bone did not apply to the head mesh's own bones below it.** Head (Solo) moved face and tongue bones by up to 11 cm; Neck (Keep) scaled the neck-back facial bones. | In copy mode, the "Keep children size" correction also applies to the mesh's own bones below a copied bone. "This bone only" is taken back out for those bones (`sculpt.cpp`, step 3). Other meshes also rebuild the bones below a Solo bone. |
| 5 | **A foot floated (up to 12 cm)** when one leg was edited more than the other, or with a turned pelvis/thigh or a moved foot or calf. The body was lifted by the deeper foot, and the other leg could not stretch past straight. | The lift is limited so every leg can reach. A foot that would end up below the ground is bent up instead (IK now works both ways). |
| 6 | **A bigger or smaller Feet slider sank or floated the sole** (only the ankle joint was measured). | The ground contact is now the sole: the ball/toe bone plus a heel point under the ankle, both carried by the foot (`LegChain::ball`, `contact_sink`). |
| 7 | A mesh with no slider bones of its own (the Hun head mesh when only the legs are edited) was not lifted if it is animated on its own. | With Auto Ground on, it is planned anyway. |
| 8 | Copy detection could fail in fast motion if the head mesh runs a frame before the body. | Bones below a shared parent are compared relative to that parent, so whole-body motion cancels out. An unclear frame no longer resets what was known. |

Also in this round:
- `ubs status` now reports, per leg, the ankle and sole sink, the lift the leg's reach allows, and the foot height before and after.
- A non-zero `AutoGroundExtraCm` now applies even with no sliders set. Mod meshes can stand higher or lower than the game's ground: in Phantom Blade Zero, the QueenMarika body floats about 5 cm even with every slider at 1.0, so `AutoGroundExtraCm = -5` puts it on the ground.

After the fixes the worst results were: sole 0.005 cm off the ground, shared bones of a copy-pose mesh 0.001 cm off the body, and copy-pose vs. own-animation 0.05 cm. The last one only happens for lifts under the 0.05 cm detection threshold. Copy detection was never wrong. `test_sculpt`, `test_mirror` and `test_ground` (410 and 274) still pass.

```
cl /std:c++latest /EHsc /O2 /I..\src test_audit.cpp ..\src\core\sculpt.cpp ..\src\core\body_groups.cpp ..\src\core\rig_names.cpp
test_audit <native\skeleton_cache.txt> <Scripts\BoneDictionary.json>
```

### Files changed

| File | Change |
|---|---|
| `src/core/sculpt.hpp`, `sculpt.cpp` | `GroundOptions`, `LegChain`, `find_legs`, `apply_ground`, `bend_leg`, copy detection (`decide_copy`, `set_follow_reference`, published poses), `ground_report`. |
| `src/core/registry.hpp`, `registry.cpp` | Ground options for every mesh. Per character: the lift, the main mesh's published pose, and bone maps for the other meshes. `ground_report()`. `RigSummary.legs`. |
| `src/dllmain.cpp` | Reads the new `setup` keys. Ground info in `ubs status`. |
| `src/win/slider_window.cpp` | Focus fix in `hide()`. |
| `tests/test_ground.cpp` | New test on a real skeleton (see below). |
| `mod/UUEPBS/Scripts/main.lua` | Sends the ground settings, switches between the modes, `ubs ground` command. New state is kept in fields of `late` (the chunk is at Lua's 200-local limit). |
| `mod/UUEPBS/Scripts/config.lua` | The `AutoGround*` settings. |
| `mod/UUEPBS/Scripts/uuepbs_ground.lua` | New: the Lua fallback. |

### Build & test

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build/Release/UUEPBS.dll

cl /std:c++latest /EHsc /O2 /I..\src test_ground.cpp ..\src\core\sculpt.cpp
test_ground <native\skeleton_cache.txt> 410
```

`test_ground` loads a real skeleton from UUEPBS's skeleton cache and bends it into game poses: standing, one foot on a step, and both knees bent. It checks that:

- every foot ends within 0.01 cm of the ground the game put it on, with the game's rotation;
- the IK'd legs keep their edited bone lengths;
- applying twice to a buffer the engine did not refresh gives the same result;
- with the feature off, the result is identical to the original;
- other meshes are handled correctly: a head-only mesh or a full-skeleton mesh that is either animated on its own or copies the body. The bones a copy shares with the body match the body exactly, and a bone only that mesh has still gets its slider.

The original `test_sculpt` still passes.

**Build tip:** build from the full path, not an 8.3 short path such as `AFRAX~1.DES`. With a short path MSBuild missed header changes, which left a stale object file and caused a crash. Do a clean rebuild after changing shared structs in `registry.hpp`.

### Known limitations

- One body offset plus IK. If a foot's target is further sideways than the leg is long, it cannot be reached and the foot may float.
- A pure body "shift down" slider on the pelvis is cancelled by Auto Ground: the feet stay on the ground, so the body comes back up.
- Legs are only found when thigh, calf and foot are direct parents of each other.
- With Auto Ground on, every bone of an edited character is rebuilt every frame. This was expected to cost microseconds, but it was not measured in game.
- The collision capsule is unchanged. The correction is visual only.

---

## 한국어

### 요약

| 구분 | 내용 |
|---|---|
| 새 기능 | **Auto Ground**: 다리 길이를 바꿔도 발이 바닥에 묻히거나 뜨지 않습니다. |
| 새 기능 | **다리별 IK**: 계단·경사에서도 발이 게임의 foot IK가 놓은 위치에 그대로 있습니다. |
| 새 기능 | 슬라이더를 적용한 모든 캐릭터에 적용되고, 몸의 포즈를 복사하는 메시(머리, 의상)도 맞게 처리합니다. |
| 버그 수정 | 기본 메시의 포즈를 복사하는 메시(Copy Pose / Leader Pose)에 슬라이더가 **두 번** 적용되던 문제. 예를 들어 허리를 줄이면 머리 메시의 목이 몸에서 떨어졌습니다. |
| 버그 수정 | 슬라이더 창을 닫은 뒤 다른 창을 눌렀다가 게임을 다시 누르기 전까지 **F6이 반응하지 않던** 문제. |

### 새 기능: Auto Ground

**문제.** DLL은 게임의 애니메이션과 foot IK가 끝난 최종 포즈를 수정합니다. 골반은 애니메이션이 놓은 자리에 그대로 있어서, 종아리·허벅지를 늘리면 발이 바닥 아래로 내려가고 줄이면 발이 뜹니다.

**동작 방식** (DLL, 매 프레임, 슬라이더 적용 직후 – `RigSculptor::apply_ground`):

1. 다리(thigh → calf → foot)마다 수정 때문에 발이 얼마나 내려갔는지 잽니다. 게임 포즈와 수정된 포즈를 컴포넌트 공간 +Z 방향으로 비교합니다.
2. 그중 가장 큰 값만큼 포즈 전체를 올려서, 가장 많이 묻힌 발을 지면에 맞춥니다.
3. 그러면 나머지 발은 조금 뜨게 되는데, **two-bone IK**(`bend_leg`)로 그 다리를 굽혀 게임이 놓았던 위치까지 내립니다. 무릎이 굽는 방향과 발의 회전은 유지하고, 각 마디에 달린 본은 그 마디를 그대로 따라갑니다.
4. 들어올릴 양은 캐릭터의 **기본 메시**(`CharacterMesh0`)만 계산합니다. 나머지 메시는 아래 방식으로 처리합니다.

**나머지 메시 처리** (`decide_copy`). 기본 메시는 매 프레임 모든 본의 수정 전·후 위치와 스케일을 공유합니다. 다른 메시는 자기 본을 이름으로 기본 메시의 본에 대응시켜 비교합니다.

- **기본 메시의 수정 후 상태와 같으면** → 포즈를 복사한 메시입니다(Copy Pose / Leader Pose). 기본 메시와 겹치는 본은 받은 그대로 두고, 슬라이더와 접지를 다시 적용하지 않습니다. 그 메시에만 있는 본(얼굴 본 등)에는 슬라이더를 적용합니다.
- **수정 전 상태와 같으면** → 독립적으로 애니메이션되는 메시입니다. 슬라이더를 적용하고 기본 메시처럼 접지합니다. 다리가 없는 메시는 기본 메시가 계산한 높이만큼 올립니다.
- **둘 다 아니면** → 건드리지 않습니다.

3프레임 연속 같은 결과가 나와야 판별이 바뀌기 때문에 깜빡이지 않습니다.

**기능을 끄면** 원본 DLL과 결과가 비트 단위까지 같습니다. 테스트로 확인합니다.

**다리는 본 이름으로 찾습니다.** foot·ankle 본의 부모가 calf·knee·lowerleg·shin 본이고, 그 부모가 thigh·upleg·hip 본이면 다리로 봅니다. IK·twist·roll·control·corrective 본은 제외하고, 최대 4개까지 찾습니다.

### Lua 대체 방식 (`AutoGroundMode = "lua"`)

새 파일 `Scripts/uuepbs_ground.lua`는 새 DLL 없이도 동작합니다. `_last_session.json`에서 플레이어의 슬라이더 값을 읽고, 실시간 본 위치(`GetSocketLocation`)로 발 높이를 잰 뒤, 플레이어 메시 컴포넌트를 옮깁니다(`RelativeLocation.Z`). 몸 전체를 한 번에 옮기는 방식이라 계단에서는 높은 쪽 발이 조금 뜰 수 있습니다. DLL을 다시 빌드할 수 없을 때만 쓰세요.

### 설정 (`Scripts/config.lua`)

| 설정 | 기본값 | 설명 |
|---|---|---|
| `AutoGround` | `true` | 기능을 켜고 끕니다. |
| `AutoGroundMode` | `"dll"` | `"dll"`: 포즈 안에서 들어올리기 + IK (새 DLL 필요). `"lua"`: 위의 대체 방식. |
| `AutoGroundLegIK` | `true` | (dll) 나머지 다리를 지면까지 굽힙니다. `false`면 들어올리기만 합니다. |
| `AutoGroundExtraCm` | `0` | 들어올리는 양에 더하는 값(+ 위, − 아래). 미세 조정용입니다. |
| `AutoGroundMaxCm` | `40` | 들어올리거나 내리는 양의 최대치입니다. |
| `AutoGroundFollowPose` | `true` | (lua) 서 있는 기준 포즈 대신 실시간 포즈를 따라갑니다. |
| `AutoGroundIntervalMs` | `16` | (lua) 실시간 보정 주기입니다. |
| `AutoGroundSmoothing` | `0` | (lua) 0~0.95. 높이 변화를 부드럽게 합니다(떨림은 줄고 약간 늦어짐). |

DLL에는 새 `setup` 키(`ground`, `ground_ik`, `ground_extra`, `ground_max`)로 전달됩니다. 예전 `main.lua`는 이 키를 보내지 않으므로 기능은 꺼진 상태로 남습니다.

### 콘솔 명령

- `ubs ground` – Auto Ground 설정을 보여 줍니다.
- `ubs ground on` / `ubs ground off` – 이번 세션 동안 켜고 끕니다.
- `ubs ground <cm>` – 이번 세션의 추가 높이를 정합니다.
- `ubs status` – 플레이어 들어올림 양, 메시별로 찾은 다리 개수, 메시별 보고(기본 메시인지 아닌지, 복사 판별 결과, 골반·목·머리 본의 수정 전후 높이)도 함께 보여 줍니다.

### 버그 수정

1. **포즈를 복사하는 메시에 슬라이더가 두 번 적용되던 문제.** 팬텀 블레이드 제로의 머리 메시는 몸의 *최종* 포즈를 복사해 오는데, UUEPBS가 거기에 슬라이더를 한 번 더 적용했습니다. 그래서 허리·목·머리 슬라이더가 두 배로 들어갔고(예: `spine_02` 0.807 → 약 0.65, `Head` 0.903 → 약 0.82), 목이 몸에서 떨어졌습니다. 이제 이런 메시를 복사 포즈로 판별해서, 겹치는 본에는 다시 적용하지 않습니다.
   *참고:* 이제 슬라이더 값이 의도대로 한 번만 적용되기 때문에 머리·목이 전보다 조금 커 보일 수 있습니다. 두 배로 들어가던 값에 맞춰 둔 프리셋은 조금 다시 맞춰야 할 수 있습니다.
2. **창을 닫은 뒤 F6으로 다시 열리지 않던 문제.** `hide()`가 창을 먼저 숨기고 나서 포커스를 돌려주려 했습니다. 그 시점에는 Windows가 이미 다른 창(UE4SS 콘솔 등)을 활성화해 버려서 포커스 반환이 실패했고, 게임에 포커스가 있을 때만 동작하는 단축키가 무시됐습니다. 이제 창을 숨기기 *전에* 게임 창으로 포커스를 넘깁니다. 원래 창을 못 찾으면 게임의 `UnrealWindow`를 찾아 넘기고, 한 번 더 시도합니다.

### 전체 슬라이더 점검 (2차)

`tests/test_audit.cpp`는 캐시에 있는 두 캐릭터, 즉 Hun(몸 274, 머리 890, 눈썹·속눈썹 199)과 QueenMarika(몸 410, 머리 1115)에서 슬라이더 전체를 점검합니다. 점검 범위는 다음과 같습니다.

- 간편 패널의 모든 부위에 12가지 수정(길이, 폭, 크기, X/Y/Z 회전, X/Y/Z 이동, 혼합)을 적용합니다.
- "Children" 모드 3가지 각각을 서 있는 포즈와 계단 포즈에서, 접지를 켠 상태와 끈 상태로 확인합니다.
- 상세 패널처럼 본을 하나씩 수정합니다.
- 모든 부위를 한꺼번에 수정합니다.
- 머리 메시가 몸보다 한 프레임 늦는 빠른 동작도 확인합니다.

모두 합쳐 약 8,000가지 경우입니다. 판정 기준은 두 가지입니다. 포즈를 복사하는 메시가 독립적으로 올바르게 수정된 결과와 정확히 같아야 하고, 몸과 겹치는 본은 몸과 정확히 같은 자리에 있어야 합니다.

점검에서 나온 문제와 수정 내용입니다.

| # | 문제 | 수정 |
|---|---|---|
| 3 | **회전만 주는 슬라이더가 머리 메시에 두 번 적용됨** (예: Head·Neck·Hands를 15° 돌리면 30°가 됨). 복사 판별이 위치와 스케일만 비교해서, 회전만 바뀌면 판단할 근거가 없었습니다. | 기본 메시가 회전값도 공유하고(`kPublishStride` 6 → 10), 판별에 사용합니다. |
| 4 | **겹치는 본의 "This bone only"·"Keep children size"가 그 아래에 있는 머리 메시 고유 본에 적용되지 않음.** Head(Solo)에서는 얼굴·혀 본이 최대 11cm 어긋났고, Neck(Keep)에서는 목 뒤 얼굴 본이 같이 커졌습니다. | 복사 모드에서도 복사된 본 아래의 고유 본에 Keep 크기 보정을 적용하고, Solo 수정은 그 본들에서 다시 빼냅니다(`sculpt.cpp` 3단계). 다른 메시는 Solo 본 아래의 본도 다시 계산합니다. |
| 5 | **한쪽 발이 최대 12cm 뜸.** 한쪽 다리만 더 수정했거나 골반·허벅지를 돌렸거나 발·종아리를 이동한 경우입니다. 더 깊이 묻힌 발 기준으로 몸을 올리면, 반대쪽 다리는 다 펴도 바닥에 닿지 못했습니다. | 모든 다리가 바닥에 닿을 수 있는 만큼만 몸을 올립니다. 그러면 바닥 아래로 내려가는 발이 생기는데, 그 발은 IK로 다리를 굽혀 올립니다(IK가 위아래 모두 동작). |
| 6 | **Feet 슬라이더로 발을 키우거나 줄이면 발바닥이 묻히거나 뜸** (발목 관절만 쟀기 때문). | 이제 발바닥으로 지면 접촉을 잽니다. 발볼·발가락 본과 발목 아래의 뒤꿈치 지점을 쓰고, 둘 다 발 본을 따라 움직입니다(`LegChain::ball`, `contact_sink`). |
| 7 | 슬라이더가 걸린 본이 하나도 없는 메시(다리만 수정했을 때의 Hun 머리 메시)가 독립 애니메이션이면 같이 올라가지 않음. | Auto Ground를 켜면 이런 메시도 계산합니다. |
| 8 | 머리 메시가 몸보다 한 프레임 먼저 처리되면 빠른 동작 중에 복사 판별이 틀릴 수 있음. | 겹치는 부모 본이 있는 본은 그 부모 기준으로 비교해서 몸 전체의 움직임을 상쇄합니다. 애매한 프레임이 와도 이미 내린 판별을 되돌리지 않습니다. |

이번에 함께 바꾼 내용:
- `ubs status`에 다리별 진단 값이 나옵니다. 발목·발바닥이 내려간 양, 다리가 닿는 범위에서 허용되는 들어올림, 수정 전후 발 높이입니다.
- `AutoGroundExtraCm`가 0이 아니면 슬라이더가 하나도 없어도 적용됩니다. 모드 메시는 게임의 바닥보다 높거나 낮게 설 수 있기 때문입니다. 팬텀 블레이드 제로의 QueenMarika 몸은 슬라이더가 모두 1.0이어도 약 5cm 떠 있어서, `AutoGroundExtraCm = -5`로 바닥에 맞춥니다.

수정 후 가장 나쁜 값은 이렇습니다. 발바닥은 지면에서 0.005cm, 복사 메시의 겹치는 본은 몸에서 0.001cm 어긋났습니다. 복사 메시와 독립 애니메이션 메시는 최대 0.05cm 차이가 났는데, 판별 기준값 0.05cm보다 작은 들어올림에서만 생깁니다. 복사 판별이 틀린 경우는 없었습니다. `test_sculpt`, `test_mirror`, `test_ground`(410·274)도 모두 통과합니다.

```
cl /std:c++latest /EHsc /O2 /I..\src test_audit.cpp ..\src\core\sculpt.cpp ..\src\core\body_groups.cpp ..\src\core\rig_names.cpp
test_audit <native\skeleton_cache.txt> <Scripts\BoneDictionary.json>
```

### 바뀐 파일

| 파일 | 내용 |
|---|---|
| `src/core/sculpt.hpp`, `sculpt.cpp` | `GroundOptions`, `LegChain`, `find_legs`, `apply_ground`, `bend_leg`, 복사 판별(`decide_copy`, `set_follow_reference`, 포즈 공유), `ground_report`. |
| `src/core/registry.hpp`, `registry.cpp` | 모든 메시에 접지 옵션 전달. 캐릭터별 들어올림 양, 기본 메시가 공유하는 포즈, 다른 메시의 본 대응표. `ground_report()`. `RigSummary.legs`. |
| `src/dllmain.cpp` | 새 `setup` 키 읽기. `ubs status`에 접지 정보 추가. |
| `src/win/slider_window.cpp` | `hide()`의 포커스 수정. |
| `tests/test_ground.cpp` | 실제 스켈레톤으로 돌리는 새 테스트(아래 참고). |
| `mod/UUEPBS/Scripts/main.lua` | 접지 설정 전달, 모드 전환, `ubs ground` 명령. 새 상태는 `late` 테이블의 필드로 둡니다(지역 변수가 Lua 한도 200개에 걸려 있음). |
| `mod/UUEPBS/Scripts/config.lua` | `AutoGround*` 설정. |
| `mod/UUEPBS/Scripts/uuepbs_ground.lua` | 새 파일: Lua 대체 방식. |

### 빌드와 테스트

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release        # build/Release/UUEPBS.dll

cl /std:c++latest /EHsc /O2 /I..\src test_ground.cpp ..\src\core\sculpt.cpp
test_ground <native\skeleton_cache.txt> 410
```

`test_ground`는 UUEPBS의 스켈레톤 캐시에서 실제 스켈레톤을 읽고, 서 있기·한쪽 발을 계단에 올리기·양쪽 무릎 굽히기 포즈를 만들어 다음을 확인합니다.

- 모든 발이 게임이 놓은 지면에서 0.01cm 이내에 있고, 회전도 게임이 정한 그대로입니다.
- IK를 적용한 다리가 슬라이더로 바꾼 길이를 유지합니다.
- 엔진이 갱신하지 않은 버퍼에 두 번 적용해도 결과가 같습니다.
- 기능을 끄면 원본과 결과가 같습니다.
- 다른 메시를 올바르게 처리합니다. 머리만 있는 메시와 몸 전체 스켈레톤 메시를 각각 독립 애니메이션인 경우와 몸을 복사하는 경우로 확인합니다. 복사 메시의 겹치는 본은 몸과 정확히 일치하고, 그 메시에만 있는 본에는 슬라이더가 적용됩니다.

원작자의 기존 테스트 `test_sculpt`도 그대로 통과합니다.

**빌드 팁:** `AFRAX~1.DES` 같은 8.3 짧은 경로가 아니라 전체 경로에서 빌드하세요. 짧은 경로에서는 MSBuild가 헤더 변경을 놓쳐서 예전 오브젝트 파일이 남았고, 그 때문에 크래시가 났습니다. `registry.hpp`의 공유 구조체를 바꾼 뒤에는 처음부터 다시 빌드하세요.

### 알려진 한계

- 몸 전체를 한 번 올리고 IK로 보정하는 방식입니다. 발이 놓일 자리가 다리 길이보다 옆으로 더 멀면 닿을 수 없어서 발이 뜰 수 있습니다.
- 골반을 아래로 이동하는 슬라이더만 쓰면 Auto Ground가 그 효과를 상쇄합니다. 발을 바닥에 붙이려고 몸을 다시 올리기 때문입니다.
- thigh, calf, foot이 서로 직접 부모·자식 관계일 때만 다리로 인식합니다.
- 기능이 켜져 있으면 슬라이더를 적용한 캐릭터의 모든 본을 매 프레임 다시 계산합니다. 수 마이크로초 수준으로 예상했지만 게임에서 직접 재 보지는 않았습니다.
- 충돌 캡슐은 그대로입니다. 보이는 모습만 보정됩니다.
