@echo off
REM Enter the existing ESP-IDF v5.4.4 toolchain environment, then run idf.py
REM with the arguments passed to this script. Reuses the installed IDF;
REM nothing is downloaded or installed here.
REM
REM PYTHONIOENCODING: idf_monitor decodes the serial stream as UTF-8 and writes
REM it back through Python's stdout. On a zh-CN (GBK) console that write raises
REM UnicodeEncodeError as soon as a non-ASCII byte arrives, which kills the
REM monitor. Forcing UTF-8 on the Python side keeps monitor stable.
set PYTHONIOENCODING=utf-8
call "D:\Espressif\frameworks\esp-idf-v5.4.4\export.bat" >nul 2>&1
cd /d "D:\DreamState\DreamState_RGB"
idf.py %*