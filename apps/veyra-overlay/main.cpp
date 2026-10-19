#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <format>
#include <memory>
#include "veyra/Log.h"
#include "veyra/RuntimePaths.h"
#include "veyra/engine/FgCompatibilityProbe.h"
#include "veyra/capture/PotPlayerCaptureController.h"

namespace {
std::unique_ptr<veyra::capture::PotPlayerCaptureController> controller;
HWND toggle=nullptr,statusText=nullptr;
ULONGLONG exitAt=0,nextUi=0;
LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_CREATE:
        toggle=CreateWindowW(L"BUTTON",L"开启视频区域捕获",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                             16,14,250,32,window,HMENU(101),GetModuleHandleW(nullptr),nullptr);
        statusText=CreateWindowW(L"STATIC",L"阶段 1：无增强对照。先开启，然后切到 PotPlayer 播放。",
                                WS_CHILD|WS_VISIBLE,16,62,510,96,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SetTimer(window,1,50,nullptr);return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==101&&controller)controller->setEnabled(!controller->enabled());
        return 0;
    case WM_TIMER:
        if(controller)controller->tick();
        if(GetTickCount64()>=nextUi&&controller) {
            nextUi=GetTickCount64()+200;
            SetWindowTextW(toggle,controller->enabled()?L"关闭视频区域捕获":L"开启视频区域捕获");
            const auto s=controller->snapshot();
            auto text=controller->status();
            text+=std::format(L"\r\n捕获：{} × {}  呈现提交：{:.1f} FPS\r\n提交统计不等于屏幕实际 FPS；本版尚未启用超分/补帧。",
                             s.sourceWidth,s.sourceHeight,s.metrics.flow.presentSubmitFps);
            SetWindowTextW(statusText,text.c_str());
        }
        if(exitAt&&GetTickCount64()>=exitAt)PostMessageW(window,WM_CLOSE,0,0);
        return 0;
    case WM_CLOSE:
        if(controller)controller->setEnabled(false);
        DestroyWindow(window);return 0;
    case WM_DESTROY:
        KillTimer(window,1);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    int argc=0;auto args=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(args&&argc==3&&std::wstring_view(args[1])==L"--fg-compat-probe") {
        auto mapping=reinterpret_cast<HANDLE>(_wcstoui64(args[2],nullptr,10));LocalFree(args);
        return veyra::engine::runFgCompatibilityProbe(mapping);
    }
    bool enable=false;unsigned testDuration=0;
    for(int i=1;args&&i<argc;++i) {
        if(std::wstring_view(args[i])==L"--enable")enable=true;
        else if(std::wstring_view(args[i])==L"--exit-after"&&i+1<argc) {
            wchar_t* end=nullptr;const auto value=wcstoul(args[++i],&end,10);
            if(!end||*end||value>300000) { LocalFree(args);return 2; }
            testDuration=unsigned(value);
        } else {LocalFree(args);return 2;}
    }
    if(args)LocalFree(args);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    veyra::engine::setFgCompatibilityProbeExecutable(executable);
    const auto logPath=veyra::runtime::logsDirectory()/(L"potplayer-overlay-"+std::to_wstring(GetCurrentProcessId())+L".log");
    if(!veyra::Logger::instance().openFile(logPath.wstring()))return 3;
    controller=std::make_unique<veyra::capture::PotPlayerCaptureController>();
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;
    wc.lpszClassName=L"VeyraPotPlayerAttachmentControl";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=HBRUSH(COLOR_WINDOW+1);
    if(!RegisterClassW(&wc))return 4;
    auto window=CreateWindowExW(0,wc.lpszClassName,L"Veyra · PotPlayer 区域捕获候选",
                               WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
                               CW_USEDEFAULT,CW_USEDEFAULT,560,220,nullptr,nullptr,instance,nullptr);
    if(!window)return 5;
    ShowWindow(window,show);UpdateWindow(window);
    controller->setEnabled(enable);
    if(testDuration)exitAt=GetTickCount64()+testDuration;
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg);DispatchMessageW(&msg); }
    controller.reset();veyra::Logger::instance().flush();CoUninitialize();
    return int(msg.wParam);
}
