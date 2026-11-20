/**
 * \file registry.hpp
 * \brief Component registry: the names, sizes and fields of component types.
 *
 * A component is a plain struct. `SDK_COMPONENT` records its name and the
 * name, type and offset of each field, so tools that know nothing about the
 * type can still read and write it: the editor's inspector, the console, and
 * scene files.
 *
 * ```cpp
 * struct Transform { float x, y, rotation; };
 * SDK_COMPONENT(Transform, x, y, rotation);
 * ```
 *
 * Registration happens before main(), into a fixed array that needs no
 * initialisation, so registrars may run in any order. Nothing here allocates.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace sdk {

  /// Largest number of component types one program may register.
  constexpr int max_components = 64;

  /// Largest number of fields one component may have (the `SDK_COMPONENT` limit).
  constexpr int max_component_fields = 16;

  /// Length of a String field, including its terminator.
  constexpr int string_field_size = 24;

  struct Name { char value[string_field_size] = {}; };
  struct Transform { float x = 0, y = 0, rotation = 0; };
  struct Entity;

  /** \brief The types a component field may have. */
  enum class FieldType : uint8_t {
    Bool,
    Int32,
    UInt32,
    Float,
    String,  ///< char[string_field_size]
    Entity,  ///< Contextual reference, resolved by the scene codec.
  };

  /** \brief One field of a component: what it is called, what it holds, where it sits. */
  struct FieldInfo {
    const char *name;
    FieldType type;
    uint16_t offset;
  };

  /** \brief One registered component type. */
  struct ComponentInfo {
    const char *name;
    uint16_t id;        ///< Registration order, stable within one program.
    uint16_t size;      ///< sizeof the struct, for copying one without knowing its type.
    const FieldInfo *fields;
    uint8_t field_count;
  };

  /// Longest text any field value needs, including the terminator: a full
  /// string field, or a float's shortest round-trip form.
  constexpr int max_value_text = 32;

  /**
   * \brief Read a field value from text.
   *
   * Accepts `true`, `false`, `1` and `0` for Bool, base-ten digits for the
   * integers, and any finite form `std::from_chars` reads for Float; `inf` and
   * `nan` are refused, because no game value is either and a nan spreads
   * silently. The whole of `text` must be the value: no leading or trailing
   * spaces, and nothing after it. Strings must fit in string_field_size
   * including the terminator.
   *
   * \param dst Storage for the field's own type, at least as large as it.
   * \return false if the text isn't a valid value of this type, leaving `dst` untouched.
   */
  bool parse_value(FieldType type, const char *text, void *dst);

  /**
   * \brief Write a field value as text, in the shortest form that reads back
   *        as the same value.
   *
   * \param buf Always terminated when this succeeds; needs max_value_text to fit any value.
   * \return Characters written, not counting the terminator, or -1 if `buf` is too small.
   */
  int format_value(FieldType type, const void *src, char *buf, int cap);

  /** \brief The name of a field type, for error messages: "bool", "int32", ... */
  const char *field_type_name(FieldType type);

  /** \brief Find a registered component by name, or nullptr. */
  const ComponentInfo *find_component(const char *name);

  /** \brief Find a registered component by id, or nullptr. */
  const ComponentInfo *component_by_id(uint16_t id);

  /** \brief Find a field by its exact name, or nullptr. */
  const FieldInfo *find_field(const ComponentInfo &component, const char *name);

  /** \brief How many component types are registered. */
  uint16_t component_count();

  namespace detail {
    /// Maps a C++ field type to a FieldType. A field of any other type fails to compile.
    template<class T> struct field_type_of;
    template<> struct field_type_of<bool>     { static constexpr FieldType value = FieldType::Bool; };
    template<> struct field_type_of<int32_t>  { static constexpr FieldType value = FieldType::Int32; };
    template<> struct field_type_of<uint32_t> { static constexpr FieldType value = FieldType::UInt32; };
    template<> struct field_type_of<float>    { static constexpr FieldType value = FieldType::Float; };
    template<> struct field_type_of<Entity>   { static constexpr FieldType value = FieldType::Entity; };

    template<size_t N> struct field_type_of<char[N]> {
      static_assert(N == string_field_size, "string fields must be char[sdk::string_field_size]");
      static constexpr FieldType value = FieldType::String;
    };

    template<class T> constexpr FieldType type_of() {
      static_assert(!std::is_const_v<T> && !std::is_volatile_v<T>, "registered fields must be writable and non-volatile");
      return field_type_of<T>::value;
    }

    template<class T> inline const ComponentInfo *registered_component = nullptr;

    /** \brief Adds one component to the registry when its program starts. */
    struct ComponentRegistrar {
      ComponentInfo info;
      ComponentRegistrar(const char *name, uint16_t size,
                         const FieldInfo *fields, uint8_t field_count, const ComponentInfo *&type_info);
    };
  }

  /// Metadata for a C++ type registered with SDK_COMPONENT, or nullptr.
  template<class T> const ComponentInfo *component_info() {
    return detail::registered_component<T>;
  }
}

/// \cond INTERNAL
// Calls `m(Type, field)` for each field. The SDK_EXPAND indirection is what
// makes this work on MSVC's traditional preprocessor, which expands __VA_ARGS__
// as a single token without it. Builds with /Zc:preprocessor too.
#define SDK_EXPAND(x) x
#define SDK_CONCAT_(a, b) a##b
#define SDK_CONCAT(a, b) SDK_CONCAT_(a, b)

#define SDK_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define SDK_NARG(...) SDK_EXPAND(SDK_ARG_N(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1))

#define SDK_FE1(m, T, a) m(T, a)
#define SDK_FE2(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE1(m, T, __VA_ARGS__))
#define SDK_FE3(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE2(m, T, __VA_ARGS__))
#define SDK_FE4(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE3(m, T, __VA_ARGS__))
#define SDK_FE5(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE4(m, T, __VA_ARGS__))
#define SDK_FE6(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE5(m, T, __VA_ARGS__))
#define SDK_FE7(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE6(m, T, __VA_ARGS__))
#define SDK_FE8(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE7(m, T, __VA_ARGS__))
#define SDK_FE9(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE8(m, T, __VA_ARGS__))
#define SDK_FE10(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE9(m, T, __VA_ARGS__))
#define SDK_FE11(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE10(m, T, __VA_ARGS__))
#define SDK_FE12(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE11(m, T, __VA_ARGS__))
#define SDK_FE13(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE12(m, T, __VA_ARGS__))
#define SDK_FE14(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE13(m, T, __VA_ARGS__))
#define SDK_FE15(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE14(m, T, __VA_ARGS__))
#define SDK_FE16(m, T, a, ...) m(T, a) SDK_EXPAND(SDK_FE15(m, T, __VA_ARGS__))

#define SDK_FOR_EACH_FIELD(m, T, ...) SDK_EXPAND(SDK_CONCAT(SDK_FE, SDK_NARG(__VA_ARGS__))(m, T, __VA_ARGS__))

#define SDK_FIELD_INFO(T, field) \
  ::sdk::FieldInfo{#field, ::sdk::detail::type_of<decltype(T::field)>(), \
                   static_cast<uint16_t>(offsetof(T, field))},
/// \endcond

/**
 * \brief Register a component type and its fields.
 *
 * Write it once, at namespace scope, in the source file that defines the
 * struct: `SDK_COMPONENT(Transform, x, y, rotation);`. Up to
 * sdk::max_component_fields fields, each of a type sdk::FieldType covers.
 */
#define SDK_COMPONENT(T, ...)                                                                  \
  static_assert(std::is_standard_layout_v<T>, #T " must be a standard-layout struct");         \
  static_assert(sizeof(T) <= UINT16_MAX, #T " exceeds the component size limit");               \
  static_assert(SDK_NARG(__VA_ARGS__) <= ::sdk::max_component_fields,                          \
                #T " has more fields than sdk::max_component_fields");                         \
  static const ::sdk::FieldInfo sdk_fields_##T[] = {                                           \
    SDK_FOR_EACH_FIELD(SDK_FIELD_INFO, T, __VA_ARGS__)                                         \
  };                                                                                           \
  static const ::sdk::detail::ComponentRegistrar sdk_registrar_##T {                           \
    #T, sizeof(T), sdk_fields_##T,                                                             \
    static_cast<uint8_t>(sizeof(sdk_fields_##T) / sizeof(::sdk::FieldInfo)),                     \
    ::sdk::detail::registered_component<T>                                                     \
  }
