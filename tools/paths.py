# paths.py 03/10/2026. Single source of install paths for the tools. Override with environment variables; defaults are the Steam locations.
#   TPP        MGSV:TPP folder (1.0.15.4)          MGV   Metal Gear Survive folder or its mgv.exe
#   MODBLDR    modbldr-tools.exe (fox2/ftex/fpk converter, optional)
import os, pathlib
_steam = r"C:\Program Files (x86)\Steam\steamapps\common"
TPP_DIR = pathlib.Path(os.environ.get("TPP", _steam + r"\MGS_TPP"))
_mgv = pathlib.Path(os.environ.get("MGV", _steam + r"\METAL GEAR SURVIVE"))
MGV_EXE = _mgv if _mgv.suffix.lower() == ".exe" else _mgv / "mgv.exe"
MGV_DIR = MGV_EXE.parent
MGV_MASTER = MGV_DIR / "master"
TPP_EXE = TPP_DIR / "mgsvtpp.exe"
GEKKO_LOG = TPP_DIR / "Gekko.log"
MODBLDR = pathlib.Path(os.environ.get("MODBLDR", "modbldr-tools.exe"))
ROOT = pathlib.Path(__file__).resolve().parent.parent
RESEARCH = ROOT / "research"
