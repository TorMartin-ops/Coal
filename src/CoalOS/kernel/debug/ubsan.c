/**
 * @file ubsan.c
 * @brief Runtime for -fsanitize=undefined: the __ubsan_handle_* entry points the compiler calls.
 *
 * Each finding is one report with file:line:column and a backtrace; a location is reported once and then
 * counted. coal.ubsan=panic turns the first finding into a panic. Handlers that the compiler marks noreturn
 * (unreachable, missing return) always stop the machine.
 */
#include <kernel/debug/ksan.h>

struct loc { const char *file; uint32_t line, column; };
struct type { uint16_t kind, info; char name[]; };          /* kind 0: integer (info bit 0: signed), 1: float */
struct overflow { struct loc loc; const struct type *type; };
struct shift { struct loc loc; const struct type *lhs, *rhs; };
struct bounds { struct loc loc; const struct type *array, *index; };
struct mismatch { struct loc loc; const struct type *type; uint8_t log_align, check_kind; };
struct invalid { struct loc loc; const struct type *type; };
struct nonnull_ret { struct loc attr; };
struct nonnull_arg { struct loc loc, attr; int idx; };
struct float_cast { struct loc loc; const struct type *from, *to; };
struct align_assume { struct loc loc, assumption; const struct type *type; };
struct plain { struct loc loc; };
struct builtin { struct loc loc; uint8_t kind; };

static const char *const check_kinds[] = {
    "load of", "store to", "reference binding to", "member access within", "member call on",
    "constructor call on", "downcast of", "downcast of", "upcast of", "cast to virtual base of",
};

#define SEEN_MAX 64
static struct { const char *file; uint32_t line, column; } g_seen[SEEN_MAX];
static unsigned g_nseen;
static unsigned long g_suppressed;

/* True the first time a location is seen. */
static bool first_time(const struct loc *l) {
    for (unsigned i = 0; i < g_nseen; i++)
        if (g_seen[i].file == l->file && g_seen[i].line == l->line && g_seen[i].column == l->column) {
            g_suppressed++;
            return false;
        }
    if (g_nseen < SEEN_MAX) {
        g_seen[g_nseen].file = l->file;
        g_seen[g_nseen].line = l->line;
        g_seen[g_nseen].column = l->column;
        g_nseen++;
    }
    return true;
}

static bool is_signed(const struct type *t) { return t->info & 1; }
static unsigned bits(const struct type *t) { return 1u << (t->info >> 1); }

/* Integers up to 64 bits arrive by value, wider ones by pointer (not used in the kernel). */
static long long as_signed(const struct type *t, uintptr_t v) {
    unsigned b = bits(t);
    return b >= 64 ? (long long)v : (long long)((long long)(v << (64 - b)) >> (64 - b));
}
static unsigned long long as_unsigned(const struct type *t, uintptr_t v) {
    unsigned b = bits(t);
    return b >= 64 ? (unsigned long long)v : (unsigned long long)(v & ((1ull << b) - 1));
}

#define REPORT(loc, ...) do { if (first_time(loc)) \
    ksan_report(KSAN_UBSAN, __VA_ARGS__); } while (0)
#define AT(l) (l)->file, (unsigned)(l)->line, (unsigned)(l)->column

static void arith(const struct overflow *d, uintptr_t l, uintptr_t r, char op) {
    const struct type *t = d->type;

    if (!first_time(&d->loc))
        return;
    if (is_signed(t))
        ksan_report(KSAN_UBSAN, "%s:%u:%u: signed integer overflow: %lld %c %lld cannot be represented in type %s",
                    AT(&d->loc), as_signed(t, l), op, as_signed(t, r), t->name);
    else
        ksan_report(KSAN_UBSAN, "%s:%u:%u: unsigned integer overflow: %llu %c %llu cannot be represented in type %s",
                    AT(&d->loc), as_unsigned(t, l), op, as_unsigned(t, r), t->name);
}

void __ubsan_handle_add_overflow(struct overflow *d, uintptr_t l, uintptr_t r) { arith(d, l, r, '+'); }
void __ubsan_handle_sub_overflow(struct overflow *d, uintptr_t l, uintptr_t r) { arith(d, l, r, '-'); }
void __ubsan_handle_mul_overflow(struct overflow *d, uintptr_t l, uintptr_t r) { arith(d, l, r, '*'); }

void __ubsan_handle_negate_overflow(struct overflow *d, uintptr_t v) {
    REPORT(&d->loc, "%s:%u:%u: negation of %lld cannot be represented in type %s", AT(&d->loc),
           as_signed(d->type, v), d->type->name);
}

void __ubsan_handle_divrem_overflow(struct overflow *d, uintptr_t l, uintptr_t r) {
    if (is_signed(d->type) && as_signed(d->type, r) == -1)
        REPORT(&d->loc, "%s:%u:%u: division of %lld by -1 cannot be represented in type %s", AT(&d->loc),
               as_signed(d->type, l), d->type->name);
    else
        REPORT(&d->loc, "%s:%u:%u: division by zero", AT(&d->loc));
}

void __ubsan_handle_shift_out_of_bounds(struct shift *d, uintptr_t l, uintptr_t r) {
    if (is_signed(d->rhs) && as_signed(d->rhs, r) < 0)
        REPORT(&d->loc, "%s:%u:%u: shift exponent %lld is negative", AT(&d->loc), as_signed(d->rhs, r));
    else if (as_unsigned(d->rhs, r) >= bits(d->lhs))
        REPORT(&d->loc, "%s:%u:%u: shift exponent %llu is too large for %u-bit type %s", AT(&d->loc),
               as_unsigned(d->rhs, r), bits(d->lhs), d->lhs->name);
    else if (is_signed(d->lhs) && as_signed(d->lhs, l) < 0)
        REPORT(&d->loc, "%s:%u:%u: left shift of negative value %lld", AT(&d->loc), as_signed(d->lhs, l));
    else
        REPORT(&d->loc, "%s:%u:%u: left shift of %llu by %llu places cannot be represented in type %s",
               AT(&d->loc), as_unsigned(d->lhs, l), as_unsigned(d->rhs, r), d->lhs->name);
}

void __ubsan_handle_out_of_bounds(struct bounds *d, uintptr_t index) {
    REPORT(&d->loc, "%s:%u:%u: index %lld is out of range for type %s", AT(&d->loc),
           is_signed(d->index) ? as_signed(d->index, index) : (long long)as_unsigned(d->index, index), d->array->name);
}

static void mismatch(struct mismatch *d, uintptr_t ptr) {
    uintptr_t align = (uintptr_t)1 << d->log_align;
    const char *kind = d->check_kind < sizeof(check_kinds) / sizeof(check_kinds[0]) ? check_kinds[d->check_kind] : "access of";

    if (!ptr)
        REPORT(&d->loc, "%s:%u:%u: %s null pointer of type %s", AT(&d->loc), kind, d->type->name);
    else if (ptr & (align - 1))
        REPORT(&d->loc, "%s:%u:%u: %s misaligned address %#lx for type %s, which requires %lu byte alignment",
               AT(&d->loc), kind, (unsigned long)ptr, d->type->name, (unsigned long)align);
    else
        REPORT(&d->loc, "%s:%u:%u: %s address %#lx with insufficient space for an object of type %s",
               AT(&d->loc), kind, (unsigned long)ptr, d->type->name);
}

void __ubsan_handle_type_mismatch_v1(struct mismatch *d, uintptr_t ptr) { mismatch(d, ptr); }

void __ubsan_handle_load_invalid_value(struct invalid *d, uintptr_t v) {
    REPORT(&d->loc, "%s:%u:%u: load of value %llu, which is not a valid value for type %s", AT(&d->loc),
           as_unsigned(d->type, v), d->type->name);
}

void __ubsan_handle_nonnull_return_v1(struct nonnull_ret *d, struct loc *l) {
    REPORT(l, "%s:%u:%u: null pointer returned from function declared to never return null", AT(l));
}

void __ubsan_handle_nonnull_arg(struct nonnull_arg *d) {
    REPORT(&d->loc, "%s:%u:%u: null pointer passed as argument %d, which is declared to never be null",
           AT(&d->loc), d->idx);
}

void __ubsan_handle_pointer_overflow(struct plain *d, uintptr_t base, uintptr_t result) {
    REPORT(&d->loc, "%s:%u:%u: pointer arithmetic %s: base %#lx, result %#lx", AT(&d->loc),
           base > result ? "wrapped below zero" : "wrapped around", (unsigned long)base, (unsigned long)result);
}

void __ubsan_handle_vla_bound_not_positive(struct invalid *d, uintptr_t bound) {
    REPORT(&d->loc, "%s:%u:%u: variable length array bound %lld is not positive", AT(&d->loc), as_signed(d->type, bound));
}

void __ubsan_handle_float_cast_overflow(struct float_cast *d, uintptr_t from) {
    REPORT(&d->loc, "%s:%u:%u: value is outside the range of type %s", AT(&d->loc), d->to->name);
}

void __ubsan_handle_alignment_assumption(struct align_assume *d, uintptr_t ptr, uintptr_t align, uintptr_t offset) {
    REPORT(&d->loc, "%s:%u:%u: assumption of %lu byte alignment for pointer %#lx failed", AT(&d->loc),
           (unsigned long)align, (unsigned long)ptr);
}

void __ubsan_handle_invalid_builtin(struct builtin *d) {
    REPORT(&d->loc, "%s:%u:%u: passing zero to %s, which is not a valid argument", AT(&d->loc),
           d->kind ? "clz()" : "ctz()");
}

void __ubsan_handle_builtin_unreachable(struct plain *d) {
    ksan_report(KSAN_UBSAN, "%s:%u:%u: execution reached an unreachable program point", AT(&d->loc));
    ksan_halt("UBSAN: unreachable");
}

void __ubsan_handle_missing_return(struct plain *d) {
    ksan_report(KSAN_UBSAN, "%s:%u:%u: execution reached the end of a value-returning function without returning a value",
                AT(&d->loc));
    ksan_halt("UBSAN: missing return");
}
