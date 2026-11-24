#pragma once

#include <charconv>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sdk::dev_bridge::json {
  // Return the size of one well-formed non-ASCII UTF-8 sequence, or zero.
  inline unsigned utf8(const char *p, const char *end) {
    const auto first = static_cast<unsigned char>(*p);
    const unsigned size = first < 0xc2 || first > 0xf4 ? 0 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    if(!size || static_cast<size_t>(end - p) < size) return 0;
    const auto second = static_cast<unsigned char>(p[1]);
    if((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0) ||
       (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) return 0;
    for(unsigned i = 1; i < size; ++i)
      if((static_cast<unsigned char>(p[i]) & 0xc0) != 0x80) return 0;
    return size;
  }

  enum class Kind { Other, Number, String, Null };
  struct Value {
    Kind kind = Kind::Other;
    std::string_view text;

    bool unsigned_integer(uint32_t &number) const {
      if(kind != Kind::Number) return false;
      const auto *end = text.data() + text.size();
      const auto parsed = std::from_chars(text.data(), end, number);
      return parsed.ec == std::errc{} && parsed.ptr == end;
    }
  };
  struct Request {
    uint32_t id = 0;
    bool has_id = false, valid = false;
    std::string_view op;
    Value payload;
  };

  // Parse only one request per line. Strings are decoded and NUL-terminated in
  // the receive buffer; other JSON values are consumed to report errors by ID.
  class Reader {
  public:
    Reader(char *start, char *end) : p_(start), end_(end) {}

    Request request() {
      Request result;
      if(!take('{') || take('}')) return result;
      unsigned ids = 0, ops = 0;
      do {
        std::string_view key;
        if(!string(key) || !take(':')) return result;
        Value value;
        if(!read_value(value, 0)) return result;
        if(key == "id") {
          result.has_id = ++ids == 1 && value.unsigned_integer(result.id);
        } else if(++ops == 1) {
          result.op = key;
          result.payload = value;
        }
      } while(take(','));
      if(!take('}')) return result;
      spaces();
      result.valid = p_ == end_ && result.has_id && ids == 1 && ops == 1;
      return result;
    }

  private:
    static bool digit(char ch) { return ch >= '0' && ch <= '9'; }
    char peek() const { return p_ == end_ ? '\0' : *p_; }
    void spaces() {
      while(p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    bool take(char ch) {
      spaces();
      if(peek() != ch) return false;
      ++p_;
      return true;
    }
    bool literal(std::string_view text) {
      if(static_cast<size_t>(end_ - p_) < text.size() || std::memcmp(p_, text.data(), text.size()) != 0) return false;
      p_ += text.size();
      return true;
    }
    bool hex4(uint32_t &value) {
      if(end_ - p_ < 4) return false;
      value = 0;
      for(unsigned i = 0; i < 4; ++i) {
        const char ch = *p_++;
        const int lowercase = ch | 0x20;
        const int nibble = digit(ch) ? ch - '0' : lowercase - 'a' + 10;
        if(!digit(ch) && (lowercase < 'a' || lowercase > 'f')) return false;
        value = (value << 4) | nibble;
      }
      return true;
    }
    bool unicode(char *&out) {
      uint32_t code;
      if(!hex4(code)) return false;
      if(code >= 0xd800 && code <= 0xdbff) {
        uint32_t low;
        if(!literal("\\u") || !hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
      } else if(code >= 0xdc00 && code <= 0xdfff) {
        return false;
      }
      if(code < 0x80) {
        *out++ = static_cast<char>(code);
      } else {
        const unsigned remaining = code < 0x800 ? 1 : code < 0x10000 ? 2 : 3;
        *out++ = static_cast<char>((remaining == 1 ? 0xc0 : remaining == 2 ? 0xe0 : 0xf0) | (code >> (remaining * 6)));
        for(unsigned i = remaining; i > 0; --i)
          *out++ = static_cast<char>(0x80 | ((code >> ((i - 1) * 6)) & 0x3f));
      }
      return true;
    }
    bool string(std::string_view &text) {
      if(!take('"')) return false;
      char *const start = p_;
      char *out = start;
      while(p_ != end_) {
        const auto ch = static_cast<unsigned char>(*p_);
        if(ch == '"') {
          ++p_;
          *out = '\0';
          text = {start, static_cast<size_t>(out - start)};
          return true;
        }
        if(ch < 0x20) return false;
        if(ch == '\\') {
          if(++p_ == end_) return false;
          switch(*p_++) {
            case '"': *out++ = '"'; break;
            case '\\': *out++ = '\\'; break;
            case '/': *out++ = '/'; break;
            case 'n': *out++ = '\n'; break;
            case 'r': *out++ = '\r'; break;
            case 't': *out++ = '\t'; break;
            case 'b': *out++ = '\b'; break;
            case 'f': *out++ = '\f'; break;
            case 'u': if(!unicode(out)) return false; break;
            default: return false;
          }
        } else {
          const unsigned size = ch < 0x80 ? 1 : utf8(p_, end_);
          if(!size) return false;
          if(out != p_) std::memmove(out, p_, size);
          out += size;
          p_ += size;
        }
      }
      return false;
    }
    bool digits() {
      if(!digit(peek())) return false;
      do { ++p_; } while(digit(peek()));
      return true;
    }
    bool number(std::string_view &text) {
      const char *const start = p_;
      if(peek() == '-') ++p_;
      if(peek() == '0') ++p_;
      else if(!digits()) return false;
      if(peek() == '.') {
        ++p_;
        if(!digits()) return false;
      }
      if(peek() == 'e' || peek() == 'E') {
        ++p_;
        if(peek() == '-' || peek() == '+') ++p_;
        if(!digits()) return false;
      }
      text = {start, static_cast<size_t>(p_ - start)};
      return true;
    }
    bool read_value(Value &value, unsigned depth) {
      spaces();
      switch(peek()) {
        case '"':
          value.kind = Kind::String;
          return string(value.text);
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n':
          value.kind = Kind::Null;
          return literal("null");
        case '{':
        case '[': {
          if(depth == 16) return false;
          const bool object = *p_++ == '{';
          const char closer = object ? '}' : ']';
          if(take(closer)) return true;
          do {
            Value child;
            if(object && (!string(child.text) || !take(':'))) return false;
            if(!read_value(child, depth + 1)) return false;
          } while(take(','));
          return take(closer);
        }
        default:
          value.kind = Kind::Number;
          return number(value.text);
      }
    }

    char *p_;
    const char *end_;
  };

  class Writer {
  public:
    Writer(char *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    bool ok() const { return ok_; }
    size_t size() const { return size_; }
    void append(std::string_view text) {
      if(!ok_ || text.empty()) return;
      if(text.size() > capacity_ - size_) { ok_ = false; return; }
      std::memcpy(buffer_ + size_, text.data(), text.size());
      size_ += text.size();
    }
    template<class Integer> void number(Integer value) {
      char text[24];
      const auto end = std::to_chars(text, text + sizeof(text), value).ptr;
      append({text, static_cast<size_t>(end - text)});
    }
    void string(std::string_view text) {
      append("\"");
      if(!text.empty()) {
        const char *run = text.data();
        const char *const end = run + text.size();
        for(const char *p = run; ok_ && p != end;) {
          const auto ch = static_cast<unsigned char>(*p);
          std::string_view escape;
          char control[] = "\\u0000";
          switch(ch) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            default:
              if(ch < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                control[4] = hex[ch >> 4];
                control[5] = hex[ch & 0x0f];
                escape = {control, 6};
              } else if(ch >= 0x80) {
                if(const auto size = utf8(p, end)) { p += size; continue; }
                escape = "\\ufffd";
              }
              break;
          }
          if(!escape.empty()) {
            append({run, static_cast<size_t>(p - run)});
            append(escape);
            run = p + 1;
          }
          ++p;
        }
        append({run, static_cast<size_t>(end - run)});
      }
      append("\"");
    }

  private:
    char *buffer_;
    size_t capacity_, size_ = 0;
    bool ok_ = true;
  };
}
