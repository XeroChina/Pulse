#pragma once

#include <windows.h>
#include <filesystem>
#include <string>

namespace pulse::portable_paths {

inline bool IsPortableMarkerFile(const std::filesystem::path& directory) noexcept {
    if (directory.empty()) return false;
    const auto marker = directory / L"portable";
    const DWORD attrs = GetFileAttributesW(marker.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

inline std::wstring ModuleDirectory(HMODULE module = nullptr) {
    std::wstring image(32768, L'\0');
    const DWORD count = GetModuleFileNameW(module, image.data(), static_cast<DWORD>(image.size()));
    if (!count || count >= image.size()) return {};
    image.resize(count);
    return std::filesystem::path(image).parent_path().wstring();
}

inline std::wstring DetectPortableRoot(HMODULE module = nullptr) {
    const std::wstring dir_text = ModuleDirectory(module);
    if (dir_text.empty()) return {};
    std::filesystem::path current(dir_text);
    // Walk up at most 5 levels to cover:
    //   <Root>\pulse.exe
    //   <Root>\Program\pulse.exe
    //   <Root>\Program\DialogBridge\<hash>\pulse.exe
    //   <Root>\Data\packs\<kind>\<version>\helper.exe
    for (int depth = 0; depth < 6 && !current.empty(); ++depth) {
        if (IsPortableMarkerFile(current)) return current.wstring();
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return {};
}

inline std::wstring PortableDataDirForModule(HMODULE module = nullptr) {
    wchar_t env_dir[32768]{};
    const DWORD env_len = GetEnvironmentVariableW(L"PULSE_PORTABLE_DATA_DIR", env_dir, ARRAYSIZE(env_dir));
    if (env_len > 0 && env_len < ARRAYSIZE(env_dir)) {
        std::wstring result(env_dir, env_len);
        CreateDirectoryW(result.c_str(), nullptr);
        return result;
    }
    const std::wstring root = DetectPortableRoot(module);
    if (root.empty()) return {};
    std::wstring data = (std::filesystem::path(root) / L"Data").wstring();
    CreateDirectoryW(data.c_str(), nullptr);
    return data;
}

inline std::wstring PortableDataDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&PortableDataDir),
        &self);
    return PortableDataDirForModule(self);
}

inline bool IsPortableMode() {
    return !PortableDataDir().empty();
}

inline void ExportPortableEnvironment() {
    const std::wstring data = PortableDataDir();
    if (!data.empty()) {
        SetEnvironmentVariableW(L"PULSE_PORTABLE_DATA_DIR", data.c_str());
    }
}

} // namespace pulse::portable_paths
