// Entry point of the packaged UWP test app (console subsystem, AppContainer).
//
// A packaged app's stdout does not reach the launching shell, so results are
// written to a report file in the package's temp folder and the exit code
// carries pass/fail. run_uwp_test_package.ps1 registers the package, launches
// it through its execution alias and reads the report back.
//
// Exit codes: 0 = all passed, 1 = test or platform failure, 2 = harness error.

#include "support/test_harness.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <appmodel.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

bool isAppContainer()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD value = 0;
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenIsAppContainer, &value, sizeof value, &size);
    CloseHandle(token);
    return ok && value != 0;
}

// Empty when the process has no package identity.
std::wstring packageFullName()
{
    UINT32 length = 0;
    if (GetCurrentPackageFullName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }
    std::wstring name(length, L'\0');
    if (GetCurrentPackageFullName(&length, name.data()) != ERROR_SUCCESS || length == 0) {
        return {};
    }
    name.resize(length - 1); // length includes the terminator
    return name;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring report_name = L"chromaroute-uwp-tests.txt";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wcscmp(argv[i], L"--report") == 0) {
            report_name = argv[i + 1];
        }
    }
    // Only a bare file name: the report must stay inside the package's temp folder.
    if (report_name.empty() || report_name.find_first_of(L"\\/:") != std::wstring::npos) {
        return 2;
    }

    wchar_t temp_dir[MAX_PATH + 1]{};
    const DWORD temp_length = GetTempPathW(MAX_PATH + 1, temp_dir);
    if (temp_length == 0 || temp_length > MAX_PATH) {
        return 2;
    }

    std::FILE* out = nullptr;
    if (_wfopen_s(&out, (std::wstring(temp_dir) + report_name).c_str(), L"w") != 0 || out == nullptr) {
        return 2;
    }

    const std::wstring package = packageFullName();
    const bool app_container = isAppContainer();
    std::fprintf(out, "platform: WINAPI_FAMILY=%d package=%ls app_container=%d\n",
                 WINAPI_FAMILY, package.empty() ? L"<none>" : package.c_str(), app_container ? 1 : 0);

    int result = chromaroute::test::runAll(out);
    if (package.empty() || !app_container) {
        std::fprintf(out, "FAIL: not running as a packaged AppContainer process\n");
        result = 1;
    }
    std::fclose(out);
    return result;
}
