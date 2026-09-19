// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "framework.hpp"

#include <map>
#include <mutex>
#include <string>

namespace tf {
namespace {

std::mutex& options_mu() {
  static std::mutex m;
  return m;
}

std::map<std::string, std::string>& options() {
  static std::map<std::string, std::string> o;
  return o;
}

}  // namespace

void set_option(const std::string& key, const std::string& value) {
  std::lock_guard<std::mutex> lock(options_mu());
  options()[key] = value;
}

std::string option(const std::string& key, const std::string& fallback) {
  std::lock_guard<std::mutex> lock(options_mu());
  const auto it = options().find(key);
  return it == options().end() ? fallback : it->second;
}

int run_all(const char* filter) {
  int failures = 0;
  int executed = 0;
  std::vector<std::string> failed_names;
  for (const TestCase& test : registry()) {
    std::string full = std::string(test.suite) + "." + test.name;
    if (filter != nullptr && filter[0] != '\0' && full.find(filter) == std::string::npos) continue;
    ++executed;
    try {
      test.fn();
      // Flushed per test so a crash in a later case cannot hide the progress
      // that was already made.
      std::cout << "[ PASS ] " << full << "\n" << std::flush;
    } catch (const Failure& failure) {
      ++failures;
      failed_names.push_back(full);
      std::cout << "[ FAIL ] " << full << "\n         " << failure.what() << "\n" << std::flush;
    } catch (const std::exception& ex) {
      ++failures;
      failed_names.push_back(full);
      std::cout << "[ FAIL ] " << full << "\n         unexpected exception: " << ex.what() << "\n"
                << std::flush;
    } catch (...) {
      ++failures;
      failed_names.push_back(full);
      std::cout << "[ FAIL ] " << full << "\n         unknown exception\n" << std::flush;
    }
  }
  std::cout << "\n" << (executed - failures) << "/" << executed << " test cases passed\n";
  for (const auto& name : failed_names) std::cout << "  failed: " << name << "\n";
  std::cout << std::flush;
  return failures == 0 ? 0 : 1;
}

}  // namespace tf

int main(int argc, char** argv) {
  std::string filter;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--", 0) == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      tf::set_option(arg.substr(2), argv[i + 1]);
      ++i;
      continue;
    }
    if (!arg.empty() && arg[0] != '-') filter = arg;
  }
  return tf::run_all(filter.c_str());
}
