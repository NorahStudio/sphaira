#pragma once

#include "base.hpp"
#include "fs.hpp"
#include <switch.h>
#include <memory>
#include <string_view>
#include <vector>

namespace sphaira::yati::source {

constexpr std::string_view SPLIT_CONTAINER_EXTENSIONS[]{
    "nsp", "nsz", "xci", "xcz",
};

struct SplitFile final : Base {
    // path can either be a directory containing split parts named
    // "00", "01", etc. or the first part of a file split, i.e "game.nsp.00".
    SplitFile(fs::Fs* fs, const fs::FsPath& path);
    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;
    Result GetSize(s64* out);

private:
    Result Open(const fs::FsPath& path);

    fs::Fs* m_fs{};
    std::vector<std::unique_ptr<fs::File>> m_files{};
    // start offset of each part, followed by the total size at the end.
    std::vector<s64> m_offsets{};
};

auto IsSplitContainerExtension(std::string_view ext) -> bool;

// returns true if the name ends with ".<container_ext>.NN", i.e "game.nsp.00".
auto IsSplitPartName(std::string_view name) -> bool;

// returns true if the path refers to a split container, either a directory
// named after a container extension which contains parts ("00", "01"...),
// or a file named "*.<container_ext>.NN".
// out_logical_path is set to the path ending with the container extension.
auto IsSplitPath(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_logical_path) -> bool;

} // namespace sphaira::yati::source
