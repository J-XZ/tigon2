#pragma once

#include <cstdio>
#include <cstring>
#include <string>

#ifndef TIGONKV_BUILD_IDENTITY_NAMESPACE
#error "TIGONKV_BUILD_IDENTITY_NAMESPACE must be supplied by CMake"
#endif

namespace TIGONKV_BUILD_IDENTITY_NAMESPACE {
std::string CompiledIdentityJson();
}

namespace tigonkv {

// This query is deliberately side-effect free. It must run before any test,
// pool, VM, gRPC or trace setup in the executable's main().
inline bool PrintBuildIdentityJsonIfRequested(int argc, char** argv) {
  if (argc != 2 || std::strcmp(argv[1], "--build-identity-json") != 0) {
    return false;
  }
  const std::string identity =
      TIGONKV_BUILD_IDENTITY_NAMESPACE::CompiledIdentityJson();
  std::puts(identity.c_str());
  return true;
}

}  // namespace tigonkv
