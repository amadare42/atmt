// cli.h - the command line half of atmt_manager (see cli.cpp for the commands).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace atmt {

// Arguments that ask for the CLI rather than the window.
bool IsCliInvocation(const std::vector<std::string>& args);
// `args` without the program name. Returns the process exit code.
int RunCli(const std::vector<std::string>& args);

}  // namespace atmt
