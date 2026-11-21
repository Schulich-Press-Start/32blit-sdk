#include "builtin_defaults.hpp"

#include <cstdio>

namespace {
  void write_json_string(std::FILE *out, const char *text) {
    std::fputc('"', out);
    for(auto p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
      if(*p == '"' || *p == '\\') std::fputc('\\', out);
      if(*p < 0x20) std::fprintf(out, "\\u%04x", static_cast<unsigned>(*p));
      else std::fputc(*p, out);
    }
    std::fputc('"', out);
  }
}

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
  // Optional scene-text values. Reflection alone can't construct custom components.
  std::fputs("},\"defaults\":{", out);
  const char *component_separator = "";
  for(uint16_t i = 0; i < sdk::component_count(); ++i) {
    const auto &info = *sdk::component_by_id(i);
    const auto *defaults = static_cast<const char *>(sdk::builtin_component_defaults(info));
    if(!defaults) continue;
    std::fprintf(out, "%s\"%s\":{", component_separator, info.name);
    component_separator = ",";
    const char *field_separator = "";
    for(uint8_t j = 0; j < info.field_count; ++j) {
      const auto &field = info.fields[j];
      char value[sdk::max_value_text];
      if(sdk::format_value(field.type, defaults + field.offset, value, sizeof(value)) < 0) continue;
      std::fprintf(out, "%s\"%s\":", field_separator, field.name);
      write_json_string(out, value);
      field_separator = ",";
    }
    std::fputc('}', out);
  }
  std::fputs("}}\n", out);
  const bool failed = std::ferror(out) != 0;
  return std::fclose(out) != 0 || failed;
}
