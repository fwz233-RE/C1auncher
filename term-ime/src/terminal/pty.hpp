#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

class Pty {
   public:
    Pty();
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    bool spawn(const std::string& shell = "/bin/bash");
    const std::string& spawn_error() const { return spawn_error_; }
    std::optional<std::vector<uint8_t>> read();
    bool write(const std::vector<uint8_t>& data);
    int fd() const;
    void resize(int rows, int cols);

   private:
    int master_fd_ = -1;
    int pid_ = -1;
    std::string spawn_error_;

    // Poll waitpid(WNOHANG) until the child exits or timeout_ms elapses.
    // Returns true if the child was reaped within the timeout.
    bool wait_for_exit(int timeout_ms);
};
