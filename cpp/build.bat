@echo off
REM Build the mirror app with MinGW g++ (no Visual Studio needed).
REM Produces mirror.exe as a GUI app (no console window), with a custom file icon.
windres app.rc -O coff -o app.res
g++ -std=c++17 -O2 -municode -mwindows mirror.cpp app.res -o mirror.exe -ld3d11 -ldxgi -ld3dcompiler -lgdi32 -ldwmapi -luuid -lole32
if %ERRORLEVEL%==0 (echo BUILD OK -^> mirror.exe) else (echo BUILD FAILED)
