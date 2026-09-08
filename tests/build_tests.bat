@echo off
rem Build the two verification test programs (x64 + x86), native.
setlocal
set "ROOT=%~dp0.."
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%ROOT%\tests" mkdir "%ROOT%\tests"

echo [x64] building packtest_x64.exe
call "%VS%" x64 >nul
if errorlevel 1 ( echo [!] vcvars x64 failed & exit /b 1 )
cl /nologo /O2 /MT /EHsc /Fe"%ROOT%\tests\packtest_x64.exe" "%ROOT%\tests\packtest.cpp" /link /SUBSYSTEM:CONSOLE >nul || exit /b 1

echo [x86] building packtest_x86.exe
call "%VS%" x86 >nul
if errorlevel 1 ( echo [!] vcvars x86 failed & exit /b 1 )
cl /nologo /O2 /MT /EHsc /Fe"%ROOT%\tests\packtest_x86.exe" "%ROOT%\tests\packtest.cpp" /link /SUBSYSTEM:CONSOLE >nul || exit /b 1

echo.
echo Done:
dir /b "%ROOT%\tests\*.exe"
endlocal
