// Audio In test (no ROM data). A 68k busy loop of our own stands in for the OS; the ESP2 runs a program of our own:
// DAC = ADC + an accumulator (a ramp tone when its step is not 0), so the input comes back on the plugin's output.
//  1. Level: a 1 kHz sine at -6 dBFS in comes out at -6 dBFS (both converters, 48 kHz host).
//  2. Latency: an impulse at host sample H comes out at H + the reported latency (+ the program's own 1-2 samples), at
//     44.1, 48 and 96 kHz; the input never runs dry.
//  3. Clip LED: lit by full-scale input (|word| >= 0x7FFFF0), not by -6 dBFS, off 100 ms after the last clip.
//  4. Silence in = the output without the input bus, sample for sample (tone program running).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "engine.h"
#include "machine.h"

namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---- ESP2 microcode (field layout of the public Object Format appendix, as in esp2_core_test)
struct Line {
    int alu = 0x0B, A = 0x3FF, B = 0x3F1, C = 0x3F1;
    int mac = 2, D = 0x1D4, E = 0x1CC, F = 0x3FF, sh = 6;
    int ag = 6, G = 0x200, rgn = 0, dl = 0;
};
std::vector<uint8_t> encode(const Line& l) {
    uint64_t hi = (uint64_t(l.A) << 22) | (uint64_t(l.B) << 12) | (uint64_t(l.C) << 2) | uint64_t(l.alu >> 3);
    uint64_t lo = (uint64_t(l.alu & 7) << 61) | (uint64_t(l.D) << 50) | (uint64_t(l.E) << 40) | (uint64_t(l.F) << 30) |
                  (uint64_t(l.mac) << 25) | (uint64_t(l.sh) << 21) | (uint64_t(l.G & 0x1FF) << 11) | (uint64_t(l.ag) << 8) |
                  (uint64_t(l.rgn) << 5) | (uint64_t(l.dl) << 1);
    std::vector<uint8_t> w(12);
    for (int i = 0; i < 4; ++i) w[size_t(i)] = uint8_t(hi >> (24 - 8 * i));
    for (int i = 0; i < 8; ++i) w[size_t(4 + i)] = uint8_t(lo >> (56 - 8 * i));
    return w;
}
void hostReg(Esp2Core& e, uint32_t a, uint32_t v) {
    e.write8(0x11, uint8_t(v >> 16)); e.write8(0x12, uint8_t(v >> 8)); e.write8(0x13, uint8_t(v));
    e.write8(0x14, uint8_t(a >> 8)); e.write8(0x15, uint8_t(a)); e.write8(0x17, 0x80);
}
void hostIns(Esp2Core& e, uint32_t a, const std::vector<uint8_t>& w) {
    for (int i = 0; i < 12; ++i) e.write8(uint32_t(i), w[size_t(i)]);
    e.write8(0x0C, uint8_t(a >> 8)); e.write8(0x0D, uint8_t(a)); e.write8(0x0F, 0x80);
}
// ADC -> 0FF/0FE; 0FD = 011 + 0FF (left), 0FC = 0FE (right); 011 += 010; 0FD/0FC -> DAC; BIOZ; jump to 0.
void loadProgram(Esp2Core& e, uint32_t step) {
    std::vector<Line> p;
    Line in; in.B = 0x3EF; in.C = 0x0FF; in.mac = 3; in.D = 0x3EE; in.E = 0x1CB; in.F = 0x0FE; in.sh = 7; p.push_back(in);
    p.push_back(Line());
    Line mix; mix.alu = 0x00; mix.A = 0x011; mix.B = 0x0FF; mix.C = 0x0FD; mix.mac = 3; mix.D = 0x0FE; mix.E = 0x1CB; mix.F = 0x0FC; mix.sh = 7; p.push_back(mix);
    Line acc; acc.alu = 0x00; acc.A = 0x010; acc.B = 0x011; acc.C = 0x011; p.push_back(acc);
    p.push_back(Line());
    Line out; out.B = 0x0FD; out.C = 0x3ED; out.mac = 3; out.D = 0x0FC; out.E = 0x1CB; out.F = 0x3EC; out.sh = 7; p.push_back(out);
    Line bioz; bioz.alu = 0x19; p.push_back(bioz);
    Line jmp; jmp.alu = 0x1C; jmp.A = 0; jmp.B = 0; jmp.C = 0x3FF; p.push_back(jmp);
    p.push_back(Line());
    for (size_t i = 0; i < p.size(); ++i) hostIns(e, uint32_t(i), encode(p[i]));
    for (uint32_t a = 0x0FC; a <= 0x0FF; ++a) hostReg(e, a, 0);
    hostReg(e, 0x010, step); hostReg(e, 0x011, 0);
    e.write8(0x19, 0x31);                                // run, IOZ enabled
}

// main() at 0x4080: count in d1 forever (no `bra *`, which the engine would report as a stall)
const uint16_t kBusy[] = {0x2200, 0x5281, 0x0C81, 0x0000, 0xFFFF, 0x66F6, 0x60F2};

struct Rig {
    Engine e;
    Rig(const fs::path& dir, uint32_t step) {
        e.setRoms((dir / "os").string(), (dir / "wave").string(), "os-md5", "wave-md5");
        for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(e.state() == Engine::State::Running);
        e.withMachineForTest([&](Machine& m) { loadProgram(m.esp2, step); });
    }
    // Runs `in` (left = right) through the engine in blocks; returns the left output.
    std::vector<float> run(double rate, bool bus, const std::vector<float>& in, int block = 256) {
        e.prepare(rate, block, bus);
        std::vector<float> out(in.size()), l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
        for (size_t p = 0; p < in.size(); p += size_t(block)) {
            const int n = int(std::min(in.size() - p, size_t(block)));
            e.process(l.data(), r.data(), n, nullptr, 0, bus ? in.data() + p : nullptr, bus ? in.data() + p : nullptr);
            std::copy(l.begin(), l.begin() + n, out.begin() + long(p));
        }
        return out;
    }
};
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_audio_in_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kBusy / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kBusy[i] >> 8); os[0x81 + 2 * i] = uint8_t(kBusy[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }

    // 1. Level: 1 kHz at -6 dBFS (0.5), 48 kHz host. Measured as RMS over whole periods after settling.
    {
        Rig rig(dir, 0);
        const double rate = 48000;
        std::vector<float> in(static_cast<size_t>(rate));
        for (size_t i = 0; i < in.size(); ++i) in[i] = float(0.5 * std::sin(2 * M_PI * 1000.0 * double(i) / rate));
        const std::vector<float> out = rig.run(rate, true, in);
        double si = 0, so = 0;
        for (size_t i = 24000; i < 24000 + 4800; ++i) { si += double(in[i]) * in[i]; so += double(out[i]) * out[i]; }
        const double db = 10 * std::log10(so / si);
        std::printf("audio_in_test: 1 kHz at -6 dBFS through Audio In: output %+.3f dB relative to input\n", db);
        CHECK(std::fabs(db) < 0.05);
        CHECK(rig.e.inputUnderrunsForTest() == 0);
        CHECK(!rig.e.inputClip());                        // -6 dBFS does not light the clip LED
    }

    // 2. Latency: impulse at host sample H comes back at H + reported latency (+ the program's own delay).
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        Rig rig(dir, 0);
        std::vector<float> in(static_cast<size_t>(rate * 0.5), 0.0f);
        const size_t H = size_t(rate * 0.2) + 37;
        in[H] = 0.9f;
        const std::vector<float> out = rig.run(rate, true, in);
        size_t peak = 0;
        for (size_t i = 0; i < out.size(); ++i) if (std::fabs(out[i]) > std::fabs(out[peak])) peak = i;
        const int lat = rig.e.latencySamples();
        const long extra = long(peak) - long(H) - lat;
        std::printf("audio_in_test: %.0f Hz: reported latency %d samples, impulse back after %ld (+%ld)\n", rate, lat, long(peak - H), extra);
        CHECK(extra >= 0 && extra <= 3);
        CHECK(std::fabs(out[peak]) > 0.25f);              // a one-sample click, band-limited to 20 kHz (about 0.37 at 96 kHz)
        CHECK(rig.e.inputUnderrunsForTest() == 0);
        // the reported latency without the bus, for comparison
        Engine probe; probe.prepare(rate, 256, false);
        std::printf("audio_in_test: %.0f Hz: latency without Audio In %d samples\n", rate, probe.latencySamples());
        CHECK(lat >= probe.latencySamples());
    }

    // 3. Clip LED: full scale lights it; it goes out 100 ms after the input drops below the clip level.
    {
        Rig rig(dir, 0);
        const double rate = 48000;
        std::vector<float> loud(4800, 1.0f), quiet(4800 * 3, 0.25f);
        rig.e.prepare(rate, 256, true);
        std::vector<float> l(256), r(256);
        bool litDuringLoud = false;
        auto feed = [&](const std::vector<float>& x) {
            for (size_t p = 0; p < x.size(); p += 256) {
                rig.e.process(l.data(), r.data(), 256, nullptr, 0, x.data() + p, x.data() + p);
                litDuringLoud |= rig.e.inputClip();
            }
        };
        feed(loud);
        CHECK(litDuringLoud);
        feed(quiet);                                     // 300 ms below the clip level
        CHECK(!rig.e.inputClip());
    }

    // 4. Silence in: the same output as without the input bus (tone program running, so the output is not silent).
    {
        Rig a(dir, 0x001000), b(dir, 0x001000);
        const std::vector<float> zeros(48000, 0.0f);
        const std::vector<float> outA = a.run(48000, false, zeros), outB = b.run(48000, true, zeros);
        double energy = 0; for (float x : outA) energy += double(x) * x;
        CHECK(energy > 1.0);
        CHECK(outA == outB);
        std::printf("audio_in_test: silence in: output %s the output without Audio In\n", outA == outB ? "equals" : "DIFFERS FROM");
    }

    fs::remove_all(dir);
    std::printf("audio_in_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
