// editor_tables: prints the editor's OS tables as decoded from the user's OS image (or the OS update file), to check
// the table layouts against the hardware. Terminal output only; writes nothing.
//
//   editor_tables <OS image or update file> [oscillator|sound|common|system]
//
// Per table: index, name, offset, record flags, value type, min, max, display kind, format flags, list index or
// formatter, hidden, the editor section, and the decoded list entries. The wave list exists only in RAM at run time
// and is not printed.
#include <cstdio>
#include <string>
#include "editor/editor_pages.h"
#include "editor/editor_tables.h"
#include "os_image.h"

namespace {
const char* displayName(edtables::Display d) {
    switch (d) {
    case edtables::Display::List: return "list";
    case edtables::Display::Formatter: return "formatter";
    default: return "number";
    }
}

void print(const edtables::Table& t, const char* which) {
    std::printf("== %s table (descriptor 0x%05X, flags 0x%04X): %zu fields, %zu value lists\n", t.id.c_str(), t.descriptor,
                t.descriptorFlags, t.fields.size(), t.lists.size());
    std::printf("%4s  %-28s %6s %5s %4s %6s %6s %-9s %6s %5s %-6s %s\n", "idx", "name", "offset", "flags", "type", "min", "max",
                "display", "format", "list", "hidden", "section");
    for (const edtables::Field& f : t.fields) {
        const char* osc = edpages::oscillatorSection(f.index);
        const std::string section = std::string(which) == "oscillator" ? (osc ? (*osc ? osc : "hidden") : "GEN")
                                    : std::string(which) == "sound" || std::string(which) == "common" ? edpages::soundSection(f.name)
                                                                                                       : std::string("-");
        const int ref = f.display == edtables::Display::List ? f.listIndex : f.display == edtables::Display::Formatter ? f.formatter : -1;
        std::printf("%4d  %-28s 0x%04X  0x%02X %4u %6d %6d %-9s 0x%04X %5d %-6s %s\n", f.index, ("\"" + f.name + "\"").c_str(), f.offset,
                    f.flags, f.type, f.min, f.max, displayName(f.display), f.format, ref, f.hidden ? "hidden" : "", section.c_str());
        if (f.param >= 0 && f.display == edtables::Display::Number && f.param != f.index)
            std::printf("      parameter number %d\n", f.param);
    }
    for (size_t i = 0; i < t.lists.size(); ++i) {
        const edtables::ValueList& l = t.lists[i];
        std::printf("  list %zu (%zu entries%s):", i, l.entries.size(), l.hasFirst ? (", first value " + std::to_string(l.first)).c_str() : "");
        for (const std::string& e : l.entries) std::printf(" [%s]", e.c_str());
        std::printf("\n");
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: editor_tables <OS image or update file> [oscillator|sound|common|system]\n");
        return 2;
    }
    OsImage os;
    std::string err;
    if (!os.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::printf("OS: %s, MD5 %s (%s)\n", osFormName(os.form), os.md5.c_str(), os.bytes.size() == 767196 ? "767,196 bytes" : "unexpected size");
    const std::string only = argc > 2 ? argv[2] : "";
    struct { const char* key; uint32_t desc; const char* id; } tables[] = {
        {"oscillator", edtables::kOscillatorTable, "oscillator"},
        {"sound", edtables::kSoundSettingsTable, "sound settings"},
        {"common", edtables::kSoundCommonTable, "sound common"},
        {"system", edtables::kSystemTable, "system"}};
    int failed = 0;
    for (const auto& t : tables) {
        if (!only.empty() && only != t.key) continue;
        edtables::Table table;
        const bool ok = edtables::readTable(os.bytes, t.desc, t.id, table, err);
        print(table, t.key);
        if (!ok) { std::printf("!! %s\n", err.c_str()); ++failed; }
    }
    return failed ? 1 : 0;
}
