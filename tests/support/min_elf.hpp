#pragma once
// Test support shared by the executor and elfpatch tests.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

// A minimal but well-formed little-endian ELF64: header, program headers, an
// optional PT_INTERP string and a PT_DYNAMIC table holding `needed` DT_NEEDED
// entries. Enough for a reader of the headers to reach a verdict; a stand-in
// that is only the ELF magic reads as UNKNOWN and is patched as before.
inline void write_min_elf64(const std::filesystem::path& p, std::uint16_t machine, bool interp,
                     int needed) {
    std::string bytes(64, '\0');
    auto put = [&](std::size_t at, std::uint64_t v, int width) {
        if (bytes.size() < at + width) bytes.resize(at + width, '\0');
        for (int i = 0; i < width; ++i)
            bytes[at + i] = static_cast<char>((v >> (8 * i)) & 0xff);
    };
    const std::string interp_path = "/lib64/ld-linux-x86-64.so.2";
    const int phnum = interp ? 2 : 1;
    const std::size_t phoff = 64;
    const std::size_t interp_off = phoff + 56 * phnum;
    const std::size_t dyn_off = interp_off + (interp ? interp_path.size() + 1 : 0);
    const std::size_t dyn_size = 16 * (needed + 1);

    bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2;    // ELFCLASS64
    bytes[5] = 1;    // little endian
    bytes[6] = 1;    // EV_CURRENT
    put(16, 3, 2);             // e_type = ET_DYN
    put(18, machine, 2);       // e_machine
    put(20, 1, 4);             // e_version
    put(32, phoff, 8);         // e_phoff
    put(52, 64, 2);            // e_ehsize
    put(54, 56, 2);            // e_phentsize
    put(56, phnum, 2);         // e_phnum

    std::size_t ph = phoff;
    if (interp) {
        put(ph + 0, 3, 4);                          // PT_INTERP
        put(ph + 8, interp_off, 8);                 // p_offset
        put(ph + 32, interp_path.size() + 1, 8);    // p_filesz
        ph += 56;
    }
    put(ph + 0, 2, 4);                              // PT_DYNAMIC
    put(ph + 8, dyn_off, 8);
    put(ph + 32, dyn_size, 8);

    if (interp) {
        bytes.resize(interp_off);
        bytes += interp_path;
        bytes.push_back('\0');
    }
    bytes.resize(dyn_off + dyn_size, '\0');
    for (int i = 0; i < needed; ++i) put(dyn_off + 16 * i, 1, 8);  // DT_NEEDED

    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    std::filesystem::permissions(p,
                    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                    std::filesystem::perm_options::replace);
}
