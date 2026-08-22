#pragma once

#include "fs.hpp"
#include <switch.h>
#include <memory>
#include <vector>

namespace sphaira::game_file_scan {

enum class InstallState {
    // file could not be parsed, or no cnmt was found.
    Unknown,
    // title is not installed.
    New,
    // installed version is older than the version in this file.
    UpdateAvailable,
    // installed version matches this file.
    UpToDate,
    // installed version is newer than the version in this file.
    Older,
};

struct CnmtInfo {
    u64 title_id{};
    u32 version{};
    u8 meta_type{};
};

struct ScanResult {
    InstallState state{InstallState::Unknown};
    u64 application_id{};
    // highest version found within the file (display as v{file_version >> 16}).
    u32 file_version{};
    u32 installed_version{};
};

// parses all cnmt entries within an nsp/nsz file.
// requires valid keys to be setup.
auto GetCnmts(const std::shared_ptr<fs::Fs>& fs, const fs::FsPath& path, std::vector<CnmtInfo>& out) -> Result;

// compares cnmts against the titles installed on this console.
auto CompareInstalled(const std::vector<CnmtInfo>& cnmts, ScanResult& out) -> void;

// helper which runs GetCnmts + CompareInstalled.
auto ScanFileInstallState(const std::shared_ptr<fs::Fs>& fs, const fs::FsPath& path, ScanResult& out) -> Result;

} // namespace sphaira::game_file_scan
