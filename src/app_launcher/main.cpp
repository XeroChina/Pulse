#include "../app/win_e_fast_entry.h"
#include "../app/single_instance_coordinator.h"
#include "../common/dialog_bundle.h"
#include "../common/app_identity.h"
#include "../common/portable_paths.h"
#include "../common/runtime_log.h"
#include "../common/shell_open_entry.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {
int LaunchFailed(DWORD error, const std::wstring& shell_target, uint64_t phase) {
    const DWORD native_error = shell_target.empty() ? ERROR_NOT_SUPPORTED : pulse::shell_open_entry::OpenNative(shell_target);
    std::wstring log_root = pulse::portable_paths::PortableDataDir();
    if (log_root.empty()) {
        PWSTR local = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
            log_root = std::wstring(local) + L"\\Pulse";
            CoTaskMemFree(local);
        }
    }
    if (!log_root.empty()) {
        pulse::diagnostics::runtime::Initialize(log_root, "launcher");
        pulse::diagnostics::runtime::Event("shell_entry_launch_failed", {{"phase", phase}, {"error", error},
            {"native_attempted", shell_target.empty() ? 0u : 1u}, {"native_error", native_error}},
            pulse::diagnostics::runtime::Level::Error);
        pulse::diagnostics::runtime::Shutdown();
    }
    return static_cast<int>(!shell_target.empty() && !native_error ? ERROR_SUCCESS : error);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int show) {
    pulse::portable_paths::ExportPortableEnvironment();
    pulse::app_identity::SetProcess();
    std::wstring tail = arguments ? arguments : L"";
    bool wait = false;
    constexpr wchar_t wait_flag[] = L"--launcher-wait";
    if (tail == wait_flag || tail.starts_with(std::wstring(wait_flag) + L" ")) {
        wait = true; tail.erase(0, std::size(wait_flag) - 1);
    }
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW((L"pulse.exe " + tail).c_str(), &argc);
    const auto shell_target = pulse::shell_open_entry::Parse(argc, argv);
    for (int i = 1; argv && i < argc; ++i)
        if (wcscmp(argv[i], L"--ai") == 0 || wcscmp(argv[i], L"--mcp") == 0 || wcscmp(argv[i], L"--mcp-stdio") == 0 ||
            wcscmp(argv[i], L"--seed-shell-verbs") == 0 || wcscmp(argv[i], L"--shot") == 0 ||
            wcscmp(argv[i], L"--selftest") == 0 || wcscmp(argv[i], L"--test-instance") == 0) wait = true;
    const auto fast = !wait ? pulse::app::TryWinEFastEntry(argc, argv,
        [] { return pulse::app::SingleInstanceCoordinator::FindPrimaryEndpoint().has_value(); },
        [] { pulse::app::SingleInstanceCoordinator existing;
             return existing.ForwardOpenPath(L"pulse:win-e", 2000); }) : std::nullopt;
    if (argv) LocalFree(argv);
    if (fast) return *fast;
    std::wstring image(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!size || size >= image.size()) return LaunchFailed(ERROR_BAD_PATHNAME, shell_target, 0);
    image.resize(size);
    pulse::dialog_bundle::BundleLease lease;
    const auto root_path = std::filesystem::path(image).parent_path();
    auto canonical_dir = [](const std::filesystem::path& dir) -> std::wstring {
        HANDLE handle = CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return dir.wstring();
        std::wstring result(32768, L'\0');
        const DWORD n = GetFinalPathNameByHandleW(handle, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
        CloseHandle(handle);
        if (!n || n >= result.size()) return dir.wstring();
        result.resize(n);
        if (result.starts_with(LR"(\\?\)")) result.erase(0, 4);
        return result;
    };
    std::wstring host_exe;
    DWORD error = ERROR_FILE_NOT_FOUND;
    const auto program_dir = root_path / L"Program";
    const DWORD program_attrs = GetFileAttributesW(program_dir.c_str());
    if (program_attrs != INVALID_FILE_ATTRIBUTES && (program_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        const std::wstring program_root = canonical_dir(program_dir);
        error = pulse::dialog_bundle::ResolveInstalledBundle(program_root, lease);
        for (DWORD delay = 10; error == ERROR_FILE_NOT_FOUND && delay <= 160; delay *= 2) {
            Sleep(delay);
            error = pulse::dialog_bundle::ResolveInstalledBundle(program_root, lease);
        }
        if (!error) {
            host_exe = lease.Host();
        } else {
            const auto direct_exe = program_dir / L"pulse.exe";
            const DWORD exe_attrs = GetFileAttributesW(direct_exe.c_str());
            if (exe_attrs != INVALID_FILE_ATTRIBUTES && !(exe_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                host_exe = direct_exe.wstring();
                error = ERROR_SUCCESS;
            }
        }
    }
    if (host_exe.empty()) {
        const std::wstring root = canonical_dir(root_path);
        error = pulse::dialog_bundle::ResolveInstalledBundle(root, lease);
        // The setup pointer transaction can briefly have no current file. Never
        // fall back to another executable or retry an invalid manifest/pointer.
        for (DWORD delay = 10; error == ERROR_FILE_NOT_FOUND && delay <= 160; delay *= 2) {
            Sleep(delay);
            error = pulse::dialog_bundle::ResolveInstalledBundle(root, lease);
        }
        if (!error) host_exe = lease.Host();
    }
    if (error || host_exe.empty()) return LaunchFailed(error ? error : ERROR_FILE_NOT_FOUND, shell_target, 1);
    // GUI launches release the stable entry promptly. Scripts can explicitly
    // wait; redirected streams and the stdio AI entry always preserve lifetime.
    std::wstring command = L"\"" + host_exe + L"\"";
    if (!tail.empty()) command += L" " + tail;
    STARTUPINFOEXW extended{};
    auto& startup = extended.StartupInfo;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(show);
    HANDLE inherited[3]{};
    const DWORD kinds[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    bool any = false;
    for (int i = 0; i < 3; ++i) {
        HANDLE original = GetStdHandle(kinds[i]);
        if (original && original != INVALID_HANDLE_VALUE &&
            DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &inherited[i], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            any = true;
            const DWORD type = GetFileType(original);
            if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK) wait = true;
        }
    }
    if (any) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = inherited[0]; startup.hStdOutput = inherited[1]; startup.hStdError = inherited[2];
    }
    std::vector<HANDLE> handle_list;
    std::vector<unsigned char> attributes;
    if (any) {
        for (HANDLE handle : inherited) if (handle) handle_list.push_back(handle);
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        attributes.resize(bytes);
        extended.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(extended.lpAttributeList, 1, 0, &bytes)) {
            const DWORD failure = GetLastError();
            for (HANDLE handle : inherited) if (handle) CloseHandle(handle);
            return LaunchFailed(failure, shell_target, 2);
        }
        if (!UpdateProcThreadAttribute(extended.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handle_list.data(), handle_list.size() * sizeof(HANDLE), nullptr, nullptr)) {
            const DWORD failure = GetLastError();
            DeleteProcThreadAttributeList(extended.lpAttributeList);
            for (HANDLE handle : inherited) if (handle) CloseHandle(handle);
            return LaunchFailed(failure, shell_target, 2);
        }
        startup.cb = sizeof(extended);
    }
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(host_exe.c_str(), command.data(), nullptr, nullptr,
        any, any ? EXTENDED_STARTUPINFO_PRESENT : 0, nullptr, nullptr, &startup, &process);
    const DWORD launch_error = created ? ERROR_SUCCESS : GetLastError();
    if (any) DeleteProcThreadAttributeList(extended.lpAttributeList);
    for (HANDLE handle : inherited) if (handle) CloseHandle(handle);
    if (!created) return LaunchFailed(launch_error, shell_target, 3);
    // Once the host exists its delivery outcome is unknown. Never open Explorer
    // on a wait timeout or host exit: it may already have dispatched this request.
    CloseHandle(process.hThread);
    DWORD result = ERROR_SUCCESS;
    if (wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        if (!GetExitCodeProcess(process.hProcess, &result)) result = GetLastError();
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
}
