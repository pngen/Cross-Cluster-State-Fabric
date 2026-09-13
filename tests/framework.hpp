// Cross-Cluster State Fabric - test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case has a stable identifier "<suite>::<case>" and can be selected
// individually with --case. Cases print BEGIN/PASS/FAIL immediately and
// unbuffered. No case is ever run under a timeout: a hanging case is a defect
// to diagnose, not something to terminate.

#ifndef CCSF_TEST_FRAMEWORK_HPP
#define CCSF_TEST_FRAMEWORK_HPP

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace ccsf::test {

class AssertionFailure : public std::exception {
 public:
  explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

class CaseContext {
 public:
  explicit CaseContext(std::string id) : id_(std::move(id)) {}

  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
  void set_seed(std::uint64_t seed) noexcept { seed_ = seed; }

  void check(bool condition, const std::string& what);
  void expect(bool condition, const std::string& what);

  template <class A, class B>
  void equal(const A& actual, const B& expected, const std::string& what) {
    if (!(actual == expected)) {
      check(false, what);
    }
  }

  void note(const std::string& text);

  /// Writes provider: the runner substitutes this so failures land on stdout.
  static void set_writer(std::function<void(const std::string&)> writer);

 private:
  std::string id_;
  std::uint64_t seed_{0};
  std::vector<std::string> expectations_;
};

using CaseFn = void (*)(CaseContext&);

class Registry {
 public:
  static Registry& instance();

  bool add(const char* suite, const char* name, CaseFn fn);

  int run(int argc, char** argv);

 private:
  struct Entry {
    std::string suite;
    std::string name;
    CaseFn fn;
  };
  std::vector<Entry> entries_;
};

}  // namespace ccsf::test

#define CCSF_CASE(suite_name, case_name)                                                    \
  static void ccsf_case_##suite_name##_##case_name(::ccsf::test::CaseContext& ctx);         \
  namespace {                                                                               \
  const bool ccsf_registered_##suite_name##_##case_name =                                   \
      ::ccsf::test::Registry::instance().add(#suite_name, #case_name,                       \
                                             &ccsf_case_##suite_name##_##case_name);         \
  }                                                                                         \
  static void ccsf_case_##suite_name##_##case_name(::ccsf::test::CaseContext& ctx)

#endif  // CCSF_TEST_FRAMEWORK_HPP
