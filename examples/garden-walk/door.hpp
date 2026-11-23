#pragma once

#include "registry.hpp"

struct Door {
  char scene[sdk::string_field_size] = {};
  char arrival[sdk::string_field_size] = {};
};
