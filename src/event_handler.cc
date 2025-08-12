#include "event_handler.hh"

#include <fcntl.h>
#include <signal.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#include <cstring>
#include <memory>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-event.h>
#elif defined(HAVE_LIBEVENT)
#include <event2/event.h>
#include <event2/thread.h>
#endif

namespace pkgfile {

namespace {

#ifdef HAVE_SYSTEMD
class SystemdEventHandler : public EventHandler {
 public:
  SystemdEventHandler() {
    if (sd_event_default(&event_) < 0) {
      throw std::runtime_error("Failed to create systemd event loop");
    }
  }

  ~SystemdEventHandler() override {
    if (event_) {
      sd_event_unref(event_);
    }
  }

  int Run() override { return sd_event_loop(event_); }

  void Stop() override { sd_event_exit(event_, 0); }

  bool AddFileWatch(const std::string& path, FileEventHandler handler) override {
    int r = sd_event_add_inotify(
        event_, &inotify_source_, path.c_str(), IN_MOVED_TO,
        [](sd_event_source* /*s*/, const struct inotify_event* event,
           void* userdata) -> int {
          auto* self = static_cast<SystemdEventHandler*>(userdata);
          self->file_handler_(event->name);
          return 0;
        },
        this);

    if (r < 0) {
      return false;
    }

    sd_event_source_set_priority(inotify_source_, SD_EVENT_PRIORITY_IMPORTANT);
    file_handler_ = std::move(handler);
    return true;
  }

  bool AddSignalHandler(int signo, SignalHandler handler) override {
    sd_event_source* source;
    int r = sd_event_add_signal(
        event_, &source, signo,
        [](sd_event_source* /*s*/, const struct signalfd_siginfo* /*si*/,
           void* userdata) -> int {
          auto* self = static_cast<SystemdEventHandler*>(userdata);
          auto it = self->signal_handlers_.find(signo);
          if (it != self->signal_handlers_.end()) {
            it->second();
          }
          return 0;
        },
        this);

    if (r < 0) {
      return false;
    }

    if (signo == SIGTERM || signo == SIGINT) {
      sd_event_source_set_priority(source, SD_EVENT_PRIORITY_IDLE);
    }

    signal_handlers_[signo] = std::move(handler);
    return true;
  }

  bool IsValid() const override { return event_ != nullptr; }

 private:
  sd_event* event_ = nullptr;
  sd_event_source* inotify_source_ = nullptr;
  FileEventHandler file_handler_;
  std::unordered_map<int, SignalHandler> signal_handlers_;
};

#elif defined(HAVE_LIBEVENT)
class LibeventHandler : public EventHandler {
 public:
  LibeventHandler() {
    event_base_ = event_base_new();
    if (!event_base_) {
      fprintf(stderr, "Failed to create libevent base\n");
      return;
    }

    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
      fprintf(stderr, "Failed to initialize inotify\n");
      event_base_free(event_base_);
      event_base_ = nullptr;
      return;
    }
  }

  ~LibeventHandler() override {
    if (inotify_event_) {
      event_free(inotify_event_);
    }
    if (inotify_fd_ >= 0) {
      close(inotify_fd_);
    }
    if (event_base_) {
      event_base_free(event_base_);
    }
  }

  int Run() override { return event_base_dispatch(event_base_); }

  void Stop() override { event_base_loopbreak(event_base_); }

  bool AddFileWatch(const std::string& path, FileEventHandler handler) override {
    int wd = inotify_add_watch(inotify_fd_, path.c_str(), IN_MOVED_TO);
    if (wd < 0) {
      return false;
    }

    file_handler_ = std::move(handler);
    return true;
  }

  bool AddSignalHandler(int signo, SignalHandler handler) override {
    event* ev = evsignal_new(event_base_, signo, &LibeventHandler::OnSignal, this);
    if (!ev || event_add(ev, nullptr) < 0) {
      if (ev) event_free(ev);
      return false;
    }

    signal_handlers_[signo] = {ev, std::move(handler)};
    return true;
  }

  bool IsValid() const override { return event_base_ != nullptr; }

 private:
  static void OnInotifyEvent(evutil_socket_t fd, [[maybe_unused]] short events, void* arg) {
    auto* self = static_cast<LibeventHandler*>(arg);
    char buf[4096] __attribute__((aligned(alignof(struct inotify_event))));
    const struct inotify_event* event;
    ssize_t len;

    while ((len = read(fd, buf, sizeof(buf))) > 0) {
      for (char* ptr = buf; ptr < buf + len;
           ptr += sizeof(struct inotify_event) + event->len) {
        event = reinterpret_cast<const struct inotify_event*>(ptr);
        self->file_handler_(event->name);
      }
    }
  }

  static void OnSignal(evutil_socket_t sig, [[maybe_unused]] short events, void* arg) {
    auto* self = static_cast<LibeventHandler*>(arg);
    auto it = self->signal_handlers_.find(sig);
    if (it != self->signal_handlers_.end()) {
      it->second.handler();
    }
  }

  event_base* event_base_ = nullptr;
  event* inotify_event_ = nullptr;
  int inotify_fd_ = -1;
  FileEventHandler file_handler_;
  
  struct SignalHandlerData {
    event* ev;
    SignalHandler handler;
    ~SignalHandlerData() { if (ev) event_free(ev); }
  };
  std::unordered_map<int, SignalHandlerData> signal_handlers_;
};
#endif

}  // namespace

std::unique_ptr<EventHandler> EventHandler::Create() {
#ifdef HAVE_SYSTEMD
  return std::make_unique<SystemdEventHandler>();
#elif defined(HAVE_LIBEVENT)
  auto handler = std::make_unique<LibeventHandler>();
  if (!handler->IsValid()) {
    return nullptr;
  }
  return handler;
#else
  return nullptr;
#endif
}

}  // namespace pkgfile
