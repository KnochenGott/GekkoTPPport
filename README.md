# TppGekko

The Metal Gear Survive gekko as a native enemy game object in MGSV:TPP. It is built by lifting Survive's own AI, movement, locomotion and action code out of the installed mgv.exe with a static x64 to C++ lifter, and answering the engine calls that code makes with real TPP services (animation controller, CharaControl, collision, targets, routes, navigation). 

Survive's code runs independent while TPP supplies the services or input.
This WILL be changed over properly to TPP-only functions step by step once the Gekko is proven to behave as intended. This work will then be reused to expand on general custom NPCs.

## Current Status
- Has life which gets reduced per bullet hit
- Can walk along nodes
- Currently WIP to use more of the SssNpc Service to automate more state changes, like Death animations, Hit reaction, proper Navigation and the "Attack Player" state. Required more work, Gekko keeps idle, will shift to more TPP native functions than pursuing to lift more ssd exe code, as mentioned above.

## Requirements
- MGSV:TPP 1.0.15.4 (mgsvtpp.exe) and Metal Gear Survive (mgv.exe).
- Windows, Visual Studio with the C++ x64 tools (cl, ml64), Python 3 with pefile and capstone (pip install pefile capstone).
- Infinite Heaven, to load the Lua modules.

## Build and test
- build_gekko.bat builds build\gekko\Gekko.dll. The first run lifts the module, which takes several minutes.
- build_gekko.bat install also copies Gekko.dll and the Lua modules into the TPP folder (close the game first).

To lift the Gekko Code:
- build_gekko.bat relift regenerates the lifted module; nopause skips the pause on failure.
- python tools\lift\tests\run_param_test.py prints PARAM_BLOCK_OK.
- python tools\lift\tests\run_tests.py runs the lifter differential test.
- python tools\lift\tests\run_world_test.py start runs the lifted brain headless (tokens are listed in docs\DOCUMENTATION.md).
- Set PYTHONIOENCODING=utf-8 when piping test output.

## Game files needed
The lifted module is generated locally from the installed games into build\ (or committed here). GekkoSigs.h is generated at build time from your exe. Assets are included. You need your own legal copies of both games.

- TPP: the MGSV:TPP folder. Default is the Steam folder under C:\Program Files (x86).
- MGV: the Metal Gear Survive folder or its mgv.exe. Default is the Steam folder under C:\Program Files (x86).


## Credits
The Gecko parameter values in src\GekkoHook\GekkoParamBlock.h come from the community server capture in alicealys/mgv-server-emulator, file src/server/resources/data/parameter/SsdNpcLevelParameters.json

Caplag & Yazed for guidance and instructions (will expand once this project goes somewhere)

A Local LLM was used in structuring and formatting this repository. Furthermore did it assist in decompiling and analysing exe functions.
