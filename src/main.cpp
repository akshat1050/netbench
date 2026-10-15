#include "netbench/config.hpp"
#include "netbench/engine.hpp"
#include "netbench/report.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>

namespace {

std::atomic_bool interrupted{};

extern "C" void handle_signal(int) {
  interrupted.store(true, std::memory_order_relaxed);
}

template <typename Work> void run_interruptible(Work &&work) {
  std::atomic_bool done{};
  std::exception_ptr failure;
  std::jthread worker([&](std::stop_token stop) {
    try {
      work(stop);
    } catch (...) {
      failure = std::current_exception();
    }
    done.store(true, std::memory_order_release);
  });
  while (!done.load(std::memory_order_acquire) &&
         !interrupted.load(std::memory_order_relaxed))
    std::this_thread::sleep_for(std::chrono::milliseconds{25});
  if (interrupted.load(std::memory_order_relaxed))
    worker.request_stop();
  worker.join();
  if (failure)
    std::rethrow_exception(failure);
}

void export_report(const std::string &path, std::string_view format,
                   const netbench::Config &config,
                   const netbench::TestResult &result) {
  if (path.empty())
    return;
  std::ofstream output(path);
  if (!output)
    throw std::runtime_error("cannot open " + std::string(format) +
                             " output: " + path);
  if (format == "JSON")
    netbench::write_json(output, config, result);
  else
    netbench::write_csv(output, config, result);
  if (!output)
    throw std::runtime_error("failed to write " + std::string(format) +
                             " output: " + path);
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto config = netbench::parse_command_line(argc, argv);
    if (config.command == netbench::Command::help) {
      std::cout << netbench::usage();
      return 0;
    }
    if (config.command == netbench::Command::version) {
      std::cout << "netbench 0.2.0\n";
      return 0;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::optional<netbench::TestResult> result;
    run_interruptible([&](std::stop_token stop) {
      const netbench::ProgressCallback progress = [&](const auto &snapshot) {
        const std::string_view protocol =
            config.command == netbench::Command::tcp    ? "tcp"
            : config.command == netbench::Command::udp  ? "udp"
            : config.command == netbench::Command::icmp ? "icmp"
                                                        : "source-validation";
        netbench::print_progress(std::cerr, protocol, snapshot);
      };
      if (config.command == netbench::Command::tcp)
        result = netbench::run_tcp_test(config, stop, progress);
      else if (config.command == netbench::Command::udp)
        result = netbench::run_udp_test(config, stop, progress);
      else if (config.command == netbench::Command::icmp)
        result = netbench::run_icmp_test(config, stop, progress);
      else
        result = netbench::SourceValidationEngine::run(config, stop, progress);
    });
    if (result) {
      netbench::print_result(std::cout, config, *result);
      export_report(config.json_path, "JSON", config, *result);
      export_report(config.csv_path, "CSV", config, *result);
    }
    return interrupted.load(std::memory_order_relaxed) ? 130 : 0;
  } catch (const std::exception &error) {
    std::cerr << "netbench: " << error.what() << "\n\n" << netbench::usage();
    return 2;
  }
}
