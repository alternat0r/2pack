@echo off
rem ============================================================
rem  2pack build script (Visual C++ 2019)
rem
rem  Produces, in bin\ :
rem    2pack.exe  - the compressor (x64, static CRT, no debug info)
rem    stub32.exe   - 32-bit runtime unpacker (no CRT, no debug)
rem    stub64.exe   - 64-bit runtime unpacker (no CRT, no debug)
rem
rem  Run from the repo root.  Requires the VS2019 "Desktop C++"
rem  workload (vcvarsall.bat) and the Windows 10/11 SDK.
rem
rem  Quoting rules honored here (cmd AND cl both re-parse the line):
rem    * /I include paths are quoted and NEVER end with a backslash
rem      (a "\" right before a closing quote is read as an escaped quote).
rem    * /Fo is a DIRECTORY (trailing backslash) that already exists.
rem    * 2pack links via cl one-step (auto static CRT + default libs).
rem    * the stubs link with /NODEFAULTLIB + /ENTRY:main + kernel32.lib
rem      (no C runtime at all).
rem    * The VS path contains "(x86)"; never place it in an if(...) block.
rem
rem  Versioning: every build auto-increments the MINOR version.
rem    State lives in version.txt (MAJ.MIN.PAT); version.rc is
rem    regenerated from it, so the resource always matches the build.
rem ============================================================
setlocal

set "ROOT=%~dp0"
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"

if not exist "%VS%" goto :no_vs

if not exist "%ROOT%bin" mkdir "%ROOT%bin"
if not exist "%ROOT%obj" mkdir "%ROOT%obj"
if not exist "%ROOT%obj\pp" mkdir "%ROOT%obj\pp"
if not exist "%ROOT%obj\stub64" mkdir "%ROOT%obj\stub64"
if not exist "%ROOT%obj\stub32" mkdir "%ROOT%obj\stub32"

rem ---- 64-bit toolchain (used for 2pack + stub64) ----
call "%VS%" x64 >nul
if errorlevel 1 ( echo [!] vcvarsall x64 failed & exit /b 1 )

set "LZMA=%ROOT%third_party\lzma"

rem ---- Version: auto-increment MINOR on every build ----
rem  State is kept in version.txt, one component per line:
rem      MAJ
rem      MIN
rem      PAT
rem  Each run bumps MIN by one, persists it, and regenerates
rem  version.rc so the embedded resource always matches the build.
set "VFILE=%ROOT%version.txt"
if not exist "%VFILE%" (
    > "%VFILE%" (
        echo 1
        echo 0
        echo 0
    )
)
set "MAJ="
set "MIN="
set "PAT="
for /f "usebackq delims=" %%n in ("%VFILE%") do (
    if not defined MAJ ( set "MAJ=%%n" ) else (
        if not defined MIN ( set "MIN=%%n" ) else (
            if not defined PAT ( set "PAT=%%n" )
        )
    )
)
if not defined MAJ set "MAJ=1"
if not defined MIN set "MIN=0"
if not defined PAT set "PAT=0"
rem Normalize: strip any trailing CR and reject non-numeric values.
set /a MAJ=%MAJ% 2>nul || set "MAJ=1"
set /a MIN=%MIN% 2>nul || set "MIN=0"
set /a PAT=%PAT% 2>nul || set "PAT=0"
set /a MIN+=1
> "%VFILE%" (
    echo %MAJ%
    echo %MIN%
    echo %PAT%
)
echo [v] Version %MAJ%.%MIN%.%PAT%  (minor auto-incremented)

rem ---- Regenerate version.rc from the new version ----
> "%ROOT%version.rc" (
    echo #include ^<windows.h^>
    echo.
    echo 1 VERSIONINFO
    echo FILEVERSION     %MAJ%,%MIN%,%PAT%,0
    echo PRODUCTVERSION  %MAJ%,%MIN%,%PAT%,0
    echo FILEOS          VOS_NT_WINDOWS32
    echo FILETYPE        VFT_APP
    echo BEGIN
    echo     BLOCK "StringFileInfo"
    echo     BEGIN
    echo         BLOCK "040904B0"
    echo         BEGIN
    echo             VALUE "FileDescription",  "2pack - portable Windows executable compressor"
    echo             VALUE "ProductName",      "2pack"
    echo             VALUE "FileVersion",      "%MAJ%.%MIN%.%PAT%"
    echo             VALUE "ProductVersion",   "%MAJ%.%MIN%.%PAT%"
    echo             VALUE "LegalCopyright",   "Public domain"
    echo         END
    echo     END
    echo     BLOCK "VarFileInfo"
    echo     BEGIN
    echo         VALUE "Translation", 0x409, 1200
    echo     END
    echo END
)

echo [1/4] Compiling version resource...
rc /nologo /fo "%ROOT%obj\version.res" "%ROOT%version.rc" || exit /b 1

echo [2/4] Building 2pack.exe (x64 compressor, static CRT)...
cl /nologo /O1 /DNDEBUG /DZ7_ST /DPACK2_USE_LZMA /MT /EHsc /I "%LZMA%" /Fo%ROOT%obj\pp\ /Fe"%ROOT%bin\2pack.exe" "%ROOT%compressor\main.cpp" "%ROOT%compressor\compress.cpp" "%ROOT%compressor\pe_build.cpp" "%LZMA%\LzmaEnc.c" "%LZMA%\LzFind.c" "%LZMA%\Bra86.c" "%LZMA%\CpuArch.c" "%ROOT%obj\version.res" /link /SUBSYSTEM:CONSOLE >nul || exit /b 1

echo [3/4] Building stub64.exe (x64 unpacker, no CRT)...
cl /nologo /c /O1 /DNDEBUG /DZ7_ST /DPACK2_USE_LZMA /W3 /GS- /EHs-c- /GR- /I "%LZMA%" /Fo%ROOT%obj\stub64\ "%ROOT%stub\stub_main.cpp" "%ROOT%stub\crt.c" "%LZMA%\LzmaDec.c" "%LZMA%\Bra86.c" >nul || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /NODEFAULTLIB /ENTRY:main /OUT:"%ROOT%bin\stub64.exe" "%ROOT%obj\stub64\stub_main.obj" "%ROOT%obj\stub64\crt.obj" "%ROOT%obj\stub64\LzmaDec.obj" "%ROOT%obj\stub64\Bra86.obj" kernel32.lib "%ROOT%obj\version.res" || exit /b 1

rem ---- 32-bit toolchain (for stub32) ----
echo [4/4] Building stub32.exe (x86 unpacker, no CRT)...
call "%VS%" x86 >nul
if errorlevel 1 ( echo [!] vcvarsall x86 failed & exit /b 1 )
cl /nologo /c /O1 /DNDEBUG /DZ7_ST /DPACK2_USE_LZMA /W3 /GS- /EHs-c- /GR- /I "%LZMA%" /Fo%ROOT%obj\stub32\ "%ROOT%stub\stub_main.cpp" "%ROOT%stub\crt.c" "%LZMA%\LzmaDec.c" "%LZMA%\Bra86.c" >nul || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /MACHINE:X86 /NODEFAULTLIB /ENTRY:main /OUT:"%ROOT%bin\stub32.exe" "%ROOT%obj\stub32\stub_main.obj" "%ROOT%obj\stub32\crt.obj" "%ROOT%obj\stub32\LzmaDec.obj" "%ROOT%obj\stub32\Bra86.obj" kernel32.lib "%ROOT%obj\version.res" || exit /b 1

echo.
echo ============================================================
echo  Build complete.  Output in %ROOT%bin :
echo ============================================================
dir /b "%ROOT%bin\*.exe"
goto :end

:no_vs
echo [!] Could not find VS2019 vcvarsall.bat at:
echo     %VS%
echo     Install Visual Studio 2019 (Community/Pro/Enterprise) with
echo     the "Desktop development with C++" workload, or edit VS above.
exit /b 1

:end
endlocal
