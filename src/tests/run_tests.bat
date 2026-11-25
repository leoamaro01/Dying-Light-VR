@echo off
rem Builds and runs every tests\*_test.cpp with MSVC (cl /W4 /std:c++17), one exe per test. None of them is part of the mod's
rem DLL build. third_party\puff\puff.c is linked into every test (setup_profile.h and calibnative_logic.h need it; puff's own
rem warnings land in each test's build.txt). Usage, from anywhere:
rem
rem     tests\run_tests.bat [binds golden file] [output folder]
rem
rem   binds golden file  dlvr_binds.default.scr for binds_logic_test. Without it that test is SKIPPED (and says so).
rem   output folder      where the exes, objs and logs go. Default %TEMP%\dlvr_tests - never inside the tree.
rem
rem Each test is run from this tests folder (setup_profile_test opens data\... relative to it). Every test prints its own
rem failures; this script prints PASS / FAIL / BUILD-FAILED / SKIPPED per test and a total. Exit code = failed + build-failed.
setlocal enabledelayedexpansion
set "TESTS=%~dp0"
set "ROOT=%~dp0.."
set "GOLD=%~1"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%TEMP%\dlvr_tests"

where cl >nul 2>&1
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%I"
    if defined VSDIR if exist "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
  )
)
where cl >nul 2>&1
if errorlevel 1 (
  echo run_tests: cl.exe not found - not on PATH, and vswhere found no Visual Studio with the x64 C++ tools.
  echo            Run this from a "x64 Native Tools Command Prompt for VS", or install the C++ build tools.
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
set "INC=/I "%ROOT%" /I "%ROOT%\ovr2xr" /I "%ROOT%\bootstrap" /I "%ROOT%\setup" /I "%ROOT%\launcher" /I "%ROOT%\common" /I "%TESTS%." /I "%ROOT%\third_party\puff""
set /a NPASS=0, NFAIL=0, NBUILD=0, NSKIP=0
echo run_tests: output in %OUT%

for %%F in ("%TESTS%*_test.cpp") do (
  set "T=%%~nF"
  set "ARG="
  set "SKIP="
  if /i "!T!"=="binds_logic_test" (
    if "%GOLD%"=="" (set "SKIP=needs dlvr_binds.default.scr as the first argument") else (set ARG="%GOLD%")
  )
  rem vsclass_c1_test classifies shaders cut from the game's shader pack: three folders of .dxbc, not in the repo.
  if /i "!T!"=="vsclass_c1_test" set "SKIP=needs three folders of captured .dxbc shaders (not in the repo)"
  if defined SKIP (
    echo SKIPPED       !T!  - !SKIP!
    set /a NSKIP+=1
  ) else (
    if not exist "%OUT%\!T!" mkdir "%OUT%\!T!"
    cl /nologo /EHsc /W4 /std:c++17 %INC% "%%F" "%ROOT%\third_party\puff\puff.c" /Fe:"%OUT%\!T!\!T!.exe" /Fo:"%OUT%\!T!\\" > "%OUT%\!T!\build.txt" 2>&1
    if errorlevel 1 (
      echo BUILD-FAILED  !T!  - see %OUT%\!T!\build.txt
      set /a NBUILD+=1
    ) else (
      pushd "%TESTS%"
      "%OUT%\!T!\!T!.exe" !ARG! > "%OUT%\!T!\run.txt" 2>&1
      set "RC=!errorlevel!"
      popd
      if "!RC!"=="0" (
        echo PASS          !T!
        set /a NPASS+=1
      ) else (
        echo FAIL          !T!  ^(exit !RC!^) - see %OUT%\!T!\run.txt
        set /a NFAIL+=1
      )
    )
  )
)

echo.
echo run_tests: %NPASS% passed, %NFAIL% failed, %NBUILD% did not build, %NSKIP% skipped
set /a RET=NFAIL+NBUILD
exit /b %RET%
