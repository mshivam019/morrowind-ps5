#ifndef OPENMW_COMPONENTS_FILES_PS5PATH_HPP
#define OPENMW_COMPONENTS_FILES_PS5PATH_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace Files
{
    // Packaged assets live on M.2 via /app0; saves use the native writable mount.
    struct Ps5Path
    {
        explicit Ps5Path(const std::string&) {}
        std::filesystem::path getUserConfigPath() const { return "/download0/config/"; }
        std::filesystem::path getUserDataPath() const { return "/download0/user/"; }
        std::filesystem::path getCachePath() const { return "/download0/cache/"; }
        std::filesystem::path getGlobalConfigPath() const { return "/app0/assets/"; }
        std::filesystem::path getLocalPath() const { return "/app0/assets/"; }
        std::filesystem::path getGlobalDataPath() const { return "/app0/assets/"; }
        std::vector<std::filesystem::path> getInstallPaths() const { return {}; }
    };
}

#endif
