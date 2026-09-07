@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
cd /d "d:\Yandex.Disk\CorelDrawComp\src"
cl.exe /nologo /EHsc /W4 /WX test_com.cpp
exit /b %ERRORLEVEL%
