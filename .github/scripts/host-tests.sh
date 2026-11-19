#!/usr/bin/env bash
# Host-side tests from source/UUEPBS/tests/README.md, in one script so CI and a
# Linux/WSL shell run exactly the same thing.
#   IMGUI=<Dear ImGui 1.92.1 checkout> .github/scripts/host-tests.sh
# Needs g++ 13+ (C++23), lua5.4 (as "lua"), python3 and fonts-liberation (ui_preview).
set -euo pipefail

: "${IMGUI:?set IMGUI to a Dear ImGui v1.92.1 checkout}"
IMGUI=$(cd "$IMGUI" && pwd)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
T=$ROOT/source/UUEPBS/tests
S=$ROOT/source/UUEPBS/src
SCRIPTS=$ROOT/mod/UUEPBS/Scripts
OUT=${OUT:-$(mktemp -d)}
CXX=${CXX:-g++}
LUA=${LUA:-lua}
mkdir -p "$OUT"
cd "$T"   # ui_preview reads roku_ref.txt from the current directory

step() { echo; echo "::group::$*"; }
end() { echo "::endgroup::"; }

CORE=("$S"/core/*.cpp)
IMGUI_SRC=("$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_tables.cpp" "$IMGUI/imgui_widgets.cpp")

for t in sculpt changes symmetry mirror preset_library looks names share compare morph_sort body_editor follow camera; do
  step "test_$t"
  opt=""; [ "$t" = sculpt ] && opt="-O1"
  $CXX -std=c++23 $opt -I"$S" "test_$t.cpp" "${CORE[@]}" -o "$OUT/test_$t"
  if [ "$t" = preset_library ]; then "$OUT/test_$t" "$OUT/library"; else "$OUT/test_$t"; fi
  end
done

step "test_groups"
$CXX -std=c++23 -I"$S" test_groups.cpp "${CORE[@]}" -o "$OUT/test_groups"
"$OUT/test_groups" "$SCRIPTS/BoneDictionary.json"
end

step "test_ground (Auto Ground on the reference skeleton)"
python3 ref_to_cache.py roku_ref.txt "$OUT/roku_cache.txt"
$CXX -std=c++23 -I"$S" test_ground.cpp "${CORE[@]}" -o "$OUT/test_ground"
"$OUT/test_ground" "$OUT/roku_cache.txt" 184
end

step "test_hands (Hand Contact: arm IK onto the edited body)"
$CXX -std=c++23 -O1 -I"$S" test_hands.cpp "${CORE[@]}" -o "$OUT/test_hands"
"$OUT/test_hands" fixtures/carnal_instinct_cache.txt 139 Jiggle_R
"$OUT/test_hands" "$OUT/roku_cache.txt" 184 boob_r
end

step "test_audit (every slider x every edit, copy-pose meshes, Auto Ground)"
$CXX -std=c++23 -O1 -I"$S" test_audit.cpp "${CORE[@]}" -o "$OUT/test_audit"
"$OUT/test_audit" "$OUT/roku_cache.txt" "$SCRIPTS/BoneDictionary.json" 184
end

step "Lua tests (portraits, looks, deformers, physics, camera, collision, copies, hands)"
$LUA portraits_test.lua "$SCRIPTS" "$OUT/portraits"
$LUA looks_test.lua "$SCRIPTS"
$LUA deformers_test.lua "$SCRIPTS"
$LUA physics_test.lua "$SCRIPTS"
$LUA camera_test.lua "$SCRIPTS"
$LUA collision_test.lua "$SCRIPTS"
$LUA copies_test.lua "$SCRIPTS"
$LUA hands_test.lua "$SCRIPTS"
end

step "Lua <-> DLL bridge harness"
$CXX -std=c++23 -I"$S" bridge_peer.cpp "${CORE[@]}" -o "$OUT/bridge_peer"
$CXX -std=c++23 -I"$S" test_bridge.cpp "${CORE[@]}" -o "$OUT/test_bridge"
mkdir -p "$OUT/bridge"
$LUA lua_harness.lua "$SCRIPTS" "$OUT/bridge_peer" "$OUT/bridge" gon
"$OUT/test_bridge" "$OUT/bridge/bridge_in.txt"
echo "--- harness mode: gon, mesh asset property unreadable (UE4SS FObjectPtrProperty)"
mkdir -p "$OUT/bridge_objptr"
UUEPBS_OBJPTR=1 $LUA lua_harness.lua "$SCRIPTS" "$OUT/bridge_peer" "$OUT/bridge_objptr" gon
for m in generic nodir broken legacy polaris nobody tekkenedit tekkenjson names sb sbon sbnew; do
  echo "--- harness mode: $m"
  $LUA lua_harness.lua "$SCRIPTS" "$OUT/bridge_peer" "$OUT/bridge" "$m"
done
end

step "mutate_lua (harness must catch every mutation)"
python3 mutate_lua.py "$SCRIPTS" "$OUT/bridge_peer" "$OUT/bridge"
end

step "test_skin"
$CXX -std=c++23 -I"$S" -I"$IMGUI" test_skin.cpp "$S/ui/skin.cpp" "$S/ui/image_decode.cpp" "${CORE[@]}" "${IMGUI_SRC[@]}" -o "$OUT/test_skin"
"$OUT/test_skin" "$ROOT/skins"
end

step "ui_preview"
[ -f roku_ref.txt ] || { echo "roku_ref.txt missing: ui_preview would only preview the no-mesh state"; exit 1; }
$CXX -std=c++23 -O2 -I"$S" -I"$IMGUI" ui_preview.cpp "$S/ui/panel_view.cpp" "$S/ui/looks_view.cpp" "$S/ui/portraits.cpp" \
  "$S/ui/soft_raster.cpp" "$S/ui/skin.cpp" "$S/ui/image_decode.cpp" "${CORE[@]}" "${IMGUI_SRC[@]}" -o "$OUT/ui_preview"
mkdir -p "$OUT/preview"
cp roku_ref.txt "$OUT/preview/"
(cd "$OUT/preview" && "$OUT/ui_preview" 1.25)
end

echo; echo "All host tests passed."
