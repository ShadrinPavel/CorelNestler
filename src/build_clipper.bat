@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
cd /d "d:\Yandex.Disk\CorelDrawComp\src"
cl.exe /nologo /EHsc /W3 /std:c++17 /I "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\include" test_clipper.cpp "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\src\*.cpp"
exit /b %ERRORLEVEL%