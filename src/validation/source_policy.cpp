#include "netbench/source_policy.hpp"

#include <net/if.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

namespace netbench {
namespace {

const std::array<CidrV4, 3> &private_ranges() {
  static const std::array<CidrV4, 3> ranges{CidrV4::parse("10.0.0.0/8"),
                                            CidrV4::parse("172.16.0.0/12"),
                                            CidrV4::parse("192.168.0.0/16")};
  return ranges;
}

bool private_cidr(const CidrV4 &cidr) {
  return std::ranges::any_of(private_ranges(), [&](const CidrV4 &range) {
    return cidr.is_subset_of(range);
  });
}

bool allowlisted_host(std::string_view address,
                      const std::vector<CidrV4> &allowlist) {
  return std::ranges::any_of(allowlist, [&](const CidrV4 &cidr) {
    return cidr.contains_usable_host(address);
  });
}

bool valid_interface_name(std::string_view name) {
  return !name.empty() && name.size() < IFNAMSIZ &&
         std::ranges::all_of(
             name,
             [](unsigned char character) {
               return std::isalnum(character) != 0 || character == '_' ||
                      character == '-' || character == '.' || character == ':';
             });
}

} // namespace

bool SourcePolicy::is_rfc1918(std::string_view address) {
  return std::ranges::any_of(private_ranges(), [&](const CidrV4 &range) {
    return range.contains(address);
  });
}

void SourcePolicy::validate(const Config &config) {
  if (config.command != Command::validate_source)
    throw std::invalid_argument(
        "source policy only applies to validate-source");
  if (!config.source_address.spoof)
    throw std::invalid_argument(
        "validate-source requires explicit --spoof-source");
  if (config.source_address.address.empty())
    throw std::invalid_argument("--spoof-source requires --source-ip");
  if (!is_rfc1918(config.source_address.address))
    throw std::invalid_argument("source IP must be an RFC 1918 address");
  if (!is_rfc1918(config.target))
    throw std::invalid_argument("destination IP must be an RFC 1918 address");
  if (config.source_address.address == config.target)
    throw std::invalid_argument("source and destination IPs must differ");
  if (config.source_address.allowlist.empty())
    throw std::invalid_argument(
        "validate-source requires at least one --source-allow-cidr");
  if (config.allowlist.empty())
    throw std::invalid_argument(
        "validate-source requires at least one --destination-allow-cidr");
  if (std::ranges::any_of(
          config.source_address.allowlist,
          [](const CidrV4 &cidr) { return !private_cidr(cidr); }) ||
      std::ranges::any_of(config.allowlist, [](const CidrV4 &cidr) {
        return !private_cidr(cidr);
      }))
    throw std::invalid_argument(
        "source and destination allowlists must be wholly inside RFC 1918");
  if (!allowlisted_host(config.source_address.address,
                        config.source_address.allowlist))
    throw std::invalid_argument(
        "source IP is not a usable host in --source-allow-cidr");
  if (!allowlisted_host(config.target, config.allowlist))
    throw std::invalid_argument(
        "destination IP is not a usable host in --destination-allow-cidr");
  if (!valid_interface_name(config.interface))
    throw std::invalid_argument("validate-source requires a valid --interface");
  if (config.allowed_interfaces.empty() ||
      std::ranges::find(config.allowed_interfaces, config.interface) ==
          config.allowed_interfaces.end())
    throw std::invalid_argument(
        "interface is not in the explicit --allow-interface list");
  if (config.pps == 0 || config.pps > maximum_pps ||
      config.pps > config.limits.pps)
    throw std::invalid_argument("validate-source PPS cannot exceed 10");
  if (config.duration <= std::chrono::milliseconds::zero() ||
      config.duration > maximum_duration ||
      config.duration > config.limits.duration)
    throw std::invalid_argument(
        "validate-source duration cannot exceed 30 seconds");
  const auto probe_count =
      (static_cast<std::uint64_t>(config.duration.count()) * config.pps + 999) /
      1000;
  if (probe_count == 0 || probe_count > maximum_probes)
    throw std::invalid_argument("validate-source cannot exceed 300 probes");
  if (config.profile != Profile::sustained)
    throw std::invalid_argument(
        "validate-source does not support traffic profiles");
}

} // namespace netbench
