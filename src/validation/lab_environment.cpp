#include "netbench/source_validation.hpp"

#include <net/if.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace netbench {
namespace {

std::string namespace_link(const char *path) {
  std::array<char, 128> buffer{};
  const auto length = ::readlink(path, buffer.data(), buffer.size() - 1);
  return length < 0
             ? std::string{}
             : std::string(buffer.data(), static_cast<std::size_t>(length));
}

std::uint64_t read_number(const std::string &path) {
  std::ifstream input(path);
  std::uint64_t value{};
  if (!(input >> value))
    throw std::runtime_error("cannot inspect lab interface: " + path);
  return value;
}

bool has_net_raw_capability() {
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key != "CapEff:") {
      std::string rest;
      std::getline(status, rest);
      continue;
    }
    std::string value;
    status >> value;
    try {
      return (std::stoull(value, nullptr, 16) & (1ULL << 13U)) != 0;
    } catch (const std::exception &) {
      return false;
    }
  }
  return false;
}

} // namespace

LabEnvironmentInfo LabEnvironment::verify(const Config &config) {
  const auto interface_index = ::if_nametoindex(config.interface.c_str());
  if (interface_index == 0)
    throw std::runtime_error("configured lab interface does not exist: " +
                             config.interface);
  if (config.interface == "lo")
    throw std::runtime_error("loopback cannot be used for source validation");

  const auto current_namespace = namespace_link("/proc/self/ns/net");
  const auto initial_namespace = namespace_link("/proc/1/ns/net");
  const bool dedicated_namespace = !current_namespace.empty() &&
                                   !initial_namespace.empty() &&
                                   current_namespace != initial_namespace;
  bool virtual_peer{};
  if (!dedicated_namespace) {
    const std::string interface_path =
        "/sys/class/net/" + config.interface + '/';
    virtual_peer = read_number(interface_path + "ifindex") !=
                   read_number(interface_path + "iflink");
  }
  if (!dedicated_namespace && !virtual_peer)
    throw std::runtime_error(
        "validate-source requires a dedicated network namespace or virtual "
        "lab interface");
  if (!has_net_raw_capability())
    throw std::runtime_error(
        "validate-source requires CAP_NET_RAW in the test namespace");

  std::array<char, IFNAMSIZ> actual{};
  if (::if_indextoname(interface_index, actual.data()) == nullptr)
    throw std::runtime_error("cannot resolve the configured lab interface");
  return {actual.data(), current_namespace};
}

} // namespace netbench
