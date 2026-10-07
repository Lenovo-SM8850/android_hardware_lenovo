// SPDX-License-Identifier: Apache-2.0
#include "MbnParser.h"
#include <algorithm>
#include <limits>

namespace mcfg {
namespace {
class Reader {
  public:
    Reader(const Bytes& bytes, size_t begin, size_t end) : bytes_(bytes), pos_(begin), end_(end) {
        require(begin <= end && end <= bytes.size(), "MBN range");
    }
    uint64_t number(size_t count) {
        require(count <= 8 && count <= end_ - pos_, "MBN truncated field");
        uint64_t value = 0;
        for (size_t n = 0; n < count; ++n) value |= uint64_t(bytes_[pos_++]) << (8 * n);
        return value;
    }
    Bytes take(size_t count) {
        require(count <= end_ - pos_, "MBN truncated data");
        Bytes result(bytes_.begin() + pos_, bytes_.begin() + pos_ + count);
        pos_ += count;
        return result;
    }
    size_t position() const { return pos_; }

  private:
    const Bytes& bytes_;
    size_t pos_, end_;
};
}  // namespace
Snapshot parseMbn(const Bytes& mbn, const std::set<std::string>& allow) {
    require(mbn.size() >= 52 && mbn.size() <= 8 * 1024 * 1024, "MBN size");
    require(mbn[0] == 0x7f && mbn[1] == 'E' && mbn[2] == 'L' && mbn[3] == 'F' &&
                    (mbn[4] == 1 || mbn[4] == 2) && mbn[5] == 1 && mbn[6] == 1,
            "MBN ELF format");
    const bool wide = mbn[4] == 2;
    Reader header(mbn, wide ? 32 : 28, mbn.size());
    const uint64_t phoff = header.number(wide ? 8 : 4);
    Reader counts(mbn, wide ? 54 : 42, mbn.size());
    const uint64_t phsize = counts.number(2), phcount = counts.number(2);
    require(phsize == (wide ? 56 : 32) && phcount >= 3 && phcount <= 64 && phoff <= mbn.size() &&
                    phcount * phsize <= mbn.size() - phoff,
            "MBN ELF segments");
    Reader segment(mbn, phoff + 2 * phsize, phoff + 3 * phsize);
    require(segment.number(4) == 1, "MBN MCFG segment type");
    if (wide) segment.number(4);
    const uint64_t offset = segment.number(wide ? 8 : 4);
    segment.number(wide ? 8 : 4);
    segment.number(wide ? 8 : 4);
    const uint64_t length = segment.number(wide ? 8 : 4);
    segment.number(wide ? 8 : 4);
    if (!wide) segment.number(4);
    require(segment.number(wide ? 8 : 4) == 4 && length % 4 == 0 && length >= 32 &&
                    offset <= mbn.size() && length <= mbn.size() - offset,
            "MBN MCFG bounds/alignment");
    Reader body(mbn, offset, offset + length);
    require(body.take(4) == Bytes({'M', 'C', 'F', 'G'}), "MBN MCFG magic");
    body.number(2);
    require(body.number(2) == 1, "MBN hardware configuration refused");
    const auto count = body.number(4);
    require(count >= 2 && count <= 65536, "MBN item count");
    body.number(2);
    body.number(2);
    require(body.number(2) == 4995, "MBN MCFG version");
    const auto versionLength = body.number(2);
    body.take(versionLength);
    Snapshot raw;
    for (uint64_t n = 1; n < count; ++n) {
        const auto start = body.position();
        const auto size = body.number(4), type = body.number(1);
        body.number(1);
        body.number(2);
        require(size >= 8 && size <= offset + length - start, "MBN item bounds");
        Reader item(mbn, body.position(), start + size);
        if (type == 2 || type == 4) {
            require(item.number(2) == 1, "MBN filename magic");
            const auto nameLength = item.number(2);
            auto name = item.take(nameLength);
            require(item.number(2) == 2, "MBN file size magic");
            const auto dataLength = item.number(2);
            require(size == 16 + nameLength + dataLength, "MBN file item length");
            auto data = item.take(dataLength);
            while (!name.empty() && name.back() == 0) name.pop_back();
            require(!name.empty() && std::find(name.begin(), name.end(), 0) == name.end(),
                    "MBN filename");
            std::string path(name.begin(), name.end());
            std::replace(path.begin(), path.end(), '\\', '/');
            path = "/" + path.substr(path.find_first_not_of('/'));
            require(path.find("//") == std::string::npos &&
                            path.find("/../") == std::string::npos &&
                            path.find("/./") == std::string::npos && path.back() != '/',
                    "MBN path traversal");
            if (allow.count(path)) raw[path] = std::move(data);  // MCFG order, last write wins.
        } else if (type == 1) {
            item.number(2);
            const auto dataLength = item.number(2);
            require(size == 12 + dataLength, "MBN numeric NV length");
            item.take(dataLength);
        }
        body.take(size - 8);
    }
    const auto trailerStart = body.position();
    const auto trailerLength = body.number(4);
    require(trailerLength >= 10 && trailerLength <= offset + length - trailerStart,
            "MBN trailer bounds");
    require(body.number(2) == 10, "MBN trailer type");
    body.number(2);
    require(body.number(2) == 0xa1, "MBN trailer magic");
    body.take(trailerLength - 10);
    const auto padding = offset + length - body.position();
    require(padding >= 1 && padding <= 4, "MBN padding length");
    Reader end(mbn, offset + length - 4, offset + length);
    require(end.number(4) == trailerLength + padding, "MBN trailer padding");
    require(!raw.empty() && raw.size() <= 85, "MBN no allowlisted file items");
    for (auto& [path, value] : raw) {
        require(value && !value->empty() && value->front() == 7, "MBN encoding marker");
        if (path != "/efsprofiles/overideconfig") value->erase(value->begin());
    }
    return raw;
}
}  // namespace mcfg
