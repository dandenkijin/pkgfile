#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <archive.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "event_handler.hh"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

#include "archive_converter.hh"
#include "compress.hh"
#include "event_handler.hh"

// Define signal numbers if not already defined
#ifndef SIGUSR1
#define SIGUSR1 10
#endif

#ifndef SIGUSR2
#define SIGUSR2 12
#endif

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kFilesExt = ".files";

bool NeedsUpdate(const fs::path& subject, fs::file_time_type mtime) {
  std::error_code ec;
  fs::file_time_type subject_mtime = fs::last_write_time(subject, ec);

  return ec.value() != 0 || subject_mtime < mtime;
}

}  // namespace

namespace pkgfile {

class Pkgfiled {
 public:
  struct Options {
    Options() {}

    int compress = 0;  // ARCHIVE_FILTER_NONE

    // If true, skip mtime comparisons between the watch path and pkgfile cache,
    // transcoding all repos found in the watch path.
    bool force = false;

    // If true, synchronize the watch path with the pkgfile cache and then
    // exit.
    bool oneshot = false;
  };

  Pkgfiled(std::string_view watch_path, std::string_view pkgfile_cache,
           Options options, int* error = nullptr)
      : watch_path_(watch_path),
        pkgfile_cache_(pkgfile_cache),
        options_(options) {
    if (error) *error = 0;
    
    // Initialize event handler
    event_handler_ = EventHandler::Create();
    if (!event_handler_) {
      if (error) *error = 1;
      return;
    }

    // Setup inotify watch
    if (!event_handler_->AddFileWatch(
            watch_path_, [this](const std::filesystem::path& path) {
              if (path.extension() == kFilesExt) {
                RepackRepo(path.filename());
              }
            })) {
      if (error) *error = 2;
      return;
    }

    // Block signals that we'll handle asynchronously
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigprocmask(SIG_BLOCK, &mask, &saved_ss_);

    // Setup signal handlers
    const int shutdown_signal = isatty(fileno(stdin)) ? SIGINT : SIGTERM;
    
    if (!event_handler_->AddSignalHandler(shutdown_signal, [this]() {
          fprintf(stderr, "Termination signal received, shutting down\n");
          event_handler_->Stop();
        })) {
      if (error) *error = 3;
      return;
    }

    if (!event_handler_->AddSignalHandler(SIGUSR1, [this]() {
          fprintf(stderr, "SIGUSR1 received, repacking repos (force=false)\n");
          Sync(false);
        })) {
      if (error) *error = 4;
      return;
    }

    if (!event_handler_->AddSignalHandler(SIGUSR2, [this]() {
          fprintf(stderr, "SIGUSR2 received, repacking repos (force=true)\n");
          Sync(true);
        })) {
      if (error) *error = 5;
      return;
    }
  }

  ~Pkgfiled() {
    // Restore original signal mask
    sigprocmask(SIG_SETMASK, &saved_ss_, nullptr);
  }

  int Run() {
    Sync(options_.force);

    if (options_.oneshot) {
      return 0;
    }

    return event_handler_->Run();
  }

  int Sync(bool force_update) {
    std::vector<std::future<void>> repack_futures;

    for (auto& p : fs::directory_iterator(watch_path_)) {
      if (!p.is_regular_file() || p.path().extension() != kFilesExt) {
        continue;
      }

      if (!force_update && !NeedsUpdate(pkgfile_cache_ / p.path().filename(),
                                        p.last_write_time())) {
        continue;
      }

      repack_futures.emplace_back(std::async(
          std::launch::async, [this, p] { RepackRepo(p.path().filename()); }));
    }

    for (auto& f : repack_futures) {
      f.get();
    }

    return 0;
  }

 private:
  bool RepackRepo(const fs::path& changed_path) {
    auto repack = [&] {
      const std::string input_repo = watch_path_ / changed_path;

      fprintf(stderr, "processing new files DB: %s\n", input_repo.c_str());

      const auto input_file = ReadOnlyFile::Open(input_repo, /*try_mmap=*/true);
      if (input_file == nullptr) {
        return false;
      }

      const std::string reponame = changed_path.filename().stem();
      auto converter = pkgfile::ArchiveConverter::New(
          reponame, input_file->fd(), pkgfile_cache_ / changed_path,
          options_.compress, -1);

      return converter != nullptr && converter->RewriteArchive();
    };

    const auto start_time = std::chrono::system_clock::now();
    const bool ok = repack();
    if (ok) {
      std::chrono::duration<double> dur =
          std::chrono::system_clock::now() - start_time;

      fprintf(stderr, "finished repacking %s (%.3fs)\n",
              changed_path.filename().c_str(), dur.count());
    }

    return ok;
  }

  // Event handler is now managed by the EventHandler class

  fs::path watch_path_;
  fs::path pkgfile_cache_;
  Options options_;
  std::unique_ptr<EventHandler> event_handler_;
  sigset_t saved_ss_{};
};

}  // namespace pkgfile

namespace {

void Usage() {
  std::string version = "pkgfiled ";
#ifdef PACKAGE_VERSION
  version += PACKAGE_VERSION;
#endif
  version += "\nUsage: pkgfiled [options] pacman_source pkgfile_dest\n\n";
  fputs(version.c_str(), stdout);
  
  const char* help_text =
      "  -f, --force             repack all repos on initial sync\n"
      "  -o, --oneshot           exit after initial sync \n"
      "  -z, --compress[=type]   compress downloaded repos\n\n"
      "  -h, --help              display this help and exit\n"
      "  -V, --version           display the version and exit\n\n";
  fputs(help_text, stdout);
}

void Version() {
  std::string version = "pkgfiled v";
#ifdef PACKAGE_VERSION
  version += PACKAGE_VERSION;
#endif
  version += "\n";
  fputs(version.c_str(), stdout);
}

std::optional<pkgfile::Pkgfiled::Options> ParseOpts(int* argc, char*** argv) {
  const char* kShortOpts = "hofVz:";
  const struct option kLongOpts[] = {
      {"help", no_argument, nullptr, 'h'},
      {"oneshot", no_argument, nullptr, 'o'},
      {"force", no_argument, nullptr, 'f'},
      {"version", no_argument, nullptr, 'V'},
      {"compress", required_argument, nullptr, 'z'},
      {nullptr, 0, nullptr, 0},
  };

  pkgfile::Pkgfiled::Options opts;
  // Reset getopt state
  optind = 0;
  
  int opt;
  while ((opt = getopt_long(*argc, *argv, kShortOpts, kLongOpts, nullptr)) != -1) {

    switch (opt) {
      case 'h':
        Usage();
        exit(0);
      case 'o':
        opts.oneshot = true;
        break;
      case 'f':
        opts.force = true;
        break;
      case 'V':
        Version();
        exit(0);
      case 'z':
        if (optarg != nullptr) {
          opts.compress = pkgfile::ValidateCompression(optarg).value_or(-1);
          if (opts.compress < 0) {
            fprintf(stderr, "error: invalid compression option %s\n", optarg);
            return std::nullopt;
          }
        } else {
          opts.compress = ARCHIVE_FILTER_GZIP;
        }
        break;
      default:
        return std::nullopt;
    }
  }

  // Adjust argc and argv to skip processed options
  *argc -= optind;
  *argv += optind;

  return opts;
}

}  // namespace

int main(int argc, char** argv) {
  auto options = ParseOpts(&argc, &argv);
  if (options == std::nullopt) {
    return 2;
  }

  if (argc < 3) {
    fprintf(stderr, "error: not enough arguments (use -h for help)\n");
    return 1;
  }

  int error = 0;
  pkgfile::Pkgfiled pkgfiled(argv[1], argv[2], options.value(), &error);
  
  if (error != 0) {
    const char* error_msg;
    switch (error) {
      case 1: error_msg = "Failed to create event handler"; break;
      case 2: error_msg = "Failed to add file watch"; break;
      case 3: error_msg = "Failed to add shutdown signal handler"; break;
      case 4: error_msg = "Failed to add SIGUSR1 handler"; break;
      case 5: error_msg = "Failed to add SIGUSR2 handler"; break;
      default: error_msg = "Unknown error initializing pkgfiled";
    }
    fprintf(stderr, "error: %s\n", error_msg);
    return 1;
  }

  return pkgfiled.Run();
}
