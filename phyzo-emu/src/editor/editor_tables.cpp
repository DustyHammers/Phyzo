#include "editor_tables.h"
#include <algorithm>
#include <cctype>
#include <map>

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
        while (j < fmt.size() && fmt[j] == 'l') ++j;
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

// ------------------------------------------------------------------ effects

namespace {
// A RAM address in the initialised data section, read from its image copy; image addresses unchanged.
uint32_t imageOf(uint32_t a) {
    using namespace fxaddr;
    return a >= kDataRam && a < kDataRam + 0x100000 ? kDataImage + (a - kDataRam) : a;
}

struct FxReader {
    Reader r;
    std::vector<uint8_t> recordSizes, displaySizes;  // the OS's walker tables (bytes)
    std::map<uint32_t, ValueList> lists;             // decoded value lists by address
    // The start of sub-block n of `block` (the OS's walker): byte 1 is the mask of the sub-blocks present; the first
    // starts after the header (table[0]); each present one before n adds table[1 + i], 0xFF: its own length byte.
    bool walk(uint32_t block, int n, const std::vector<uint8_t>& table, uint32_t& out) {
        if (n + 1 >= int(table.size())) return false;
        const uint8_t mask = r.u8(block + 1);
        uint32_t pos = block + table[0];
        for (int i = 0; i < n; ++i) {
            if (!(mask & (1 << i))) continue;
            const uint8_t size = table[size_t(1 + i)];
            pos += size == 0xFF ? r.u8(pos) : size;
        }
        if (!(mask & (1 << n)) || !r.ok) return false;
        out = pos;
        return true;
    }
    const ValueList& list(uint32_t a) {
        auto it = lists.find(a);
        if (it == lists.end()) it = lists.emplace(a, decodeList(r.img, a)).first;
        return it->second;
    }
    // One record at `a` (byte 0 = its length): range, display and PID (`hasPid` false without section 7).
    void record(uint32_t a, const std::vector<std::string>& names, uint32_t listArray, FxParam& p, bool& hasPid) {
        p.min = int16_t(r.u16(a + 4));
        p.max = int16_t(r.u16(a + 6));
        uint32_t s0, at;
        if (walk(a, 0, recordSizes, s0)) {
            if (walk(s0, 0, displaySizes, at)) {
                const uint32_t n = r.u32(at);
                if (n < names.size()) p.name = names[n];
            }
            if (walk(s0, 1, displaySizes, at)) {
                const uint32_t n = r.u32(at), count = r.u32(listArray);
                if (r.ok && n < count && n < 4096) { p.hasList = true; p.list = list(r.u32(listArray + 4 + 4 * n)).entries; }
            }
            if (walk(s0, 2, displaySizes, at)) p.formatter = int(r.u32(at));
            if (walk(s0, 3, displaySizes, at)) { p.prefix = r.u8(at); p.suffix = r.u8(at + 1); }
            if (walk(s0, 4, displaySizes, at)) p.decimals = std::min<int>(r.u8(at + 1), 6);
        }
        hasPid = walk(a, 7, recordSizes, at);
        if (hasPid) p.pid = r.u16(at);
    }
    std::vector<std::string> names(uint32_t a) {
        std::vector<std::string> out;
        for (uint32_t p : r.array(imageOf(a), 1024)) out.push_back(r.str(imageOf(p), 64));
        return out;
    }
    // A family descriptor's records (byte 0 = length; zero bytes between records are padding): each record's PID is
    // its section 7, or the previous id + 1 (from 0) without one.
    bool family(uint32_t desc, int fam, std::map<uint16_t, FxParam>& out, std::vector<FxParam>* inOrder) {
        const uint32_t recs = imageOf(r.u32(desc)), namesA = r.u32(desc + 4), listsA = imageOf(r.u32(desc + 12));
        const uint16_t count = r.u16(desc + 0x14);
        if (!r.ok) return false;
        const std::vector<std::string> nm = names(namesA);
        uint32_t a = recs;
        int prevId = -1;
        for (uint16_t i = 0; i < count && r.ok; ++i) {
            while (r.in(a, 1) && r.u8(a) == 0) ++a;
            const uint8_t len = r.u8(a);
            if (!r.ok || len < 8) return false;
            FxParam p;
            bool hasPid = false;
            record(a, nm, listsA, p, hasPid);
            if (!hasPid) p.pid = uint16_t(fam << 8 | ((prevId + 1) & 0xFF));
            prevId = p.pid & 0xFF;
            p.found = true;
            if (inOrder) inOrder->push_back(p);
            out.emplace(p.pid, p);
            a += len;
        }
        return r.ok;
    }
};

std::string fixed16(Reader& r, uint32_t a) {         // a 16-character field ('~' shown as a space)
    std::string s;
    for (uint32_t i = 0; i < 16 && r.in(a + i, 1); ++i) {
        const char c = char(r.u8(a + i));
        if (!c) break;
        s.push_back(c == '~' ? ' ' : c);
    }
    return s;
}
}  // namespace

FxTables readEffects(const std::vector<uint8_t>& image) {
    using namespace fxaddr;
    FxTables t;
    FxReader x{Reader{image}, {}, {}, {}};
    Reader& r = x.r;
    for (int i = 0; i < 9; ++i) x.recordSizes.push_back(r.u8(kRecordSizes + uint32_t(i)));
    for (int i = 0; i < 6; ++i) x.displaySizes.push_back(r.u8(kDisplaySizes + uint32_t(i)));
    if (!r.ok) { t.error = "effects: walker tables outside the image"; return t; }
    // Every family's records by PID.
    std::map<uint16_t, FxParam> params;
    for (int f = 0; f < kFamilyCount; ++f) {
        const uint32_t d = r.u32(kFamilies + 4 * uint32_t(f));
        if (!r.ok) break;
        if (d && !x.family(d, f, params, nullptr)) { t.error = "effects: family " + std::to_string(f) + " is malformed"; return t; }
    }
    for (int i = 0; i < kSyncCount; ++i) t.syncNames.push_back(r.str(r.u32(kSyncNames + 4 * uint32_t(i)), 32));
    t.msFormat = r.str(kMsFormat, 16);
    for (const std::string& e : x.list(r.u32(kPrefixList)).entries) t.prefixes.push_back(e);
    for (const std::string& e : x.list(r.u32(kSuffixList)).entries) t.suffixes.push_back(e);
    auto sync = [&](FxParam& p) {
        bool same = p.hasList && p.list.size() >= size_t(kSyncCount);
        for (int i = 0; same && i < kSyncCount; ++i) same = p.list[size_t(i)] == t.syncNames[size_t(i)];
        p.tempoSync = p.formatter == 80 || same;
    };
    for (auto& kv : params) sync(kv.second);
    // Edit-order maps: family, sub, count, count slots.
    std::vector<uint32_t> maps;
    for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t m = r.u32(kEditOrder + 4 * i);
        if (!r.ok || !m) break;
        maps.push_back(m);
    }
    auto algo = [&](uint32_t h) {
        FxAlgo a;
        if (!h) return a;
        const uint32_t namePtr = r.u32(h);
        a.name = namePtr ? fixed16(r, namePtr) : std::string();
        a.family = r.u16(h + 6);
        a.sub = r.u16(h + 8);
        const uint16_t n = r.u16(h + 0x0E);
        const uint32_t map = r.u32(h + 0x10);
        const uint16_t nv = r.u16(h + 0x18);
        const uint32_t vt = r.u32(h + 0x1A);
        if (!r.ok || n > 64 || nv > 128) return a;
        for (uint16_t k = 0; k < n; ++k) {
            const uint32_t e = map + 16u * k;
            const uint16_t pid = uint16_t((r.u16(e) & 0xFF) << 8 | (r.u16(e + 2) & 0xFF));
            auto it = params.find(pid);
            FxParam p;
            if (it != params.end()) p = it->second;
            p.pid = pid;
            a.params.push_back(p);
        }
        for (uint16_t v = 0; v < nv; ++v) a.variations.push_back(fixed16(r, r.u32(vt + 16u * v)));
        bool mapped = false;
        for (uint32_t m : maps) {
            if (r.u8(m) != (a.family & 0xFF) || r.u8(m + 1) != (a.sub & 0xFF)) continue;
            const uint8_t count = r.u8(m + 2);
            for (uint8_t i = 0; i < count; ++i) if (r.u8(m + 3 + i) < n) a.order.push_back(r.u8(m + 3 + i));
            mapped = true;
            break;
        }
        if (!mapped) for (int k = 0; k < n; ++k) if (!a.params[size_t(k)].hidden()) a.order.push_back(k);
        a.ok = r.ok;
        return a;
    };
    for (int i = 0; i < kInsertCount; ++i) t.inserts.push_back(algo(r.u32(kInsertAlgos + 4 * uint32_t(i))));
    t.routing = algo(r.u32(kRoutingAlgo));
    t.reverb = algo(r.u32(kReverbAlgo));
    for (int i = 0; i < kInsertCount; ++i) t.codes.push_back(r.str(kCodes + 5 * uint32_t(i), 5));
    t.preset = r.str(kPreset, 8);
    t.insertVariationFmt = r.str(kInsertVariationFmt, 8);
    t.reverbVariationFmt = r.str(kReverbVariationFmt, 8);
    // The insert modulation: an ordinary descriptor whose records, names and offsets are in RAM (image copy).
    {
        std::map<uint16_t, FxParam> byPid;
        std::vector<FxParam> inOrder;
        const uint32_t d = kModDescriptor;
        if (x.family(d, 0, byPid, &inOrder)) {
            const std::vector<uint32_t> offs = r.array(imageOf(r.u32(d + 8)), 64);
            for (size_t i = 0; i < inOrder.size() && i < offs.size(); ++i) {
                sync(inOrder[i]);
                t.mod.push_back(inOrder[i]);
                t.modOffsets.push_back(offs[i]);
            }
        }
    }
    bool all = r.ok && t.routing.ok && t.reverb.ok && t.routing.params.size() >= 32 && t.mod.size() == 6;
    for (const FxAlgo& a : t.inserts) all = all && a.ok;
    t.ok = all;
    if (!all && t.error.empty()) t.error = "effects: an algorithm, the routing shell, the reverb or the modulation fields could not be read";
    return t;
}

std::string formatEffect(const FxParam& p, int v, const FxTables& t, const FxAlgo* insert) {
    auto trim = [](std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') ++i;
        return s.substr(i);
    };
    if (p.hasList) {
        if (v >= 0 && size_t(v) < p.list.size()) return trim(p.list[size_t(v)]);
        return std::to_string(v);
    }
    if (p.formatter == 80) {
        if (v >= 0 && v < int(t.syncNames.size())) return trim(t.syncNames[size_t(v)]);
        return formatNumber(t.msFormat, v - int(t.syncNames.size()));
    }
    if (p.formatter == 81) return formatNumber(t.msFormat, v);
    if (p.formatter == 82) {
        if (insert && v >= 0 && size_t(v) < insert->params.size()) return trim(insert->params[size_t(v)].name);
        return std::to_string(v);
    }
    std::string n;
    if (p.decimals > 0) {
        int scale = 1;
        for (int i = 0; i < p.decimals; ++i) scale *= 10;
        const int a = v < 0 ? -v : v;
        std::string frac = std::to_string(a % scale);
        frac.insert(0, size_t(p.decimals) - frac.size(), '0');
        n = (v < 0 ? "-" : "") + std::to_string(a / scale) + "." + frac;
    } else {
        n = std::to_string(v);
    }
    auto from = [](const std::vector<std::string>& l, uint8_t i) { return i >= 1 && size_t(i) <= l.size() ? l[size_t(i - 1)] : std::string(); };
    return from(t.prefixes, p.prefix) + n + from(t.suffixes, p.suffix);
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
    if (t.ok) t.fx = readEffects(image);
    return t;
}

}  // namespace edtables
