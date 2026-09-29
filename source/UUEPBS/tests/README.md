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

    # UI preview + CPU renderer check (needs Dear ImGui 1.92.1 sources in IMGUI=...)
    # Renders every tab with a reference rasteriser and with src/ui/soft_raster.cpp,
    # fails if they differ, and prints ms per drawn frame and per skipped (unchanged) frame.
    g++ -std=c++23 -O2 -I$S -I$IMGUI ui_preview.cpp $S/ui/panel_view.cpp $S/ui/soft_raster.cpp $S/core/*.cpp \
        $IMGUI/imgui.cpp $IMGUI/imgui_draw.cpp $IMGUI/imgui_tables.cpp $IMGUI/imgui_widgets.cpp -o ui_preview
    ./ui_preview 1.25        # writes preview_*.ppm and soft_preview_*.ppm (font path inside is Linux-specific)

After editing Scripts/BoneDictionary.json, regenerate the DLL's built-in copy:

    python ../tools/gen_dictionary_inc.py
