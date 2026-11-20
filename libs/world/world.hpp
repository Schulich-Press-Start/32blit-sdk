#pragma once

#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

#include "registry.hpp"

namespace sdk {

  /// Runtime handle. Only the issuing World accepts it; never persist it.
  struct Entity {
    Entity() = default;
    uint32_t generation() const { return generation_; }
    uint16_t index() const { return index_; }
    explicit operator bool() const { return generation_ != 0; }

  private:
    friend class World;
    Entity(uint32_t generation, uint16_t index, uint32_t world_id)
      : generation_(generation), index_(index), world_id_(world_id) {}
    uint32_t generation_ = 0;
    uint16_t index_ = UINT16_MAX;
    uint32_t world_id_ = 0;
  };

  class ComponentPoolBase;
  template<class T> class ComponentPool;

  /// Fixed entity budget; construction allocates, create/destroy never do.
  /// Non-copyable/non-movable so attached pools keep a stable owner.
  class World {
  public:
    explicit World(uint16_t capacity);
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    /// Named creation needs a Name pool and rejects empty, long or duplicate names.
    /// Returns an invalid handle if the entity or Name budget is exhausted.
    Entity create(const char *name = nullptr);
    Entity find(const char *name) const;
    bool destroy(Entity entity);     ///< False for an invalid, stale or foreign handle.
    bool alive(Entity entity) const;
    /// Return the live handle at a slot, or an invalid handle for a dead/out-of-range slot.
    Entity entity_at(uint16_t index) const;
    uint16_t capacity() const { return capacity_; }
    uint16_t size() const { return count_; }

    /// Access a registered component whose pool was attached during setup.
    /// add returns the existing value, or default-constructs it if there is room.
    /// A missing pool, invalid entity, or unsupported construction returns nullptr.
    void *add(Entity entity, uint16_t component_id);
    void *get(Entity entity, uint16_t component_id);
    const void *get(Entity entity, uint16_t component_id) const;
    bool remove(Entity entity, uint16_t component_id);

    template<class T> T *add(Entity entity) {
      const auto *info = component_info<T>();
      return info ? static_cast<T *>(add(entity, info->id)) : nullptr;
    }
    template<class T> T *get(Entity entity) {
      const auto *info = component_info<T>();
      return info ? static_cast<T *>(get(entity, info->id)) : nullptr;
    }
    template<class T> const T *get(Entity entity) const {
      const auto *info = component_info<T>();
      return info ? static_cast<const T *>(get(entity, info->id)) : nullptr;
    }
    template<class T> bool remove(Entity entity) {
      const auto *info = component_info<T>();
      return info && remove(entity, info->id);
    }

    /// Call fn(Entity, C&...) for entities with all requested registered components.
    /// Iteration order is unspecified. Fields may be edited; structural operations
    /// on this World fail until all callbacks return. Do not retain references.
    /// Attaching a pool throws during iteration; destroying the World or a pool terminates.
    template<class... C, class F> void each(F &&fn);

  private:
    friend class ComponentPoolBase;
    friend struct SceneAccess;
    struct Slot { uint32_t generation = 1; uint16_t next = UINT16_MAX; bool alive = false; };
    const uint32_t id_;
    std::unique_ptr<Slot[]> slots_;
    uint16_t capacity_, count_ = 0, free_ = UINT16_MAX;
    ComponentPoolBase *pools_ = nullptr;
    ComponentPoolBase *registered_pools_[max_components] = {};
    bool querying_ = false;
    ComponentPoolBase *pool_for(uint16_t component_id) const;
  };

  /// Intrusive pool attachment: no allocation per attachment or entity removal.
  class ComponentPoolBase {
  public:
    ComponentPoolBase(const ComponentPoolBase &) = delete;
    ComponentPoolBase &operator=(const ComponentPoolBase &) = delete;

  private:
    explicit ComponentPoolBase(World &world, const ComponentInfo *info);
    virtual ~ComponentPoolBase();
    World *world_;
    friend class World;
    friend struct SceneAccess;
    template<class T> friend class ComponentPool;
    bool querying() const { return world_ && world_->querying_; }
    virtual void *add_entity(uint16_t index) = 0;
    virtual const void *get_entity(uint16_t index) const = 0;
    virtual bool remove_entity(uint16_t index) noexcept = 0;
    virtual uint16_t size() const noexcept = 0;
    virtual uint16_t capacity() const noexcept = 0;
    virtual bool default_constructible() const noexcept = 0;
    uint16_t component_id_;
    ComponentPoolBase *next_;
  };

  /// Packed typed storage with a sparse entity-to-slot lookup.
  /// Capacity is chosen at setup, separately from the World's entity budget.
  /// Structural changes invalidate pointers; no mutation during iteration.
  /// Attach after registration (e.g. in init()). Only one pool per registered
  /// type per World; unregistered types are available through the pool directly.
  template<class T> class ComponentPool final : public ComponentPoolBase {
    friend class World;
    static_assert(std::is_nothrow_move_constructible_v<T>, "packed components must move without throwing");
    static_assert(std::is_nothrow_destructible_v<T>, "component destructors must not throw");
    struct Storage { alignas(T) unsigned char bytes[sizeof(T)]; };
  public:
    ComponentPool(World &world, uint16_t capacity)
      : ComponentPoolBase(world, component_info<T>()), capacity_(checked_capacity(world, capacity)),
        data_(new Storage[capacity_]), entities_(new uint16_t[capacity_]),
        sparse_(new uint16_t[world.capacity()]) {
      for(uint16_t i = 0; i < world.capacity(); ++i) sparse_[i] = UINT16_MAX;
    }
    ~ComponentPool() override {
      if(querying()) std::terminate();
      clear();
    }

    template<class... Args> T *emplace(Entity entity, Args&&... args) {
      static_assert(std::is_nothrow_constructible_v<T, Args...>, "component construction must not throw during play");
      if(!world_ || querying() || !world_->alive(entity)) return nullptr;
      return emplace_index(entity.index(), std::forward<Args>(args)...);
    }
    T *get(Entity entity) {
      return const_cast<T *>(std::as_const(*this).get(entity));
    }
    const T *get(Entity entity) const {
      return world_ && world_->alive(entity) ? static_cast<const T *>(get_entity(entity.index())) : nullptr;
    }
    bool erase(Entity entity) {
      return world_ && !querying() && world_->alive(entity) && remove_entity(entity.index());
    }
    uint16_t size() const noexcept override { return count_; }
    uint16_t capacity() const noexcept override { return capacity_; }

  private:
    bool default_constructible() const noexcept override { return std::is_nothrow_default_constructible_v<T>; }
    template<class... Args> T *emplace_index(uint16_t entity, Args&&... args) {
      const uint16_t existing = sparse_[entity];
      if(existing != UINT16_MAX) return at(existing);
      if(count_ == capacity_) return nullptr;
      T *value = ::new (static_cast<void *>(data_[count_].bytes)) T(std::forward<Args>(args)...);
      entities_[count_] = entity;
      sparse_[entity] = count_++;
      return value;
    }
    void *add_entity(uint16_t entity) override {
      if constexpr(std::is_nothrow_default_constructible_v<T>)
        return emplace_index(entity);
      else
        return const_cast<void *>(get_entity(entity));
    }
    const void *get_entity(uint16_t entity) const override {
      const uint16_t index = sparse_[entity];
      return index == UINT16_MAX ? nullptr : at(index);
    }
    static uint16_t checked_capacity(const World &world, uint16_t capacity) {
      if(capacity > world.capacity()) throw std::invalid_argument("component capacity exceeds entity budget");
      return capacity;
    }
    T *at(uint16_t index) { return std::launder(reinterpret_cast<T *>(data_[index].bytes)); }
    const T *at(uint16_t index) const { return std::launder(reinterpret_cast<const T *>(data_[index].bytes)); }
    bool remove_entity(uint16_t entity) noexcept override {
      const uint16_t index = sparse_[entity];
      if(index == UINT16_MAX) return false;
      at(index)->~T();
      --count_;
      if(index != count_) {
        ::new (static_cast<void *>(data_[index].bytes)) T(std::move(*at(count_)));
        at(count_)->~T();
        entities_[index] = entities_[count_];
        sparse_[entities_[index]] = index;
      }
      sparse_[entity] = UINT16_MAX;
      return true;
    }
    void clear() noexcept { while(count_) at(--count_)->~T(); }
    uint16_t capacity_, count_ = 0;
    std::unique_ptr<Storage[]> data_;
    std::unique_ptr<uint16_t[]> entities_, sparse_;
  };

  template<class... C, class F> void World::each(F &&fn) {
    static_assert(sizeof...(C) > 0, "each requires at least one component type");
    auto attached = [this](const ComponentInfo *info) {
      return info ? pool_for(info->id) : nullptr;
    };
    auto pools = std::tuple{static_cast<ComponentPool<C> *>(attached(component_info<C>()))...};
    std::apply([&](auto *...pool) {
      if((!pool || ...)) return;

      const ComponentPoolBase *driver = nullptr;
      uint16_t count = UINT16_MAX;
      auto choose_driver = [&](const ComponentPoolBase *candidate, uint16_t size) {
        if(!driver || size < count) {
          driver = candidate;
          count = size;
        }
      };
      (choose_driver(pool, pool->size()), ...);
      if(!count) return;

      struct QueryScope {
        bool &active;
        bool previous;
        ~QueryScope() { active = previous; }
      };
      const QueryScope scope{querying_, std::exchange(querying_, true)};

      // Specialize the loop for the driver once, not once per entity. Its
      // component is already at the dense row; only the other pools need a lookup.
      auto visit = [&](auto *dense) {
        for(uint16_t row = 0; row < count; ++row) {
          const uint16_t index = dense->entities_[row];
          auto component_at = [&](auto *candidate) {
            if constexpr(std::is_same_v<decltype(candidate), decltype(dense)>) {
              return dense->at(row);
            } else {
              const uint16_t slot = candidate->sparse_[index];
              return slot == UINT16_MAX ? nullptr : candidate->at(slot);
            }
          };
          auto call_if_match = [&](auto *...value) {
            if((value && ...)) fn(Entity{slots_[index].generation, index, id_}, *value...);
          };
          call_if_match(component_at(pool)...);
        }
      };
      static_cast<void>(((driver == pool ? (visit(pool), true) : false) || ...));
    }, pools);
  }
}
