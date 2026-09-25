// Reads the vertex input locations a SPIR-V vertex shader declares
// (Input-storage OpVariables decorated with Location, built-ins excluded).
#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

namespace spirv {

inline std::vector<uint32_t> VertexInputLocations(const std::vector<uint32_t>& code) {
  std::unordered_map<uint32_t, uint32_t> location;  // id -> Location
  std::set<uint32_t> builtins;
  std::set<uint32_t> inputPointerTypes;  // OpTypePointer ids with storage class Input
  std::vector<std::pair<uint32_t, uint32_t>> variables;  // (result type, id) of OpVariable Input
  for (size_t i = 5; i < code.size();) {
    uint32_t word = code[i], op = word & 0xFFFF, count = word >> 16;
    if (!count || i + count > code.size()) break;
    const uint32_t* w = &code[i];
    if (op == 71 && count >= 4) {  // OpDecorate id decoration ...
      if (w[2] == 30) location[w[1]] = w[3];   // Location
      if (w[2] == 11) builtins.insert(w[1]);   // BuiltIn
    } else if (op == 32 && count >= 4 && w[2] == 1) {  // OpTypePointer Input
      inputPointerTypes.insert(w[1]);
    } else if (op == 59 && count >= 4 && w[3] == 1) {  // OpVariable type id Input
      variables.push_back({w[1], w[2]});
    }
    i += count;
  }
  std::vector<uint32_t> out;
  for (auto& [type, id] : variables) {
    if (builtins.count(id)) continue;
    auto it = location.find(id);
    if (it != location.end()) out.push_back(it->second);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace spirv
