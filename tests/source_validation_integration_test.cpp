#include "netbench/config.hpp"
#include "netbench/engine.hpp"
#include "netbench/source_validation.hpp"

#include <linux/capability.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace {

constexpr int skipped = 77;

bool command(const std::string &value) {
  return std::system(value.c_str()) == 0;
}

netbench::Config config() {
  netbench::Config value;
  value.command = netbench::Command::validate_source;
  value.target = "10.254.253.2";
  value.allowlist.push_back(netbench::CidrV4::parse("10.254.253.0/24"));
  value.interface = "nbveth0";
  value.allowed_interfaces.push_back(value.interface);
  value.source_address.spoof = true;
  value.source_address.address = "10.254.253.3";
  value.source_address.allowlist.push_back(
      netbench::CidrV4::parse("10.254.253.0/24"));
  value.pps = 4;
  value.duration = std::chrono::milliseconds{250};
  value.interval = std::chrono::seconds{1};
  return value;
}

bool clear_capabilities() {
  __user_cap_header_struct header{};
  header.version = _LINUX_CAPABILITY_VERSION_3;
  __user_cap_data_struct data[2]{};
  return ::syscall(SYS_capset, &header, &data) == 0;
}

void check_permission_failure(const netbench::Config &value) {
  const auto child = ::fork();
  if (child < 0)
    throw std::runtime_error("fork failed");
  if (child == 0) {
    if (!clear_capabilities())
      ::_exit(2);
    try {
      (void)netbench::LabEnvironment::verify(value);
      ::_exit(1);
    } catch (const std::exception &) {
      ::_exit(0);
    }
  }
  int status{};
  if (::waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    throw std::runtime_error("missing CAP_NET_RAW was not rejected");
}

pid_t start_peer(int parent_to_child[2], int child_to_parent[2]) {
  if (::pipe(parent_to_child) != 0 || ::pipe(child_to_parent) != 0)
    throw std::runtime_error("pipe failed");
  const auto child = ::fork();
  if (child < 0)
    throw std::runtime_error("fork failed");
  if (child != 0) {
    ::close(parent_to_child[0]);
    ::close(child_to_parent[1]);
    return child;
  }

  ::close(parent_to_child[1]);
  ::close(child_to_parent[0]);
  char state = 'E';
  if (::unshare(CLONE_NEWNET) == 0)
    state = 'R';
  (void)::write(child_to_parent[1], &state, 1);
  if (state != 'R' || ::read(parent_to_child[0], &state, 1) != 1)
    ::_exit(skipped);
  const bool configured = command("ip addr add 10.254.253.2/24 dev nbveth1") &&
                          command("ip link set nbveth1 up");
  state = configured ? 'C' : 'E';
  (void)::write(child_to_parent[1], &state, 1);
  if (!configured)
    ::_exit(1);
  (void)::read(parent_to_child[0], &state, 1);
  ::_exit(0);
}

void stop_peer(pid_t child, int parent_to_child, int child_to_parent) {
  ::close(parent_to_child);
  ::close(child_to_parent);
  int status{};
  (void)::waitpid(child, &status, 0);
}

} // namespace

int main(int argc, char **argv) {
  if (!command("ip -Version >/dev/null 2>&1")) {
    std::cout << "source validation namespace test skipped: namespace or ip "
                 "tool unavailable\n";
    return skipped;
  }
  const bool inside_user_namespace =
      argc == 2 && std::string(argv[1]) == "--inside-user-namespace";
  if (!inside_user_namespace && ::unshare(CLONE_NEWNET) != 0) {
    ::execlp("unshare", "unshare", "-Urnm", "--", argv[0],
             "--inside-user-namespace", static_cast<char *>(nullptr));
    std::cout << "source validation namespace test skipped: namespace "
                 "creation unavailable\n";
    return skipped;
  }
  if (inside_user_namespace && !command("mount -t sysfs sysfs /sys")) {
    std::cout << "source validation namespace test skipped: namespace-local "
                 "sysfs unavailable\n";
    return skipped;
  }
  if (!command("ip link add nbveth0 type veth peer name nbveth1") ||
      !command("ip addr add 10.254.253.1/24 dev nbveth0") ||
      !command("ip link set nbveth0 up")) {
    std::cout
        << "source validation namespace test skipped: veth setup denied\n";
    return skipped;
  }

  int parent_to_child[2]{};
  int child_to_parent[2]{};
  const auto peer = start_peer(parent_to_child, child_to_parent);
  char state{};
  if (::read(child_to_parent[0], &state, 1) != 1 || state != 'R' ||
      !command("ip link set nbveth1 netns " + std::to_string(peer))) {
    stop_peer(peer, parent_to_child[1], child_to_parent[0]);
    std::cout << "source validation namespace test skipped: peer namespace "
                 "setup denied\n";
    return skipped;
  }
  state = 'M';
  (void)::write(parent_to_child[1], &state, 1);
  if (::read(child_to_parent[0], &state, 1) != 1 || state != 'C') {
    stop_peer(peer, parent_to_child[1], child_to_parent[0]);
    std::cerr << "peer interface configuration failed\n";
    return 1;
  }

  try {
    const auto value = config();
    try {
      (void)netbench::LabEnvironment::verify(value);
    } catch (const std::exception &error) {
      stop_peer(peer, parent_to_child[1], child_to_parent[0]);
      std::cout << "source validation namespace test skipped: " << error.what()
                << '\n';
      return skipped;
    }
    check_permission_failure(value);

    const auto result = netbench::SourceValidationEngine::run(value, {});
    if (result.metrics.operations_attempted != 1 ||
        result.metrics.operations_sent != 1 || !result.source_validation ||
        result.source_validation->probes_transmitted != 1 ||
        result.source_validation->forwarding_drop_externally_verified)
      throw std::runtime_error(
          "transmitted and externally verified probe counts were conflated");

    std::stop_source interrupted;
    interrupted.request_stop();
    const auto started = std::chrono::steady_clock::now();
    const auto stopped =
        netbench::SourceValidationEngine::run(value, interrupted.get_token());
    if (stopped.metrics.operations_attempted != 0 ||
        std::chrono::steady_clock::now() - started >
            std::chrono::milliseconds{250})
      throw std::runtime_error("source validation did not stop immediately");

    stop_peer(peer, parent_to_child[1], child_to_parent[0]);
    std::cout << "source validation namespace integration passed\n";
    return 0;
  } catch (const std::exception &error) {
    stop_peer(peer, parent_to_child[1], child_to_parent[0]);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
