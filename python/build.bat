@echo off
REM Build a standalone, no-console mirror.exe with PyInstaller.
REM Output: dist\mirror.exe  (~35 MB, bundles Python + all deps).
REM First time: pip install dxcam pygame pystray Pillow pyinstaller
python -m PyInstaller --onefile --noconsole --name mirror --icon mirror.ico ^
    --collect-all dxcam --collect-submodules comtypes ^
    --hidden-import pystray._win32 mirror.py
if %ERRORLEVEL%==0 (echo BUILD OK -^> dist\mirror.exe) else (echo BUILD FAILED)
