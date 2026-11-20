#include "world.hpp"

#include <atomic>
#include <cstring>

namespace sdk {
  namespace {
    // Reserve zero for invalid handles. Never recycle IDs, even if a new World
    // is constructed at the same address. Only World setup touches this counter.
    uint32_t issue_world_id() {
      static std::atomic<uint32_t> last_id{0};
      auto last = last_id.load(std::memory_order_relaxed);
      do {
        if(last == std::numeric_limits<uint32_t>::max())
          throw std::overflow_error("World ID space exhausted");
      } while(!last_id.compare_exchange_weak(last, last + 1, std::memory_order_relaxed));
      return last + 1;
    }
  }

  World::World(uint16_t capacity) : id_(issue_world_id()), slots_(new Slot[capacity]), capacity_(capacity) {
    for(uint32_t i = 0; i < capacity; ++i)
      slots_[i].next = i + 1 < capacity ? static_cast<uint16_t>(i + 1) : UINT16_MAX;
    if(capacity) free_ = 0;
  }

  World::~World() {
    if(querying_) std::terminate();
    // Pools may be destroyed after their owner; their own storage remains valid.
    for(auto *pool = pools_; pool; pool = pool->next_) pool->world_ = nullptr;
  }

  Entity World::create(const char *name) {
    if(querying_ || free_ == UINT16_MAX) return {};

    ComponentPool<Name> *names = nullptr;
    size_t length = 0;
    if(name) {
      const auto *info = component_info<Name>();
      names = info ? static_cast<ComponentPool<Name> *>(pool_for(info->id)) : nullptr;
      if(!names || names->size() == names->capacity()) return {};
      length = std::strlen(name);
      if(length == 0 || length >= string_field_size || find(name)) return {};
    }

    const uint16_t index = free_;
    Slot &slot = slots_[index];
    free_ = slot.next;
    slot.alive = true;
    ++count_;
    if(names)
      std::memcpy(names->emplace_index(index)->value, name, length + 1);
    return {slot.generation, index, id_};
  }

  Entity World::find(const char *name) const {
    if(!name) return {};
    const size_t length = std::strlen(name);
    if(length == 0 || length >= string_field_size) return {};
    const auto *info = component_info<Name>();
    const auto *names = info ? static_cast<const ComponentPool<Name> *>(pool_for(info->id)) : nullptr;
    if(!names) return {};
    for(uint16_t i = 0; i < names->count_; ++i) {
      if(std::strncmp(names->at(i)->value, name, length + 1) != 0) continue;
      const uint16_t index = names->entities_[i];
      return {slots_[index].generation, index, id_};
    }
    return {};
  }

  bool World::alive(Entity entity) const {
    return entity.world_id_ == id_ && entity.index_ < capacity_
      && slots_[entity.index_].alive && slots_[entity.index_].generation == entity.generation_;
  }

  Entity World::entity_at(uint16_t index) const {
    if(index >= capacity_ || !slots_[index].alive) return {};
    return {slots_[index].generation, index, id_};
  }

  bool World::destroy(Entity entity) {
    if(querying_ || !alive(entity)) return false;
    for(auto *pool = pools_; pool; pool = pool->next_) pool->remove_entity(entity.index_);
    Slot &slot = slots_[entity.index_];
    slot.alive = false;
    --count_;
    // Retire the index at wraparound rather than resurrect an ancient handle.
    if(slot.generation != std::numeric_limits<uint32_t>::max()) {
      ++slot.generation;
      slot.next = free_;
      free_ = entity.index_;
    }
    return true;
  }

  ComponentPoolBase *World::pool_for(uint16_t component_id) const {
    return component_id < max_components ? registered_pools_[component_id] : nullptr;
  }

  void *World::add(Entity entity, uint16_t component_id) {
    if(querying_) return nullptr;
    auto *pool = pool_for(component_id);
    return pool && alive(entity) ? pool->add_entity(entity.index_) : nullptr;
  }

  void *World::get(Entity entity, uint16_t component_id) {
    return const_cast<void *>(std::as_const(*this).get(entity, component_id));
  }

  const void *World::get(Entity entity, uint16_t component_id) const {
    const auto *pool = pool_for(component_id);
    return pool && alive(entity) ? pool->get_entity(entity.index_) : nullptr;
  }

  bool World::remove(Entity entity, uint16_t component_id) {
    if(querying_) return false;
    auto *pool = pool_for(component_id);
    return pool && alive(entity) && pool->remove_entity(entity.index_);
  }

  ComponentPoolBase::ComponentPoolBase(World &world, const ComponentInfo *info)
    : world_(&world), component_id_(info ? info->id : UINT16_MAX), next_(world.pools_) {
    if(world.querying_) throw std::logic_error("cannot attach a component pool during a query");
    if(info) {
      auto *&registered = world.registered_pools_[component_id_];
      if(registered) throw std::invalid_argument("component pool already attached");
      registered = this;
    }
    world.pools_ = this;
  }

  ComponentPoolBase::~ComponentPoolBase() {
    if(!world_) return;
    if(component_id_ != UINT16_MAX) world_->registered_pools_[component_id_] = nullptr;
    auto **link = &world_->pools_;
    while(*link != this) link = &(*link)->next_;
    *link = next_;
  }
}
