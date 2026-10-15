#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <string_view>

namespace netbench {

class Socket {
public:
  Socket() noexcept = default;
  explicit Socket(int fd) noexcept : fd_(fd) {}
  ~Socket();
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&other) noexcept;
  Socket &operator=(Socket &&other) noexcept;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int release() noexcept;
  void reset(int fd = -1) noexcept;

private:
  int fd_{-1};
};

void set_nonblocking(int fd);
void set_close_on_exec(int fd);
[[nodiscard]] Socket make_epoll();
void epoll_add(int epoll_fd, int fd, std::uint32_t events);
void epoll_modify(int epoll_fd, int fd, std::uint32_t events);
[[nodiscard]] sockaddr_in ipv4_endpoint(std::string_view address,
                                        std::uint16_t port = 0);
[[nodiscard]] std::string socket_error(std::string_view operation,
                                       int error_number);

} // namespace netbench
