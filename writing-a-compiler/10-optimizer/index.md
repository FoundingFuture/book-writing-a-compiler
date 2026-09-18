---
title: "The optimizer"
description: "The optimizer of antic: constant folding, copy propagation, peephole rules and dead code elimination on a program or one module, and why sizes stay symbolic."
summary: "Constant folding, dead code elimination, copy propagation and a small set of peephole rules. All passes run on the whole program after the library IR is loaded, or on one module in dev mode. What each pass may assume, and why no pass folds a symbolic size. A test setup that shows the IR before and after."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:57:00+02:00
draft: false
weight: 100
tags: [compilers, programming-languages]
keywords: [constant folding, copy propagation, dead code elimination, peephole optimization, definite assignment, whole-program optimization, jump threading, symbolic size]
---

## Previously

[Chapter 9, Modules and library files]({{% relref "/programming/writing-a-compiler/09-modules-and-library-files" %}}), adds module paths that mirror a directory tree under the search roots, `import` with `as`, `pub`, `export` and name mangling with length-prefixed segments on COFF. The root `anti.` belongs to the language's own libraries, third-party libraries take reverse-domain roots, and a program's own files take single-segment names. The `.antl` file of that chapter holds a package header with licence fields, the public interface with its doc text, and the unoptimised IR. A serialiser and a deserialiser with a version stamp write and read it. The tests check that library files are byte-identical on every host and that both doc comment forms write the same file.

## Optimizer in the pipeline

The optimizer rewrites the IR of a program into IR that computes the same results with fewer instructions. It runs after the driver has loaded every library file and lowered the main module. The whole program is then one IR module, so library functions and program functions go through the same passes. A library file keeps the unoptimised IR. The header `src/optimize.h` declares three entry points.

```c
void ir_optimize(struct ir_module *program, const char *entry);

/* Optimize the functions of module alone, for an object of its own in dev
   mode. The functions of other modules become declarations, whose symbols
   the objects of those modules define. Every function of module stays. */
void ir_optimize_module(struct ir_module *program, const char *module);

/* Run the passes of ir_optimize on one function, without removing unused
   functions. The back end uses it after it folds symbolic values. */
void ir_optimize_function(struct ir_function *f);
```

The function `ir_optimize` serves a build of the whole program in one call. It optimizes each function with a body, then removes the functions that the program cannot reach.

```c
void ir_optimize(struct ir_module *program, const char *entry)
{
    size_t i;

    for (i = 0; i < program->function_count; i++) {
        if (!program->functions[i]->is_extern) {
            ir_optimize_function(program->functions[i]);
        }
    }
    remove_unused_functions(program, entry, false);
}
```

The function `ir_optimize_module` serves dev mode, in which the option `--dev` compiles one module into an object file of its own. The section on unused functions below describes it. The back end of chapter 12 calls `ir_optimize_function` again after it has folded the sizes of its target.

Each function goes through four passes in a loop until no pass changes anything. The temporaries are then numbered again from the parameters on.

```c
void ir_optimize_function(struct ir_function *f)
{
    bool changed = true;

    /* The peephole rules run before copy propagation. Propagation would
       give t in the pair t = op and x = copy t a second use. */
    while (changed) {
        changed = fold_constants(f);
        changed = apply_peephole_rules(f) || changed;
        /* Forwarding runs before propagation, so the copy it leaves is
           removed in the same round. */
        changed = split_slots(f) || changed;
        changed = forward_stores(f) || changed;
        changed = propagate_copies(f) || changed;
        changed = remove_dead_code(f) || changed;
    }
    renumber_temps(f);
}
```

The loop ends when a round of the four passes changes nothing. The option `--dump-opt` prints the program after `ir_optimize`, and the option `--dump-ir` of chapter 8 prints the program before the optimizer.

## Assumptions

A pass may change the IR only in ways that keep every result of the program. It needs facts about the IR to know which changes do. The optimizer of antic relies on three facts, and the comment at the top of `src/optimize.c` lists them.

```c
/* DESIGN: the passes rely on three properties and nothing else.
   1. The IR passes ir_verify, including its check that every path to a
      use of a temporary passes a definition of it.
   2. Integer operations wrap, and chapter 2 leaves division by zero,
      MIN / -1, a shift by the width or more and an out-of-range float
      conversion undefined. Folding skips exactly those cases.
   3. Memory may change at every store and every call. No pass reasons
      about the contents of memory. */
```

The first fact makes the IR in memory trustworthy. The IR of antic is not in static single assignment form, SSA, so a temporary may receive values in several instructions. A pass that replaces a use of a temporary by its value must know that some definition reaches every use. Definite assignment is that property: every path from the entry of the function to a use of a temporary passes a definition of the temporary.

Lowering produces IR with that property, because chapter 2 requires every `let` to have an initialiser. A library file can hold any bytes, so the verifier checks the property for every function. The check is a forward data-flow analysis. The set of temporaries defined on entry to a block is the intersection of the sets defined on exit from its predecessors.

```c
/* The temporaries defined on every path into block b: the parameters at
   the entry, intersected with what each predecessor defines on exit. */
static void entry_set(const struct ir_function *f, size_t b,
                      const uint64_t *out, size_t words, uint64_t *in)
{
    size_t i;
    size_t k;

    memset(in, b == 0 ? 0 : 0xff, words * sizeof *in);
    if (b == 0) {
        for (i = 0; i < f->param_count; i++) {
            set_bit(in, f->params[i].temp);
        }
    }
    for (i = 0; i < f->block_count; i++) {
        const struct ir_block *pred = f->blocks[i];
        const struct ir_inst *last;
        if (pred->count == 0) {
            continue;
        }
        last = &pred->insts[pred->count - 1];
        if ((last->op == IR_JUMP && last->a.as.index == b) ||
            (last->op == IR_BRANCH &&
             (last->b.as.index == b || last->c.as.index == b))) {
            for (k = 0; k < words; k++) {
                in[k] &= out[i * words + k];
            }
        }
    }
}
```

The analysis starts every block with the set of all temporaries and shrinks the sets until they stop changing. A use of a temporary outside its block's set fails verification with a message such as `ret uses %1 before a definition on some path`. The unit test `unassigned` in `tests/unit/test_ir.c` builds such a function.

The second fact comes from chapter 2. Integer arithmetic wraps, and the language leaves four cases undefined. They are division by zero, `MIN / -1`, a shift by the bit width or more, and a float to integer conversion outside the range. The optimizer leaves those cases in the IR, so a program behaves the same with and without the optimizer.

The third fact limits what the optimizer knows. A `store` or a `call` may change any memory, and no pass follows values through `load` and `store`. A variable whose address is taken therefore keeps every `load` of chapter 8.

## Constant folding

An operation whose operands are all constants is computed at compile time, and a `copy` of the result replaces the instruction. The function `fold_int` computes the integer operations in 64 bits and trims the result to the width of its type, which is the wrap-around of chapter 2. A signed operation reads its operands with `signed_value`, which sign-extends from the width of the type.

```c
static bool fold_int(enum ir_op op, enum ir_type t, uint64_t a, uint64_t b,
                     uint64_t *out)
{
    int n = bits(t);
    uint64_t min = trim(t, (uint64_t)1 << (n - 1));
    int64_t x = signed_value(t, a);
    int64_t y = signed_value(t, b);

    switch (op) {
    case IR_ADD: *out = a + b; break;
    case IR_SUB: *out = a - b; break;
    case IR_MUL: *out = a * b; break;
    case IR_SDIV:
    case IR_SREM:
        if (b == 0 || (a == min && b == trim(t, (uint64_t)-1))) {
            return false;
        }
        *out = (uint64_t)(op == IR_SDIV ? x / y : x % y);
        break;
    case IR_UDIV:
    case IR_UREM:
        if (b == 0) {
            return false;
        }
        *out = op == IR_UDIV ? a / b : a % b;
        break;
    case IR_AND: *out = a & b; break;
    case IR_OR: *out = a | b; break;
    case IR_XOR: *out = a ^ b; break;
    case IR_SHL:
    case IR_SHR_S:
    case IR_SHR_U:
        if (b >= (uint64_t)n) {
            return false;
        }
        if (op == IR_SHL) {
            *out = a << b;
        } else if (op == IR_SHR_U) {
            *out = a >> b;
        } else {
            /* C leaves >> of a negative value to the implementation. */
            *out = x < 0 ? ~(~(uint64_t)x >> b) : (uint64_t)x >> b;
        }
        break;
    case IR_EQ: *out = a == b; return true;
    case IR_NE: *out = a != b; return true;
    case IR_SLT: *out = x < y; return true;
    case IR_SLE: *out = x <= y; return true;
    case IR_SGT: *out = x > y; return true;
    case IR_SGE: *out = x >= y; return true;
    case IR_ULT: *out = a < b; return true;
    case IR_ULE: *out = a <= b; return true;
    case IR_UGT: *out = a > b; return true;
    case IR_UGE: *out = a >= b; return true;
    default:
        return false;
    }
    *out = trim(t, *out);
    return true;
}
```

The C standard makes the result of `>>` on a negative signed value implementation-defined[^1]. The fold of `sshr` therefore builds the arithmetic shift from shifts of unsigned values.

Float operations fold in the type they have in the program. An `f32` addition computes in C `float`, and an `f64` addition in `double`. A conversion from an integer to `f32` rounds once, directly from the integer. A detour through `double` rounds twice and can reach the other neighbouring `f32` value. The unit test `r` converts 2^60 + 2^36 + 1, which lies just above the midpoint between two `f32` values, and expects the upper value.

A `branch` on a constant becomes a `jump` to the block the constant selects. The test `tests/opt/fold.anti` has four functions.

```anti
fn quotient() -> int
{
    return -9 / 2 + -9 % 2;
}

fn shifted() -> i32
{
    return (-16 >> 2) as i32;
}

fn compared() -> bool
{
    return 3 < 4 && 2.0 != 2.0;
}

fn undefined() -> int
{
    let zero = 0;
    return 1 / zero;
}
```

The IR before the optimizer is in `tests/opt/fold.ir`.

```text
fn fold.quotient() -> i64 {
b0:
    %0 = sdiv i64 -9, 2
    %1 = srem i64 -9, 2
    %2 = add i64 %0, %1
    ret i64 %2
}
fn fold.shifted() -> i32 {
b0:
    %0 = sshr i64 -16, 2
    %1 = trunc i32 %0
    ret i32 %1
}
fn fold.compared() -> i8 {
b0:
    %0 = slt i8 3, 4
    %1 = copy i8 %0
    branch %0, b1, b2
b1:
    %2 = fne i8 2, 2
    %1 = copy i8 %2
    jump b2
b2:
    ret i8 %1
}
fn fold.undefined() -> i64 {
b0:
    %0 = copy i64 0
    %1 = sdiv i64 1, %0
    ret i64 %1
}
```

The optimizer leaves one instruction besides the returns, in `tests/opt/fold.opt`.

```text
fn fold.quotient() -> i64 {
b0:
    ret i64 -5
}
fn fold.shifted() -> i32 {
b0:
    ret i32 -4
}
fn fold.compared() -> i8 {
b0:
    ret i8 0
}
fn fold.undefined() -> i64 {
b0:
    %0 = sdiv i64 1, 0
    ret i64 %0
}
```

The quotient `-9 / 2` truncates toward zero to `-4`, and `-9 % 2` takes the sign of the dividend, `-1`. In `compared`, the branch on `3 < 4` becomes a jump, and the function returns the folded `2.0 != 2.0`. The division `1 / zero` stays, because the divisor is zero.

## Symbolic sizes and target widths

The size of a type, the offset of a field and the stride of an array depend on the target. The IR of chapter 7 holds each of them as a symbolic value. An operand of kind `IR_SYM` refers to an entry of the module's table of symbolic values, printed as in `size_of main.H`. The optimizer takes no target, so it has no number for such a value. For every pass, a constant is an integer or a float operand, and the helper `is_constant` accepts those two kinds alone.

```c
static bool is_constant(const struct ir_operand *o)
{
    return o->kind == IR_INT || o->kind == IR_FLOAT;
}
```

A symbolic operand therefore never enters folding, and copy propagation across blocks never spreads it. A rule that tests only its constant operand still applies, as the rule for `x * 1` with a symbolic `x`. A unit test in `tests/unit/test_optimize.c` compiles this program as the module `main`.

```anti
struct H
{
    tag: u8,
    n: i32,
}

fn f() -> int
{
    let n = size_of(H);
    return n * 1 + 4 - 4;
}
```

```text
type main.H = struct { tag: i8, n: i32 }
fn main.f() -> i64 {
b0:
    %0 = copy i64 size_of main.H
    %1 = mul i64 %0, 1
    %2 = add i64 %1, 4
    %3 = sub i64 %2, 4
    ret i64 %3
}
```

```text
type main.H = struct { tag: i8, n: i32 }
fn main.f() -> i64 {
b0:
    %0 = add i64 size_of main.H, 4
    %1 = sub i64 %0, 4
    ret i64 %1
}
```

The peephole rule for `x * 1` turns `%1` into a copy of `%0`. Copy propagation inside the block then puts `size_of main.H` into the addition. The addition and the subtraction stay, because folding stops at the symbolic operand. Chapter 12 folds the size to 8 on each of the six targets and runs the passes again, which leave a return of the constant 8.

The C types `c_long` and `c_ulong` have the IR type `clong`, and `c_wchar` has the IR type `cwchar`. Their widths differ between targets, as chapter 11 lists, and their arithmetic wraps at the width of the target. The function `fold_inst` refuses every operation with such a type in its result or its operands.

```c
static bool is_target_sized(enum ir_type type)
{
    return type == IR_CLONG || type == IR_CWCHAR;
}

/* DESIGN: an operation on c_long or c_wchar wraps at a width that only the
   back end knows, so no pass computes one. */
static bool fold_inst(const struct ir_inst *inst, struct ir_operand *out)
{
    uint64_t v;

    if (is_target_sized(inst->type) || is_target_sized(inst->a.type) ||
        is_target_sized(inst->b.type)) {
        return false;
    }
    switch (inst->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV:
    case IR_SREM: case IR_UREM: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR_S: case IR_SHR_U:
    case IR_EQ: case IR_NE: case IR_SLT: case IR_SLE: case IR_SGT:
    case IR_SGE: case IR_ULT: case IR_ULE: case IR_UGT: case IR_UGE:
        if (inst->a.kind != IR_INT || inst->b.kind != IR_INT ||
            !fold_int(inst->op, inst->a.type, inst->a.as.integer,
                      inst->b.as.integer, &v)) {
            return false;
        }
        *out = ir_int_op(inst->type, v);
        return true;
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
    case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT:
    case IR_FGE:
        return inst->a.kind == IR_FLOAT && inst->b.kind == IR_FLOAT &&
               fold_float(inst->op, inst->a.type, inst->a.as.floating,
                          inst->b.as.floating, out);
    case IR_NEG:
    case IR_NOT:
        if (inst->a.kind != IR_INT) {
            return false;
        }
        v = inst->op == IR_NEG ? 0 - inst->a.as.integer : ~inst->a.as.integer;
        *out = ir_int_op(inst->type, v);
        return true;
    case IR_FNEG:
        if (inst->a.kind != IR_FLOAT) {
            return false;
        }
        *out = ir_float_op(inst->type, -inst->a.as.floating);
        return true;
    case IR_TRUNC: case IR_SEXT: case IR_ZEXT: case IR_SITOF: case IR_UITOF:
    case IR_FTOSI: case IR_FTOUI: case IR_FEXT: case IR_FTRUNC:
        return is_constant(&inst->a) && fold_conversion(inst, out);
    default:
        return false;
    }
}
```

A second unit test multiplies a variable of type `c_long` by a constant.

```anti
fn f() -> c_long
{
    let x: c_long = 65536;
    return x * 70000;
}
```

```text
fn main.f() -> clong {
b0:
    %0 = mul clong 65536, 70000
    ret clong %0
}
```

Copy propagation moves the constant 65536 into the multiplication, and the multiplication stays. The product 4587520000 fits the 64 bits of `c_long` on Linux and macOS. At the 32 bits of `c_long` on Windows it wraps to 292552704, and chapter 12 shows both results on their targets.

## Copy propagation

After `x = copy v`, a use of `x` can read `v` instead. Inside one block the replacement holds until the block defines `x` or `v` again. The function `propagate_in_block` keeps the known values in an array indexed by temporary and forgets an entry at the next definition of either side.

```c
/* Inside one block, a use after x = copy v and before the next
   definition of x or of v reads v. */
static bool propagate_in_block(struct ir_function *f, struct ir_block *b)
{
    struct ir_operand *known = allocate(f->temp_count, sizeof *known);
    uint32_t *active = allocate(f->temp_count, sizeof *active);
    size_t active_count = 0;
    bool changed = false;
    size_t i;
    size_t k;

    for (i = 0; i < b->count; i++) {
        struct ir_inst *inst = &b->insts[i];
        uint32_t r = inst->result;
        for (k = 0; k < operand_count(inst); k++) {
            struct ir_operand *o = operand(inst, k);
            if (o->kind == IR_TEMP && known[o->as.temp].kind != IR_NONE) {
                *o = known[o->as.temp];
                changed = true;
            }
        }
        if (r == IR_NO_RESULT) {
            continue;
        }
        for (k = 0; k < active_count;) {
            uint32_t t = active[k];
            if (t == r || is_temp(&known[t], r)) {
                known[t] = none();
                active[k] = active[--active_count];
            } else {
                k++;
            }
        }
        if (inst->op == IR_COPY && !is_temp(&inst->a, r)) {
            known[r] = inst->a;
            active[active_count++] = r;
        }
    }
    free(known);
    free(active);
    return changed;
}
```

Across blocks, the order in which instructions run depends on the path through the function. The function `propagate_copies` therefore replaces every use of `x` only when `x` has exactly one definition, `x = copy v`, and `v` meets one of three conditions.

- `v` is a constant. Definite assignment makes that single definition reach every use, and it always stores the same constant.
- `v` is a parameter that no instruction assigns. Its value never changes after the entry.
- `v` has exactly one definition, earlier in the same block as the copy. Any path that runs the definition of `v` again runs the copy right after it, because a block runs from its first instruction to its last. A use of `x` between the two instructions would come before any definition of `x` on the first pass through the block, which definite assignment excludes.

```c
/* Across blocks, x = copy v replaces every use of x when x has a single
   definition. v must be a constant, a parameter that no instruction
   assigns, or a temporary with a single definition earlier in the same
   block. Definite assignment, which the verifier checks, makes each case
   exact. */
static bool propagate_copies(struct ir_function *f)
{
    struct counts c;
    bool changed = false;
    size_t b;
    size_t i;

    for (b = 0; b < f->block_count; b++) {
        changed = propagate_in_block(f, f->blocks[b]) || changed;
    }
    count(f, &c);
    for (b = 0; b < f->block_count; b++) {
        for (i = 0; i < f->blocks[b]->count; i++) {
            struct ir_inst *inst = &f->blocks[b]->insts[i];
            uint32_t x = inst->result;
            uint32_t y = inst->a.as.temp;
            bool exact;
            if (inst->op != IR_COPY || c.defs[x] != 1 || is_temp(&inst->a, x)) {
                continue;
            }
            exact = is_constant(&inst->a) ||
                    (inst->a.kind == IR_TEMP && c.defs[y] == 1 &&
                     (y < f->param_count ||
                      (c.block[y] == b && c.index[y] < i)));
            if (exact) {
                changed = replace_uses(f, x, inst->a) || changed;
            }
        }
    }
    free_counts(&c);
    return changed;
}
```

A variable of a loop has several definitions, one per assignment, and keeps its copies. Chapter 25 lists SSA among the extensions. SSA gives every value one definition, which widens this pass.

## Peephole rules

A peephole rule looks at one instruction, or two adjacent ones, and replaces them with a simpler form. The optimizer has this set of rules. Apart from the constant moving to the right, which also applies to `fadd`, `fmul`, `feq` and `fne`, every rule works on integers.

| Before | After |
|---|---|
| `add 2, x` and other commutative operations with the constant first | `add x, 2` |
| `add x, 0`, `sub x, 0`, `or x, 0`, `xor x, 0`, a shift by 0 | `copy x` |
| `mul x, 1`, `sdiv x, 1`, `udiv x, 1` | `copy x` |
| `mul x, 0`, `and x, 0` | `copy 0` |
| `and x, -1` | `copy x` |
| `mul x, 8` | `shl x, 3` |
| `udiv x, 8` | `ushr x, 3` |
| `urem x, 8` | `and x, 7` |
| `t = op a, b` followed by `x = copy t`, where `t` has no other use | `x = op a, b` |
| `branch c, b1, b1` | `jump b1` |
| A jump to a block that holds only a jump | A jump to the final block |

A signed division by a power of two keeps `sdiv`, because a right shift rounds a negative value down and `sdiv` rounds toward zero. With the constant always on the right, instruction selection in chapter 12 needs one pattern per operation for a constant operand. The pair `t = op` and `x = copy t` comes from lowering a variable assignment, as `i += 1` in chapter 8. The peephole rules run before copy propagation, because propagation would give `t` a second use and block this rule.

```c
/* Rewrite inst into a simpler instruction with the same result. */
static bool simplify(struct ir_inst *inst)
{
    enum ir_type t = inst->type;
    int k;

    if (is_commutative(inst->op) && is_constant(&inst->a) &&
        !is_constant(&inst->b)) {
        struct ir_operand swap = inst->a;
        inst->a = inst->b;
        inst->b = swap;
        return true;
    }
    switch (inst->op) {
    case IR_ADD: case IR_SUB: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR_S: case IR_SHR_U:
        if (is_int(&inst->b, 0)) {
            make_copy(inst, inst->a);
            return true;
        }
        return false;
    case IR_MUL:
        if (is_int(&inst->b, 0)) {
            make_copy(inst, inst->b);
            return true;
        }
        if (is_int(&inst->b, 1)) {
            make_copy(inst, inst->a);
            return true;
        }
        if ((k = power_of_two(&inst->b)) > 0) {
            inst->op = IR_SHL;
            inst->b = ir_int_op(t, (uint64_t)k);
            return true;
        }
        return false;
    case IR_SDIV:
    case IR_UDIV:
        if (is_int(&inst->b, 1)) {
            make_copy(inst, inst->a);
            return true;
        }
        if (inst->op == IR_UDIV && (k = power_of_two(&inst->b)) > 0) {
            inst->op = IR_SHR_U;
            inst->b = ir_int_op(t, (uint64_t)k);
            return true;
        }
        return false;
    case IR_UREM:
        if (power_of_two(&inst->b) > 0) {
            inst->op = IR_AND;
            inst->b = ir_int_op(t, inst->b.as.integer - 1);
            return true;
        }
        return false;
    case IR_AND:
        if (is_int(&inst->b, 0)) {
            make_copy(inst, inst->b);
            return true;
        }
        if (is_int(&inst->b, (uint64_t)-1)) {
            make_copy(inst, inst->a);
            return true;
        }
        return false;
    case IR_BRANCH:
        if (inst->b.as.index == inst->c.as.index) {
            inst->op = IR_JUMP;
            inst->a = inst->b;
            inst->b = none();
            inst->c = none();
            return true;
        }
        return false;
    default:
        return false;
    }
}
```

The test `tests/opt/identities.anti` combines several rules in one expression.

```anti
fn mix(x: int) -> int
{
    return (x + 0) * 8 + 2 * x - x * 1;
}
```

```text
fn identities.mix(%0: i64) -> i64 {
b0:
    %1 = add i64 %0, 0
    %2 = mul i64 %1, 8
    %3 = mul i64 2, %0
    %4 = add i64 %2, %3
    %5 = mul i64 %0, 1
    %6 = sub i64 %4, %5
    ret i64 %6
}
```

```text
fn identities.mix(%0: i64) -> i64 {
b0:
    %1 = shl i64 %0, 3
    %2 = shl i64 %0, 1
    %3 = add i64 %1, %2
    %4 = sub i64 %3, %0
    ret i64 %4
}
```

The operations `add %0, 0` and `mul %0, 1` become copies of `%0`, and copy propagation removes them. The multiplications by 8 and by 2 become shifts, after the constant 2 has moved to the right.

### Jump targets

A jump to a block that holds only a jump goes directly to the final block. The function `final_target` follows such blocks. Blocks that only jump to each other form an endless loop, and the function then keeps the original target.

```c
/* The block that a jump to b reaches after blocks that only jump. Jumps
   that form a cycle keep b, so a jump never moves around a cycle. */
static uint32_t final_target(const struct ir_function *f, uint32_t b)
{
    uint32_t target = b;
    size_t steps;

    for (steps = 0; steps <= f->block_count; steps++) {
        const struct ir_block *block = f->blocks[target];
        if (block->count != 1 || block->insts[0].op != IR_JUMP) {
            return target;
        }
        target = block->insts[0].a.as.index;
    }
    return b;
}
```

## Dead code elimination

An instruction or a block that cannot affect a result is removed. The pass has four parts.

- A pure instruction whose result has no use. A `store`, a `bitstore`, a `memcopy`, a `call` and the terminators have effects besides their result and stay.
- A pure definition of a temporary that its block defines again before any use.
- A block that no path from the entry reaches.
- A block whose only predecessor ends with a jump to it. The predecessor takes over its instructions.

```c
static bool remove_unused_results(struct ir_function *f)
{
    struct counts c;
    bool changed = false;
    size_t b;
    size_t i;
    size_t j;
    size_t k;

    count(f, &c);
    for (b = 0; b < f->block_count; b++) {
        struct ir_block *block = f->blocks[b];
        for (i = block->count; i-- > 0;) {
            struct ir_inst *inst = &block->insts[i];
            bool dead;
            if (inst->result == IR_NO_RESULT || !is_pure(inst->op)) {
                continue;
            }
            dead = c.uses[inst->result] == 0;
            /* A definition that the block overwrites before any use. */
            for (j = i + 1; j < block->count && !dead; j++) {
                bool used = false;
                for (k = 0; k < operand_count(&block->insts[j]); k++) {
                    used = used || is_temp(operand(&block->insts[j], k),
                                           inst->result);
                }
                if (used) {
                    break;
                }
                dead = block->insts[j].result == inst->result;
            }
            if (dead) {
                delete_inst(block, i);
                changed = true;
            }
        }
    }
    free_counts(&c);
    return changed;
}
```

The test `tests/opt/loops.anti` holds the `do while` loop of chapter 8.

```anti
fn count(n: int) -> int
{
    let i = 0;
    do {
        i += 1;
        if i == 5 {
            continue;
        }
        if i > 8 {
            break;
        }
    } while i < n
    return i;
}
```

```text
fn loops.count(%0: i64) -> i64 {
b0:
    %1 = copy i64 0
    jump b1
b1:
    %2 = add i64 %1, 1
    %1 = copy i64 %2
    %3 = eq i8 %1, 5
    branch %3, b4, b5
b2:
    %5 = slt i8 %1, %0
    branch %5, b1, b3
b3:
    ret i64 %1
b4:
    jump b2
b5:
    %4 = sgt i8 %1, 8
    branch %4, b6, b7
b6:
    jump b3
b7:
    jump b2
}
```

```text
fn loops.count(%0: i64) -> i64 {
b0:
    %1 = copy i64 0
    jump b1
b1:
    %1 = add i64 %1, 1
    %2 = eq i8 %1, 5
    branch %2, b2, b4
b2:
    %3 = slt i8 %1, %0
    branch %3, b1, b3
b3:
    ret i64 %1
b4:
    %4 = sgt i8 %1, 8
    branch %4, b3, b2
}
```

The blocks `b4`, `b6` and `b7` of the input only jump. The branches go directly to their targets in the output, and the three blocks are gone. The addition moved into the assignment of `i`, which is `%1 = add i64 %1, 1`. The remaining blocks and temporaries carry new numbers.

## Memory

The passes so far read and write temporaries. A load and a store reach memory, and the optimizer left both alone until now.

### Store-to-load forwarding

A load of an address that was just stored to, or just loaded from, reads a value the block already holds.

```anti
fn twice(p: *P) -> int
{
    return p.y + p.y;
}
```

Lowering computes the address of `p.y` twice and loads twice. The second load reads what the first one read, so it becomes a copy of that value. Copy propagation removes the copy in the same round.

```
fn fw.twice(%0: ptr) -> i64 {
b0:
    %1 = ptradd %0, offset_of fw.P.y
    %2 = load i64 %1
    %3 = add i64 %2, %2
    ret i64 %3
}
```

The pass walks one block. It remembers the last address written or read and the value that went there, and a load of that address becomes a copy.

```c
/* The address an operand names, as a base temporary and an offset. Two
   `ptradd` results of one base and one offset are the same address, which
   is what a field read twice produces. */
struct address {
    struct ir_operand base;
    struct ir_operand offset;
    bool known;
};
```

Two addresses are the same when they are the same `ptradd` of one base by one offset. That is what a field read twice produces, and it is the whole of the comparison. The optimizer has no alias analysis.

```c
static bool same_address(const struct address *a, const struct address *b)
{
    return a->known && b->known &&
           a->base.kind == b->base.kind && a->base.as.temp == b->base.as.temp &&
           a->offset.kind == b->offset.kind &&
           a->offset.as.integer == b->offset.as.integer;
}
```

Without alias analysis the pass has to be careful. A call may write anything, and an atomic operation is a synchronisation point. A store to an address it does not recognise may write the one it holds. Each of those wipes what the block remembers.

```c
static bool forward_stores(struct ir_function *f)
{
    bool changed = false;
    size_t b;
    size_t i;

    for (b = 0; b < f->block_count; b++) {
        struct ir_block *block = f->blocks[b];
        struct address held;
        struct ir_operand value = {IR_NONE, IR_VOID, {0}};
        enum ir_type type = IR_VOID;
        memset(&held, 0, sizeof held);
        for (i = 0; i < block->count; i++) {
            struct ir_inst *inst = &block->insts[i];
            struct address at;
            switch (inst->op) {
            case IR_LOAD:
                at = address_of(block, i, inst->a);
                if (held.known && same_address(&held, &at) &&
                    type == inst->type) {
                    make_copy(inst, value);
                    changed = true;
                    break;
                }
                held = at;
                type = inst->type;
                value = ir_temp_op(f, inst->result);
                break;
            case IR_STORE:
                at = address_of(block, i, inst->b);
                if (!held.known || !same_address(&held, &at)) {
                    memset(&held, 0, sizeof held);
                }
                if (at.known) {
                    held = at;
                    type = inst->type;
                    value = inst->a;
                }
                break;
            case IR_CALL:
            case IR_MEMCOPY:
            case IR_BITSTORE:
            case IR_BITLOAD:
                memset(&held, 0, sizeof held);
                break;
            default:
                break;
            }
        }
    }
    return changed;
}
```

The pass runs before copy propagation, so the copy it leaves does not survive the round. It runs per block and forgets everything at a block boundary, because a value that reached one block may not reach the next.

The file `tests/dump/forward.anti` holds the two bodies above. The tests `listing_forward.ir` and `listing_forward.opt` pin the IR before and after, so the difference the pass makes is the difference between two files of the repository. Before the pass `twice` holds two `ptradd` instructions and two loads, and `round_trip` loads twice what it has just stored.

### Scalar replacement of aggregates

A struct local that never leaves the function needs no memory at all.

```anti
fn sum(a: int, b: int) -> int
{
    let p = P { x: a, y: b };
    return p.x + p.y;
}
```

Lowering gives `p` a slot, because a struct is an aggregate and an aggregate lives in memory. Nothing takes its address, so each field becomes a temporary and the slot goes.

```
fn sroa.sum(%0: i64, %1: i64) -> i64 {
b0:
    %2 = add i64 %0, %1
    ret i64 %2
}
```

An address escapes when `&` is taken of the local or of a field. It escapes when it is passed as a pointer, and when it is stored into another object. The pass looks at every instruction that names the slot. It asks whether that instruction is a load, a store, or a `ptradd` whose result addresses one of those.

A `ptradd` of a `ptradd` escapes as well. The pass rewrites one level of address, so a second level would lose its base. A `use` field of struct type produces exactly that shape, because `sp.x` is the `x` of the `rect` inside the sprite.

```c
/* DESIGN: whether inst lets the address in temp reach somewhere the pass
   cannot follow. A load of it reads one field and never escapes. A store
   of it as the value does escape. So does an address that feeds another
   ptradd: the pass rewrites one level and would leave a second level
   without its base. */
static bool escapes_in(const struct ir_inst *inst, uint32_t temp)
{
    size_t i;

    if (inst->op == IR_LOAD) {
        return false;
    }
    if (inst->op == IR_STORE) {
        return is_temp(&inst->a, temp);
    }
    if (inst->op == IR_PTRADD) {
        return is_temp(&inst->b, temp);
    }
    for (i = 0; i < operand_count(inst); i++) {
        if (is_temp(operand((struct ir_inst *)inst, i), temp)) {
            return true;
        }
    }
    return false;
}
```

A field is reached by a constant offset or by `offset_of`. An index computed at run time reaches a different element on every pass, so a slot addressed that way keeps its memory. That rule is what makes the pass safe without alias analysis. Every address it rewrites is one it can name.

`self` is a parameter, and a parameter holds an address the caller owns, so a class body never loses its object to this pass.

## Devirtualisation

A call on a class pointer reads the entry of a table, because the object may be of a class below the static type. Two declarations remove that possibility. A `final class` has no class below it, so every call through a pointer to it is direct. A `final fn` cannot be replaced, so a call to it is direct whatever the class. A private or protected function has no entry at all and is direct for that reason.

A third form is missing here, and the reason is worth the paragraph. A public function that no class of the program replaces has one body, so the call could be direct. The checker cannot tell. It reads one module, and a class that replaces the function is declared in a module that imports this one, which the checker never sees. A library compiled on its own would bake in a direct call that a program later contradicts, and the program would call the wrong body. The scan therefore waits for a pass over the whole program, which the driver does not have.

The choice happens in the checker and not in the optimizer, because the optimizer works on IR that holds no classes. Once a call is a load and an indirect call, nothing in the IR says which entries the load can reach.

One class stays on the table whatever a later scan says. A pointer to a class that another class implements as an interface points into the middle of an object. The receiver of a call through it needs an offset that only the thunk of the concrete class knows.

The listings `devirt.ir` and `devirt.dev.ir` hold the same program in both modes, and they are the same text.

```anti
pub fn total(p: *Only, q: *Base) -> int
{
	return p.value() + q.value() + q.same();
}
```

```text
fn devirt.total(%0: ptr, %1: ptr) -> i64 {
b0:
    %2 = call i64 @devirt.Only.value(%0)
    %3 = load ptr %1
    %4 = mul i64 8, size_of ptr
    %5 = ptradd %3, %4
    %6 = load ptr %5
    %7 = call i64 %6 via @devirt.fn.0(%1)
    %8 = add i64 %2, %7
    %9 = call i64 @devirt.Base.same(%1)
    %10 = add i64 %8, %9
    ret i64 %10
}
```

`Only` is a `final class`, so `p.value()` is direct. `Base` is open and `Derived` replaces `value`, so `q.value()` reads the table. `Base.same` is a `final fn`, so `q.same()` is direct again.

## The pass that only reports

A whole-program pass that changes nothing runs in every build. It walks every function a worker can reach and reports what may not stand there.

- A `delete`, because another worker may hold the same object.
- A read or a write of a `mutable` field of a singleton, because two workers would race on it.

Each message names the field and the worker.

Release mode also reports an abstract class that no class of the program fills, since only that mode sees every class.

The pass reads the checked tree rather than the IR. Dev mode compiles one module, so no point of the driver holds the IR of a whole program. The checks also need names and types that the IR no longer carries.

## Unused functions

The whole program is known after loading, so the optimizer can remove every function that nothing calls. The pass starts from a set of entries. It marks the functions that the entries call, directly or through other functions, and the globals that marked functions use. It removes the rest, `extern` declarations included, and numbers the remaining functions again. Three kinds of function are entries.

- The function `main` of the entry module, which `ir_optimize` receives as the main module of the program.
- Every function of the entry module, when that module has no `main` or when the caller sets `all`. The tests above have no `main`, and `ir_optimize_module` sets `all`.
- Every `export fn`, which C code may call even when no Anti code does.

```c
/* Remove the functions and globals that the entry cannot reach. The entry
   is main in module entry, or every function of that module when it has
   no main or when all is set. Every export fn is an entry too, because C
   code may call it. */
static void remove_unused_functions(struct ir_module *m, const char *entry,
                                    bool all)
{
    bool *live = allocate(m->function_count, sizeof *live);
    bool *live_globals = allocate(m->global_count, sizeof *live_globals);
    uint32_t *map = allocate(m->function_count, sizeof *map);
    uint32_t *global_map = allocate(m->global_count, sizeof *global_map);
    bool has_main = false;
    bool grew = true;
    size_t n = 0;
    size_t i;
    size_t j;
    size_t b;
    size_t k;

    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (!all && !f->is_extern && strcmp(f->module, entry) == 0 &&
            strcmp(f->name, "main") == 0) {
            has_main = true;
        }
    }
    for (i = 0; i < m->function_count; i++) {
        const struct ir_function *f = m->functions[i];
        if (!f->is_extern &&
            (f->exported || (strcmp(f->module, entry) == 0 &&
                             (!has_main || strcmp(f->name, "main") == 0)))) {
            mark_function(m, (uint32_t)i, live, live_globals);
        }
    }
```

The rest of the function marks the globals that marked globals refer to, removes every unmarked function and global, and renumbers the references. The test `tests/opt/unused.anti` has a `main`.

```anti
extern fn putchar(c: i32) -> i32;

fn helper() -> int
{
    return 1;
}

fn used() -> int
{
    return 2;
}

fn main() -> int
{
    return used();
}
```

```text
extern fn putchar(i32) -> i32
fn unused.helper() -> i64 {
b0:
    ret i64 1
}
fn unused.used() -> i64 {
b0:
    ret i64 2
}
fn unused.main() -> i64 {
b0:
    %0 = call i64 @unused.used()
    ret i64 %0
}
```

```text
fn unused.used() -> i64 {
b0:
    ret i64 2
}
fn unused.main() -> i64 {
b0:
    %0 = call i64 @unused.used()
    ret i64 %0
}
```

The function `helper` and the declaration of `putchar` are gone, and `used` stays because `main` calls it.

### One module in dev mode

With the option `--dev`, each call of antic compiles one module into its own object file, and the linker joins the objects. The function `ir_optimize_module` optimizes the functions of that module and turns every function of another module into a declaration. The object of the other module defines its symbol. The removal of unused functions then runs with `all` set, so every function of the module stays for calls from other objects.

```c
void ir_optimize_module(struct ir_module *program, const char *module)
{
    size_t i;

    for (i = 0; i < program->function_count; i++) {
        struct ir_function *f = program->functions[i];
        if (f->is_extern) {
            continue;
        }
        if (strcmp(f->module, module) != 0) {
            drop_body(f);
        } else {
            ir_optimize_function(f);
        }
    }
    remove_unused_functions(program, module, true);
}
```

The unit test `one_module` in `tests/unit/test_optimize.c` builds a module `main` with an unused function and a call of `seven` in the module `com.example.lib`. After `ir_optimize_module`, the function `seven` is a declaration without its body, and `unused` stays.

```c
    ir_optimize_module(&m, "main");
    ir_print(&out, &m);
    CHECK_STR(text_cstr(&out),
              "extern fn com.example.lib.seven(i64) -> i64\n"
              "fn main.unused() -> i64 {\n"
              "b0:\n"
              "    ret i64 1\n"
              "}\n"
              "fn main.main() -> i64 {\n"
              "b0:\n"
              "    %0 = call i64 @com.example.lib.seven(3)\n"
              "    ret i64 %0\n"
              "}\n");
```

## Test setup

Every file `NAME.anti` in `tests/opt` has two expected files beside it. `NAME.ir` holds the output of `--dump-ir` and `NAME.opt` the output of `--dump-opt`. The CMake file of the tests creates two tests per source file, `ir_NAME` and `opt_NAME`, from a file glob. A new example needs only the three files.

```cmake
file(GLOB opt_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opt/*.anti")
foreach(source ${opt_sources})
    get_filename_component(name "${source}" NAME_WE)
    get_filename_component(dir "${source}" DIRECTORY)
    add_test(NAME ir_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DOPTION=--dump-ir"
            "-DSOURCE=${source}"
            "-DEXPECTED=${dir}/${name}.ir"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
    add_test(NAME opt_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DANTIC=$<TARGET_FILE:antic>"
            "-DOPTION=--dump-opt"
            "-DSOURCE=${source}"
            "-DEXPECTED=${dir}/${name}.opt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_dump.cmake")
endforeach()
```

The test `dump_opt_main` pins the optimized listing of chapter 1. The unit tests in `tests/unit/test_optimize.c` run the verifier before and after the optimizer on every program. They cover folding with wrap-around and truncation, the undefined cases and the rules above. They also cover a variable assigned in a loop, the endless loop `while true do { }` and the removal of unused functions. Further unit tests check the operation on `c_long`, the symbolic size and the module compiled alone. The unit tests also pass in a build with `-fsanitize=address,undefined`. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 11, Targets]({{% relref "/programming/writing-a-compiler/11-targets-and-abis" %}}), object formats and ABIs, describes the six targets as a matrix. It covers ELF, Mach-O and COFF, and the calling conventions System V, Windows x64 and AAPCS64 with Apple's deviations. The widths of `c_long` and `c_wchar` follow for each target. The chapter lists symbol naming, position-independent code, the entry point and libc linkage for each operating system.

## References

[^1]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, section 6.5.7, paragraph 5, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf
