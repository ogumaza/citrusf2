// SPDX-License-Identifier: MIT

// citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont and SFZ.
//
//     citrusf2 [options] <archive.bcsar> [<archive.bcsar> ...]
//
// Output goes beside each archive. Windows accepts files dropped on the executable; macOS uses Citrusf2.app.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "convert.h"
#include "csar.h"
#include "performance.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// shellapi.h, for CommandLineToArgvW, needs windows.h first.
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace citrusf2;

namespace
{

// Command-line options.
struct Options
{
    std::vector<std::string> archives;
    std::string output; // override the output directory beside the archive
    std::string only;
    bool list = false;
    bool quiet = false;
    bool help = false;
    bool version = false;
    PerformOptions perform;
};

// Convert a UTF-8 string to a filesystem path.
fs::path PathFromUtf8(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

// Convert a filesystem path to UTF-8.
std::string Utf8(const fs::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Print usage and options.
void Usage()
{
    std::puts(
        "citrusf2: convert 3DS sound archive sequences (.bcsar) to MIDI, SoundFont 2 and\n"
        "SFZ.\n"
        "\n"
        "usage: citrusf2 [options] <archive.bcsar> [<archive.bcsar> ...]\n"
        "\n"
        "Each sequence gets <name>.mid, <name>.sf2, and per-track SFZ files in sfz/<name>.\n"
        "Shared samples go in sfz/samples. Output is written to <archive name>_citrusf2\n"
        "beside the archive. You can also drop archives onto citrusf2.exe on Windows\n"
        "or Citrusf2.app on macOS.\n"
        "\n"
        "options:\n"
        "  --output DIR   write to DIR (one archive only)\n"
        "  --only REGEX   convert matching sequence names (for example --only BGM)\n"
        "  --loops N      repeat the main loop N times (default 1)\n"
        "                 MIDI loop markers are included for players that support them\n"
        "  --seed N       initial game PRNG state (default 0x12345678)\n"
        "  --list         list sequences without converting\n"
        "  --quiet        print errors only\n"
        "  --version      show the version\n");
}

// Parse arguments, stopping at --help or --version. Throw std::runtime_error for unknown options, missing values or
// invalid values.
Options ParseOptions(const std::vector<std::string>& args)
{
    Options o;
    for (std::size_t i = 1; i < args.size(); i++)
    {
        const std::string& a = args[i];

        auto value = [&]() -> std::string
        {
            if (i + 1 >= args.size())
            {
                throw std::runtime_error(a + " needs a value");
            }

            return args[++i];
        };

        // Parse an integer no greater than `max`, in decimal or hexadecimal with a 0x prefix.
        auto number = [&](uint32_t max)
        {
            const std::string v = value();
            const bool hex = v.starts_with("0x") || v.starts_with("0X");
            const char* last = v.data() + v.size();
            uint32_t n = 0;
            const auto [end, error] = std::from_chars(v.data() + (hex ? 2 : 0), last, n, hex ? 16 : 10);
            if (error == std::errc::invalid_argument || end != last)
            {
                throw std::runtime_error(a + " needs a number, not " + v);
            }
            if (error == std::errc::result_out_of_range || n > max)
            {
                throw std::runtime_error(a + " " + v + " is out of range");
            }

            return n;
        };

        if (a == "--output" || a == "-o")
        {
            o.output = value();
        }
        else if (a == "--only")
        {
            o.only = value();
        }
        else if (a == "--loops")
        {
            o.perform.loops = std::max(1, static_cast<int>(number(std::numeric_limits<int>::max())));
        }
        else if (a == "--seed")
        {
            o.perform.seed = number(std::numeric_limits<uint32_t>::max());
        }
        else if (a == "--list")
        {
            o.list = true;
        }
        else if (a == "--quiet" || a == "-q")
        {
            o.quiet = true;
        }
        else if (a == "--help" || a == "-h")
        {
            o.help = true;
            return o;
        }
        else if (a == "--version")
        {
            o.version = true;
            return o;
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            throw std::runtime_error("unknown option " + a);
        }
        else
        {
            o.archives.push_back(a);
        }
    }

    return o;
}

// Read a file; return an empty vector if it cannot be opened.
std::optional<std::vector<uint8_t>> ReadFile(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return std::nullopt;
    }

    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Write `data` to a file. Check after closing, when the final buffered bytes are flushed, and name the file in any
// std::runtime_error.
void WriteFile(const fs::path& path, std::span<const uint8_t> data)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    f.close();
    if (!f)
    {
        throw std::runtime_error("can't write " + Utf8(path));
    }
}

// Write `text` verbatim. Throw std::runtime_error if the write fails.
void WriteFile(const fs::path& path, const std::string& text)
{
    WriteFile(path, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

// Lowercase the ASCII letters in `s` to detect file name collisions on Windows and macOS.
std::string AsciiLower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return s;
}

// Seconds as minutes and seconds, like 1:05.3.
std::string Duration(double seconds)
{
    int m = static_cast<int>(seconds / 60.0);
    double s = seconds - m * 60.0;
    if (s >= 59.95) // %04.1f would round it up to 60.0
    {
        m++;
        s = 0.0;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%04.1f", m, s);

    return buf;
}

// Print the sequence's duration, loop, channel/preset/sample counts, approximations and warnings.
void PrintConversion(const std::string& name, const Conversion& c)
{
    const Performance& p = c.performance;
    std::string loop;
    if (p.loop)
    {
        loop = p.loops > 1 ? ", looped " + std::to_string(p.loops) + " times" : ", loops";
    }
    else if (p.holds)
    {
        loop = ", holds until stopped";
    }
    else if (p.truncated)
    {
        loop = ", cut at the time limit";
    }

    std::printf("  %-28s %8s%s, %d channels, %zu presets, %zu samples\n", name.c_str(), Duration(p.seconds).c_str(),
                loop.c_str(), c.channels, p.presets.size(), c.samples);
    for (const auto& [what, n] : p.approximations)
    {
        std::printf("      approximated: %s (%u)\n", what.c_str(), n);
    }
    for (const std::string& w : c.warnings)
    {
        std::printf("      warning: %s\n", w.c_str());
    }
}

// Get command-line arguments as UTF-8. On Windows, read the wide command line because argv uses the ANSI code page.
std::vector<std::string> Arguments(int argc, char** argv)
{
    std::vector<std::string> args(argv, argv + argc);
#ifdef _WIN32
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc))
    {
        args.clear();
        for (int i = 0; i < wargc; i++)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 0 ? n - 1 : 0, '\0');
            if (n > 1)
            {
                WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
            }

            args.push_back(std::move(a));
        }
        LocalFree(wargv);
    }
#endif

    return args;
}

// Switch the Windows console from its default ANSI code page to UTF-8.
void UseUtf8Console()
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
}

// Line-buffer stdout to keep it ordered with errors when both share a pipe, as in Citrusf2.app. The app runs only on
// macOS; Windows' C runtime does not support line buffering.
void LineBufferOutput()
{
#ifndef _WIN32
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif
}

// Keep the console open after drag-and-drop or double-click launches on Windows, so the user can read the results.
void PauseIfOwnConsole()
{
#ifdef _WIN32
    DWORD ids[2];
    if (GetConsoleProcessList(ids, 2) == 1)
    {
        std::puts("\nPress Enter to close.");
        std::getchar();
    }
#endif
}

// Find sequence indexes whose names match `only`.
std::vector<uint32_t> SelectSequences(const SoundArchive& archive, const std::regex& only)
{
    std::vector<uint32_t> selected;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const SoundInfo& s = archive.Sounds()[i];
        if (s.type == SoundType::kSequence && std::regex_search(s.name, only))
        {
            selected.push_back(i);
        }
    }

    return selected;
}

// Convert selected sequences to `out_dir` and report each result unless quiet mode is set. Return the failure count.
int ConvertAll(const SoundArchive& archive, const std::vector<uint32_t>& selected, const fs::path& out_dir,
               const Options& options)
{
    int failed = 0;
    std::set<std::string> used_names = {kSfzSamplesFolder}; // lowercase names for collision checks on Windows and macOS
    std::set<std::string> samples_written;
    const fs::path sfz_dir = out_dir / "sfz";
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t index : selected)
    {
        const std::string& name = archive.Sounds()[index].name;
        try
        {
            const Conversion c = ConvertSequence(archive, index, options.perform);

            // Disambiguate names that differ only in case, and the reserved SFZ samples directory name.
            std::string base = FileName(name);
            while (!used_names.insert(AsciiLower(base)).second)
            {
                base += "_" + std::to_string(index);
            }

            WriteFile(out_dir / PathFromUtf8(base + ".mid"), c.midi);
            WriteFile(out_dir / PathFromUtf8(base + ".sf2"), c.sf2);

            // Write SFZ files to each sequence's directory and samples to the shared directory. Sequences with no
            // playing tracks produce no SFZ files.
            const fs::path sequence_dir = sfz_dir / PathFromUtf8(base);
            const fs::path samples_dir = sfz_dir / PathFromUtf8(kSfzSamplesFolder);
            for (const SfzFile& f : c.sfz.files)
            {
                fs::create_directories(sequence_dir);
                WriteFile(sequence_dir / PathFromUtf8(f.name), f.text);
            }
            for (const SfzSample& sample : c.sfz.samples)
            {
                if (samples_written.contains(sample.name))
                {
                    continue; // already written for another sequence
                }

                fs::create_directories(samples_dir);
                WriteFile(samples_dir / PathFromUtf8(sample.name), sample.wav);
                samples_written.insert(sample.name);
            }

            if (!options.quiet)
            {
                PrintConversion(name, c);
            }
        }
        catch (const std::exception& e)
        {
            failed++;
            std::fprintf(stderr, "  %s: %s\n", name.c_str(), e.what());
        }
    }

    if (!options.quiet)
    {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("done: %zu converted, %d failed, %.1f s\n", selected.size() - failed, failed, secs);
    }

    return failed;
}

// Convert or list selected sequences in `path`. Return 1 on any failure, otherwise 0. If `several` is set, prefix
// listings with the archive name. `used_folders` maps output directories to archives already processed. Throw for
// corrupt data, directory creation failure or a conflicting output directory.
int ConvertArchive(const std::string& path, const Options& options, const std::regex& only, bool several,
                   std::vector<std::pair<fs::path, std::string>>& used_folders)
{
    const fs::path input = PathFromUtf8(path);

    // Some systems allow opening directories as empty files. Report those explicitly.
    std::error_code error;
    if (fs::is_directory(input, error))
    {
        std::fprintf(stderr, "%s is a folder, not a sound archive\n", path.c_str());
        return 1;
    }

    std::optional<std::vector<uint8_t>> bytes = ReadFile(input);
    if (!bytes)
    {
        std::fprintf(stderr, "can't read %s\n", path.c_str());
        return 1;
    }

    const std::string name = Utf8(input.filename());
    const std::size_t file_size = bytes->size();
    const SoundArchive archive = SoundArchive::Load(std::move(*bytes));
    if (archive.Truncated())
    {
        std::fprintf(stderr, "warning: %s is truncated (%zu bytes): skipping sounds with missing data\n", name.c_str(),
                     file_size);
    }
    for (const std::string& group_error : archive.GroupErrors())
    {
        std::fprintf(stderr, "warning: %s\n", group_error.c_str());
    }

    const std::vector<uint32_t> selected = SelectSequences(archive, only);
    if (options.list)
    {
        if (several)
        {
            std::printf("%s:\n", name.c_str());
        }
        for (uint32_t i : selected)
        {
            std::printf("%s%s\n", several ? "  " : "", archive.Sounds()[i].name.c_str());
        }
        return 0;
    }

    if (selected.empty())
    {
        std::fprintf(stderr, "%s: no sequences%s\n", name.c_str(), options.only.empty() ? "" : " match");
        return 1;
    }

    const fs::path out_dir = options.output.empty()
                                 ? input.parent_path() / PathFromUtf8(FileName(Utf8(input.stem()) + "_citrusf2"))
                                 : PathFromUtf8(options.output);

    // Archive names differing only in extension produce the same output directory. Case differences can collide too,
    // depending on the filesystem. Ask the filesystem whether the directory already belongs to an earlier archive
    // before writing anything.
    for (const auto& [folder, earlier] : used_folders)
    {
        std::error_code ignored; // no existing directory to compare
        if (fs::equivalent(out_dir, folder, ignored))
        {
            throw std::runtime_error("not converted: output would overwrite files from " + earlier + " in " +
                                     Utf8(out_dir));
        }
    }

    fs::create_directories(out_dir);
    used_folders.emplace_back(out_dir, name);
    if (!options.quiet)
    {
        std::printf("%s: %zu sequences -> %s\n", name.c_str(), selected.size(), Utf8(out_dir).c_str());
    }

    return ConvertAll(archive, selected, out_dir, options) > 0 ? 1 : 0;
}

// Process the archives named on the command line and return the exit status.
int Run(const std::vector<std::string>& args)
{
    const Options options = ParseOptions(args);
    if (options.help)
    {
        Usage();
        return 0;
    }
    if (options.version)
    {
        std::puts("citrusf2 " CITRUSF2_VERSION);
        return 0;
    }
    if (options.archives.empty())
    {
        Usage();
        return 2;
    }
    if (!options.output.empty() && options.archives.size() > 1)
    {
        throw std::runtime_error(
            "--output requires a single archive. Otherwise, output goes in a folder "
            "beside each archive.");
    }

    const std::regex only(options.only.empty() ? std::string(".*") : options.only);
    const bool several = options.archives.size() > 1;
    std::vector<std::pair<fs::path, std::string>> used_folders;
    int rc = 0;
    for (std::size_t i = 0; i < options.archives.size(); i++)
    {
        // Separate archive reports with a blank line; the macOS app uses this to split them.
        if (i > 0 && (options.list || !options.quiet))
        {
            std::printf("\n");
        }

        // Continue with the remaining archives after an error.
        const std::string& path = options.archives[i];
        try
        {
            rc = std::max(rc, ConvertArchive(path, options, only, several, used_folders));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", Utf8(PathFromUtf8(path).filename()).c_str(), e.what());
            rc = 1;
        }
    }

    return rc;
}

} // namespace

int main(int argc, char** argv)
{
    UseUtf8Console();
    LineBufferOutput();

    int rc;
    try
    {
        rc = Run(Arguments(argc, argv));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        rc = 1;
    }

    PauseIfOwnConsole();

    return rc;
}
