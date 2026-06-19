/**
 * @file printf.c
 * @brief Minimal vsnprintf/snprintf implementation for kernel use.
 *
 * No heap allocation. Supports: %s %d %u %x %lx %lu %ld %p %c %%
 * Width and zero-padding for hex: %08x, %016lx, etc.
 */

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Internal helpers                                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

static size_t _emit(char *buf, size_t pos, size_t max, char c) {
    if (pos < max - 1) {
        buf[pos] = c;
    }
    return pos + 1;
}

static size_t _emit_str(char *buf, size_t pos, size_t max, const char *s) {
    while (*s) {
        pos = _emit(buf, pos, max, *s++);
    }
    return pos;
}

static size_t _emit_unsigned(char *buf, size_t pos, size_t max,
                              uint64_t val, int base, int width, char pad,
                              int uppercase) {
    char tmp[24];
    int i = 0;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }

    /* Pad to width */
    while (i < width) {
        tmp[i++] = pad;
    }

    /* Emit in reverse order */
    while (--i >= 0) {
        pos = _emit(buf, pos, max, tmp[i]);
    }

    return pos;
}

static size_t _emit_signed(char *buf, size_t pos, size_t max,
                             int64_t val, int width, char pad) {
    if (val < 0) {
        pos = _emit(buf, pos, max, '-');
        val = -val;
        if (width > 0) width--;
    }
    return _emit_unsigned(buf, pos, max, (uint64_t)val, 10, width, pad, 0);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Minimal vsnprintf supporting %s %d %u %x %lx %lu %ld %p %c %%.
 */
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap) {
    size_t pos = 0;

    if (size == 0) return 0;

    while (*fmt) {
        if (*fmt != '%') {
            pos = _emit(buf, pos, size, *fmt++);
            continue;
        }

        fmt++;  /* skip '%' */

        /* Parse flags */
        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                fmt++;  /* %lld — treat same as %ld on 64-bit */
            }
        }

        /* Parse conversion specifier */
        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            pos = _emit_str(buf, pos, size, s ? s : "(null)");
            break;
        }
        case 'd': {
            int64_t val = is_long ? __builtin_va_arg(ap, int64_t)
                                  : (int64_t)__builtin_va_arg(ap, int);
            pos = _emit_signed(buf, pos, size, val, width, pad);
            break;
        }
        case 'u': {
            uint64_t val = is_long ? __builtin_va_arg(ap, uint64_t)
                                   : (uint64_t)__builtin_va_arg(ap, unsigned int);
            pos = _emit_unsigned(buf, pos, size, val, 10, width, pad, 0);
            break;
        }
        case 'x': {
            uint64_t val = is_long ? __builtin_va_arg(ap, uint64_t)
                                   : (uint64_t)__builtin_va_arg(ap, unsigned int);
            pos = _emit_unsigned(buf, pos, size, val, 16, width, pad, 0);
            break;
        }
        case 'X': {
            uint64_t val = is_long ? __builtin_va_arg(ap, uint64_t)
                                   : (uint64_t)__builtin_va_arg(ap, unsigned int);
            pos = _emit_unsigned(buf, pos, size, val, 16, width, pad, 1);
            break;
        }
        case 'p': {
            uint64_t val = (uint64_t)__builtin_va_arg(ap, void *);
            pos = _emit_str(buf, pos, size, "0x");
            pos = _emit_unsigned(buf, pos, size, val, 16, 16, '0', 0);
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            pos = _emit(buf, pos, size, c);
            break;
        }
        case '%':
            pos = _emit(buf, pos, size, '%');
            break;
        default:
            /* Unknown format specifier — emit literally */
            pos = _emit(buf, pos, size, '%');
            pos = _emit(buf, pos, size, *fmt);
            break;
        }

        fmt++;
    }

    /* Null-terminate */
    if (pos < size) {
        buf[pos] = '\0';
    } else {
        buf[size - 1] = '\0';
    }

    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}
