#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sdk {
  /// One-based atlas tile IDs (0 is empty), with no dependency on a map type.
  class TileProps {
  public:
    /// Read a BTP1 asset for the C++ schema. Failure leaves this table unchanged.
    bool load(const uint8_t *data, std::size_t size, uint64_t schema) {
      constexpr std::size_t header = 12;
      if(!data || size != header + sizeof(uint32_t) * flags_.size() ||
         data[0] != 'B' || data[1] != 'T' || data[2] != 'P' || data[3] != '1' ||
         read_word(data + header) != 0) return false;
      for(unsigned i = 0; i < 8; ++i)
        if(data[4 + i] != static_cast<uint8_t>(schema >> (8 * i))) return false;
      for(std::size_t i = 0; i < flags_.size(); ++i)
        flags_[i] = read_word(data + header + sizeof(uint32_t) * i);
      return true;
    }

    uint32_t mask(unsigned tile_id) const { return tile_id < flags_.size() ? flags_[tile_id] : 0; }

    /// True if all requested flags are set; an empty mask never matches.
    template<class Flag> bool has(unsigned tile_id, Flag flags) const {
      const auto requested = static_cast<uint32_t>(flags);
      return requested && (mask(tile_id) & requested) == requested;
    }

  private:
    static uint32_t read_word(const uint8_t *bytes) {
      return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    }

    std::array<uint32_t, 256> flags_{};
  };
}
