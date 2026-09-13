// Cross-Cluster State Fabric - test framework implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ccsf::test {
namespace {

std::function<void(const std::string&)> g_writer;

void default_writer(const std::string& text) {
  std::fputs(text.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void write_line(const std::string& text) {
  if (g_writer) {
    g_writer(text);
  } else {
    default_writer(text);
  }
}

std::vector<std::string> split(const std::string& text, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  for (char character : text) {
    if (character == delimiter) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }
  parts.push_back(current);
  return parts;
}

}  // namespace

void CaseContext::set_writer(std::function<void(const std::string&)> writer) {
  g_writer = std::move(writer);
}

void CaseContext::check(bool condition, const std::string& what) {
  if (!condition) {
    throw AssertionFailure(what + " [" + id_ + "]");
  }
}

void CaseContext::expect(bool condition, const std::string& what) {
  if (!condition) {
    expectations_.push_back(what);
  }
}

void CaseContext::note(const std::string& text) {
  write_line("NOTE " + id_ + ": " + text);
}

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

bool Registry::add(const char* suite, const char* name, CaseFn fn) {
  Entry entry;
  entry.suite = suite;
  entry.name = name;
  entry.fn = fn;
  entries_.push_back(std::move(entry));
  return true;
}

int Registry::run(int argc, char** argv) {
  std::vector<std::string> filters;
  bool list_only = false;
  std::uint64_t seed = 20260101ULL;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--case" && index + 1 < argc) {
      filters.push_back(argv[++index]);
    } else if (argument == "--seed" && index + 1 < argc) {
      seed = std::strtoull(argv[++index], nullptr, 10);
    }
  }

  if (list_only) {
    for (const Entry& entry : entries_) {
      write_line(entry.suite + "::" + entry.name);
    }
    return 0;
  }

  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t skipped = 0;
  for (const Entry& entry : entries_) {
    const std::string id = entry.suite + "::" + entry.name;
    bool selected = filters.empty();
    for (const std::string& filter : filters) {
      if (filter == id || filter == entry.suite || filter == (entry.suite + "::*")) {
        selected = true;
      }
    }
    if (!selected) {
      ++skipped;
      continue;
    }
    write_line("BEGIN " + id);
    CaseContext context(id);
    context.set_seed(seed);
    try {
      entry.fn(context);
      write_line("PASS " + id);
      ++passed;
    } catch (const AssertionFailure& failure) {
      write_line(std::string("FAIL ") + id + ": " + failure.what());
      ++failed;
    } catch (const std::exception& error) {
      write_line(std::string("FAIL ") + id + ": unexpected exception: " + error.what());
      ++failed;
    } catch (...) {
      write_line("FAIL " + id + ": unexpected non-standard exception");
      ++failed;
    }
  }

  write_line("SUMMARY passed=" + std::to_string(passed) + " failed=" + std::to_string(failed) +
             " skipped=" + std::to_string(skipped));
  return failed == 0 ? 0 : 1;
}

}  // namespace ccsf::test
