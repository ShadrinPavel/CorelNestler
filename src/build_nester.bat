@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
cd /d "d:\Yandex.Disk\CorelDrawComp\src"
rc.exe nesting.rc
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
cl.exe /nologo /c /MT /O2 /EHsc /W3 /WX /D UNICODE /D _UNICODE /std:c++17 /I "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\include" "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\src\clipper.engine.cpp" "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\src\clipper.offset.cpp" "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\src\clipper.rectclip.cpp"
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
cl.exe /nologo /c /MT /O2 /EHsc /W3 /WX /D UNICODE /D _UNICODE /std:c++17 /I "D:\Yandex.Disk\CorelDrawComp\libs\Clipper2-Clipper2_1.3.0\CPP\Clipper2Lib\include" CorelNester.cpp
if %ERRORLEVEL% neq 0 ( exit /b %ERRORLEVEL% )
link.exe /NOLOGO /SUBSYSTEM:WINDOWS /OUT:CorelNester.exe CorelNester.obj clipper.engine.obj clipper.offset.obj clipper.rectclip.obj nesting.res user32.lib ole32.lib oleaut32.lib gdi32.lib gdiplus.lib
set LINK_ERR=%ERRORLEVEL%
taskkill /F /IM mspdbsrv.exe >NUL 2>&1
taskkill /F /IM vctip.exe >NUL 2>&1
exit /b %LINK_ERR%