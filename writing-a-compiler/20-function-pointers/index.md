---
title: "Function pointers"
description: "How antic compiles function values: addresses of functions, calls through a register, call signatures in the IR and GOT entries for C functions."
summary: "Function types, taking the address of a function, indirect calls in both back ends, and the struct-of-function-pointers pattern."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:51:48+02:00
draft: false
weight: 200
tags: [compilers, assembly]
keywords: [function pointers, indirect calls, global offset table, call signature, callbacks, qsort comparator, blr, struct of function pointers]
---

## Previously

[Chapter 19, Strings, slices and bytes]({{% relref "/programming/writing-a-compiler/19-strings-slices-and-bytes" %}}), lowers `str` and `[]T` as pointer-plus-length values and puts string literals into read-only data. The chapter adds byte strings, decodes UTF-8 into `char` values in a test program and calls libc with `s.ptr`.

## Function values

[Chapter 2]({{% relref "/programming/writing-a-compiler/02-the-anti-language" %}}) gives every function a type such as `fn(i32) -> i32`, the address of code with those parameters and that result. The name of a function used as a value has that type, and `null` is a value of every function type. The IR of chapter 7 maps a function type to `ptr`. Function types are the last types of chapter 2 that lowering handles, after `str` and slices in chapter 19.

A function name in value position lowers to the operation `addr`, which chapter 14 and chapter 15 select as a pc-relative address. A qualified name such as `geometry.length` lowers the same way.

```c
static struct ir_operand lower_name(struct lowerer *l, const struct expr *e)
{
    const struct symbol *sym = e->symbol;
    struct place p;

    if (sym->kind == SYMBOL_CONST) {
        return constant(l, sym->value, ir_type_of(e->type));
    }
    if (sym->kind == SYMBOL_FN || sym->kind == SYMBOL_EXTERN_FN) {
        return temp(l, ir_addr(l->f, l->b,
                               ir_func_op(callee_function(l, sym))));
    }
    lower_place(l, e, &p);
    return read_place(l, &p);
}
```

The comparisons `==` and `!=` of chapter 8 compare two `ptr` values, so function pointers need no comparison of their own. The test program `tests/dump/fnptr.anti` stores two functions in an array of structs and calls each through `run`.

```anti
extern fn abs(x: i32) -> i32;

struct Op
{
    apply: fn(i32) -> i32,
}

fn twice(x: i32) -> i32
{
    return x * 2;
}

fn run(op: *Op, x: i32) -> i32
{
    return op.apply(x);
}

fn main() -> int
{
    let ops = [Op { apply: twice }, Op { apply: abs }];
    return run(&ops[0], 5) as int + run(&ops[1], -3) as int;
}
```

## Signatures

A direct call names its callee. The back ends of chapters 14 to 18 read the parameters, their extensions and the result from the callee's IR function. A call through a pointer has only an address. Its parameters and result come from the function type of the callee expression.

Lowering therefore records each function type that a module calls through as a signature. A signature is a function that the module declares and never defines, named `fn.N` after its index among the module's signatures. No Anti identifier contains a dot, so no function of the module has that name. The IR of chapter 7 stores declared functions, and so do the library files of chapter 9, so a signature needs no record of its own in either.

```c
/* DESIGN: a call through a function pointer takes its parameters and
   result from a signature: a function fn.N that the module declares and
   never defines. No identifier contains a dot, so no Anti function has
   that name. Equal signatures share one declaration. */
static const struct ir_function *signature(struct lowerer *l,
                                           const struct type *t)
{
    struct ir_function *f;
    uint32_t count = 0;
    char name[24];
    size_t i;

    for (i = 0; i < l->m->function_count; i++) {
        const struct ir_function *g = l->m->functions[i];
        if (!g->is_extern || g->module == NULL ||
            strcmp(g->module, l->module_name) != 0 ||
            strncmp(g->name, "fn.", 3) != 0) {
            continue;
        }
        if (has_signature(l, g, t)) {
            return g;
        }
        count++;
    }
    snprintf(name, sizeof name, "fn.%u", count);
    f = ir_declare_add(l->m, l->module_name, name, ir_type_of(t->result),
                       result_agg(l, t->result));
    for (i = 0; i < t->param_count; i++) {
        add_param(l, f, t->params[i]);
    }
    return f;
}
```

Two function types share one signature when `has_signature` finds the same parameters and result. It compares the IR types, the extensions of 8-bit and 16-bit parameters and the aggregate of each aggregate parameter and the result. An aggregate is the index of an entry in the type table of chapter 7. The function `agg_of` gives each type name one entry, so equal indices stand for equal Anti types.

```c
/* Whether declared function g has the parameters and result of t. */
static bool has_signature(struct lowerer *l, const struct ir_function *g,
                          const struct type *t)
{
    size_t i;

    if (g->result != ir_type_of(t->result) ||
        g->result_agg != result_agg(l, t->result) ||
        g->param_count != t->param_count) {
        return false;
    }
    for (i = 0; i < t->param_count; i++) {
        if (g->params[i].type != ir_type_of(t->params[i]) ||
            g->params[i].ext != param_ext(t->params[i]) ||
            g->params[i].agg != result_agg(l, t->params[i])) {
            return false;
        }
    }
    return true;
}
```

## Calls through a pointer

The operation `call` keeps the address in its operand `a`, and the signature in operand `b`. A direct call leaves `b` empty. The callee expression is evaluated before the arguments, the left-to-right order of chapter 2.

```c
static struct ir_operand lower_call(struct lowerer *l, const struct expr *e);
static struct ir_operand lower_parallel(struct lowerer *l,
                                       const struct expr *e);
static struct ir_operand lower_dispatch(struct lowerer *l,
                                        const struct expr *e);
static struct ir_operand lower_join(struct lowerer *l, const struct expr *e);

/* The address of the memory that holds the aggregate value of e. A
   literal gets a slot of its own, and a constant is read-only data. */
static struct ir_operand lower_address(struct lowerer *l,
                                       const struct expr *e)
{
    const struct symbol *sym = e->symbol;
    uint32_t slot;

    if (l->failed) {
        return none();
    }
    if (sym != NULL && sym->kind == SYMBOL_CONST &&
        (e->kind == EXPR_NAME || e->kind == EXPR_FIELD)) {
        return const_address(l, sym->value, e->type);
    }
    /* A static field is a global, and its name is its whole address. */
    if (sym != NULL && sym->kind == SYMBOL_GLOBAL) {
        return temp(l, ir_addr(l->f, l->b,
                               ir_global_op(static_global(l, sym))));
    }
    switch (e->kind) {
    case EXPR_NAME:
        return temp(l, sym->ir);
    case EXPR_FIELD:
        return field_address(l, e);
    case EXPR_INDEX:
        return element_address(l, e);
    case EXPR_UNARY:
        return lower_expr(l, e->as.unary.operand);
    case EXPR_CALL:
        return lower_call(l, e);
    case EXPR_PARALLEL:
        return lower_parallel(l, e);
    case EXPR_DISPATCH:
        return lower_dispatch(l, e);
    case EXPR_JOIN:
        return lower_join(l, e);
    default:
        slot = ir_entry_slot(l->f, vtype_of(l, e->type));
        build_into(l, e, temp(l, slot));
        return temp(l, slot);
    }
}
```

The IR prints such a call with `via` and the signature. In `run`, `%2` loads the field `apply` from the struct at `%0`. In `main`, the address of `twice` and the address of `abs` go into the array `ops`, of the type `[2]fnptr.Op` in the type table. The second element lies `size_of fnptr.Op` bytes after the first, a symbolic value that the back end folds to 8.

```text
type fnptr.Op = struct { apply: ptr }
type [2]fnptr.Op = array 2 of fnptr.Op
extern fn abs(i32) -> i32
extern fn fnptr.fn.0(i32) -> i32
fn fnptr.twice(%0: i32) -> i32 {
b0:
    %1 = shl i32 %0, 1
    ret i32 %1
}
fn fnptr.run(%0: ptr, %1: i32) -> i32 {
b0:
    %2 = load ptr %0
    %3 = call i32 %2 via @fnptr.fn.0(%1)
    ret i32 %3
}
fn fnptr.main() -> i64 {
b0:
    %0 = slot [2]fnptr.Op
    %1 = addr @fnptr.twice
    store ptr %1, %0
    %2 = ptradd %0, size_of fnptr.Op
    %3 = addr @abs
    store ptr %3, %2
    %4 = ptradd %0, 0
    %5 = call i32 @fnptr.run(%4, 5)
    %6 = sext i64 %5
    %7 = ptradd %0, size_of fnptr.Op
    %8 = call i32 @fnptr.run(%7, -3)
    %9 = sext i64 %8
    %10 = add i64 %6, %9
    ret i64 %10
}
```

The verifier of chapter 7 checks the argument count of a direct call against the callee and of a call through a pointer against its signature.

```c
    case IR_CALL: {
        /* A direct call names its callee in a, a call through a pointer
           its signature in b. */
        const struct ir_operand *named =
            inst->b.kind == IR_FUNC ? &inst->b : &inst->a;
        if (inst->a.kind != IR_FUNC) {
            same_type(v, inst, &inst->a, IR_PTR);
        }
        if (named->kind == IR_FUNC && operand_ok(v, inst, named)) {
            const struct ir_function *callee = v->m->functions[named->as.index];
            if (callee->variadic ? inst->arg_count < callee->param_count
                                 : inst->arg_count != callee->param_count) {
                fail(v, "call passes %zu arguments to %s, which takes %zu",
                     inst->arg_count, callee->name, callee->param_count);
            }
        }
        break;
    }
```

## Selection of indirect calls

Both back ends take the parameters and the result from `select_callee`, which returns the signature or the callee. The code of `emit_call` from chapter 14 then serves both kinds of call up to its last instruction. That instruction is `call` of a symbol for a direct call and a call through a register for a call through a pointer.

```c
const struct ir_function *select_callee(const struct selector *s,
                                        const struct ir_inst *inst)
{
    return s->m->functions[inst->b.kind == IR_FUNC ? inst->b.as.index
                                                   : inst->a.as.index];
}
```

On x86_64 the instruction `call` also takes its target from a register or memory operand[^1]. AT&T syntax prefixes such an absolute operand with `*`, as in `call *%rax`[^2]. Its opcode `X64_CALLR` names the register as a use. The register allocator of chapter 13 therefore keeps the address in a register that no argument move overwrites.

```c
    bool indirect = inst->b.kind == IR_FUNC;
    struct mach_operand target = indirect ? select_reg(s, &inst->a)
                                          : mach_imm(0);
```

```c
    call = emit1(s, indirect ? X64_CALLR : X64_CALL, indirect ? target : f);
    call->uses = uses;
    call->defs = s->abi->caller_saved;
```

In `run` on linux-x86_64 the address loads into `rax`, the argument moves to `edi`, and `call *%rax` jumps to it.

```text
fnptr.run:
b0:
    pushq %rbp
    movq %rsp, %rbp
    movq (%rdi), %rax
    movl %esi, %edi
    call *%rax
    popq %rbp
    ret
```

On ARM64 the instruction `blr` branches to the address in a register and stores the return address in `x30`[^3]. In `run` on linux-arm64 the address loads into `x9`, which no convention uses for arguments.

```text
fnptr.run:
b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ldr x9, [x0]
    mov w0, w1
    blr x9
    ldp x29, x30, [sp], #16
    ret
```

## Signatures with aggregates

A parameter or result of aggregate type names its entry in the type table. The IR prints such a parameter as `agg` and the name of the entry, and the function `signature` of `src/ir_print.c` prints an aggregate result as `-> agg` and the name.

```c
static void param_type(struct text *out, const struct ir_module *m,
                       const struct ir_param *p)
{
    if (p->type == IR_AGG) {
        text_appendf(out, "agg %s", m->aggs[p->agg]->name);
    } else {
        text_append(out, ir_type_name(p->type));
    }
    if (p->ext != IR_EXT_NONE) {
        text_append(out, p->ext == IR_EXT_SIGN ? " signext" : " zeroext");
    }
}
```

The program `measure.anti` calls a function of type `fn(str) -> int` and a function of type `fn(V2) -> int`, both through locals.

```anti
struct V2
{
    x: f32,
    y: f32,
}

fn length_of(s: str) -> int
{
    return s.len;
}

fn sum(v: V2) -> int
{
    return (v.x + v.y) as int;
}

fn main() -> int
{
    let f = length_of;
    let g = sum;
    return f("four") + g(V2 { x: 1.5, y: 1.5 });
}
```

Both function types have one parameter of IR type `agg` and the result `i64`. Their aggregates are `str` and `measure.V2`, so `has_signature` gives them two signatures, `fn.0` and `fn.1`. The output of `antic --dump-opt measure.anti` shows both.

```text
type str = struct { ptr: ptr, len: i64 }
type measure.V2 = struct { x: f32, y: f32 }
extern fn measure.fn.0(agg str) -> i64
extern fn measure.fn.1(agg measure.V2) -> i64
global measure.0 size 5 align 1 bytes 66 6f 75 72 00
fn measure.length_of(%0: agg str) -> i64 {
b0:
    %1 = ptradd %0, offset_of str.len
    %2 = load i64 %1
    ret i64 %2
}
fn measure.sum(%0: agg measure.V2) -> i64 {
b0:
    %1 = load f32 %0
    %2 = ptradd %0, offset_of measure.V2.y
    %3 = load f32 %2
    %4 = fadd f32 %1, %3
    %5 = ftosi i64 %4
    ret i64 %5
}
fn measure.main() -> i64 {
b0:
    %0 = slot str
    %1 = slot measure.V2
    %2 = addr @measure.length_of
    %3 = addr @measure.sum
    %4 = addr @measure.0
    store ptr %4, %0
    %5 = ptradd %0, offset_of str.len
    store i64 4, %5
    %6 = call i64 %2 via @measure.fn.0(%0)
    store f32 1.5, %1
    %7 = ptradd %1, offset_of measure.V2.y
    store f32 1.5, %7
    %8 = call i64 %3 via @measure.fn.1(%1)
    %9 = add i64 %6, %8
    ret i64 %9
}
```

The back end places the arguments of each call by its signature. On linux-arm64 the `str` goes to `x0` and `x1`, and the struct of two `f32` fields goes to `s0` and `s1`, by the rules of chapter 18. The program exits with 7 on the development Mac, the length 4 of `"four"` plus 3, the sum `1.5 + 1.5` converted to `int`.

```text
    adrp x10, measure.length_of
    add x10, x10, :lo12:measure.length_of
    adrp x20, measure.sum
    add x20, x20, :lo12:measure.sum
    adrp x11, measure.0
    add x11, x11, :lo12:measure.0
    str x11, [x9]
    mov x11, #4
    str x11, [x9, #8]
    ldr x0, [x9]
    ldr x1, [x9, #8]
    blr x10
    mov x21, x0
    movz w9, #16320, lsl #16
    fmov s18, w9
    str s18, [x19]
    movz w9, #16320, lsl #16
    fmov s18, w9
    str s18, [x19, #4]
    ldr s0, [x19]
    ldr s1, [x19, #4]
    blr x20
```

## Addresses of C functions

The functions of an Anti program are linked into its executable, so the address of `twice` is pc-relative. A C function such as `abs` may lie in a shared library, which the dynamic linker loads at an address that the static linker does not know. Position-independent code loads the address of such a symbol from the global offset table, the GOT[^4]. The dynamic linker fills the GOT before any code of the process runs[^4].

On ARM64 ELF the operator `:got:` on `adrp` gives the page of the GOT entry, and `:got_lo12:` on `ldr` its low 12 bits[^5]. On x86_64 `sym@GOTPCREL(%rip)` addresses the GOT entry relative to `rip`[^4]. Mach-O names the entry `sym@GOTPAGE` and `sym@GOTPAGEOFF` on ARM64 and uses `@GOTPCREL` on x86_64. Apple clang 21.0.0 with `-fPIE` compiles `return abs;` in a C function to these forms for x86_64 Linux, ARM64 Linux and ARM64 macOS. For `x86_64-pc-windows-msvc` it emits `leaq` with the relocation `IMAGE_REL_AMD64_REL32`.

```c
bool select_uses_got(const struct selector *s, const struct ir_operand *o)
{
    return o->kind == IR_FUNC && s->m->functions[o->as.index]->module == NULL &&
           s->convention != CONVENTION_WINDOWS_X64 &&
           s->convention != CONVENTION_WINDOWS_ARM64;
}
```

Windows links the static C runtime `libcmt.lib`, as [chapter 16]({{% relref "/programming/writing-a-compiler/16-assembly-emission" %}}) decided, so its C functions lie in the executable itself. For the other targets the x86_64 selector loads the GOT entry with `mov` in place of `lea`.

```c
/* The address of a function or a global, relative to rip. A C function
   under System V loads its address from the GOT entry. */
static void emit_addr(struct selector *s, const struct ir_inst *inst)
{
    struct mach_operand symbol = mach_imm(inst->a.as.index);

    symbol.kind = inst->a.kind == IR_FUNC ? MACH_FUNC : MACH_GLOBAL;
    symbol.pc_relative = true;
    symbol.got = select_uses_got(s, &inst->a);
    emit2(s, symbol.got ? X64_MOV : X64_LEA, select_result(s, inst), symbol);
}
```

The ARM64 selector writes `adrp` and then `ldr` with the opcode `A64_LDRGOT` in place of `add`. The printer writes the ELF or the Mach-O operators.

```c
        } else if (inst->op == A64_LDRGOT && i == 1) {
            /* ldr x0, [x0, :got_lo12:sym] or [x0, sym@GOTPAGEOFF]. */
            text_append(out, "[");
            print_operand(out, m, names, o);
            text_append(out, macho ? ", " : ", :got_lo12:");
            print_operand(out, m, names, &inst->operands[2]);
            text_append(out, macho ? "@GOTPAGEOFF]" : "]");
            break;
        } else if (inst->op == A64_ADRP && i == 1 && o->got) {
            text_append(out, macho ? "" : ":got:");
            print_operand(out, m, names, o);
            text_append(out, macho ? "@GOTPAGE" : "");
        } else if (inst->op == A64_ADRP && i == 1) {
```

The assembly file of `fnptr.anti` for macos-arm64 takes the address of `twice` with `@PAGE` and `@PAGEOFF`, and the address of `abs` from its GOT entry. The assembler llvm-mc writes the relocations `ARM64_RELOC_GOT_LOAD_PAGE21` and `ARM64_RELOC_GOT_LOAD_PAGEOFF12` for the second pair, as `llvm-objdump -r` shows.

```text
_fnptr.main:
L_fnptr.main.b0:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #32
    str x19, [sp, #24]
    str x20, [sp, #16]
    add x19, sp, #0
    adrp x9, _fnptr.twice@PAGE
    add x9, x9, _fnptr.twice@PAGEOFF
    str x9, [x19]
    add x9, x19, #8
    adrp x10, _abs@GOTPAGE
    ldr x10, [x10, _abs@GOTPAGEOFF]
    str x10, [x9]
```

## Callbacks and structs of functions

Chapter 2 resolves `v.f(args)` to a call of the field `f` when the struct has a field of that name, and to a method call otherwise. A struct of function pointers gives each value its own functions. The C library function `qsort` sorts an array with a comparison function that it calls with pointers to two elements[^6]. The comparison returns a negative value, zero or a positive value for less, equal or greater[^6].

The test program `tests/programs/callbacks.anti` passes the Anti function `descending` to `qsort`, so C code calls Anti code. It passes the C function `abs` to the Anti function `apply` and calls a method and a field on the same struct.

```anti
extern fn printf(format: *byte, ...) -> i32;
extern fn qsort(base: *byte, count: u64, size: u64,
    compare: fn(*byte, *byte) -> i32);
extern fn abs(x: i32) -> i32;

struct Shape
{
    area: fn(*Shape) -> f64,
    size: f64,
}

fn square_area(s: *Shape) -> f64
{
    return s.size * s.size;
}

fn circle_area(s: *Shape) -> f64
{
    return 3.0 * s.size * s.size;
}

fn scale(s: *Shape, k: f64)
{
    s.size *= k;
}

// qsort passes pointers to two elements of the array.
fn descending(a: *byte, b: *byte) -> i32
{
    let x = *(a as *i32);
    let y = *(b as *i32);
    if x < y {
        return 1;
    }
    if x > y {
        return -1;
    }
    return 0;
}

fn apply(f: fn(i32) -> i32, values: []i32)
{
    let i = 0;
    while i < values.len do {
        values[i] = f(values[i]);
        i += 1;
    }
}

fn negate(x: i32) -> i32
{
    return -x;
}

fn main() -> int
{
    let shapes = [Shape { area: square_area, size: 2.0 },
        Shape { area: circle_area, size: 1.0 }];
    shapes[0].scale(1.5);
    let i = 0;
    while i < shapes.len do {
        let s = &shapes[i];
        printf("%.2f\n".ptr, s.area(s));
        i += 1;
    }

    let numbers = [3 as i32, -7, 12, 0, -1];
    apply(abs, numbers[0..5]);
    qsort(&numbers[0] as *byte, 5, 4, descending);
    apply(negate, numbers[3..5]);
    printf("%d %d %d %d %d\n".ptr, numbers[0], numbers[1], numbers[2],
        numbers[3], numbers[4]);

    let chosen: fn(i32) -> i32 = null;
    if chosen == null {
        chosen = negate;
    }
    return (chosen(-42) + (abs == abs) as i32) as int;
}
```

The two shapes have the areas 9 and 3 after `scale` multiplies the size of the first by 1.5. The array becomes `3 7 12 0 1` through `abs`, `12 7 3 1 0` through `qsort` and `12 7 3 -1 0` through `negate`. The exit code 43 is `chosen(-42)` plus 1 for `abs == abs`.

```text
exit 43
9.00
3.00
12 7 3 -1 0
```

The program runs on the development Mac. Its assembly files for the other five targets assemble with llvm-mc, where `llvm-objdump -r` shows `R_X86_64_REX_GOTPCRELX` and `R_AARCH64_ADR_GOT_PAGE` against `abs` on Linux.

## Tests

Function values, calls through parameters, locals and fields, and shared signatures are pinned, in the IR and in the assembly of every target. The build has zero warnings, and every `ctest` test passes.

## Next

Chapter 21, Testing six targets, covers expected outputs per program and the assembly and link of every target on one machine. It runs the tests on VMs and on CI started by hand, and it compares the results of x86_64 and ARM64. It adds dev and release modes to the test suite, byte-level output comparison and the raw-bytes rule for Windows consoles.

## References

[^1]: Intel, *Intel 64 and IA-32 Architectures Software Developer's Manual*, Volume 2, CALL, December 2023 edition as indexed by F. Cloutier, https://www.felixcloutier.com/x86/call

[^2]: GNU Binutils, *Using as*, section "AT&T Syntax versus Intel Syntax" of the i386 chapter, https://sourceware.org/binutils/docs/as/i386_002dVariations.html

[^3]: Arm Limited, *Arm A-profile A64 Instruction Set Architecture*, release 2025-12, XML file `blr.xml`, https://developer.arm.com/-/cdn-downloads/permalink/Exploration-Tools-A64-ISA/ISA_A64/ISA_A64_xml_A_profile-2025-12.tar.gz

[^4]: H.J. Lu and others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, March 12, 2025, sections "Global Offset Table" and "Architectural Constraints", https://gitlab.com/x86-psABIs/x86-64-ABI

[^5]: Arm Limited, *System V ABI for the Arm 64-bit Architecture*, release 2025Q4, table "GOT operators", https://github.com/ARM-software/abi-aa/blob/main/sysvabi64/sysvabi64.rst

[^6]: ISO/IEC JTC1/SC22/WG14, *N1570, Committee Draft of ISO/IEC 9899:201x*, 2011, section 7.22.5.2, https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf
