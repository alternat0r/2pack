@echo off
rem ============================================================
rem  2pack release script (GitHub)
rem
rem  Builds the project, then creates a GitHub Release tagged
rem  vX.Y.Z with the three binaries as assets and a SHA-256
rem  "hashes comment" in the release notes.
rem
rem  Usage:
rem    release.bat               build + release
rem    release.bat --no-build    release the binaries already in bin\
rem    release.bat --dry-run     build+hash+notes, but no commit/push/release
rem
rem  Requirements:
rem    * git, with a remote 'origin' pointing at the GitHub repo
rem    * gh (GitHub CLI) installed and authenticated (gh auth login)
rem    * Visual Studio 2019 "Desktop C++" workload (for the build step)
rem
rem  Run from the repo root.
rem ============================================================
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "DOBUILD=1"
set "DRYRUN=0"
if /i "%~1"=="--no-build" set "DOBUILD=0"
if /i "%~2"=="--no-build" set "DOBUILD=0"
if /i "%~1"=="--dry-run"  set "DRYRUN=1"
if /i "%~2"=="--dry-run"  set "DRYRUN=1"

rem ---- preflight ----
where git >nul 2>nul || ( echo [!] git not found. & exit /b 1 )
git rev-parse --is-inside-work-tree >nul 2>nul || ( echo [!] Not a git repo. & exit /b 1 )
git remote get-url origin >nul 2>nul || ( echo [!] No remote 'origin'. & exit /b 1 )
where gh >nul 2>nul || ( echo [!] gh GitHub CLI not found. Install: https://cli.github.com/ & exit /b 1 )
if %DRYRUN%==0 (
    gh auth status >nul 2>nul || ( echo [!] gh is not authenticated. Run: gh auth login & exit /b 1 )
)

rem ---- [1/5] build ----
if %DOBUILD%==1 (
    echo [1/5] Building...
    call build.bat
    if errorlevel 1 ( echo [!] Build failed. & exit /b 1 )
) else (
    echo [1/5] Skipping build -- no-build requested.
)

rem ---- [2/5] version + guard ----
set "MAJ="
set "MIN="
set "PAT="
for /f "usebackq delims=" %%n in ("%ROOT%version.txt") do (
    if not defined MAJ ( set "MAJ=%%n" ) else (
        if not defined MIN ( set "MIN=%%n" ) else (
            if not defined PAT ( set "PAT=%%n" )
        )
    )
)
set "TAG=v%MAJ%.%MIN%.%PAT%"
echo [2/5] Version %MAJ%.%MIN%.%PAT%  (tag %TAG%)

git rev-parse -q --verify "refs/tags/%TAG%" >nul 2>nul
if not errorlevel 1 ( echo [!] Tag %TAG% already exists. Nothing to do. & exit /b 0 )

for %%f in (bin\2pack.exe bin\stub32.exe bin\stub64.exe) do (
    if not exist "%%f" ( echo [!] Missing %%f - build first. & exit /b 1 )
)

rem ---- [3/5] commit the version bump (real run only) ----
if %DRYRUN%==1 (
    echo [3/5] Dry run - skipping commit.
    for /f %%c in ('git rev-parse HEAD') do set "COMMIT=%%c"
) else (
    echo [3/5] Committing version bump...
    git add -- version.txt version.rc
    git diff --cached --quiet -- version.txt version.rc 2>nul
    if errorlevel 1 (
        git commit -m "Release %TAG%" -- version.txt version.rc || ( echo [!] Commit failed. & exit /b 1 )
    )
    for /f %%c in ('git rev-parse HEAD') do set "COMMIT=%%c"
)

rem ---- [4/5] SHA-256 + release notes ----
echo [4/5] Computing SHA-256 and writing notes...
rem certutil output: line 1 = "SHA256 hash of <file>:", line 2 = the hash,
rem line 3 = "CertUtil: -hashfile command completed successfully."
rem Drop the CertUtil footer, then skip=1 to drop the header line.
for /f "skip=1 tokens=1" %%h in ('certutil -hashfile "bin\2pack.exe" SHA256 ^| findstr /v /c:"CertUtil"') do set "H2=%%h"
for /f "skip=1 tokens=1" %%h in ('certutil -hashfile "bin\stub32.exe" SHA256 ^| findstr /v /c:"CertUtil"') do set "H32=%%h"
for /f "skip=1 tokens=1" %%h in ('certutil -hashfile "bin\stub64.exe" SHA256 ^| findstr /v /c:"CertUtil"') do set "H64=%%h"
if not defined H2  ( echo [!] Failed to hash 2pack.exe.  & exit /b 1 )
if not defined H32 ( echo [!] Failed to hash stub32.exe. & exit /b 1 )
if not defined H64 ( echo [!] Failed to hash stub64.exe. & exit /b 1 )

set "NOTES=%TEMP%\2pack-release-notes.md"
>  "%NOTES%" echo ## 2pack %TAG%
>> "%NOTES%" echo.
>> "%NOTES%" echo Portable Windows executable compressor (LZMA, self-extracting, no dependencies).
>> "%NOTES%" echo.
>> "%NOTES%" echo ### Assets
>> "%NOTES%" echo * `2pack.exe` - the compressor (x64)
>> "%NOTES%" echo * `stub32.exe` - 32-bit runtime unpacker
>> "%NOTES%" echo * `stub64.exe` - 64-bit runtime unpacker
>> "%NOTES%" echo.
>> "%NOTES%" echo ### SHA-256
>> "%NOTES%" echo.
>> "%NOTES%" echo ```
>> "%NOTES%" echo %H2%  2pack.exe
>> "%NOTES%" echo %H32%  stub32.exe
>> "%NOTES%" echo %H64%  stub64.exe
>> "%NOTES%" echo ```
>> "%NOTES%" echo.
>> "%NOTES%" echo Commit: `%COMMIT%`

rem ---- [5/5] release ----
if %DRYRUN%==1 (
    echo [5/5] Dry run - nothing pushed.
    echo.
    echo ==== Would run: ====
    echo   git tag %TAG%
    echo   git push origin HEAD
    echo   git push origin %TAG%
    echo   gh release create %TAG% bin\2pack.exe bin\stub32.exe bin\stub64.exe --title "%TAG%" --notes-file "%NOTES%"
    echo.
    echo ==== Release notes preview ====
    type "%NOTES%"
    exit /b 0
)

echo [5/5] Pushing and creating GitHub release %TAG% ...
git tag %TAG% || ( echo [!] Tag failed. & exit /b 1 )
git push origin HEAD || ( echo [!] Push failed. & exit /b 1 )
git push origin %TAG% || ( echo [!] Tag push failed. & exit /b 1 )
gh release create %TAG% bin\2pack.exe bin\stub32.exe bin\stub64.exe --title "%TAG%" --notes-file "%NOTES%" || ( echo [!] gh release create failed. & exit /b 1 )
echo.
echo Done. Release %TAG% created.
endlocal
