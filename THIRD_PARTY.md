# Third-party code and data in Free Touch

- **nlohmann/json** (MIT, Niels Lohmann): JSON parsing. From vcpkg (`nlohmann-json`).
- **Dear ImGui** (MIT, Omar Cornut) and the add-ons in `src/IMGUI/`: the copy that comes with the BakkesMod plugin
  template.
- **BakkesMod SDK**: `bakkesmod/...` headers and `pluginsdk.lib`, from the BakkesMod install (`BakkesMod.props`).
- **Barlow** fonts (SIL Open Font License 1.1, The Barlow Project Authors, https://github.com/jpt/barlow):
  `data/freetouch/content/fonts/`, with the license in `OFL.txt`.
- **Necto and Nexto** (CC BY-NC-SA 4.0, Rolv-Arild and the Necto team,
  https://github.com/Rolv-Arild/Necto). Free Touch uses them for its bots:
  - `data/freetouch/content/bots/necto/policy.bin` and `.../nexto/policy.bin` are their trained networks, converted
    to Free Touch's file format.
  - `src/bots/policy/NectoObs.h` and `NextoObs.h` port their observation builders to C++.
  - `src/bots/policy/EarlPolicy.cpp` runs their network.
  
  These adaptations are shared under the same license: non-commercial use, with credit to the authors. Free Touch is
  free.

Everything else is Free Touch's own: the code, the drills (JSON), the menu art (`images/`) and the sounds
(`sounds/`, synthesised by a script).
