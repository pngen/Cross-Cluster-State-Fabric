// Cross-Cluster State Fabric - test runner entry point.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"

#include <cstdio>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
  // No crash dialogs, no abort windows: failures must surface as process exit
  // codes and stderr text so that automated validation never blocks on a modal
  // window.
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  return ccsf::test::Registry::instance().run(argc, argv);
}
