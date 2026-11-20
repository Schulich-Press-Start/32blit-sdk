#include "registry.hpp"

#include <cstdio>

int main(int argc, char **argv) {
  if(argc != 2) return 1;
  auto *out = std::fopen(argv[1], "wb");
  if(!out) {
    std::perror(argv[1]);
    return 1;
  }
  std::fputs("{\"version\":1,\"components\":{", out);
  for(uint16_t i = 0; i < sdk::component_count(); ++i) {
    const auto &info = *sdk::component_by_id(i);
    // SDK_COMPONENT stringifies C++ identifiers; they cannot contain quotes or backslashes.
    std::fprintf(out, "%s\"%s\":{", i ? "," : "", info.name);
    for(uint8_t j = 0; j < info.field_count; ++j) {
      const auto &field = info.fields[j];
      std::fprintf(out, "%s\"%s\":\"%s\"", j ? "," : "", field.name, sdk::field_type_name(field.type));
    }
    std::fputc('}', out);
  }
  std::fputs("}}\n", out);
  const bool failed = std::ferror(out) != 0;
  return std::fclose(out) != 0 || failed;
}
