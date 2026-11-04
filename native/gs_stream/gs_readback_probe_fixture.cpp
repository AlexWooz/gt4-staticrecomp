// CTest native_gs_readback_probe: the TASK-116 step 0 (P0 gate) probe aids, host only.
//   - download_mode_from_ini / download_mode_from_text: the HWDownloadMode of the very gt4-gs.ini the launcher writes;
//   - fnv1a64: the reply hash (the same function as the TASK-041 stream hash);
//   - ReadbackProbe: per-destination aggregation (calls, distinct replies, repeated, zero) and its two output forms.
// The probe is a test switch: with GT4_GS_READBACK_LOG unset it must do nothing at all.
#include "gs_readback_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gt4::gs_stream;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

const char *kModeKey = "HWDownloadMode";

void require_mode(const std::string &text, int expected, const char *label) {
  const int mode = download_mode_from_text(text);
  if (mode != expected) {
    std::fprintf(stderr, "[gs-readback-probe] %s: mode %d, expected %d\n", label, mode, expected);
    throw std::runtime_error(label);
  }
}

void check_ini_text() {
  require_mode("[EmuCore/GS]\nHWDownloadMode = 2\n", 2, "no-readbacks in its section");
  require_mode("[EmuCore/GS]\r\nHWDownloadMode=0\r\n", 0, "CRLF file, accurate");
  require_mode("[EmuCore/GS]\nHWDownloadMode   =   1  \n", 1, "spaces around the value");
  require_mode("[emucore/gs]\nhwdownloadmode=3\n", 3, "section and key case-insensitive");
  require_mode("; header\n[EmuCore/GS]\n; HWDownloadMode = 2\nHWDownloadMode = 0\n", 0, "a commented key is not read");
  require_mode("[EmuCore/GS]\n", -1, "key missing");
  require_mode("[EmuCore/GS]\nHWDownloadModeExtra = 2\n", -1, "a longer key is not the key");
  require_mode("HWDownloadMode = 2\n[EmuCore/GS]\n", -1, "key before its section");
  require_mode("[GT4GS]\nHWDownloadMode = 2\n", -1, "key in another section");
  require_mode("[EmuCore/GS]\nHWDownloadMode = 4\n", -1, "value out of range");
  require_mode("[EmuCore/GS]\nHWDownloadMode = -1\n", -1, "negative value");
  require_mode("[EmuCore/GS]\nHWDownloadMode = two\n", -1, "not a number");
  require_mode("[EmuCore/GS]\nHWDownloadMode =\n", -1, "empty value");
  // The launcher's Performance mode writes 2 into the same ini the GS process reads (docs/launcher/gs-settings.toml).
  const std::string launcher_ini = "; gt4-gs.ini\n[EmuCore/GS]\nRenderer = 14\nupscale_multiplier = 6\nHWDownloadMode = 2\n"
                                   "\n[Folders]\nCache = C:/cache\n\n[GT4GS]\nDataRoot = C:/data\nSurfaceless = true\n";
  require_mode(launcher_ini, 2, "launcher ini with Performance mode on");
  require_mode("[EmuCore/GS]\nHWDownloadMode = 0\n\n[Folders]\nCache = x\n", 0, "launcher ini with Performance mode off");
}

void check_ini_file(const std::filesystem::path &work) {
  const std::filesystem::path path = work / "gt4-gs.ini";
  { std::ofstream file(path, std::ios::binary); file << "[EmuCore/GS]\n" << kModeKey << " = 2\n"; }
  require(download_mode_from_ini(path.string().c_str()) == 2, "download_mode_from_ini must read the file");
  require(download_mode_from_ini((work / "missing.ini").string().c_str()) == -1, "a missing ini is unknown");
  require(download_mode_from_ini(nullptr) == -1, "a null path is unknown");
  require(download_mode_from_ini("") == -1, "an empty path is unknown");
}

void check_hash() {
  require(fnv1a64("", 0) == 0xCBF29CE484222325ull, "FNV-1a 64 offset basis");
  require(fnv1a64("a", 1) == 0xAF63DC4C8601EC8Cull, "FNV-1a 64 of 'a'");
  require(fnv1a64("foobar", 6) == 0x85944171F73967E8ull, "FNV-1a 64 of 'foobar'");
}

std::string read_text(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read the probe output");
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void require_contains(const std::string &text, const char *needle, const char *label) {
  if (text.find(needle) == std::string::npos) {
    std::fprintf(stderr, "[gs-readback-probe] %s: %s not in\n%s", label, needle, text.c_str());
    throw std::runtime_error(label);
  }
}

void check_probe(const std::filesystem::path &work) {
  ReadbackProbe off(false);
  std::vector<std::uint8_t> zeros(128 * 16, 0x00);
  std::vector<std::uint8_t> ones(128 * 16, 0x11);
  std::FILE *quiet = std::fopen((work / "probe-off.txt").string().c_str(), "wb");
  require(quiet != nullptr, "cannot open the probe output");
  off.record({1, 128, 0x0031A0C0u, 0, 10, zeros.data()}, quiet);
  off.print_summary(quiet);
  std::fclose(quiet);
  require(off.calls() == 0, "a disabled probe must not record");
  require(read_text(work / "probe-off.txt").empty(), "a disabled probe must print nothing");

  ReadbackProbe probe(true);
  std::vector<std::uint8_t> zeros512(512 * 16, 0x00);
  std::FILE *output = std::fopen((work / "probe-on.txt").string().c_str(), "wb");
  require(output != nullptr, "cannot open the probe output");
  probe.record({10, 128, 0x0031A0C0u, 0, 100, zeros.data()}, output);   // first: all zeros
  probe.record({11, 128, 0x0031A0C0u, 0, 200, ones.data()}, output);    // second: 0x11 everywhere
  probe.record({12, 128, 0x0031A0C0u, 0, 300, ones.data()}, output);    // third: repeats the second
  probe.record({13, 512, 0x00400000u, 1, 400, zeros512.data()}, output);  // another destination, zero-filled
  probe.print_summary(output);
  std::fclose(output);
  require(probe.calls() == 4, "every readback is counted");

  const std::string text = read_text(work / "probe-on.txt");
  std::fprintf(stderr, "%s", text.c_str());
  require_contains(text, "[gs-readback] f=10 dst=0x0031a0c0 qw=128 status=0 wait_us=100 nz=0", "per-readback line");
  require_contains(text, "[gs-readback] f=12 dst=0x0031a0c0 qw=128 status=0 wait_us=300 nz=2048", "non-zero line");
  require_contains(text, "same=0", "first reply is not a repeat");
  require_contains(text, "same=1", "third reply repeats the second");
  require_contains(text, "[gs-readback] summary: readbacks=4 destinations=2", "summary header");
  require_contains(text, "dst=0x0031a0c0 qw=128 calls=3 distinct=2 repeated=1 zero=1 nz=0..2048 statuses=0", "summary row");
  require_contains(text, "dst=0x00400000 qw=512 calls=1 distinct=1 repeated=0 zero=1 nz=0..0 statuses=1", "zero-filled row");
}

void check_environment_switch() {
  _putenv_s("GT4_GS_READBACK_LOG", "1");
  require(ReadbackProbe::enabled_from_environment(), "GT4_GS_READBACK_LOG=1 turns the probe on");
  _putenv_s("GT4_GS_READBACK_LOG", "2");
  require(ReadbackProbe::enabled_from_environment(), "any non-zero number turns the probe on");
  _putenv_s("GT4_GS_READBACK_LOG", "0");
  require(!ReadbackProbe::enabled_from_environment(), "GT4_GS_READBACK_LOG=0 keeps the probe off");
  _putenv_s("GT4_GS_READBACK_LOG", "");
  require(!ReadbackProbe::enabled_from_environment(), "an empty switch keeps the probe off");
  _putenv_s("GT4_GS_READBACK_LOG", "yes");
  require(!ReadbackProbe::enabled_from_environment(), "a non-number keeps the probe off (no accidental log in a run)");
  _putenv_s("GT4_GS_READBACK_LOG", "");
}

}  // namespace

int main(int argc, char **argv) {
  try {
    const std::filesystem::path work = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("readback-probe-work");
    std::error_code error;
    std::filesystem::create_directories(work, error);
    check_ini_text();
    check_ini_file(work);
    check_hash();
    check_probe(work);
    check_environment_switch();
    std::puts("native_gs_readback_probe: ok (ini parse, FNV-1a 64, probe aggregation, environment switch)");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "native_gs_readback_probe: %s\n", error.what());
    return 1;
  }
}
