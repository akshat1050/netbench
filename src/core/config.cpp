#include "netbench/config.hpp"
#include "netbench/source_policy.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <stdexcept>

namespace netbench {
namespace {

std::string lowercase(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

std::uint64_t unsigned_value(std::string_view value, std::string_view label) {
  if (value.empty())
    throw std::invalid_argument(std::string(label) + " must not be empty");
  std::uint64_t parsed{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("invalid " + std::string(label) + ": " +
                                std::string(value));
  return parsed;
}

std::string_view require_value(int &index, int argc, char **argv,
                               std::string_view option) {
  if (++index >= argc)
    throw std::invalid_argument(std::string(option) + " requires a value");
  return argv[index];
}

std::uint64_t checked_multiply(std::uint64_t value, std::uint64_t multiplier,
                               std::string_view label) {
  if (value > std::numeric_limits<std::uint64_t>::max() / multiplier)
    throw std::invalid_argument(std::string(label) + " is too large");
  return value * multiplier;
}

Profile parse_profile(std::string_view value) {
  if (value == "baseline")
    return Profile::baseline;
  if (value == "sustained")
    return Profile::sustained;
  if (value == "ramp")
    return Profile::ramp;
  if (value == "burst")
    return Profile::burst;
  throw std::invalid_argument(
      "profile must be baseline, sustained, ramp, or burst");
}

} // namespace

CidrV4 CidrV4::parse(std::string_view text) {
  const auto slash = text.find('/');
  if (slash == std::string_view::npos)
    throw std::invalid_argument("CIDR requires a prefix length: " +
                                std::string(text));
  const auto prefix = unsigned_value(text.substr(slash + 1), "CIDR prefix");
  if (prefix > 32)
    throw std::invalid_argument("IPv4 CIDR prefix must be between 0 and 32");
  in_addr address{};
  const std::string address_string(text.substr(0, slash));
  if (::inet_pton(AF_INET, address_string.c_str(), &address) != 1)
    throw std::invalid_argument("invalid IPv4 CIDR address: " + address_string);
  const auto host_address = ntohl(address.s_addr);
  const auto mask = prefix == 0 ? 0U
                                : std::numeric_limits<std::uint32_t>::max()
                                      << (32U - prefix);
  return CidrV4(host_address & mask, mask, std::string(text));
}

bool CidrV4::contains(std::string_view address_text) const {
  in_addr address{};
  const std::string address_string(address_text);
  if (::inet_pton(AF_INET, address_string.c_str(), &address) != 1)
    return false;
  return (ntohl(address.s_addr) & mask_) == network_;
}

bool CidrV4::contains_usable_host(std::string_view address_text) const {
  in_addr address{};
  const std::string address_string(address_text);
  if (::inet_pton(AF_INET, address_string.c_str(), &address) != 1)
    return false;
  const auto host_address = ntohl(address.s_addr);
  if ((host_address & mask_) != network_)
    return false;
  return mask_ == 0xffffffffU || mask_ == 0xfffffffeU ||
         (host_address != network_ && host_address != (network_ | ~mask_));
}

bool CidrV4::is_subset_of(const CidrV4 &other) const noexcept {
  const auto last = network_ | ~mask_;
  return (network_ & other.mask_) == other.network_ &&
         (last & other.mask_) == other.network_;
}

std::uint64_t parse_bit_rate(std::string_view text) {
  const auto normalized = lowercase(text);
  const std::pair<std::string_view, std::uint64_t> suffixes[] = {
      {"gbps", 1'000'000'000ULL},
      {"mbps", 1'000'000ULL},
      {"kbps", 1'000ULL},
      {"bps", 1ULL}};
  for (const auto &[suffix, multiplier] : suffixes) {
    if (normalized.size() > suffix.size() && normalized.ends_with(suffix)) {
      const auto bits = checked_multiply(
          unsigned_value(std::string_view(normalized)
                             .substr(0, normalized.size() - suffix.size()),
                         "bandwidth"),
          multiplier, "bandwidth");
      if (bits < 8)
        throw std::invalid_argument("bandwidth must be at least 8 bps");
      return bits / 8;
    }
  }
  throw std::invalid_argument(
      "bandwidth requires bps, kbps, mbps, or gbps suffix");
}

std::uint64_t parse_operation_rate(std::string_view text,
                                   std::string_view suffix) {
  auto normalized = lowercase(text);
  if (normalized.ends_with(suffix))
    normalized.resize(normalized.size() - suffix.size());
  const auto value = unsigned_value(normalized, suffix);
  if (value == 0)
    throw std::invalid_argument(std::string(suffix) + " must be positive");
  return value;
}

std::chrono::milliseconds parse_duration(std::string_view text) {
  const auto normalized = lowercase(text);
  std::uint64_t multiplier{};
  std::string_view number;
  if (normalized.ends_with("ms")) {
    multiplier = 1;
    number = std::string_view(normalized).substr(0, normalized.size() - 2);
  } else if (normalized.ends_with('s')) {
    multiplier = 1000;
    number = std::string_view(normalized).substr(0, normalized.size() - 1);
  } else if (normalized.ends_with('m')) {
    multiplier = 60'000;
    number = std::string_view(normalized).substr(0, normalized.size() - 1);
  } else {
    throw std::invalid_argument("duration requires ms, s, or m suffix");
  }
  const auto milliseconds = checked_multiply(unsigned_value(number, "duration"),
                                             multiplier, "duration");
  if (milliseconds == 0 ||
      milliseconds >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::invalid_argument("duration must be positive and representable");
  return std::chrono::milliseconds{milliseconds};
}

double profile_factor(Profile profile, std::chrono::nanoseconds elapsed,
                      std::chrono::nanoseconds duration) noexcept {
  if (profile == Profile::baseline)
    return 0.1;
  if (profile != Profile::ramp || duration <= std::chrono::nanoseconds::zero())
    return 1.0;
  return 0.1 +
         0.9 * std::clamp(std::chrono::duration<double>(elapsed).count() /
                              std::chrono::duration<double>(duration).count(),
                          0.0, 1.0);
}

std::string_view profile_name(Profile profile) noexcept {
  switch (profile) {
  case Profile::baseline:
    return "baseline";
  case Profile::sustained:
    return "sustained";
  case Profile::ramp:
    return "ramp";
  case Profile::burst:
    return "burst";
  }
  return "unknown";
}

Config parse_command_line(int argc, char **argv) {
  Config config;
  if (argc < 2)
    return config;
  const std::string_view command(argv[1]);
  if (command == "--help" || command == "help")
    return config;
  if (command == "--version" || command == "version") {
    config.command = Command::version;
    return config;
  }
  if (command == "tcp")
    config.command = Command::tcp;
  else if (command == "udp")
    config.command = Command::udp;
  else if (command == "icmp") {
    config.command = Command::icmp;
    config.payload_size = 56;
  } else if (command == "validate-source") {
    config.command = Command::validate_source;
  } else
    throw std::invalid_argument("unknown command: " + std::string(command));

  for (int index = 2; index < argc; ++index) {
    const std::string_view option(argv[index]);
    if (option == "--help") {
      config.command = Command::help;
      return config;
    } else if (option == "--target") {
      config.target = require_value(index, argc, argv, option);
    } else if (option == "--interface") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--interface is only valid with validate-source");
      config.interface = require_value(index, argc, argv, option);
    } else if (option == "--allow-interface") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--allow-interface is only valid with validate-source");
      config.allowed_interfaces.emplace_back(
          require_value(index, argc, argv, option));
    } else if (option == "--spoof-source") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--spoof-source is only valid with validate-source");
      config.source_address.spoof = true;
    } else if (option == "--source-ip") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--source-ip is only valid with validate-source");
      config.source_address.address = require_value(index, argc, argv, option);
    } else if (option == "--source-allow-cidr") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--source-allow-cidr is only valid with validate-source");
      config.source_address.allowlist.push_back(
          CidrV4::parse(require_value(index, argc, argv, option)));
    } else if (option == "--destination-allow-cidr") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--destination-allow-cidr is only valid with validate-source");
      config.allowlist.push_back(
          CidrV4::parse(require_value(index, argc, argv, option)));
    } else if (option == "--port") {
      const auto value =
          unsigned_value(require_value(index, argc, argv, option), "port");
      if (value > 65'535)
        throw std::invalid_argument("port is out of range");
      config.port = static_cast<std::uint16_t>(value);
    } else if (option == "--connections") {
      const auto value = unsigned_value(
          require_value(index, argc, argv, option), "connections");
      if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("connections is too large");
      config.connections = static_cast<std::uint32_t>(value);
    } else if (option == "--rate") {
      const auto value = require_value(index, argc, argv, option);
      if (config.command == Command::tcp)
        config.connection_rate = parse_operation_rate(value, "cps");
      else
        config.pps = parse_operation_rate(value, "pps");
    } else if (option == "--pps") {
      config.pps =
          parse_operation_rate(require_value(index, argc, argv, option), "pps");
    } else if (option == "--bandwidth") {
      config.bandwidth_bytes_per_second =
          parse_bit_rate(require_value(index, argc, argv, option));
    } else if (option == "--duration") {
      config.duration =
          parse_duration(require_value(index, argc, argv, option));
    } else if (option == "--timeout") {
      config.timeout = parse_duration(require_value(index, argc, argv, option));
    } else if (option == "--recovery") {
      config.recovery =
          parse_duration(require_value(index, argc, argv, option));
    } else if (option == "--interval") {
      config.interval =
          parse_duration(require_value(index, argc, argv, option));
    } else if (option == "--size") {
      const auto value = unsigned_value(
          require_value(index, argc, argv, option), "payload size");
      if (value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("payload size is too large");
      config.payload_size = static_cast<std::size_t>(value);
    } else if (option == "--profile") {
      config.profile = parse_profile(require_value(index, argc, argv, option));
    } else if (option == "--json") {
      config.json_path = require_value(index, argc, argv, option);
    } else if (option == "--csv") {
      config.csv_path = require_value(index, argc, argv, option);
    } else if (option == "--report") {
      if (config.command != Command::validate_source)
        throw std::invalid_argument(
            "--report is only valid with validate-source");
      const std::string path(require_value(index, argc, argv, option));
      if (path.ends_with(".json"))
        config.json_path = path;
      else if (path.ends_with(".csv"))
        config.csv_path = path;
      else
        throw std::invalid_argument("--report requires a .json or .csv path");
    } else if (option == "--max-pps") {
      config.limits.pps = unsigned_value(
          require_value(index, argc, argv, option), "maximum PPS");
    } else if (option == "--max-bandwidth") {
      config.limits.bandwidth_bytes_per_second =
          parse_bit_rate(require_value(index, argc, argv, option));
    } else if (option == "--max-connection-rate") {
      config.limits.connection_rate =
          parse_operation_rate(require_value(index, argc, argv, option), "cps");
    } else if (option == "--max-concurrency") {
      const auto value = unsigned_value(
          require_value(index, argc, argv, option), "maximum concurrency");
      if (value > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("maximum concurrency is too large");
      config.limits.concurrency = static_cast<std::uint32_t>(value);
    } else if (option == "--max-duration") {
      config.limits.duration =
          parse_duration(require_value(index, argc, argv, option));
    } else {
      throw std::invalid_argument("unknown option: " + std::string(option));
    }
  }
  validate(config);
  return config;
}

void validate(const Config &config) {
  if (config.command != Command::validate_source &&
      (config.source_address.spoof || !config.source_address.address.empty() ||
       !config.source_address.allowlist.empty() || !config.interface.empty() ||
       !config.allowed_interfaces.empty()))
    throw std::invalid_argument(
        "source-address options are only valid with validate-source");
  if (config.limits.pps == 0 || config.limits.pps > SafetyLimits::absolute_pps)
    throw std::invalid_argument("maximum PPS exceeds compiled ceiling");
  if (config.limits.bandwidth_bytes_per_second == 0 ||
      config.limits.bandwidth_bytes_per_second >
          SafetyLimits::absolute_bandwidth_bytes_per_second)
    throw std::invalid_argument("maximum bandwidth exceeds compiled ceiling");
  if (config.limits.connection_rate == 0 ||
      config.limits.connection_rate > SafetyLimits::absolute_connection_rate)
    throw std::invalid_argument(
        "maximum connection rate exceeds compiled ceiling");
  if (config.limits.concurrency == 0 ||
      config.limits.concurrency > SafetyLimits::absolute_concurrency)
    throw std::invalid_argument("maximum concurrency exceeds compiled ceiling");
  if (config.limits.duration <= std::chrono::milliseconds::zero() ||
      config.limits.duration > SafetyLimits::absolute_duration)
    throw std::invalid_argument("maximum duration exceeds compiled ceiling");
  if (config.target.empty())
    throw std::invalid_argument("test requires --target");
  in_addr address{};
  if (::inet_pton(AF_INET, config.target.c_str(), &address) != 1)
    throw std::invalid_argument("target must be an IPv4 literal");
  if (!config.allowlist.empty() &&
      std::ranges::none_of(config.allowlist, [&](const CidrV4 &cidr) {
        return cidr.contains(config.target);
      }))
    throw std::invalid_argument(
        "target is not inside the configured destination CIDR allowlist");
  if (config.duration <= std::chrono::milliseconds::zero() ||
      config.duration > config.limits.duration)
    throw std::invalid_argument("duration exceeds the runtime safety limit");
  if (config.profile == Profile::burst &&
      config.duration > std::chrono::seconds{10})
    throw std::invalid_argument("burst duration cannot exceed 10 seconds");
  if (config.timeout <= std::chrono::milliseconds::zero() ||
      config.timeout > config.limits.duration ||
      config.recovery <= std::chrono::milliseconds::zero() ||
      config.recovery > config.limits.duration ||
      config.interval <= std::chrono::milliseconds::zero())
    throw std::invalid_argument(
        "timeout, recovery, and interval must be positive and bounded");

  if (config.command == Command::tcp) {
    if (config.port == 0)
      throw std::invalid_argument("tcp requires --port");
    if (config.connections == 0 ||
        config.connections > config.limits.concurrency)
      throw std::invalid_argument("connections exceed maximum concurrency");
    if (config.connection_rate == 0 ||
        config.connection_rate > config.limits.connection_rate)
      throw std::invalid_argument("connection rate exceeds its runtime limit");
  } else if (config.command == Command::udp) {
    if (config.port == 0)
      throw std::invalid_argument("udp requires --port");
    if (config.payload_size == 0 || config.payload_size > 65'507)
      throw std::invalid_argument("UDP size must be between 1 and 65507 bytes");
    if (config.pps == 0 || config.pps > config.limits.pps)
      throw std::invalid_argument("PPS exceeds its runtime safety limit");
    if (config.bandwidth_bytes_per_second == 0 ||
        config.bandwidth_bytes_per_second >
            config.limits.bandwidth_bytes_per_second)
      throw std::invalid_argument("bandwidth exceeds its runtime safety limit");
    if (config.pps > config.bandwidth_bytes_per_second / config.payload_size)
      throw std::invalid_argument(
          "PPS and datagram size exceed the bandwidth limit");
  } else if (config.command == Command::icmp) {
    if (config.pps == 0 || config.pps > config.limits.pps)
      throw std::invalid_argument("PPS exceeds its runtime safety limit");
    if (config.payload_size > 1400)
      throw std::invalid_argument("ICMP payload size cannot exceed 1400 bytes");
    if (config.bandwidth_bytes_per_second == 0 ||
        config.bandwidth_bytes_per_second >
            config.limits.bandwidth_bytes_per_second ||
        config.pps >
            config.bandwidth_bytes_per_second / (config.payload_size + 8U))
      throw std::invalid_argument(
          "ICMP rate and size exceed the bandwidth safety limit");
  } else if (config.command == Command::validate_source) {
    SourcePolicy::validate(config);
  }
}

std::string usage() {
  return R"(NETBENCH v0.2.0
Usage:
  netbench tcp  --target IPv4 --port PORT
                --connections N --rate Ncps --duration TIME [options]
  netbench udp  --target IPv4 --port PORT
                --pps N --size BYTES --duration TIME [options]
  netbench icmp --target IPv4
                --rate Npps --duration TIME [options]
  netbench validate-source --target IPv4 --interface IFACE --spoof-source
                --source-ip IPv4 --source-allow-cidr CIDR
                --destination-allow-cidr CIDR --allow-interface IFACE
                --pps N --duration TIME [--report FILE.json|FILE.csv]

Options:
  --profile baseline|sustained|ramp|burst
  --timeout TIME --recovery TIME --interval TIME
  --bandwidth RATE --json FILE --csv FILE
  --max-pps N --max-bandwidth RATE --max-connection-rate Ncps
  --max-concurrency N --max-duration TIME

TCP, UDP, and ICMP accept any IPv4 literal target.
validate-source is limited to allowlisted RFC 1918 lab networks and interfaces.
)";
}

} // namespace netbench
