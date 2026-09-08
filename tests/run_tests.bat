@echo off
rem ============================================================
rem  2pack end-to-end test harness.
rem  For each arch: run original, pack it, run packed, compare
rem  the file each writes (avoids depending on console capture).
rem ============================================================
setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
set "PP=%ROOT%\bin\2pack.exe"
set "T=%ROOT%\tests"
cd /d "%T%"

set "FAIL=0"

call :runarch x64
call :runarch x86

echo.
if "!FAIL!"=="0" ( echo ==== ALL TESTS PASSED ==== ) else ( echo ==== !FAIL! TESTS FAILED ==== )
endlocal & exit /b %FAIL%

:runarch
set "A=%~1"
echo ============================================================
echo [arch %A%]
echo ============================================================
if not exist "%T%\packtest_%A%.exe" ( echo  [skip] no packtest_%A%.exe & exit /b 0 )

rem --- 1. baseline: run the ORIGINAL ---
del "%T%\orig_out.txt" 2>nul
"%T%\packtest_%A%.exe"
set "ORIG_EC=%errorlevel%"
copy /y "%T%\packtest_out.txt" "%T%\orig_out.txt" >nul
echo  original exit=%ORIG_EC%

rem --- 2. pack it ---
del "%T%\packtest_%A%.packed.exe" 2>nul
"%PP%" "%T%\packtest_%A%.exe" -o "%T%\packtest_%A%.packed.exe"
if errorlevel 1 ( echo  [FAIL] 2pack returned %errorlevel% & set "FAIL=1" & exit /b 0 )

rem --- 3. run the PACKED exe ---
del "%T%\packtest_out.txt" 2>nul
"%T%\packtest_%A%.packed.exe"
set "PACKED_EC=%errorlevel%"
echo  packed   exit=%PACKED_EC%

rem --- 4. compare outputs ---
if not exist "%T%\packtest_out.txt" ( echo  [FAIL] packed exe wrote no output file & set "FAIL=1" & exit /b 0 )
copy /y "%T%\packtest_out.txt" "%T%\packed_out.txt" >nul
fc /b "%T%\orig_out.txt" "%T%\packed_out.txt" >nul
if errorlevel 1 (
    echo  [FAIL] output mismatch:
    echo     orig  : %T%\orig_out.txt
    echo     packed: %T%\packed_out.txt
    set "FAIL=1"
    exit /b 0
)
echo  [PASS] %A% packed exe output matches original
exit /b 0
