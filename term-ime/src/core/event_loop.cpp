#include "event_loop.hpp"
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <cstring>

EventLoop::EventLoop() {
    uv_loop_init(&loop_);
}

EventLoop::~EventLoop() {
    stop();

    // uv_close() is asynchronous: libuv keeps accessing the handle memory until
    // its close callback has run, so ownership is released to libuv here. The
    // close callbacks delete the wrappers once the closes complete.
    for (auto& [id, timer] : timers_) {
        TimerHandle* raw = timer.release();
        uv_close(reinterpret_cast<uv_handle_t*>(&raw->handle), timer_close_cb);
    }
    timers_.clear();

    for (auto& [fd, io] : io_handles_) {
        IoHandle* raw = io.release();
        uv_close(reinterpret_cast<uv_handle_t*>(&raw->handle), io_close_cb);
    }
    io_handles_.clear();

    for (auto& [signum, sig] : signal_handles_) {
        SignalHandle* raw = sig.release();
        uv_close(reinterpret_cast<uv_handle_t*>(&raw->handle), signal_close_cb);
    }
    signal_handles_.clear();

    // Run until every close callback has fired (handles closed earlier at
    // runtime are drained here too), then the loop has no handles left.
    uv_run(&loop_, UV_RUN_DEFAULT);

    int rc = uv_loop_close(&loop_);
    if (rc != 0) {
        spdlog::error("uv_loop_close failed: {}", uv_strerror(rc));
    }
}

void EventLoop::run() {
    running_ = true;
    spdlog::debug("EventLoop started");
    uv_run(&loop_, UV_RUN_DEFAULT);
    running_ = false;
    spdlog::debug("EventLoop stopped");
}

void EventLoop::stop() {
    if (!running_)
        return;
    running_ = false;
    uv_stop(&loop_);
}

uint64_t EventLoop::set_timer(TimerCallback callback, uint64_t timeout_ms, bool repeat) {
    auto handle = std::make_unique<TimerHandle>();
    handle->callback = std::move(callback);
    handle->id = next_timer_id_++;
    handle->handle.data = handle.get();

    uv_timer_init(&loop_, &handle->handle);
    uv_timer_start(&handle->handle, timer_callback, timeout_ms, repeat ? timeout_ms : 0);

    uint64_t id = handle->id;
    timers_[id] = std::move(handle);
    return id;
}

void EventLoop::clear_timer(uint64_t timer_id) {
    auto it = timers_.find(timer_id);
    if (it == timers_.end())
        return;
    TimerHandle* raw = it->second.release();
    timers_.erase(it);
    uv_close(reinterpret_cast<uv_handle_t*>(&raw->handle), timer_close_cb);
}

void EventLoop::watch_fd(int fd, IoCallback callback, bool readable) {
    auto handle = std::make_unique<IoHandle>();
    handle->callback = std::move(callback);
    handle->fd = fd;
    handle->owner = this;
    handle->handle.data = handle.get();

    int events = readable ? UV_READABLE : UV_WRITABLE;
    uv_poll_init(&loop_, &handle->handle, fd);
    uv_poll_start(&handle->handle, events, io_callback);

    io_handles_[fd] = std::move(handle);
}

void EventLoop::unwatch_fd(int fd) {
    auto it = io_handles_.find(fd);
    if (it == io_handles_.end())
        return;
    IoHandle* raw = it->second.release();
    io_handles_.erase(it);
    // uv_close() also stops the poll, so no further callbacks fire for this fd;
    // the wrapper stays alive until io_close_cb() runs.
    uv_close(reinterpret_cast<uv_handle_t*>(&raw->handle), io_close_cb);
}

void EventLoop::watch_signal(int signum, SignalCallback callback) {
    auto handle = std::make_unique<SignalHandle>();
    handle->callback = std::move(callback);
    handle->signum = signum;
    handle->handle.data = handle.get();

    uv_signal_init(&loop_, &handle->handle);
    uv_signal_start(&handle->handle, signal_callback, signum);

    signal_handles_[signum] = std::move(handle);
}

void EventLoop::unwatch_signal(int signum) {
    auto it = signal_handles_.find(signum);
    if (it == signal_handles_.end())
        return;
    SignalHandle* raw = it->second.release();
    signal_handles_.erase(it);
    uv_close(reinterpret_cast<uv_handle_t*>(&raw->handle), signal_close_cb);
}

// Static callbacks
void EventLoop::timer_callback(uv_timer_t* handle) {
    auto* timer = static_cast<TimerHandle*>(handle->data);
    if (timer && timer->callback) {
        timer->callback();
    }
}

void EventLoop::io_callback(uv_poll_t* handle, int status, int /*events*/) {
    auto* io = static_cast<IoHandle*>(handle->data);
    if (!io || !io->callback)
        return;

    // An EOF/errored fd stays "readable" forever, so polling it again would make
    // uv_run() spin at 100% CPU. Drop the watch, then notify the owner (which may
    // stop the loop); the wrapper outlives this callback via the close callback.
    if (status < 0) {
        spdlog::warn("Poll error on fd {}: {}", io->fd, uv_strerror(status));
    } else {
        // Read available data
        char buf[4096];
        ssize_t n = read(io->fd, buf, sizeof(buf));
        if (n > 0) {
            io->callback(buf, n);
            return;
        }
        if (n == 0) {
            spdlog::info("EOF on fd {}", io->fd);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;  // spurious wakeup, keep watching
        } else {
            spdlog::warn("Read error on fd {}: {}", io->fd, strerror(errno));
        }
    }

    const int fd = io->fd;
    IoCallback callback = io->callback;
    io->owner->unwatch_fd(fd);
    callback(nullptr, 0);
}

void EventLoop::signal_callback(uv_signal_t* handle, int signum) {
    auto* sig = static_cast<SignalHandle*>(handle->data);
    if (sig && sig->callback) {
        sig->callback(signum);
    }
}

// Close callbacks: libuv has finished with the handle here, so the wrapper
// released by its owner can be freed. `data` still points at the wrapper.
void EventLoop::timer_close_cb(uv_handle_t* handle) {
    delete static_cast<TimerHandle*>(handle->data);
}

void EventLoop::io_close_cb(uv_handle_t* handle) {
    delete static_cast<IoHandle*>(handle->data);
}

void EventLoop::signal_close_cb(uv_handle_t* handle) {
    delete static_cast<SignalHandle*>(handle->data);
}