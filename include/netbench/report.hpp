#pragma once

#include <iosfwd>

#include "netbench/config.hpp"
#include "netbench/engine.hpp"

namespace netbench {

void print_progress(std::ostream &output, std::string_view protocol,
                    const MetricsSnapshot &snapshot);
void print_result(std::ostream &output, const Config &config,
                  const TestResult &result);
void write_json(std::ostream &output, const Config &config,
                const TestResult &result);
void write_csv(std::ostream &output, const Config &config,
               const TestResult &result);

} // namespace netbench
