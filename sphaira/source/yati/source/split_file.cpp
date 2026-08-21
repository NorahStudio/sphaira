#include "yati/source/split_file.hpp"

#include <cstring>
#include <cstdio>
#include <algorithm>

namespace sphaira::yati::source {
namespace {

constexpr u32 SPLIT_PART_MAX = 100;

auto is_two_digits(std::string_view str) -> bool {
    if (str.length() != 2) {
        return false;
    }

    for (const auto c : str) {
        if (c < '0' || c > '9') {
            return false;
        }
    }

    return true;
}

auto get_base_name(std::string_view path) -> std::string_view {
    if (const auto slash = path.find_last_of('/'); slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    return path;
}

} // namespace

auto IsSplitContainerExtension(std::string_view ext) -> bool {
    for (const auto& e : SPLIT_CONTAINER_EXTENSIONS) {
        if (e.length() == ext.length() && !strncasecmp(e.data(), ext.data(), e.length())) {
            return true;
        }
    }
    return false;
}

auto IsSplitPartName(std::string_view name) -> bool {
    const auto last_dot = name.find_last_of('.');
    if (last_dot == std::string_view::npos || last_dot == 0) {
        return false;
    }

    if (!is_two_digits(name.substr(last_dot + 1))) {
        return false;
    }

    const auto prev_dot = name.find_last_of('.', last_dot - 1);
    if (prev_dot == std::string_view::npos) {
        return false;
    }

    return IsSplitContainerExtension(name.substr(prev_dot + 1, last_dot - prev_dot - 1));
}

auto IsSplitPath(fs::Fs* fs, const fs::FsPath& path, fs::FsPath& out_logical_path) -> bool {
    const auto name = get_base_name(path);

    if (IsSplitPartName(name)) {
        if (!fs->FileExists(path)) {
            return false;
        }

        out_logical_path.From(std::string_view{path.s, path.size() - 3});
        return true;
    }

    const auto last_dot = name.find_last_of('.');
    if (last_dot == std::string_view::npos || !IsSplitContainerExtension(name.substr(last_dot + 1))) {
        return false;
    }

    if (!fs->DirExists(path)) {
        return false;
    }

    char part_name[8];
    std::snprintf(part_name, sizeof(part_name), "%02u", 0u);
    if (!fs->FileExists(fs::AppendPath(path, part_name))) {
        return false;
    }

    out_logical_path = path;
    return true;
}

SplitFile::SplitFile(fs::Fs* fs, const fs::FsPath& path) : m_fs{fs} {
    m_open_result = Open(path);
}

Result SplitFile::Open(const fs::FsPath& path) {
    const auto is_dir = m_fs->DirExists(path);
    const auto path_size = path.size();

    if (!is_dir) {
        R_UNLESS(path_size > 3, Result_FsEmpty);
        R_UNLESS(is_two_digits(std::string_view{path.s + path_size - 2, 2}), Result_FsEmpty);
    }

    s64 total{};
    m_offsets.push_back(total);

    for (u32 i = 0; i < SPLIT_PART_MAX; i++) {
        char part_name[8];
        std::snprintf(part_name, sizeof(part_name), "%02u", i);

        fs::FsPath part_path{};
        if (is_dir) {
            part_path = fs::AppendPath(path, part_name);
        } else {
            part_path.From(std::string_view{path.s, path_size - 2});
            part_path += part_name;
        }

        auto file = std::make_unique<fs::File>();
        const auto rc = m_fs->OpenFile(part_path, FsOpenMode_Read, file.get());
        if (R_FAILED(rc)) {
            if (m_files.empty()) {
                return rc;
            }
            break;
        }

        s64 part_size{};
        R_TRY(file->GetSize(&part_size));

        total += part_size;
        m_offsets.push_back(total);
        m_files.emplace_back(std::move(file));
    }

    R_SUCCEED();
}

Result SplitFile::Read(void* buf, s64 off, s64 size, u64* bytes_read) {
    R_TRY(GetOpenResult());
    *bytes_read = 0;

    const s64 total = m_offsets.back();
    if (off < 0 || off >= total || size <= 0) {
        R_SUCCEED();
    }

    auto* out = static_cast<u8*>(buf);

    size_t index{};
    while (index + 2 < m_offsets.size() && off >= m_offsets[index + 1]) {
        index++;
    }

    while (size > 0 && off < total && index + 1 < m_offsets.size()) {
        const auto local_off = off - m_offsets[index];
        const auto to_read = std::min<s64>(size, m_offsets[index + 1] - off);

        u64 read{};
        R_TRY(m_files[index]->Read(local_off, out, to_read, 0, &read));
        if (!read) {
            break;
        }

        out += read;
        off += read;
        size -= read;
        *bytes_read += read;

        if (off >= m_offsets[index + 1]) {
            index++;
        }
    }

    R_SUCCEED();
}

Result SplitFile::GetSize(s64* out) {
    R_TRY(GetOpenResult());
    *out = m_offsets.back();
    R_SUCCEED();
}

} // namespace sphaira::yati::source
