// Formatação estilo printf do CRT do Xbox 360 (sprintf, _snprintf, _vsnprintf,
// DbgPrint), lendo os argumentos dos registradores/pilha do guest.
// Portado de Xenia (src/xenia/kernel/xboxkrnl/xboxkrnl_strings.{h,cc}),
// https://github.com/xenia-canary/xenia-canary, licença BSD-3-Clause
// (ver THIRD_PARTY_NOTICES.md). Só a camada de compatibilidade mudou.
#include "guest_printf.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include "memory.h"

// ---- Compatibilidade com utilitários do Xenia ----
#define assert_always() assert(false)
#define assert_not_null(x) assert((x) != nullptr)
#define assert_not_zero(x) assert((x) != 0)
#define assert_true(x) assert(x)

namespace xe
{
template<typename T, size_t N>
constexpr size_t countof(T (&)[N]) { return N; }
inline uint16_t byte_swap(uint16_t v) { return __builtin_bswap16(v); }
template<typename T>
void store_and_swap(void* p, T v)
{
    if constexpr (sizeof(T) == 2) v = __builtin_bswap16(v);
    else if constexpr (sizeof(T) == 4) v = __builtin_bswap32(v);
    memcpy(p, &v, sizeof(T));
}
}

// Argumento variádico n (0 = r3): 8 em registradores, o resto na pilha (r1 + 0x54).
static uint64_t GuestArg64(PPCContext* ctx, int32_t index)
{
    switch (index)
    {
    case 0: return ctx->r3.u64;
    case 1: return ctx->r4.u64;
    case 2: return ctx->r5.u64;
    case 3: return ctx->r6.u64;
    case 4: return ctx->r7.u64;
    case 5: return ctx->r8.u64;
    case 6: return ctx->r9.u64;
    case 7: return ctx->r10.u64;
    default:
        return __builtin_bswap64(*static_cast<const uint64_t*>(g_memory.Translate(ctx->r1.u32 + 0x54 + (index - 8) * 8)));
    }
}

namespace
{
enum FormatState {
  FS_Invalid = 0,
  FS_Unknown,
  FS_Start,
  FS_Flags,
  FS_Width,
  FS_PrecisionStart,
  FS_Precision,
  FS_Size,
  FS_Type,
  FS_End,
};

enum FormatFlags {
  FF_LeftJustify = 1 << 0,
  FF_AddLeadingZeros = 1 << 1,
  FF_AddPositive = 1 << 2,
  FF_AddPositiveAsSpace = 1 << 3,
  FF_AddNegative = 1 << 4,
  FF_AddPrefix = 1 << 5,
  FF_IsShort = 1 << 6,
  FF_IsLong = 1 << 7,
  FF_IsLongLong = 1 << 8,
  FF_IsWide = 1 << 9,
  FF_IsSigned = 1 << 10,
  FF_ForceLeadingZero = 1 << 11,
  FF_InvertWide = 1 << 12,
};

enum ArgumentSize {
  AS_Default = 0,
  AS_Short,
  AS_Long,
  AS_LongLong,
};

class FormatData {
 public:
  virtual uint16_t get() = 0;
  virtual uint16_t peek(int32_t offset) = 0;
  virtual void skip(int32_t count) = 0;
  virtual bool put(uint16_t c) = 0;
};

class ArgList {
 public:
  virtual uint32_t get32() = 0;
  virtual uint64_t get64() = 0;
};

// Making the assumption that the Xbox 360's implementation of the
// printf-functions matches what is described on MSDN's documentation for the
// Windows CRT:
//
// "Format Specification Syntax: printf and wprintf Functions"
// https://msdn.microsoft.com/en-us/library/56e442dc.aspx



class StackArgList : public ArgList {
 public:
  StackArgList(PPCContext* ppc_context, int32_t index)
      : ppc_context(ppc_context), index_(index) {}

  uint32_t get32() { return (uint32_t)get64(); }

  uint64_t get64() { return GuestArg64(ppc_context, index_++); }

 private:
  PPCContext* ppc_context;
  int32_t index_;
};

class ArrayArgList : public ArgList {
 public:
  ArrayArgList(PPCContext* ppc_context, uint32_t arg_ptr)
      : ppc_context(ppc_context), arg_ptr_(arg_ptr), index_(0) {}

  uint32_t get32() { return (uint32_t)get64(); }

  uint64_t get64() {
    return __builtin_bswap64(*static_cast<const uint64_t*>(g_memory.Translate(arg_ptr_ + (8 * index_++))));
  }

 private:
  PPCContext* ppc_context;
  uint32_t arg_ptr_;
  int32_t index_;
};

class StringFormatData : public FormatData {
 public:
  StringFormatData(const uint8_t* input) : input_(input) {}

  uint16_t get() {
    uint16_t result = *input_;
    if (result) {
      input_++;
    }
    return result;
  }

  uint16_t peek(int32_t offset) { return input_[offset]; }

  void skip(int32_t count) {
    while (count-- > 0) {
      if (!get()) {
        break;
      }
    }
  }

  bool put(uint16_t c) {
    if (c >= 0x100) {
      return false;
    }
    output_.push_back(char(c));
    return true;
  }

  const std::string& str() const { return output_; }

 private:
  const uint8_t* input_;
  std::string output_;
};


std::string format_double(double value, int32_t precision, uint16_t c,
                          uint32_t flags) {
  if (precision < 0) {
    precision = 6;
  } else if (precision == 0 && c == 'g') {
    precision = 1;
  }

  std::ostringstream temp;
  temp << std::setprecision(precision);

  if (c == 'f') {
    temp << std::fixed;
  } else if (c == 'e' || c == 'E') {
    temp << std::scientific;
  } else if (c == 'a' || c == 'A') {
    temp << std::hexfloat;
  } else if (c == 'g' || c == 'G') {
    temp << std::defaultfloat;
  }

  if (c == 'E' || c == 'G' || c == 'A') {
    temp << std::uppercase;
  }

  if (flags & FF_AddPrefix) {
    temp << std::showpoint;
  }

  temp << value;
  return temp.str();
}

int32_t format_core(PPCContext* ppc_context, FormatData& data, ArgList& args,
                    const bool wide) {
  int32_t count = 0;

  char work8[512];
  char16_t work16[4];

  struct {
    const void* buffer;
    int32_t length;
    bool is_wide;
    bool swap_wide;
  } text;

  struct {
    char buffer[2];
    int32_t length;
  } prefix;

  auto state = FS_Unknown;
  uint32_t flags = 0;
  int32_t width = 0;
  int32_t precision = -1;
  ArgumentSize size = AS_Default;
  int32_t radix = 0;
  const char* digits = nullptr;

  text.buffer = nullptr;
  text.is_wide = false;
  text.swap_wide = true;
  text.length = 0;
  prefix.buffer[0] = '\0';
  prefix.length = 0;

  for (uint16_t c = data.get();; c = data.get()) {
    if (state == FS_Unknown) {
      if (!c) {  // the end
        return count;
      } else if (c != '%') {
      output:
        if (!data.put(c)) {
          return -1;
        }
        ++count;
        continue;
      }

      state = FS_Start;
      c = data.get();
      // fall through
    }

    // in any state, if c is \0, it's bad
    if (!c) {
      return -1;
    }

  restart:
    switch (state) {
      case FS_Invalid:
      case FS_Unknown:
      case FS_End:
      default: {
        assert_always();
      }

      case FS_Start: {
        if (c == '%') {
          state = FS_Unknown;
          goto output;
        }

        state = FS_Flags;

        // reset to defaults
        flags = 0;
        width = 0;
        precision = -1;
        size = AS_Default;
        radix = 0;
        digits = nullptr;

        text.buffer = nullptr;
        text.is_wide = false;
        text.swap_wide = true;
        text.length = 0;
        prefix.buffer[0] = '\0';
        prefix.length = 0;

        // fall through, don't need to goto restart
      }

      // https://msdn.microsoft.com/en-us/library/8aky45ct.aspx
      case FS_Flags: {
        if (c == '-') {
          flags |= FF_LeftJustify;
          continue;
        } else if (c == '+') {
          flags |= FF_AddPositive;
          continue;
        } else if (c == '0') {
          flags |= FF_AddLeadingZeros;
          continue;
        } else if (c == ' ') {
          flags |= FF_AddPositiveAsSpace;
          continue;
        } else if (c == '#') {
          flags |= FF_AddPrefix;
          continue;
        }
        state = FS_Width;
        // fall through, don't need to goto restart
      }

      // https://msdn.microsoft.com/en-us/library/25366k66.aspx
      case FS_Width: {
        if (c == '*') {
          width = (int32_t)args.get32();
          if (width < 0) {
            flags |= FF_LeftJustify;
            width = -width;
          }
          state = FS_PrecisionStart;
          continue;
        } else if (c >= '0' && c <= '9') {
          width *= 10;
          width += c - '0';
          continue;
        }
        state = FS_PrecisionStart;
        // fall through, don't need to goto restart
      }

      // https://msdn.microsoft.com/en-us/library/0ecbz014.aspx
      case FS_PrecisionStart: {
        if (c == '.') {
          state = FS_Precision;
          precision = 0;
          continue;
        }
        state = FS_Size;
        goto restart;
      }

      // https://msdn.microsoft.com/en-us/library/0ecbz014.aspx
      case FS_Precision: {
        if (c == '*') {
          precision = (int32_t)args.get32();
          if (precision < 0) {
            precision = -1;
          }
          state = FS_Size;
          continue;
        } else if (c >= '0' && c <= '9') {
          precision *= 10;
          precision += c - '0';
          continue;
        }
        state = FS_Size;
        // fall through
      }

      // https://msdn.microsoft.com/en-us/library/tcxf1dw6.aspx
      case FS_Size: {
        if (c == 'l') {
          if (data.peek(0) == 'l') {
            data.skip(1);
            flags |= FF_IsLongLong;
          } else {
            flags |= FF_IsLong;
          }
          state = FS_Type;
          continue;
        } else if (c == 'L') {
          // 58410826 incorrectly uses 'L' instead of 'l'.
          // TODO(gibbed): L appears to be treated as an invalid token by
          // xboxkrnl, investigate how invalid tokens are processed in xboxkrnl
          // formatting when state FF_Type is reached.
          state = FS_Type;
          continue;
        } else if (c == 'h') {
          flags |= FF_IsShort;
          state = FS_Type;
          continue;
        } else if (c == 'w') {
          flags |= FF_IsWide;
          state = FS_Type;
          continue;
        } else if (c == 'I') {
          if (data.peek(0) == '6' && data.peek(1) == '4') {
            data.skip(2);
            flags |= FF_IsLongLong;
            state = FS_Type;
            continue;
          } else if (data.peek(0) == '3' && data.peek(1) == '2') {
            data.skip(2);
            state = FS_Type;
            continue;
          } else {
            state = FS_Type;
            continue;
          }
        }
        // fall through
      }

      // https://msdn.microsoft.com/en-us/library/hf4y5e3w.aspx
      case FS_Type: {
        // wide character
        switch (c) {
          case 'C': {
            flags |= FF_InvertWide;
            // fall through
          }

          // character
          case 'c': {
            bool is_wide;
            if (flags & (FF_IsLong | FF_IsWide)) {
              // "An lc, lC, wc or wC type specifier is synonymous with C in
              // printf functions and with c in wprintf functions."
              is_wide = true;
            } else if (flags & FF_IsShort) {
              // "An hc or hC type specifier is synonymous with c in printf
              // functions and with C in wprintf functions."
              is_wide = false;
            } else {
              is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
            }

            auto value = args.get32();

            if (!is_wide) {
              work8[0] = (uint8_t)value;
              text.buffer = &work8[0];
              text.length = 1;
              text.is_wide = false;
            } else {
              work16[0] = (uint16_t)value;
              text.buffer = &work16[0];
              text.length = 1;
              text.is_wide = true;
              text.swap_wide = false;
            }

            break;
          }

          // signed decimal integer
          case 'd':
          case 'i': {
            flags |= FF_IsSigned;
            digits = "0123456789";
            radix = 10;

          integer:
            assert_not_null(digits);
            assert_not_zero(radix);

            int64_t value;

            if (flags & FF_IsLongLong) {
              value = (int64_t)args.get64();
            } else if (flags & FF_IsLong) {
              value = (int32_t)args.get32();
            } else if (flags & FF_IsShort) {
              value = (int16_t)args.get32();
            } else {
              value = (int32_t)args.get32();
            }

            if (precision >= 0) {
              precision = std::min(precision, (int32_t)xe::countof(work8));
            } else {
              precision = 1;
            }

            if ((flags & FF_IsSigned) && value < 0) {
              value = -value;
              flags |= FF_AddNegative;
            }

            if (!(flags & FF_IsLongLong)) {
              value &= UINT32_MAX;
            }

            if (value == 0) {
              prefix.length = 0;
            }

            char* end = &work8[xe::countof(work8) - 1];
            char* start = end;
            start[0] = '\0';

            while (precision-- > 0 || value != 0) {
              const auto digit = static_cast<uint64_t>(value) % radix;
              value /= radix;
              assert_true(digit < strlen(digits));
              *--start = digits[digit];
            }

            if ((flags & FF_ForceLeadingZero) &&
                (start == end || *start != '0')) {
              *--start = '0';
            }

            text.buffer = start;
            text.length = (int32_t)(end - start);
            text.is_wide = false;
            break;
          }

          // unsigned octal integer
          case 'o': {
            digits = "01234567";
            radix = 8;
            if (flags & FF_AddPrefix) {
              flags |= FF_ForceLeadingZero;
            }
            goto integer;
          }

          // unsigned decimal integer
          case 'u': {
            digits = "0123456789";
            radix = 10;
            goto integer;
          }

          // unsigned hexadecimal integer
          case 'x':
          case 'X': {
            digits = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
            radix = 16;

            if (flags & FF_AddPrefix) {
              prefix.buffer[0] = '0';
              prefix.buffer[1] = c == 'x' ? 'x' : 'X';
              prefix.length = 2;
            }

            goto integer;
          }

          // floating-point with exponent
          case 'e':
          case 'E': {
            // fall through
          }

          // floating-point without exponent
          case 'f': {
          floatingpoint:
            flags |= FF_IsSigned;

            int64_t dummy = args.get64();
            double value = *(double*)&dummy;

            if (value < 0) {
              value = -value;
              flags |= FF_AddNegative;
            }

            auto s = format_double(value, precision, c, flags);
            auto length = (int32_t)s.size();
            assert_true(length < xe::countof(work8));

            auto start = &work8[0];
            auto end = &start[length];

            std::memcpy(start, s.c_str(), length);
            end[0] = '\0';

            text.buffer = start;
            text.length = (int32_t)(end - start);
            text.is_wide = false;
            break;
          }

          // floating-point with or without exponent
          case 'g':
          case 'G': {
            goto floatingpoint;
          }

          // floating-point in hexadecimal
          case 'a':
          case 'A': {
            goto floatingpoint;
          }

          // pointer to integer
          case 'n': {
            auto pointer = (uint32_t)args.get32();
            if (flags & FF_IsShort) {
              xe::store_and_swap<uint16_t>(
                  g_memory.Translate(pointer), (uint16_t)count);
            } else {
              xe::store_and_swap<uint32_t>(
                  g_memory.Translate(pointer), (uint32_t)count);
            }
            continue;
          }

          // pointer
          case 'p': {
            digits = "0123456789ABCDEF";
            radix = 16;
            precision = 8;
            flags &= ~(FF_IsLongLong | FF_IsShort);
            flags |= FF_IsLong;
            goto integer;
          }

          // wide string
          case 'S': {
            flags |= FF_InvertWide;
            // fall through
          }

          // string
          case 's': {
            uint32_t pointer = args.get32();
            int32_t cap = precision < 0 ? INT32_MAX : precision;

            if (pointer == 0) {
              auto nullstr = "(null)";
              text.buffer = nullstr;
              text.length = std::min((int32_t)strlen(nullstr), cap);
              text.is_wide = false;
            } else {
              void* str = g_memory.Translate(pointer);
              bool is_wide;
              if (flags & (FF_IsLong | FF_IsWide)) {
                // "An ls, lS, ws or wS type specifier is synonymous with S in
                // printf functions and with s in wprintf functions."
                is_wide = true;
              } else if (flags & FF_IsShort) {
                // "An hs or hS type specifier is synonymous with s in printf
                // functions and with S in wprintf functions."
                is_wide = false;
              } else {
                is_wide = ((flags & FF_InvertWide) != 0) ^ wide;
              }
              int32_t length;

              if (!is_wide) {
                length = 0;
                for (auto s = (const uint8_t*)str; cap > 0 && *s; ++s, cap--) {
                  length++;
                }
              } else {
                length = 0;
                for (auto s = (const uint16_t*)str; cap > 0 && *s; ++s, cap--) {
                  length++;
                }
              }

              text.buffer = str;
              text.length = length;
              text.is_wide = is_wide;
            }
            break;
          }

          // ANSI_STRING / UNICODE_STRING
          case 'Z': {
            assert_always();
            break;
          }

          default: {
            assert_always();
          }
        }
      }
    }

    if (flags & FF_IsSigned) {
      if (flags & FF_AddNegative) {
        prefix.buffer[0] = '-';
        prefix.length = 1;
      } else if (flags & FF_AddPositive) {
        prefix.buffer[0] = '+';
        prefix.length = 1;
      } else if (flags & FF_AddPositiveAsSpace) {
        prefix.buffer[0] = ' ';
        prefix.length = 1;
      }
    }

    int32_t padding = width - text.length - prefix.length;

    if (!(flags & (FF_LeftJustify | FF_AddLeadingZeros)) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put(' ')) {
          return -1;
        }
      }
    }

    if (prefix.length > 0) {
      int32_t remaining = prefix.length;
      count += prefix.length;
      auto b = &prefix.buffer[0];
      while (remaining-- > 0) {
        if (!data.put(*b++)) {
          return -1;
        }
      }
    }

    if ((flags & FF_AddLeadingZeros) && !(flags & (FF_LeftJustify)) &&
        padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put('0')) {
          return -1;
        }
      }
    }

    int32_t remaining = text.length;
    if (!text.is_wide) {
      // it's a const char*
      auto b = (const uint8_t*)text.buffer;
      while (remaining-- > 0) {
        if (!data.put(*b++)) {
          return -1;
        }
      }
    } else {
      // it's a const char16_t*
      auto b = (const uint16_t*)text.buffer;
      if (text.swap_wide) {
        while (remaining-- > 0) {
          if (!data.put(xe::byte_swap(*b++))) {
            return -1;
          }
        }
      } else {
        while (remaining-- > 0) {
          if (!data.put(*b++)) {
            return -1;
          }
        }
      }
    }
    count += text.length;

    // right padding
    if ((flags & FF_LeftJustify) && padding > 0) {
      count += padding;
      while (padding-- > 0) {
        if (!data.put(' ')) {
          return -1;
        }
      }
    }

    state = FS_Unknown;
  }

  return count;
}

} // namespace
