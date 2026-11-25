// Builds small zip archives in memory for the Nano parser tests (stored or raw-deflated members,
// with the directory fields overridable so a test can lie about sizes and CRCs).
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <string>
#include <vector>

namespace nanotest {

struct ZipMember {
    std::string name;
    std::vector<uint8_t> data;
    int method = 0;          // 0 stored, 8 deflated
    bool fakeCrc = false;    // use crc below instead of the real one
    uint32_t crc = 0;
};

inline void put16(std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = (uint8_t)x; v[at + 1] = (uint8_t)(x >> 8); }
inline void put32(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; i++) v[at + i] = (uint8_t)(x >> (8 * i));
}

inline std::vector<uint8_t> rawDeflate(const std::vector<uint8_t>& in) {
    z_stream zs{};
    deflateInit2(&zs, 1, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);   // fast: the 24 MB cap test deflates 48 MB
    std::vector<uint8_t> out(deflateBound(&zs, in.size()));
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = (uInt)in.size();
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    deflate(&zs, Z_FINISH);
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

inline std::vector<uint8_t> makeZip(const std::vector<ZipMember>& members) {
    std::vector<uint8_t> z, cd;
    for (const auto& m : members) {
        const std::vector<uint8_t> data = m.method == 8 ? rawDeflate(m.data) : m.data;
        const uint32_t crc = m.fakeCrc ? m.crc : (uint32_t)crc32(0, m.data.data(), (uInt)m.data.size());
        const uint16_t nl = (uint16_t)m.name.size();
        const uint32_t lho = (uint32_t)z.size();
        std::vector<uint8_t> lh(30 + nl, 0);
        put32(lh, 0, 0x04034b50); put16(lh, 8, (uint16_t)m.method); put32(lh, 14, crc);
        put32(lh, 18, (uint32_t)data.size()); put32(lh, 22, (uint32_t)m.data.size()); put16(lh, 26, nl);
        memcpy(&lh[30], m.name.data(), nl);
        z.insert(z.end(), lh.begin(), lh.end());
        z.insert(z.end(), data.begin(), data.end());
        std::vector<uint8_t> c(46 + nl, 0);
        put32(c, 0, 0x02014b50); put16(c, 10, (uint16_t)m.method); put32(c, 16, crc);
        put32(c, 20, (uint32_t)data.size()); put32(c, 24, (uint32_t)m.data.size()); put16(c, 28, nl);
        put32(c, 42, lho);
        memcpy(&c[46], m.name.data(), nl);
        cd.insert(cd.end(), c.begin(), c.end());
    }
    const uint32_t cdOff = (uint32_t)z.size();
    z.insert(z.end(), cd.begin(), cd.end());
    std::vector<uint8_t> e(22, 0);
    put32(e, 0, 0x06054b50); put16(e, 8, (uint16_t)members.size()); put16(e, 10, (uint16_t)members.size());
    put32(e, 12, (uint32_t)cd.size()); put32(e, 16, cdOff);
    z.insert(z.end(), e.begin(), e.end());
    return z;
}

inline bool writeFile(const std::string& path, const void* data, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
inline bool writeFile(const std::string& path, const std::vector<uint8_t>& d) { return writeFile(path, d.data(), d.size()); }

}  // namespace nanotest
