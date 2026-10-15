#include "netbench/config.hpp"
#include "netbench/engine.hpp"
#include "netbench/report.hpp"
#include "netbench/source_policy.hpp"
#include "netbench/source_validation.hpp"

#include <chrono>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char *expression, int line) {
  if (!condition)
    throw std::runtime_error(std::string("check failed at line ") +
                             std::to_string(line) + ": " + expression);
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Function> void check_throws(Function &&function) {
  try {
    function();
  } catch (const std::exception &) {
    return;
  }
  throw std::runtime_error("expected exception was not thrown");
}

netbench::Config source_config() {
  netbench::Config config;
  config.command = netbench::Command::validate_source;
  config.target = "10.90.0.2";
  config.allowlist.push_back(netbench::CidrV4::parse("10.90.0.0/24"));
  config.interface = "veth-lab0";
  config.allowed_interfaces.push_back("veth-lab0");
  config.source_address.spoof = true;
  config.source_address.address = "10.90.0.3";
  config.source_address.allowlist.push_back(
      netbench::CidrV4::parse("10.90.0.0/24"));
  config.pps = 5;
  config.duration = std::chrono::seconds{1};
  return config;
}

netbench::Config parse(std::vector<std::string> arguments) {
  std::vector<char *> values;
  values.reserve(arguments.size());
  for (auto &argument : arguments)
    values.push_back(argument.data());
  return netbench::parse_command_line(static_cast<int>(values.size()),
                                      values.data());
}

void test_policy() {
  CHECK(!netbench::Config{}.source_address.spoof);
  auto config = source_config();
  netbench::SourcePolicy::validate(config);
  CHECK(netbench::SourcePolicy::is_rfc1918("10.1.2.3"));
  CHECK(netbench::SourcePolicy::is_rfc1918("172.16.0.1"));
  CHECK(netbench::SourcePolicy::is_rfc1918("192.168.1.1"));
  CHECK(!netbench::SourcePolicy::is_rfc1918("8.8.8.8"));

  for (const std::string invalid : {"127.0.0.1", "169.254.1.1", "224.0.0.1",
                                    "8.8.8.8", "10.90.0.0", "10.90.0.255"}) {
    auto copy = config;
    copy.source_address.address = invalid;
    check_throws([&] { netbench::SourcePolicy::validate(copy); });
  }

  auto missing_source_allowlist = config;
  missing_source_allowlist.source_address.allowlist.clear();
  check_throws(
      [&] { netbench::SourcePolicy::validate(missing_source_allowlist); });
  auto broad_source_allowlist = config;
  broad_source_allowlist.source_address.allowlist = {
      netbench::CidrV4::parse("0.0.0.0/0")};
  check_throws(
      [&] { netbench::SourcePolicy::validate(broad_source_allowlist); });
  auto missing_destination_allowlist = config;
  missing_destination_allowlist.allowlist.clear();
  check_throws(
      [&] { netbench::SourcePolicy::validate(missing_destination_allowlist); });
  auto unapproved_interface = config;
  unapproved_interface.allowed_interfaces = {"veth-other"};
  check_throws([&] { netbench::SourcePolicy::validate(unapproved_interface); });
  auto excessive_rate = config;
  excessive_rate.pps = netbench::SourcePolicy::maximum_pps + 1;
  check_throws([&] { netbench::SourcePolicy::validate(excessive_rate); });
  auto excessive_duration = config;
  excessive_duration.duration =
      netbench::SourcePolicy::maximum_duration + std::chrono::seconds{1};
  check_throws([&] { netbench::SourcePolicy::validate(excessive_duration); });
  auto disabled = config;
  disabled.source_address.spoof = false;
  check_throws([&] { netbench::SourcePolicy::validate(disabled); });
  auto missing_interface = config;
  missing_interface.interface = "netbench-no-such-interface";
  missing_interface.allowed_interfaces = {missing_interface.interface};
  check_throws(
      [&] { (void)netbench::LabEnvironment::verify(missing_interface); });
}

void test_cli() {
  const auto config = parse({"netbench",
                             "validate-source",
                             "--target",
                             "10.90.0.2",
                             "--interface",
                             "veth-lab0",
                             "--allow-interface",
                             "veth-lab0",
                             "--spoof-source",
                             "--source-ip",
                             "10.90.0.3",
                             "--source-allow-cidr",
                             "10.90.0.0/24",
                             "--destination-allow-cidr",
                             "10.90.0.0/24",
                             "--pps",
                             "5",
                             "--duration",
                             "1s",
                             "--report",
                             "validation.json"});
  CHECK(config.command == netbench::Command::validate_source);
  CHECK(config.source_address.spoof);
  CHECK(config.json_path == "validation.json");
  check_throws([] {
    (void)parse({"netbench", "udp", "--target", "127.0.0.1", "--port", "9000",
                 "--source-ip", "10.0.0.1"});
  });
  check_throws([] {
    (void)parse(
        {"netbench", "icmp", "--target", "127.0.0.1", "--spoof-source"});
  });
  check_throws([] {
    (void)parse({"netbench", "validate-source", "--target", "10.90.0.2",
                 "--interface", "veth-lab0", "--allow-interface", "veth-lab0",
                 "--source-ip", "10.90.0.3", "--source-allow-cidr",
                 "10.90.0.0/24", "--destination-allow-cidr", "10.90.0.0/24"});
  });
}

void test_reporting() {
  const auto config = source_config();
  netbench::MetricsSnapshot metrics;
  metrics.operations_attempted = 2;
  metrics.operations_sent = 2;
  netbench::TestResult result{
      "source-validation", metrics,      std::nullopt, std::nullopt,
      std::nullopt,        std::nullopt, {0.0, 1}};
  result.source_validation =
      netbench::SourceValidationReport{"veth-lab0",
                                       "10.90.0.3",
                                       "10.90.0.2",
                                       "net:[123]",
                                       2,
                                       {"one local error"},
                                       {"ICMP echo reply for probe 1"},
                                       false};
  std::ostringstream terminal;
  std::ostringstream json;
  std::ostringstream csv;
  netbench::print_result(terminal, config, result);
  netbench::write_json(json, config, result);
  netbench::write_csv(csv, config, result);
  CHECK(terminal.str().find("Local transmission is not proof") !=
        std::string::npos);
  CHECK(json.str().find("\"probes_transmitted\":2") != std::string::npos);
  CHECK(json.str().find("\"forwarding_drop_externally_verified\":false") !=
        std::string::npos);
  CHECK(csv.str().find("actual_local_interface") != std::string::npos);
}

} // namespace

int main() {
  try {
    test_policy();
    test_cli();
    test_reporting();
    std::cout << "source policy tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
