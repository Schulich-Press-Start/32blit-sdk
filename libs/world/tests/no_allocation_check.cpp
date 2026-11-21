// The library must not allocate while a game is running: the handheld has no
// room for fragmentation and no tolerance for a malloc inside a frame. This
// counts allocations around the calls under test, which catches the usual way
// that rule gets broken, someone reaching for std::string or std::vector.
//
// A program of its own, with no GoogleTest. Replacing global operator new and
// delete in a binary means every allocation in it must go through the pair
// below; GoogleTest allocates through variants it is not worth replacing
// (std::get_temporary_buffer uses nothrow new), and the mismatch shows up
// under AddressSanitizer as alloc-dealloc-mismatch.
#include <cstdio>
#include <cstdlib>
#include <new>

#include "actions.hpp"
#include "registry.hpp"
#include "scene.hpp"
#include "world.hpp"

namespace {
  int allocations = 0;

  void *counted(size_t size) {
    allocations++;
    void *memory = malloc(size ? size : 1);

    if(!memory)
      abort();

    return memory;
  }
}

void *operator new(size_t size) { return counted(size); }
void *operator new[](size_t size) { return counted(size); }
void *operator new(size_t size, const std::nothrow_t &) noexcept { return counted(size); }
void *operator new[](size_t size, const std::nothrow_t &) noexcept { return counted(size); }

void operator delete(void *memory) noexcept { free(memory); }
void operator delete[](void *memory) noexcept { free(memory); }
void operator delete(void *memory, size_t) noexcept { free(memory); }
void operator delete[](void *memory, size_t) noexcept { free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { free(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { free(memory); }

namespace {
  bool read_scene_fixture(void *, std::string_view path, std::string_view &text) {
    if(path != "level.scene") return false;
    text = "scene 1 Level\nprefab enemy\n  Transform2D rotation=5\nentity pilot\n  Transform2D x=9\n";
    return true;
  }
}

int main() {
  char text[sdk::max_value_text];
  char name[sdk::string_field_size] = {};
  float number = 0.0f;
  sdk::World world(8);
  sdk::ComponentPool<sdk::Name> names(world, 4);
  sdk::ComponentPool<sdk::Transform2D> positions(world, 4);
  const sdk::SceneLimits scene_limits{8, 8, 16, 2};
  sdk::SceneRuntime scenes(world, scene_limits, {nullptr, read_scene_fixture});
  sdk::SceneDocument scratch(scene_limits);
  sdk::Entity bindings[8];

  const int before = allocations;

  // lookup
  auto component = sdk::find_component("Transform2D");
  if(!component) return 1;
  sdk::component_by_id(0);
  sdk::component_count();
  sdk::field_type_name(sdk::FieldType::Float);

  // values, including a rejected one
  sdk::parse_value(sdk::FieldType::Float, "not-a-float", &number);
  sdk::parse_value(sdk::FieldType::Float, "1.5", &number);
  sdk::format_value(sdk::FieldType::Float, &number, text, sizeof(text));
  sdk::parse_value(sdk::FieldType::String, "player", name);
  sdk::format_value(sdk::FieldType::String, name, text, sizeof(text));

  for(int tick = 0; tick < 1000; ++tick) {
    auto entity = world.create("player");
    sdk::Action action;
    sdk::Result result;
    if(!sdk::parse_console("add player.Transform2D", action, result) || !sdk::apply(world, action).ok) return 2;
    if(!sdk::parse_console("set player.Transform2D.x 2", action, result) || !sdk::apply(world, action).ok) return 3;
    char printed[128];
    if(sdk::to_console(action, printed, sizeof(printed)) < 0 ||
       sdk::to_cpp(action, printed, sizeof(printed)) < 0 ||
       sdk::to_scene(action, printed, sizeof(printed)) < 0) return 6;
    int visited = 0;
    world.each<sdk::Name, sdk::Transform2D>([&](sdk::Entity match, const sdk::Name &, sdk::Transform2D &t) {
      if(match.index() == entity.index() && t.x == 2) ++visited;
    });
    if(visited != 1) return 5;
    if(!sdk::parse_console("remove player.Transform2D", action, result) || !sdk::apply(world, action).ok) return 4;
    if(!sdk::parse_console("destroy player", action, result) || !sdk::apply(world, action).ok || world.find("player")) return 4;
  }

  sdk::Action scene_action;
  sdk::Result scene_result;
  if(!sdk::parse_console("load_scene level.scene", scene_action, scene_result) ||
     !sdk::apply(world, scene_action, &scenes).ok) return 7;
  if(!sdk::parse_console("spawn enemy 300 100", scene_action, scene_result) ||
     !sdk::apply(world, scene_action, &scenes).ok) return 8;
  char snapshot[512];
  sdk::SceneError error;
  const int length = sdk::save_world(world, snapshot, sizeof(snapshot));
  if(length < 0 || !sdk::load_world(world, {snapshot, static_cast<size_t>(length)}, scratch,
                                    bindings, 8, error) || !world.find("pilot") || world.size() != 2) return 9;

  const int allocated = allocations - before;

  if(allocated) {
    printf("FAILED: %d allocation(s) during runtime World operations\n", allocated);
    return 1;
  }

  printf("PASSED: no allocations during runtime World operations\n");
  return 0;
}
