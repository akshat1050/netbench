#include "netbench/report.hpp"

#include <iomanip>
#include <ostream>
#include <string>
#include <vector>

namespace netbench {
namespace {

void print_latency(std::ostream &output, std::string_view label,
                   const std::optional<LatencySummary> &latency) {
  output << std::left << std::setw(29) << label << ": ";
  if (!latency) {
    output << "unavailable\n";
    return;
  }
  output << std::fixed << std::setprecision(3) << latency->average_ms
         << " ms avg (p50 " << latency->p50_ms << ", p95 " << latency->p95_ms
         << ", p99 " << latency->p99_ms << ")\n";
}

void json_latency(std::ostream &output,
                  const std::optional<LatencySummary> &latency) {
  if (!latency) {
    output << "null";
    return;
  }
  output << "{\"min_ms\":" << latency->minimum_ms
         << ",\"avg_ms\":" << latency->average_ms
         << ",\"max_ms\":" << latency->maximum_ms
         << ",\"p50_ms\":" << latency->p50_ms
         << ",\"p95_ms\":" << latency->p95_ms
         << ",\"p99_ms\":" << latency->p99_ms << '}';
}

void json_string(std::ostream &output, std::string_view value) {
  output << '"';
  for (const char character : value) {
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      output << character;
    }
  }
  output << '"';
}

void json_strings(std::ostream &output,
                  const std::vector<std::string> &values) {
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0)
      output << ',';
    json_string(output, values[index]);
  }
  output << ']';
}

std::string joined(const std::vector<std::string> &values) {
  std::string result;
  for (const auto &value : values) {
    if (!result.empty())
      result += "; ";
    result += value;
  }
  return result;
}

void csv_cell(std::ostream &output, std::string_view value) {
  output << '"';
  for (const char character : value) {
    if (character == '"')
      output << '"';
    output << character;
  }
  output << '"';
}

} // namespace

void print_progress(std::ostream &output, std::string_view protocol,
                    const MetricsSnapshot &snapshot) {
  output << '[' << protocol << " " << std::fixed << std::setprecision(1)
         << std::chrono::duration<double>(snapshot.elapsed).count() << "s] "
         << snapshot.attempted_per_second() << " attempts/s, "
         << snapshot.successful_per_second() << " submitted/s, "
         << snapshot.transmit_mbps() << " Mbps, "
         << snapshot.connections_succeeded << " connected, "
         << snapshot.socket_errors + snapshot.local_send_failures
         << " errors\n";
}

void print_result(std::ostream &output, const Config &config,
                  const TestResult &result) {
  const auto &m = result.metrics;
  output << "NETBENCH v0.2.0\n"
         << "------------------------------------------------------------\n"
         << std::left << std::setw(29) << "Protocol" << ": " << result.protocol
         << '\n'
         << std::setw(29) << "Target" << ": " << config.target;
  if (config.port != 0)
    output << ':' << config.port;
  output << '\n'
         << std::setw(29) << "Profile" << ": " << profile_name(config.profile)
         << '\n';
  if (result.source_validation) {
    const auto &source = *result.source_validation;
    output << std::setw(29) << "Actual local interface" << ": "
           << source.actual_interface << '\n'
           << std::setw(29) << "Configured source IP" << ": "
           << source.configured_source_ip << '\n'
           << std::setw(29) << "Destination IP" << ": " << source.destination_ip
           << '\n'
           << std::setw(29) << "Test namespace" << ": " << source.test_namespace
           << '\n'
           << std::setw(29) << "Probes transmitted" << ": "
           << source.probes_transmitted << '\n'
           << std::setw(29) << "Local errors" << ": "
           << source.local_errors.size() << '\n'
           << std::setw(29) << "Response observations" << ": "
           << source.response_observations.size() << '\n'
           << std::setw(29) << "Forward/drop externally verified" << ": "
           << (source.forwarding_drop_externally_verified ? "yes" : "no")
           << '\n';
    for (const auto &error : source.local_errors)
      output << "  local error: " << error << '\n';
    for (const auto &observation : source.response_observations)
      output << "  response: " << observation << '\n';
  }
  output << std::setw(29) << "Attempted operations" << ": "
         << m.operations_attempted << '\n'
         << std::setw(29) << "Successful submissions" << ": "
         << m.operations_sent << '\n'
         << std::setw(29) << "Attempted rate" << ": " << std::fixed
         << std::setprecision(3) << m.attempted_per_second() << "/s\n"
         << std::setw(29) << "Successful rate" << ": "
         << m.successful_per_second() << "/s\n"
         << std::setw(29) << "Submission success rate" << ": "
         << m.success_percent() << "%\n"
         << std::setw(29) << "Sender throughput" << ": " << m.transmit_mbps()
         << " Mbps\n"
         << std::setw(29) << "Connections succeeded" << ": "
         << m.connections_succeeded << '\n'
         << std::setw(29) << "Connections failed" << ": "
         << m.connections_failed << '\n'
         << std::setw(29) << "Timeouts" << ": " << m.timeouts << '\n'
         << std::setw(29) << "Connection resets" << ": " << m.resets << '\n'
         << std::setw(29) << "Socket errors" << ": " << m.socket_errors << '\n'
         << std::setw(29) << "Local send failures" << ": "
         << m.local_send_failures << '\n'
         << std::setw(29) << "ICMP unreachable feedback" << ": "
         << m.icmp_unreachable << '\n';
  print_latency(output, "Response/connect latency", result.latency);
  print_latency(output, "Before-load responsiveness",
                result.before_responsiveness);
  print_latency(output, "After-load responsiveness",
                result.after_responsiveness);
  output << std::setw(29) << "Packet loss" << ": ";
  if (result.packet_loss_percent)
    output << *result.packet_loss_percent << "%\n";
  else
    output << "unavailable (no receiver observation)\n";
  output << std::setw(29) << "TCP retransmissions" << ": ";
  if (m.tcp_retransmissions)
    output << *m.tcp_retransmissions << '\n';
  else
    output << "unavailable\n";
  output << std::setw(29) << "Process CPU utilization" << ": "
         << result.resources.cpu_percent << "%\n"
         << std::setw(29) << "Process peak memory" << ": "
         << result.resources.peak_memory_kib << " KiB\n"
         << std::setw(29) << "Measured load duration" << ": "
         << std::chrono::duration<double>(m.elapsed).count() << " s\n"
         << "------------------------------------------------------------\n";
  if (result.protocol == "udp")
    output << "UDP submissions are sender-side evidence; receiver loss is not "
              "inferred.\n";
  if (result.source_validation)
    output << "Local transmission is not proof that a firewall forwarded or "
              "dropped a probe.\n";
}

void write_json(std::ostream &output, const Config &config,
                const TestResult &result) {
  const auto &m = result.metrics;
  output << std::setprecision(10) << "{\n  \"version\":\"0.2.0\",\n"
         << "  \"protocol\":\"" << result.protocol << "\",\n"
         << "  \"target\":\"" << config.target << "\",\n"
         << "  \"port\":" << config.port << ",\n"
         << "  \"profile\":\"" << profile_name(config.profile) << "\",\n"
         << "  \"elapsed_seconds\":"
         << std::chrono::duration<double>(m.elapsed).count() << ",\n"
         << "  \"operations_attempted\":" << m.operations_attempted << ",\n"
         << "  \"operations_submitted\":" << m.operations_sent << ",\n"
         << "  \"responses_received\":" << m.operations_received << ",\n"
         << "  \"bytes_sent\":" << m.bytes_sent << ",\n"
         << "  \"sender_mbps\":" << m.transmit_mbps() << ",\n"
         << "  \"connections_succeeded\":" << m.connections_succeeded
         << ",\n  \"connections_failed\":" << m.connections_failed
         << ",\n  \"timeouts\":" << m.timeouts << ",\n  \"resets\":" << m.resets
         << ",\n  \"socket_errors\":" << m.socket_errors
         << ",\n  \"local_send_failures\":" << m.local_send_failures
         << ",\n  \"icmp_unreachable\":" << m.icmp_unreachable
         << ",\n  \"packet_loss_percent\":";
  if (result.packet_loss_percent)
    output << *result.packet_loss_percent;
  else
    output << "null";
  output << ",\n  \"latency\":";
  json_latency(output, result.latency);
  output << ",\n  \"before_responsiveness\":";
  json_latency(output, result.before_responsiveness);
  output << ",\n  \"after_responsiveness\":";
  json_latency(output, result.after_responsiveness);
  if (result.source_validation) {
    const auto &source = *result.source_validation;
    output << ",\n  \"source_validation\":{\n    \"actual_local_interface\":";
    json_string(output, source.actual_interface);
    output << ",\n    \"configured_source_ip\":";
    json_string(output, source.configured_source_ip);
    output << ",\n    \"destination_ip\":";
    json_string(output, source.destination_ip);
    output << ",\n    \"test_namespace\":";
    json_string(output, source.test_namespace);
    output << ",\n    \"probes_transmitted\":" << source.probes_transmitted
           << ",\n    \"local_errors\":";
    json_strings(output, source.local_errors);
    output << ",\n    \"response_observations\":";
    json_strings(output, source.response_observations);
    output << ",\n    \"forwarding_drop_externally_verified\":"
           << (source.forwarding_drop_externally_verified ? "true" : "false")
           << "\n  }";
  }
  output << ",\n  \"cpu_percent\":" << result.resources.cpu_percent
         << ",\n  \"peak_memory_kib\":" << result.resources.peak_memory_kib
         << "\n}\n";
}

void write_csv(std::ostream &output, const Config &config,
               const TestResult &result) {
  const auto &m = result.metrics;
  output << "version,protocol,target,port,profile,elapsed_seconds,attempted,"
            "submitted,responses,bytes_sent,sender_mbps,connections_succeeded,"
            "connections_failed,timeouts,resets,socket_errors,send_failures,"
            "icmp_unreachable,packet_loss_percent,latency_p50_ms,"
            "latency_p95_ms,latency_p99_ms,cpu_percent,peak_memory_kib";
  if (result.source_validation)
    output << ",actual_local_interface,configured_source_ip,destination_ip,"
              "test_namespace,probes_transmitted,local_errors,"
              "response_observations,forwarding_drop_externally_verified";
  output << '\n'
         << "0.2.0," << result.protocol << ',' << config.target << ','
         << config.port << ',' << profile_name(config.profile) << ','
         << std::chrono::duration<double>(m.elapsed).count() << ','
         << m.operations_attempted << ',' << m.operations_sent << ','
         << m.operations_received << ',' << m.bytes_sent << ','
         << m.transmit_mbps() << ',' << m.connections_succeeded << ','
         << m.connections_failed << ',' << m.timeouts << ',' << m.resets << ','
         << m.socket_errors << ',' << m.local_send_failures << ','
         << m.icmp_unreachable << ',';
  if (result.packet_loss_percent)
    output << *result.packet_loss_percent;
  output << ',';
  if (result.latency)
    output << result.latency->p50_ms << ',' << result.latency->p95_ms << ','
           << result.latency->p99_ms;
  else
    output << ",,";
  output << ',' << result.resources.cpu_percent << ','
         << result.resources.peak_memory_kib;
  if (result.source_validation) {
    const auto &source = *result.source_validation;
    output << ',';
    csv_cell(output, source.actual_interface);
    output << ',';
    csv_cell(output, source.configured_source_ip);
    output << ',';
    csv_cell(output, source.destination_ip);
    output << ',';
    csv_cell(output, source.test_namespace);
    output << ',' << source.probes_transmitted << ',';
    csv_cell(output, joined(source.local_errors));
    output << ',';
    csv_cell(output, joined(source.response_observations));
    output << ','
           << (source.forwarding_drop_externally_verified ? "true" : "false");
  }
  output << '\n';
}

} // namespace netbench
