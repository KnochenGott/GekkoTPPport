# Gekko.dll: build and install

A standalone DLL. It needs no HookSample and no IHHook. It is loaded the same way as OctoCamo's material_probe.dll:
the IH module calls `package.loadlib(MGS_TPP\plugins\Gekko.dll, "luaopen_Gekko")`. That function checks the
game's Lua C API and then registers the `Gekko` table.

Only TPP English 1.0.15.4 is supported. On any other exe the signature checks fail, the reason is logged, and nothing
is called.

MakeBite route: run `python tools/populate_project.py`. It builds the DLL (through build_gekko.bat), puts it
in `ProjectFolder/GekkoPort/GameDir/plugins/Gekko.dll`, fills the pack folders, and leaves the module in
`GameDir/mod/modules/`. Pack GekkoPort with MakeBite and install. Use `--no-dll` to skip the build.

Quick way without MakeBite: run `build_gekko.bat install` from the repo root. It does steps 1 to 4. Edit the TPP path at its
top if your game is elsewhere.

1. Generate the signatures from your exe. Every address the DLL uses gets a prologue signature:
   ```
   pip install pefile capstone
   python tools/make_gekko_sigs.py %TPP%\mgsvtpp.exe
   ```
   This writes `src/GekkoHook/GekkoSigs.h`. The printout must show an address for `File::Find(PathId)`.
2. Build (Visual Studio 2019/2022 with the C++ workload): run `build_gekko.bat` from the repo root; it writes `build/gekko/Gekko.dll`.
3. Copy `Gekko.dll` to `MGS_TPP\plugins\` (the IH plugin folder).
4. Copy `ProjectFolder/GekkoPort/GameDir/mod/modules/InfGekkoSpawn.lua` to `MGS_TPP\mod\modules\`.
5. In game, open IH > GekkoSpawn > "Verify exe" (this loads the DLL the first time).

Logs:
- `MGS_TPP\Gekko.log`: DLL side, appended across sessions, one line per engine call. Flushed every line.
- `ih_log.txt`: the menu side.


