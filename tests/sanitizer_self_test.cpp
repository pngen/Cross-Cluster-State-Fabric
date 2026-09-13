// Cross-Cluster State Fabric - address sanitizer self test.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program exists to prove that the sanitizer is genuinely instrumented. It
// performs a deliberate heap out-of-bounds write and MUST produce a sanitizer
// report with a non-zero exit code. It is never part of the first-party test
// suite, which is required to be clean.

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  const bool expect_report = argc > 1 && std::strcmp(argv[1], "--expect-report") == 0;
  std::printf("sanitizer self test: allocating a 16-byte buffer\n");
  std::fflush(stdout);
  auto* buffer = static_cast<unsigned char*>(std::malloc(16));
  if (buffer == nullptr) {
    std::fprintf(stderr, "allocation failed\n");
    return 2;
  }
  std::memset(buffer, 0x41, 16);
  std::printf("sanitizer self test: writing one byte past the end\n");
  std::fflush(stdout);
  buffer[16] = 0x42;  // deliberate out-of-bounds write
  std::printf("sanitizer self test: no report was produced (instrumentation missing)\n");
  std::fflush(stdout);
  std::free(buffer);
  return expect_report ? 3 : 0;
}
