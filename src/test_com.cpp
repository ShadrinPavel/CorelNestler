#include <iostream>
#include <windows.h>

#import "C:\\Program Files\\Corel\\CorelDRAW Graphics Suite\\26\\Programs64\\TypeLibs\\VGCoreAuto.tlb" no_namespace named_guids \
    rename("GetCommandLine", "VGGetCommandLine") \
    rename("CopyFile", "VGCopyFile") \
    rename("FindWindow", "VGFindWindow") \
    rename("DrawText", "VGDrawText") \
    rename("ReplaceText", "VGReplaceText") \
    rename("GetUserName", "VGGetUserName")

int main() {
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) return 1;
    try {
        IVGApplicationPtr pApp(L"CorelDRAW.Application");
        std::cout << "COM OK" << std::endl;
    } catch (...) {}
    CoUninitialize();
    return 0;
}
