#include "editor_tables.h"
#include <algorithm>
#include <cctype>

namespace edtables {

namespace {
struct Reader {
    const std::vector<uint8_t>& img;
    bool ok = true;
    bool in(uint32_t addr, uint32_t n) const {
        return addr >= kImageBase && uint64_t(addr - kImageBase) + n <= img.size();
    }
    uint8_t u8(uint32_t a) {
        if (!in(a, 1)) { ok = false; return 0; }
        return img[a - kImageBase];
    }
    uint16_t u16(uint32_t a) { return uint16_t(u8(a) << 8 | u8(a + 1)); }
    uint32_t u32(uint32_t a) { return uint32_t(u16(a)) << 16 | u16(a + 2); }
    std::string str(uint32_t a, size_t max = 256) {
        std::string s;
        for (size_t i = 0; i < max && in(a + uint32_t(i), 1); ++i) {
            const char c = char(img[a + uint32_t(i) - kImageBase]);
            if (!c) return s;
            s.push_back(c);
        }
        ok = false;
        return s;
    }
    // A long count, then that many longs.
    std::vector<uint32_t> array(uint32_t a, uint32_t limit = 4096) {
        const uint32_t n = u32(a);
        std::vector<uint32_t> v;
        if (!ok || n > limit || !in(a + 4, n * 4)) { ok = false; return v; }
        for (uint32_t i = 0; i < n; ++i) v.push_back(u32(a + 4 + 4 * i));
        return v;
    }
};

std::string squash(const std::string& s) {
    std::string out;
    for (char c : s) if (std::isalnum(static_cast<unsigned char>(c)) || c == '?') out.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    return out;
}
}  // namespace

bool isHiddenName(const std::string& name) {
    const std::string s = squash(name);
    if (s.find("spare") != std::string::npos) return true;
    if (!s.empty() && s.find_first_not_of('?') == std::string::npos) return true;   // an unused code: only '?'
    static const char* const kHidden[] = {"waveclass", "wavechecksum", "wavecksum", "wavechksum", "wavesum",
                                          "programclass", "progclass", "uselayer", "oldkeybdtrack", "soundfinder",
                                          "sndfinder", "finder"};
    for (const char* h : kHidden) if (s.find(h) != std::string::npos) return true;
    return false;
}

ValueList decodeList(const std::vector<uint8_t>& image, uint32_t address) {
    Reader r{image};
    ValueList list;
    uint32_t a = address;
    if (r.u8(a) == 0x80) ++a;                        // marker
    if (r.u8(a) == 0x05) {                           // header: value of the first entry
        list.hasFirst = true;
        list.first = int16_t(r.u16(a + 1));
        a += 3;
    }
    std::string prev, cur;
    for (int guard = 0; guard < 8192 && r.ok; ++guard) {
        const uint8_t c = r.u8(a++);
        if (!r.ok) break;
        if (c == 0x00 || c == 0x0D) {
            list.entries.push_back(cur);
            prev = cur;
            cur.clear();
            if (c == 0x00) break;
        } else if (c == 0x80) {                      // the next list's start marker: this list ends (one OS list has
            if (!cur.empty()) list.entries.push_back(cur);   // no terminator before it)
            break;
        } else if (c == 0x10) {                      // the rest of the previous entry, then the entry ends
            if (cur.size() < prev.size()) cur += prev.substr(cur.size());
            list.entries.push_back(cur);
            prev = cur;
            cur.clear();
            if (r.u8(a) == 0x0D) ++a;                // a separator right after it belongs to the same entry
            else if (r.u8(a) == 0x00) break;
        } else if (c >= 0x11 && c <= 0x1F) {         // copy (c - 0x10) characters from the same position of the previous entry
            const size_t n = c - 0x10;
            for (size_t i = 0; i < n; ++i) cur.push_back(cur.size() < prev.size() ? prev[cur.size()] : ' ');
        } else {
            cur.push_back(char(c));
        }
    }
    return list;
}

bool readTable(const std::vector<uint8_t>& image, uint32_t descriptor, const std::string& id, Table& out, std::string& err) {
    Reader r{image};
    out = Table();
    out.id = id;
    out.descriptor = descriptor;
    const uint32_t recs = r.u32(descriptor), namesA = r.u32(descriptor + 4), offsA = r.u32(descriptor + 8), listsA = r.u32(descriptor + 12);
    const uint32_t countFlags = r.u32(descriptor + 20);
    if (!r.ok) { err = id + ": descriptor outside the image"; return false; }
    const uint32_t count = countFlags >> 16;
    out.descriptorFlags = uint16_t(countFlags);
    const std::vector<uint32_t> names = r.array(namesA), offsets = r.array(offsA), lists = listsA ? r.array(listsA) : std::vector<uint32_t>();
    if (!r.ok) { err = id + ": names, offsets or lists array outside the image"; return false; }
    if (names.size() != count || offsets.size() != count) {
        err = id + ": the names (" + std::to_string(names.size()) + ") and offsets (" + std::to_string(offsets.size()) +
              ") arrays do not match the field count " + std::to_string(count);
        return false;
    }
    for (uint32_t la : lists) out.lists.push_back(decodeList(image, la));

    uint32_t a = recs;
    for (uint32_t i = 0; i < count; ++i) {
        while (r.in(a, 1) && image[a - kImageBase] == 0) ++a;    // padding between records
        const uint8_t len = r.u8(a);
        if (!r.ok || len < 8 || !r.in(a, len)) { err = id + ": field record " + std::to_string(i) + " is malformed"; return false; }
        Field f;
        f.index = int(i);
        f.name = r.str(names[i]);
        f.offset = offsets[i];
        f.flags = r.u8(a + 1);
        f.type = r.u16(a + 2);
        f.min = int16_t(r.u16(a + 4));
        f.max = int16_t(r.u16(a + 6));
        for (uint32_t s = a + 8; s + 2 <= a + len;) {           // sub-records: length, kind, ...
            const uint8_t sl = r.u8(s), kind = r.u8(s + 1);
            if (sl < 2) break;
            const uint32_t end = std::min<uint32_t>(s + sl, a + len);   // trust the record's length over the sub-record's
            if ((kind == 1 || kind == 9) && sl >= 8 && s + 8 <= end) {
                f.display = Display::Number;
                f.format = r.u16(s + 2);
                f.param = int32_t(r.u32(s + 4));
            } else if (kind == 3 && sl >= 12 && s + 12 <= end) {
                f.display = Display::List;
                f.listIndex = int(r.u32(s + 8));
            } else if (kind == 5 && sl >= 12 && s + 12 <= end) {
                f.display = Display::Formatter;
                f.formatter = int(r.u32(s + 8));
            }
            s += sl;
        }
        if (f.display == Display::List && (f.listIndex < 0 || f.listIndex >= int(out.lists.size()))) f.display = Display::Number;
        f.hidden = isHiddenName(f.name);
        out.fields.push_back(f);
        a += len;
    }
    if (!r.ok) { err = id + ": a name is outside the image or unterminated"; return false; }
    return true;
}

ArpText readArpText(const std::vector<uint8_t>& image) {
    Reader r{image};
    ArpText t;
    auto s = [&](uint32_t a) { return r.str(a, 16); };
    namespace A = arpaddr;
    t.tempoFmt = s(A::kTempoFmt); t.rangeFmt = s(A::kRangeFmt); t.resolutionCode = s(A::kResolutionCode);
    t.on = s(A::kOn); t.off = s(A::kOff); t.hold = s(A::kHold); t.centre = s(A::kCentre); t.low = s(A::kLow);
    t.high = s(A::kHigh); t.duration = s(A::kDuration); t.limit = s(A::kLimit); t.feel = s(A::kFeel); t.touch = s(A::kTouch);
    t.presetCode = s(A::kPresetCode); t.ownSettings = s(A::kOwnSettings); t.arpPresetFmt = s(A::kArpPresetFmt);
    for (size_t i = 0; i < 5; ++i) t.intervals[i] = s(A::kIntervals[i]);
    for (size_t i = 0; i < 12; ++i) t.resolution[i] = s(r.u32(A::kResolutionNames + 4 * uint32_t(i)));
    t.ok = r.ok;
    return t;
}

std::string formatNumber(const std::string& fmt, int v) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) { out.push_back(fmt[i]); continue; }
        size_t j = i + 1;
        if (fmt[j] == '%') { out.push_back('%'); i = j; continue; }
        const bool zero = j < fmt.size() && fmt[j] == '0';
        int width = 0;
        while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])) && width < 16) width = width * 10 + (fmt[j++] - '0');
        if (j < fmt.size() && fmt[j] == 'd') {
            std::string n = std::to_string(v);
            if (int(n.size()) < width) n.insert(0, size_t(width) - n.size(), zero ? '0' : ' ');
            out += n;
            i = j;
        } else {
            out.push_back(fmt[i]);
        }
    }
    return out;
}

OsTables readAll(const std::vector<uint8_t>& image) {
    OsTables t;
    std::string err;
    t.ok = readTable(image, kOscillatorTable, "oscillator", t.oscillator, err) &&
           readTable(image, kSoundSettingsTable, "sound settings", t.soundSettings, err) &&
           readTable(image, kSoundCommonTable, "sound common", t.soundCommon, err) &&
           readTable(image, kSystemTable, "system", t.system, err);
    t.error = err;
    if (t.ok) t.arp = readArpText(image);
    return t;
}

}  // namespace edtables
