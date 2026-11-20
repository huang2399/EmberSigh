@echo off
REM Enter the existing ESP-IDF v5.4.4 toolchain environment, then run idf.py
REM with the arguments passed to this script. Reuses the installed IDF;
REM nothing is downloaded or installed here.
call "D:\Espressif\frameworks\esp-idf-v5.4.4\export.bat" >nul 2>&1
cd /d "D:\DreamState\DreamState_RGB"
idf.py %*