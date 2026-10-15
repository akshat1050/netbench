#include "netbench/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace netbench {

Socket::~Socket() { reset(); }

Socket::Socket(Socket &&other) noexcept : fd_(other.release()) {}

Socket &Socket::operator=(Socket &&other) noexcept {
  if (this != &other)
    reset(other.release());
  return *this;
}

int Socket::release() noexcept {
  const int result = fd_;
  fd_ = -1;
  return result;
}

void Socket::reset(int fd) noexcept {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = fd;
}

std::string socket_error(std::string_view operation, int error_number) {
  return std::string(operation) + ": " + std::strerror(error_number);
}

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    throw std::runtime_error(socket_error("fcntl(O_NONBLOCK)", errno));
}

void set_close_on_exec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    throw std::runtime_error(socket_error("fcntl(FD_CLOEXEC)", errno));
}

Socket make_epoll() {
  const int fd = ::epoll_create1(EPOLL_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error(socket_error("epoll_create1", errno));
  return Socket(fd);
}

void epoll_add(int epoll_fd, int fd, std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0)
    throw std::runtime_error(socket_error("epoll_ctl(ADD)", errno));
}

void epoll_modify(int epoll_fd, int fd, std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) < 0)
    throw std::runtime_error(socket_error("epoll_ctl(MOD)", errno));
}

sockaddr_in ipv4_endpoint(std::string_view address, std::uint16_t port) {
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_port = htons(port);
  const std::string text(address);
  if (::inet_pton(AF_INET, text.c_str(), &endpoint.sin_addr) != 1)
    throw std::invalid_argument("invalid IPv4 address: " + text);
  return endpoint;
}

} // namespace netbench
