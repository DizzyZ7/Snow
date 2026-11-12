#include "application.hpp"
#include <shellapi.h>
#include <cwchar>
#include <exception>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    // The embedded manifest is authoritative; an already-set awareness returns
    // access denied here and must not be downgraded by a legacy API fallback.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    bool smoke = false, gpu = false, benchmark = false, valid = true;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--smoke-test") == 0) smoke = true;
        else if (std::wcscmp(argv[i], L"--benchmark") == 0) benchmark = true;
        else if (std::wcscmp(argv[i], L"--renderer=d3d11") == 0) gpu = true;
        else if (std::wcscmp(argv[i], L"--renderer=gdi") != 0) valid = false;
    }
    LocalFree(argv);
    if (!valid) return 2;
    try { snow::Application app(instance, smoke, gpu, benchmark); return app.run(); }
    catch (const std::exception&) {
        if (!smoke) MessageBoxW(nullptr, L"Snow не удалось запустить. Проверьте доступность памяти и графического устройства.", L"Snow", MB_OK | MB_ICONERROR);
        return 1;
    }
}
