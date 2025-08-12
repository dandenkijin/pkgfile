#ifndef PKGFILE_EVENT_HANDLER_HH
#define PKGFILE_EVENT_HANDLER_HH

#include <functional>
#include <memory>
#include <string>
#include <filesystem>

namespace pkgfile {

class EventHandler {
 public:
  using FileEventHandler = std::function<void(const std::filesystem::path&)>;
  using SignalHandler = std::function<void()>;

  virtual ~EventHandler() = default;

  // Start the event loop
  virtual int Run() = 0;

  // Stop the event loop
  virtual void Stop() = 0;

  // Add a watch for file system events
  virtual bool AddFileWatch(const std::string& path, FileEventHandler handler) = 0;

  // Add a signal handler
  virtual bool AddSignalHandler(int signo, SignalHandler handler) = 0;

  // Check if the event handler is in a valid state
  virtual bool IsValid() const = 0;

  // Factory method to create the appropriate event handler
  static std::unique_ptr<EventHandler> Create();
};

}  // namespace pkgfile

#endif  // PKGFILE_EVENT_HANDLER_HH
