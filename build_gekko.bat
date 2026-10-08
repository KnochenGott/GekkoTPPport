@echo off
rem build_gekko.bat 30/09/2026. Generates GekkoSigs.h from your exe, builds Gekko.dll, optionally installs it.
rem Usage: build_gekko.bat            (build only)
rem        build_gekko.bat install    (build, then copy the dll and the IH module into the game)
rem        build_gekko.bat nopause    (build only, no pause on failure; used by tools\populate_project.py)
rem        build_gekko.bat relift     (also regenerate the lifted Survive gekko module; done automatically when missing)
rem Speed: dependencies, signatures (until the exe changes), Visual Studio setup and the 8 MB module object are only redone when needed; relift redoes the module.
rem The lifted module (build\gekko\gekko_module.cpp, about 22 MB, not committed) is generated from Survive's mgv.exe with tools\lift\lift.py.
setlocal
rem Paths: set TPP (MGSV:TPP 1.0.15.4 folder), MGV (Metal Gear Survive folder or its mgv.exe) and optionally GEKKO_PARAM_JSON (only to regenerate GekkoParamBlock.h, see README.md) in the environment; the defaults below are the Steam locations.
if not defined TPP set "TPP=C:\Program Files (x86)\Steam\steamapps\common\MGS_TPP"
set "ROOT=%~dp0"
set "OUT=%ROOT%build\gekko"
if not defined MGV set "MGV=C:\Program Files (x86)\Steam\steamapps\common\METAL GEAR SURVIVE\mgv.exe"
if exist "%MGV%\mgv.exe" set "MGV=%MGV%\mgv.exe"

echo [1/4] python deps
python -c "import pefile, capstone" 2>nul || python -m pip install --quiet pefile capstone || goto :fail

echo [2/4] signatures from %TPP%\mgsvtpp.exe
if not exist "%OUT%" mkdir "%OUT%"
for %%F in ("%TPP%\mgsvtpp.exe") do set "SIGKEY=%%~zF %%~tF"
for %%G in ("%ROOT%tools\make_gekko_sigs.py") do set "SIGKEY=%SIGKEY% %%~zG %%~tG"
set "OLDKEY="
if exist "%OUT%\sigs.key" set /p OLDKEY=<"%OUT%\sigs.key"
if "%SIGKEY%"=="%OLDKEY%" (echo   unchanged exe, signatures kept) else (
  python "%ROOT%tools\make_gekko_sigs.py" "%TPP%\mgsvtpp.exe" || goto :fail
  >"%OUT%\sigs.key" echo %SIGKEY%
)

echo [2b/4] gecko parameter block
if defined GEKKO_PARAM_JSON (
  if not exist "%GEKKO_PARAM_JSON%" (echo GEKKO_PARAM_JSON file not found: %GEKKO_PARAM_JSON% & goto :fail)
  python "%ROOT%tools\gen_gekko_params.py" "%GEKKO_PARAM_JSON%" || goto :fail
) else (echo   using the committed src\GekkoHook\GekkoParamBlock.h)

echo [3/4] locate Visual Studio
where cl >nul 2>nul && goto :havecl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere not found: install Visual Studio with "Desktop development with C++" & goto :fail)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo no VS install with the C++ x64 tools & goto :fail)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || goto :fail
:havecl

echo [4/4] build
if not exist "%OUT%" mkdir "%OUT%"
set "LIFTOUT=%OUT%\gekko_module"
if /i "%~1"=="relift" (del "%LIFTOUT%.cpp" 2>nul & del "%OUT%\gekko_module.obj" 2>nul)
rem the module is stale when the lift scripts are newer (a changed root list is not picked up otherwise: MEASURED 01/10, game ran a module without 0x140f7f7f0)
python -c "import os,sys; m=r'%LIFTOUT%.cpp'; sys.exit(0 if os.path.exists(m) and all(os.path.getmtime(r'%ROOT%tools/lift/'+f)<=os.path.getmtime(m) for f in ('lift.py','lift_gekko.py')) else 1)" || (del "%LIFTOUT%.cpp" 2>nul & del "%OUT%\gekko_module.obj" 2>nul)
if not exist "%LIFTOUT%.cpp" (
  if not exist "%MGV%" (echo mgv.exe not found: %MGV% & goto :fail)
  echo   lifting the Survive gekko module
  python "%ROOT%tools\lift\lift_gekko.py" "%LIFTOUT%" > "%OUT%\lift.log" 2>&1 || (type "%OUT%\lift.log" & goto :fail)
)
pushd "%OUT%"
if not exist gekko_module.obj cl /nologo /c /O1 /MT /EHa /std:c++17 /I"%ROOT%src\GekkoHook" /I"%ROOT%tools\lift" "%LIFTOUT%.cpp" /Fo:gekko_module.obj > module.log 2>&1 || (type module.log & popd & goto :fail)
if not exist lift_native_call.obj ml64 /nologo /c /Fo lift_native_call.obj "%ROOT%tools\lift\lift_native_call.asm" > ml.log 2>&1 || (type ml.log & popd & goto :fail)
ml64 /nologo /c /Fo ctrl_fwd.obj "%ROOT%src\GekkoHook\GekkoCtrlFwd.asm" > ml2.log 2>&1 || (type ml2.log & popd & goto :fail)
cl /nologo /MP /LD /O2 /MT /EHa /std:c++17 /W4 /I"%ROOT%src\GekkoHook" /I"%ROOT%tools\lift" ^
   "%ROOT%src\GekkoHook\dllmain.cpp" "%ROOT%src\GekkoHook\GekkoLog.cpp" ^
   "%ROOT%src\GekkoHook\GekkoLua.cpp" "%ROOT%src\GekkoHook\GekkoDriver.cpp" "%ROOT%src\GekkoHook\GekkoFiles.cpp" "%ROOT%src\GekkoHook\GekkoBlocks.cpp" "%ROOT%src\GekkoHook\GekkoSigCheck.cpp" "%ROOT%src\GekkoHook\GekkoAi.cpp" "%ROOT%src\GekkoHook\GekkoGround.cpp" "%ROOT%src\GekkoHook\GekkoType.cpp" "%ROOT%src\GekkoHook\GekkoFull.cpp" "%ROOT%src\GekkoHook\GekkoHit.cpp" "%ROOT%src\GekkoHook\GekkoHitFx.cpp" "%ROOT%src\GekkoHook\GekkoParam.cpp" "%ROOT%src\GekkoHook\GekkoTargetProbe.cpp" "%ROOT%src\GekkoHook\GekkoPushProbe.cpp" "%ROOT%src\GekkoHook\GekkoPush.cpp" "%ROOT%src\GekkoHook\GekkoRoute.cpp" "%ROOT%src\GekkoHook\GekkoNavProxy.cpp" "%ROOT%src\GekkoHook\GekkoWorld.cpp" "%ROOT%src\GekkoHook\GekkoLife.cpp" "%ROOT%src\GekkoHook\GekkoNodes.cpp" "%ROOT%src\GekkoHook\GekkoBrain.cpp" "%ROOT%src\GekkoHook\GekkoDebug.cpp" "%ROOT%src\GekkoHook\GekkoLegIk.cpp" "%ROOT%src\GekkoHook\GekkoNavStep.cpp" "%ROOT%src\GekkoHook\GekkoTurnLog.cpp" "%ROOT%tools\lift\lift_rt.cpp" gekko_module.obj lift_native_call.obj ctrl_fwd.obj ^
   /Fe:Gekko.dll /link /DLL /MAP:Gekko.map > build.log 2>&1
set ERR=%ERRORLEVEL%
type build.log
popd
if not "%ERR%"=="0" goto :fail
echo built %OUT%\Gekko.dll

if /i "%~1"=="install" (
  if not exist "%TPP%\plugins" mkdir "%TPP%\plugins"
  copy /y "%OUT%\Gekko.dll" "%TPP%\plugins\" >nul || goto :fail
  if exist "%TPP%\mod\plugins\Gekko.dll" del "%TPP%\mod\plugins\Gekko.dll"
  copy /y "%ROOT%ProjectFolder\GekkoPort\GameDir\mod\modules\InfGekkoSpawn.lua" "%TPP%\mod\modules\" >nul || goto :fail
  copy /y "%ROOT%ProjectFolder\GekkoPort\GameDir\mod\modules\InfGekkoRoutes.lua" "%TPP%\mod\modules\" >nul || goto :fail
  echo installed into %TPP%\mod
)
echo OK
exit /b 0

:fail
echo BUILD FAILED (compiler output: build\gekko\build.log)
if /i not "%~1"=="nopause" pause
exit /b 1
