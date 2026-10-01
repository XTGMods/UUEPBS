Host-side tests (Linux/macOS/WSL with g++ 13+ or clang 17+). None of them need Windows or the game.

    python3 make_ref.py path/to/SK_Roku_v3_Skeleton.json      # FModel export -> roku_ref.txt
    # no FModel export? a synthetic 184-bone stand-in (71 mirrored pairs) works for every test:
    g++ -std=c++23 -I../src make_synthetic_ref.cpp -o make_synthetic_ref && ./make_synthetic_ref
    S=../src

    # pose maths, presets, registry (UE5 double and UE4 float poses), pose hook cost per call
    g++ -std=c++23 -O1 -I$S test_sculpt.cpp $S/core/*.cpp -o test_sculpt && ./test_sculpt

    # left/right naming for common rigs + mirroring measured from the reference pose
    g++ -std=c++23 -I$S test_mirror.cpp $S/core/*.cpp -o test_mirror && ./test_mirror
    g++ -std=c++23 -I$S test_groups.cpp $S/core/*.cpp -o test_groups
    ./test_groups ../../../mod/UUEPBS/Scripts/BoneDictionary.json [bridge_in files from games...]

    # hook finder against a real game exe on disk (expects GON's slot 374, +0x644/+0x648)
    g++ -std=c++23 -O2 -I$S test_resolver.cpp $S/core/*.cpp -o test_resolver && ./test_resolver path/to/Roku3-Win64-Shipping.exe

    # Lua <-> DLL file bridge
    g++ -std=c++23 -I$S bridge_peer.cpp $S/core/*.cpp -o bridge_peer
    g++ -std=c++23 -I$S test_bridge.cpp $S/core/*.cpp -o test_bridge
    mkdir -p /tmp/uuepbs && lua lua_harness.lua ../../../mod/UUEPBS/Scripts ./bridge_peer /tmp/uuepbs gon
    ./test_bridge /tmp/uuepbs/bridge_in.txt
    for m in generic nodir broken legacy; do lua lua_harness.lua ../../../mod/UUEPBS/Scripts ./bridge_peer /tmp/uuepbs $m; done
    # the harness also fails if the script searches the object array or rewrites bridge_in.txt while idle
    # mutation check: breaks one behaviour of main.lua at a time (per-character scans, settle checks, range,
    # mesh budget, world-search fallbacks...) and expects the harness to catch each
    python mutate_lua.py ../../../mod/UUEPBS/Scripts ./bridge_peer /tmp/uuepbs

    # window skins: skin.json parsing, image decoding and limits (needs Dear ImGui 1.92.1 sources in IMGUI=...)
    g++ -std=c++23 -I$S -I$IMGUI test_skin.cpp $S/ui/skin.cpp $S/ui/image_decode.cpp $S/core/*.cpp \
        $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp -o test_skin
    ./test_skin ../../../skins

    # UI preview + CPU renderer check (needs Dear ImGui 1.92.1 sources in IMGUI=...)
    # Renders every tab (Simplified, Detailed > Bones, Detailed > Morphs, Presets, Status) with a reference
    # rasteriser and with src/ui/soft_raster.cpp, first with the built-in look, then with the example skin
    # (written into a fresh skins folder the way the DLL does it). Fails if the two differ, if a frame drawn
    # from the quad cache differs from the first draw, or if switching skins leaks textures.
    # Prints ms for the first draw, per drawn frame and per skipped (unchanged) frame.
    g++ -std=c++23 -O2 -I$S -I$IMGUI ui_preview.cpp $S/ui/panel_view.cpp $S/ui/soft_raster.cpp $S/ui/skin.cpp $S/ui/image_decode.cpp $S/core/*.cpp \
        $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp -o ui_preview
    ./ui_preview 1.25        # writes preview_*.ppm, skin_preview_*.ppm and soft_*.ppm (font path inside is Linux-specific)

After editing Scripts/BoneDictionary.json, regenerate the DLL's built-in copy:

    python ../tools/gen_dictionary_inc.py

After changing the example skin (skins/Example - Midnight Rose, drawn by tools/gen_example_skin.py with Pillow),
re-embed it into the DLL:

    python ../tools/gen_example_skin.py && python ../tools/gen_skin_inc.py
