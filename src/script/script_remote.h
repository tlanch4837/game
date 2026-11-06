// Pipe / MCP side of scripting: the commands RemoteRun (main.cpp) forwards, and the same code the headless runner tests.
//   script.list                              the whole registry as JSON (modules, functions, parameter types, docs)
//   script.call|module|function|[json args]  calls one registered function; arguments are a JSON array in declared order
//                                            (vec3 as [x,y,z], quat as [x,y,z,w]); the answer is the result as JSON
//   script.eval|luau code                    runs Luau in the script sandbox and answers with its returned value as text
//                                            (editor only, local pipe only; newlines may be sent as the character 0x1f)
#pragma once
#include <string>
#include <vector>

namespace tbd::script {

class LuauHost;

// args[0] is the command name ("script.list" ...). Returns false with the reason in `out`.
bool RemoteScriptCommand(LuauHost& host, const std::vector<std::string>& args, std::string& out);

} // namespace tbd::script
