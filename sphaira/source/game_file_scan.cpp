#include "game_file_scan.hpp"
#include "defines.hpp"
#include "log.hpp"
#include "title_info.hpp"

#include "yati/container/nsp.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ncm.hpp"
#include "yati/nx/keys.hpp"
#include "yati/source/file.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

namespace sphaira::game_file_scan {
namespace {

// limits a source to [base, base+size), so that nca offsets can be used as-is.
struct SubSource final : yati::source::Base {
    SubSource(std::shared_ptr<yati::source::Base> source, s64 base, s64 size)
    : m_source{std::move(source)}
    , m_base{base}
    , m_size{size} {

    }

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override {
        *bytes_read = 0;
        if (off < 0 || off >= m_size || size <= 0) {
            R_SUCCEED();
        }

        size = std::min<s64>(size, m_size - off);
        return m_source->Read(buf, m_base + off, size, bytes_read);
    }

private:
    std::shared_ptr<yati::source::Base> m_source;
    const s64 m_base;
    const s64 m_size;
};

constexpr u32 PFS0_MAGIC = 0x30534650;

#pragma pack(push, 1)
struct Pfs0Header {
    u32 magic;
    u32 total_files;
    u32 string_table_size;
    u32 _0xC;
};
struct Pfs0Entry {
    u64 data_offset;
    u64 data_size;
    u32 name_offset;
    u32 _0x14;
};
#pragma pack(pop)

constexpr u32 MAX_PFS0_FILES = 0x10000;
constexpr u32 MAX_PFS0_STRING_TABLE_SIZE = 16 * 1024 * 1024;

Result ReadExact(yati::source::Base* source, void* buf, s64 off, u64 size) {
    if (!size) {
        R_SUCCEED();
    }

    u64 bytes_read{};
    R_TRY(source->Read(buf, off, size, std::addressof(bytes_read)));
    R_UNLESS(bytes_read == size, Result_NspInvalidHeader);
    R_SUCCEED();
}

} // namespace

auto GetCnmts(const std::shared_ptr<fs::Fs>& fs, const fs::FsPath& path, std::vector<CnmtInfo>& out) -> Result {
    out.clear();

    yati::source::File file_source{fs.get(), path};
    R_TRY(file_source.GetOpenResult());

    yati::container::Nsp nsp{std::addressof(file_source)};
    yati::container::Collections collections{};
    R_TRY(nsp.GetCollections(collections));

    keys::Keys keys{};
    R_TRY(keys::parse_keys(keys, true));

    for (const auto& e : collections) {
        // nsz files may contain .cnmt.ncz, these are not supported (yet).
        constexpr std::string_view cnmt_ext{".cnmt.nca"};
        if (!e.name.ends_with(cnmt_ext)) {
            continue;
        }

        log_write("[game_file_scan] found cnmt: %s off: %lld size: %lld\n", e.name.c_str(), (long long)e.offset, (long long)e.size);

        // read and decrypt the nca header.
        nca::Header header{};
        R_TRY(ReadExact(std::addressof(file_source), std::addressof(header), e.offset, sizeof(header)));
        R_TRY(nca::DecryptHeader(std::addressof(header), keys, header));
        R_UNLESS(header.GetSectionCount() >= 1, Result_YatiInvalidNcaMagic);
        R_UNLESS(header.fs_table[0].IsValid(), Result_YatiInvalidNcaMagic);
        R_UNLESS(header.fs_header[0].fs_type == nca::FileSystemType_PFS0, Result_YatiInvalidNcaMagic);

        // decrypt the key area in place, needed by the reader for aes-ctr.
        R_TRY(nca::DecryptKeak(keys, header));

        // limit the source to this nca so that nca-relative offsets work.
        auto nca_source = std::make_shared<SubSource>(
            std::shared_ptr<yati::source::Base>{std::addressof(file_source), [](yati::source::Base*){}},
            e.offset, e.size
        );
        nca::NcaReader reader{header, header.key_area[0].area, static_cast<u64>(e.size), nca_source};

        // parse the inner pfs0.
        const auto pfs0_off = static_cast<s64>(header.fs_table[0].GetOffset());

        Pfs0Header pfs0_header{};
        R_TRY(ReadExact(std::addressof(reader), std::addressof(pfs0_header), pfs0_off, sizeof(pfs0_header)));
        R_UNLESS(pfs0_header.magic == PFS0_MAGIC, Result_NspBadMagic);
        R_UNLESS(pfs0_header.total_files > 0 && pfs0_header.total_files <= MAX_PFS0_FILES, Result_NspInvalidHeader);
        R_UNLESS(pfs0_header.string_table_size <= MAX_PFS0_STRING_TABLE_SIZE, Result_NspInvalidHeader);

        const auto table_off = pfs0_off + sizeof(Pfs0Header);
        const auto strings_off = table_off + static_cast<s64>(pfs0_header.total_files) * sizeof(Pfs0Entry);

        std::vector<Pfs0Entry> entries(pfs0_header.total_files);
        R_TRY(ReadExact(std::addressof(reader), entries.data(), table_off, entries.size() * sizeof(Pfs0Entry)));

        std::vector<char> string_table(pfs0_header.string_table_size);
        R_TRY(ReadExact(std::addressof(reader), string_table.data(), strings_off, string_table.size()));

        const auto data_off = strings_off + pfs0_header.string_table_size;

        for (const auto& entry : entries) {
            R_UNLESS(entry.name_offset < string_table.size(), Result_NspInvalidHeader);
            const char* name = string_table.data() + entry.name_offset;

            constexpr std::string_view cnmt_name{".cnmt"};
            std::string_view name_sv{name};
            if (!name_sv.ends_with(cnmt_name)) {
                continue;
            }

            ncm::PackagedContentMeta meta{};
            R_TRY(ReadExact(std::addressof(reader), std::addressof(meta), data_off + static_cast<s64>(entry.data_offset), sizeof(meta)));

            log_write("[game_file_scan] cnmt id: %016lX version: %u type: %u\n", meta.title_id, meta.title_version, meta.meta_type);
            out.push_back({meta.title_id, meta.title_version, meta.meta_type});
        }
    }

    R_SUCCEED();
}

auto CompareInstalled(const std::vector<CnmtInfo>& cnmts, ScanResult& out) -> void {
    out = {};
    if (cnmts.empty()) {
        return;
    }

    // priority order if a file contains multiple metas (e.g base+update+dlc).
    auto rank = [](InstallState state) {
        switch (state) {
            case InstallState::UpdateAvailable: return 0;
            case InstallState::New: return 1;
            case InstallState::Unknown: return 2;
            case InstallState::Older: return 3;
            case InstallState::UpToDate: return 4;
        }
        return 5;
    };

    for (const auto& cnmt : cnmts) {
        ScanResult r{};
        r.application_id = ncm::GetAppId(cnmt.meta_type, cnmt.title_id);
        r.state = InstallState::New;

        title::MetaEntries entries{};
        if (R_SUCCEEDED(title::GetMetaEntries(r.application_id, entries))) {
            for (const auto& e : entries) {
                if (e.meta_type != cnmt.meta_type) {
                    continue;
                }

                r.installed_version = e.version;
                if (cnmt.version > e.version) {
                    r.state = InstallState::UpdateAvailable;
                } else if (cnmt.version == e.version) {
                    r.state = InstallState::UpToDate;
                } else {
                    r.state = InstallState::Older;
                }
                break;
            }
        }

        // keep track of the highest file version for display purposes.
        if (cnmt.version > out.file_version) {
            out.file_version = cnmt.version;
        }

        if (out.application_id == 0) {
            // prefer the application id over patch/dlc ids.
            if (cnmt.meta_type == NcmContentMetaType_Application || out.state == InstallState::Unknown) {
                out.application_id = r.application_id;
            }
        }

        if (rank(r.state) < rank(out.state) || out.state == InstallState::Unknown) {
            out.state = r.state;
        }
    }
}

auto ScanFileInstallState(const std::shared_ptr<fs::Fs>& fs, const fs::FsPath& path, ScanResult& out) -> Result {
    std::vector<CnmtInfo> cnmts;
    R_TRY(GetCnmts(fs, path, cnmts));
    CompareInstalled(cnmts, out);
    R_SUCCEED();
}

} // namespace sphaira::game_file_scan
