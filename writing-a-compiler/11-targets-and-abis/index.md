---
title: "Targets, object formats and ABIs"
description: "The six targets of antic: ELF, Mach-O and COFF, the System V, Windows x64 and AAPCS64 conventions, C type widths, symbol names, PIE and the C runtime."
summary: "The six targets as a matrix. ELF, Mach-O and COFF. System V, Windows x64 and AAPCS64 with Apple's deviations. The widths of `c_long` and `c_wchar` per target. Symbol naming, position-independent code, entry point and libc linkage per operating system."
date: 2026-09-13T22:32:00+02:00
lastmod: 2026-09-15T03:56:45+02:00
draft: false
weight: 110
tags: [compilers, assembly]
keywords: [calling convention, application binary interface, object file format, ELF, Mach-O, COFF, AAPCS64, System V AMD64]
---

## Previously

[Chapter 10, The optimizer]({{% relref "/programming/writing-a-compiler/10-optimizer" %}}), adds constant folding, dead code elimination, copy propagation and a small set of peephole rules. All passes run on the whole program after the library IR is loaded, or on one module in dev mode. The chapter states what each pass may assume and why no pass folds a symbolic size. Its tests show the IR before and after.

## Target matrix

A target is a combination of an operating system and a processor. antic has six: Linux, macOS and Windows, each on x86_64 and ARM64. The operating system fixes the object file format, the symbol names, the entry point and the C library. The processor and the operating system together fix the calling convention, the rules by which functions pass arguments and results. The option `--print-targets` prints the matrix, and the test `print_targets` compares it with `tests/dump/targets.txt`.

```text
target          format  convention      triple
linux-x86_64    ELF     System V AMD64  x86_64-unknown-linux-gnu
linux-arm64     ELF     AAPCS64         aarch64-unknown-linux-gnu
macos-x86_64    Mach-O  System V AMD64  x86_64-apple-macos
macos-arm64     Mach-O  Apple ARM64     arm64-apple-macos
windows-x86_64  COFF    Windows x64     x86_64-pc-windows-msvc
windows-arm64   COFF    Windows ARM64   aarch64-pc-windows-msvc
```

The structure `target_info` in `src/target.h` holds these facts for the rest of antic. The back ends of chapters 14 and 15 read the calling convention from it, and the driver of chapter 16 reads the triple and the file suffixes.

```c
/* What the rest of antic needs to know about a target. */
struct target_info {
    enum target_os os;
    enum target_arch arch;
    enum object_format format;
    enum convention convention;
    const char *triple;             /* the -triple option of llvm-mc */
    const char *object_suffix;
    const char *executable_suffix;
};
```

The triple is the value of the option `-triple` of llvm-mc. It selects the processor and the object file format of the output. The table in `src/target.c` lists one row per target.

```c
/* DESIGN: one row per target, in the order of enum target. The triples
   select the object format in llvm-mc, and the minimum macOS version comes
   from the .build_version directive in the assembly. */
static const struct target_info infos[TARGET_COUNT] = {
    [TARGET_LINUX_X86_64] = {OS_LINUX, ARCH_X86_64, FORMAT_ELF,
                             CONVENTION_SYSV, "x86_64-unknown-linux-gnu",
                             ".o", ""},
    [TARGET_LINUX_ARM64] = {OS_LINUX, ARCH_ARM64, FORMAT_ELF,
                            CONVENTION_AAPCS64, "aarch64-unknown-linux-gnu",
                            ".o", ""},
    [TARGET_MACOS_X86_64] = {OS_MACOS, ARCH_X86_64, FORMAT_MACHO,
                             CONVENTION_SYSV, "x86_64-apple-macos", ".o", ""},
    [TARGET_MACOS_ARM64] = {OS_MACOS, ARCH_ARM64, FORMAT_MACHO,
                            CONVENTION_APPLE_ARM64, "arm64-apple-macos", ".o",
                            ""},
    [TARGET_WINDOWS_X86_64] = {OS_WINDOWS, ARCH_X86_64, FORMAT_COFF,
                               CONVENTION_WINDOWS_X64,
                               "x86_64-pc-windows-msvc", ".obj", ".exe"},
    [TARGET_WINDOWS_ARM64] = {OS_WINDOWS, ARCH_ARM64, FORMAT_COFF,
                              CONVENTION_WINDOWS_ARM64,
                              "aarch64-pc-windows-msvc", ".obj", ".exe"},
};
```

The assembler llvm-mc 23.1.1 assembled a file with one function for each of the six triples on the development Mac. The tool llvm-objdump from the same release reports `elf64-x86-64` and `elf64-littleaarch64` for the Linux triples. It reports `mach-o 64-bit x86-64` and `mach-o arm64` for macOS, and `coff-x86-64` and `coff-arm64` for Windows.

## Object file formats

The assembler writes machine code, data, symbols and relocations, and the linker combines several such outputs into an executable. Linux, macOS and Windows each define their own format for both.

### ELF

Linux uses ELF, the Executable and Linking Format. The manual page elf(5) describes it as the format of "normal executable files, relocatable object files, core files, and shared objects"[^1]. An ELF file starts with the four bytes `0x7f`, `E`, `L` and `F`[^1]. The object files that llvm-mc wrote for both Linux triples start with the bytes `7f 45 4c 46`.

### Mach-O

macOS uses Mach-O. Apple's Mach-O Programming Topics states that "the object file format used in OS X is Mach-O"[^2]. The header `mach-o/loader.h` in Apple's xnu sources defines the 64-bit magic number `MH_MAGIC_64` as `0xfeedfacf`[^3]. Both Mac targets are little-endian, so the file starts with the bytes `cf fa ed fe`, which the objects of llvm-mc confirm. An executable names its start address with the load command `LC_MAIN`, which loader.h describes as the location of `main()` in a main executable[^3].

### COFF and PE

Windows uses two related formats. Microsoft's PE Format specification calls object files Common Object File Format, COFF, files and executable image files Portable Executable, PE, files[^4]. An object file starts directly with the COFF file header, and an image file has the signature `PE\0\0` before that header[^4]. The first field of the COFF file header is the machine type. The object file of llvm-mc for `x86_64-pc-windows-msvc` starts with `64 86`, the value `0x8664` in little-endian order.

The COFF symbol table stores a name of at most 8 bytes in the symbol record itself. A longer name goes into a string table, and the record holds its offset[^4]. A mangled name such as `_A8geometry_length` from chapter 9 therefore lives in the string table.

## Calling conventions

Every call follows rules for the place of arguments and results, the registers that survive the call and the state of the stack. Those rules are fixed per target. Code compiled by antic calls C functions and is called from `rt/start.c`, so it follows the rules of each target exactly. The compiler needs three conventions and two variants.

| Convention | Targets | Integer arguments | Integer result | Preserved by the callee |
|---|---|---|---|---|
| System V AMD64 | linux-x86_64, macos-x86_64 | `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` | `rax` | `rbx`, `rbp`, `r12` to `r15` |
| Windows x64 | windows-x86_64 | `rcx`, `rdx`, `r8`, `r9` | `rax` | `rbx`, `rbp`, `rdi`, `rsi`, `r12` to `r15`, `xmm6` to `xmm15` |
| AAPCS64 | linux-arm64, and with changes macos-arm64 and windows-arm64 | `x0` to `x7` | `x0` | `x19` to `x29`, the low 64 bits of `v8` to `v15` |

The rows follow the System V psABI[^5], Microsoft's x64 calling convention[^6] and the Arm procedure call standard[^7]. All three require the stack pointer to be a multiple of 16 at a call.

### System V AMD64

The target linux-x86_64 follows the x86-64 processor supplement to the System V ABI, the psABI, version 1.0 of March 12, 2025[^5]. Apple states that "the OS X x86-64 function calling conventions are the same" as in that document[^8], so macos-x86_64 follows it as well.

Integer and pointer arguments take the next free register of `%rdi`, `%rsi`, `%rdx`, `%rcx`, `%r8` and `%r9`, and floating-point arguments take `%xmm0` to `%xmm7`[^5]. An integer result returns in `%rax`, and a floating-point result in `%xmm0`[^5]. The callee preserves `%rbp`, `%rbx` and `%r12` to `%r15`, and no `xmm` register[^5]. Immediately before the `call` instruction the stack is aligned to 16 bytes[^5].

Two further rules shape the back end of chapter 14.

- The 128 bytes below `%rsp` form the red zone, which signal handlers do not modify. A leaf function may keep its whole frame there without moving `%rsp`[^5].
- A call to a variadic function sets `%al` to the number of vector registers that carry arguments[^5]. Section 3.2.3 of the psABI calls it an upper bound, and section 3.5.7 asks for the exact number. The exact number satisfies both.

An aggregate larger than two 8-byte words and without vector members is passed in memory. A result of class MEMORY goes to storage whose address the caller passes in `%rdi`[^5]. Chapter 18 applies these rules to structs.

### Windows x64

The Microsoft x64 convention passes the first four arguments by position. Integer arguments in the first four positions go in `RCX`, `RDX`, `R8` and `R9`, and a floating-point argument in position one to four goes in `XMM0` to `XMM3` of that position[^6]. The call `func3(int a, double b, int c, float d, int e, float f)` puts `a` in `RCX`, `b` in `XMM1`, `c` in `R8` and `d` in `XMM3`, and passes `e` and `f` on the stack[^6].

The caller always reserves stack space for four register parameters, the shadow store, even when the callee takes fewer[^6]. Every argument fits in 8 bytes and keeps an 8-byte alignment[^6], so the shadow store takes 32 bytes. Further arguments follow it on the stack.

A struct of 1, 2, 4 or 8 bytes is passed like an integer of that size. Any other struct is passed as a pointer to memory that the caller allocates[^6]. A scalar result returns in `RAX` or `XMM0`. A larger result goes to memory whose address the caller passes as a hidden first argument. The callee returns that address in `RAX`[^6]. The registers `RAX`, `RCX`, `RDX`, `R8` to `R11` and `XMM0` to `XMM5` are volatile, and the others must be saved by a function that uses them[^6]. A floating-point argument of a variadic function is also copied into the integer register of its position[^6].

### AAPCS64

The Procedure Call Standard for the Arm 64-bit Architecture, release 2025Q4, is the convention of linux-arm64 and the base of the two other ARM64 targets[^7].

| Register | Role |
|---|---|
| `x0` to `x7` | Arguments and results |
| `x8` | Address of a result in memory |
| `x9` to `x15` | Caller-saved |
| `x16`, `x17` | Scratch registers of call veneers and PLT code, otherwise caller-saved |
| `x18` | Platform register if the platform needs one, otherwise caller-saved |
| `x19` to `x28` | Callee-saved |
| `x29` | Frame pointer |
| `x30` | Link register |

The first eight SIMD and floating-point registers, `v0` to `v7`, pass floating-point arguments and results. A callee preserves the low 64 bits of `v8` to `v15`[^7]. At a public interface `SP mod 16 = 0` holds[^7]. A composite argument larger than 16 bytes is copied to memory that the caller allocates and replaced by a pointer to the copy[^7]. A result that does not fit the argument registers goes to memory whose address the caller passes in `x8`[^7].

### Apple ARM64

The target macos-arm64 follows AAPCS64 with documented deviations[^9], and five of them matter for antic.

- `x18` is reserved, and code must not use it.
- Every variadic argument goes on the stack in 8-byte slots, and none in a register.
- Stack arguments take only their own size, as in two 1-byte arguments at `sp` and `sp+1`. Padding at the end restores the 8-byte alignment.
- The caller sign- or zero-extends an argument of fewer than 32 bits. AAPCS64 leaves that to the callee.
- The frame pointer `x29` always addresses a valid frame record, and a leaf function may skip creating one.

Apple also makes `char` signed[^9], where AAPCS64 makes it unsigned[^7]. Anti's `c_char` is `i8` on every target, and `byte` is `u8`.

### Windows ARM64

The target windows-arm64 applies the AAPCS64 rules to non-variadic functions and adds four rules of its own[^10].

- In user mode, `x18` points to the thread environment block, TEB, and is not available to generated code.
- A variadic function uses no floating-point registers. Its arguments fill a stack image whose first 64 bytes load into `x0` to `x7`.
- The frame pointer `x29` must point to the previous `x29` and `x30` pair on the stack, for stack walking by Event Tracing for Windows and other services.
- A function that allocates 4 KB or more of stack must touch each page in order, usually through the helper `__chkstk`.

### Unions as arguments

A convention passes an aggregate by the scalars it holds and their offsets. AAPCS64 defines a union as a composite type "where each of the members has the same address"[^7], so every field of a union lies at offset 0. The three conventions treat a union by the rule they apply to a struct of the same scalars.

- System V classifies structures, arrays and unions by one algorithm[^5]. Each field is classified recursively, and the classes of the fields in one eightbyte, an 8-byte part of the aggregate, combine into its class[^5]. An eightbyte of class INTEGER goes to a general-purpose register, and one of class SSE to a vector register[^5].
- Windows x64 passes "structs and unions of size 8, 16, 32, or 64 bits" as integers of the same size[^6]. A union of another size goes as a pointer to memory that the caller allocates[^6].
- AAPCS64 calls a composite type with members of one fundamental data type a homogeneous aggregate[^7]. With a floating-point base type and at most four uniquely addressable members it is an HFA, which takes floating-point registers[^7].

The back end of antic describes every aggregate of up to 32 bytes by one list of members for these rules. The function `flatten` in `src/layout.c` records each scalar of a type with its offset. It starts every field of a union at the union's own offset, and two equal scalars at one offset count as one member.

```c
/* The scalars of a value of type v at offset. Every field of a union
   starts at the union's own offset. Two equal scalars at one offset are
   one member. */
static void flatten(struct layouts *l, struct ir_vtype v, uint64_t offset,
                    struct members *out)
{
    const struct ir_aggtype *t;
    const struct layout *layout;
    uint64_t step;
    uint64_t i;

    if (v.type != IR_AGG) {
        for (i = 0; i < out->count; i++) {
            if (out->items[i].offset == offset &&
                out->items[i].type == target_type(l, v.type)) {
                return;
            }
        }
        if (out->count < LAYOUT_MEMBER_LIMIT) {
            out->items[out->count].offset = offset;
            out->items[out->count].type = target_type(l, v.type);
            out->count++;
        }
        return;
    }
    t = l->m->aggs[v.agg];
    layout = layout_agg(l, v.agg);
    if (t->kind == IR_AGG_ARRAY) {
        step = layout_size(l, t->fields[0].type);
        for (i = 0; step > 0 && i < layout->size / step; i++) {
            flatten(l, t->fields[0].type, offset + i * step, out);
        }
        return;
    }
    for (i = 0; i < t->field_count; i++) {
        if (!is_unit_break(&t->fields[i])) {
            flatten(l, t->fields[i].type, offset + layout->offsets[i], out);
        }
    }
}
```

Its call of `target_type` gives `c_long` and `c_wchar` their width on the target, as the next section shows. Chapter 18 computes the offsets that `flatten` reads and classifies the members for each convention. The test `tests/abi/abi_structs.anti` declares three unions. The table gives the register of each union as the first parameter, printed by `antic --dump-select` for the targets of each convention.

| Union | System V AMD64 | Windows x64 | AAPCS64, Apple ARM64, Windows ARM64 |
|---|---|---|---|
| `FF { a: f32, b: f32 }` | `xmm0` | `ecx` | `s0` |
| `Mix { f: f32, i: i32 }` | `edi` | `ecx` | `w0` |
| `Num { i: int, d: f64 }` | `rdi` | `rcx` | `x0` |

The two `f32` fields of `FF` form one member, so `FF` is an HFA of one member on ARM64 and of class SSE under System V. The union `Mix` holds a float and an integer at offset 0, which gives the class INTEGER under System V. On ARM64 it is no homogeneous aggregate and takes an integer register. Windows x64 passes all three by their size of 4 or 8 bytes. The test `program_abi_structs` passes each union to a C function compiled by clang and checks the results on the development Mac, a macos-arm64 host.

## Widths of C types

The widths of C's `long` and `wchar_t` depend on the target. AAPCS64 names the data model LP64, with a 64-bit `long`, and the Windows-like model LLP64, with a 32-bit `long`[^7]. Its table of types by data model gives `wchar_t` as an unsigned word in LP64 and an unsigned halfword in LLP64[^7]. Apple clang 21.0.0 on the development Mac reports these predefined macros for the triples of the target table, with `clang -target <triple> -x c -dM -E - < /dev/null`.

| Target | `__SIZEOF_LONG__` | `__WCHAR_TYPE__` |
|---|---|---|
| linux-x86_64 | 8 | `int` |
| linux-arm64 | 8 | `unsigned int` |
| macos-x86_64 | 8 | `int` |
| macos-arm64 | 8 | `int` |
| windows-x86_64 | 4 | `unsigned short` |
| windows-arm64 | 4 | `unsigned short` |

The Anti types `c_long` and `c_ulong` have the width of `long`, 32 bits on Windows and 64 bits elsewhere. The type `c_wchar` has the width of `wchar_t`, 16 bits on Windows and 32 bits elsewhere. It also has the signedness of `wchar_t`, which is signed on linux-x86_64 and on both macOS targets and unsigned on the other three. The other C types of Anti, such as `c_int` for `i32`, have one width on all six targets.

The IR records `c_long` and `c_ulong` as the type `clong` and `c_wchar` as `cwchar`, so a library file holds the same bytes on every host. The back end gives both types their width on its target with the function `target_type`.

```c
/* The fixed-width IR type of c_long or c_wchar on the target, or type. */
static enum ir_type target_type(const struct layouts *l, enum ir_type type)
{
    bool windows = target_info(l->target)->os == OS_WINDOWS;

    if (type == IR_CLONG) {
        return windows ? IR_I32 : IR_I64;
    }
    if (type == IR_CWCHAR) {
        return windows ? IR_I16 : IR_I32;
    }
    return type;
}
```

Lowering writes every operation on `c_wchar` in its unsigned form: `zext`, `udiv`, `urem`, `shr_u`, the unsigned comparisons, `uitof` and `ftoui`. Before `target_type` replaces the type, `layout_resolve` calls `wchar_signedness`, which picks the signed form on a target with a signed `wchar_t`.

```c
/* Whether wchar_t is signed on the target, as clang defines it. It is int
   on Linux x86_64 and macOS, unsigned int on Linux ARM64 and unsigned
   short on Windows. */
static bool wchar_signed(const struct layouts *l)
{
    const struct target_info *t = target_info(l->target);

    return t->os == OS_MACOS ||
           (t->os == OS_LINUX && t->arch == ARCH_X86_64);
}

/* The signed form of an operation that lowering writes unsigned. */
static enum ir_op signed_op(enum ir_op op)
{
    switch (op) {
    case IR_UDIV: return IR_SDIV;
    case IR_UREM: return IR_SREM;
    case IR_SHR_U: return IR_SHR_S;
    case IR_ULT: return IR_SLT;
    case IR_ULE: return IR_SLE;
    case IR_UGT: return IR_SGT;
    case IR_UGE: return IR_SGE;
    case IR_ZEXT: return IR_SEXT;
    case IR_UITOF: return IR_SITOF;
    case IR_FTOUI: return IR_FTOSI;
    default: return op;
    }
}

/* DESIGN: c_wchar has the signedness of wchar_t on the target. Lowering
   writes the unsigned form of every operation on it, and the back end
   turns that into the signed form where wchar_t is signed. */
static void wchar_signedness(const struct layouts *l, struct ir_inst *inst)
{
    bool on_wchar = inst->type == IR_CWCHAR || inst->a.type == IR_CWCHAR;

    if (on_wchar && wchar_signed(l)) {
        inst->op = signed_op(inst->op);
    }
}
```

The program test `abi_wchar` gets the value `0x80000001` as a `wchar_t` from C. It converts the value to `i64` and to `i32` and compares it with 1, and C does the same in `tests/abi/wchar.c`. On macos-arm64 both sides print -2147483647 for each conversion and 1 for the comparison.

A literal or constant of these types must fit the narrower width, so it has one value on every target. For the statement `let x: c_long = 4294967296;` the compiler reports that `4294967296` does not fit `c_long` on every target. The ABI probe `tests/abi/probe.anti` holds `c_long` and `c_wchar` in its struct `Wide`. The test `abi_probe` compares the layouts that the probe prints with those of `tests/abi/probe.c` compiled by the C compiler of the host.

## Symbol names

A C compiler writes the symbol of a C function in a form that each object format fixes. Apple names the symbol of a C function after the function with a `_` in front[^11]. Microsoft decorates C functions in 64-bit code only under the `__vectorcall` convention[^12]. Apple clang 21.0.0 on the development Mac compiled `int scale(int x)` for five of the targets. The symbol is `_scale` in the Mach-O object and `scale` in the ELF and COFF objects.

Chapter 16 writes the symbol of an `extern fn` in the C form of its target, so the call finds the C definition. On macOS the symbol of `extern fn puts` is `_puts`, and on Linux and Windows it is `puts`. An `export fn` gets the C form of its own name, so C code calls it by that name. Every remaining Anti function has the mangled form of chapter 9, which the function `mangle` derives from the object format in the target table. The function `push` of the module `com.example.geometry.vec` is `com.example.geometry.vec.push` on ELF, `_com.example.geometry.vec.push` on Mach-O and `_A3com7example8geometry3vec_push` on COFF.

## Position-independent code

Code that refers to other code and data by their distance from the current instruction runs at any load address. Such code is position-independent, and an executable made of it is a PIE. The operating system can then load it at a different address on every start.

| System | Rule |
|---|---|
| macOS on ARM64 | The kernel loads an ARM64 executable only with the flag `MH_PIE`[^13] |
| macOS on x86_64 | `ld` makes an executable PIE by default for Mac OS X 10.7 and later[^14] |
| Ubuntu | Since 17.10 the packages are built as PIE on all architectures by default[^15] |
| Windows | The linker option `/DYNAMICBASE`, address space layout randomization, is on by default and cannot be disabled on ARM64[^16] |

The executable that antic linked for the test `program_return42` on the development Mac carries the flag `PIE`. The tool `otool -hv` prints its flags as `NOUNDEFS DYLDLINK TWOLEVEL PIE`. The compiler antic therefore generates position-independent code on all six targets. An x86_64 instruction reaches a global through an offset from `rip`, and an ARM64 instruction through `adrp` and a page offset. Chapter 16 writes the relocations of each format.

## Entry point and C library

A process starts at an address that its executable names. On every target that address belongs to the C runtime, which calls the C function `main` in `rt/start.c`. That function calls `anti_rt_init`, converts the arguments and the environment, and calls the Anti `main`, as chapter 2 specifies. The compiler antic never writes an entry point itself.

The C `main` reaches the Anti `main` through the global symbol `anti.rt.main`. The compiler defines it as a second name of the main module's `main`. In the symbol forms of the formats it is `anti.rt.main` on ELF, `_anti.rt.main` on Mach-O and `_A4anti2rt_main` on COFF. A name with a dot is no C identifier, so `rt/start.c` declares it with an assembler label.

```c
/* A symbol with a dot is not a C identifier, so the declaration names it
   with an assembler label. Mach-O adds '_' to C symbols, ELF does not.
   The COFF symbol is an identifier, and MSVC has no assembler labels. */
#if defined(_WIN32)
extern int64_t _A4anti2rt_main(struct anti_slice args, struct anti_slice env);
#define anti_main _A4anti2rt_main
#else
#if defined(__APPLE__)
#define ANTI_ENTRY_SYMBOL "_anti.rt.main"
#else
#define ANTI_ENTRY_SYMBOL "anti.rt.main"
#endif
extern int64_t anti_main(struct anti_slice args, struct anti_slice env)
    __asm__(ANTI_ENTRY_SYMBOL);
#endif
```

The file `rt/start.c` belongs to the runtime library `anti_rt`. The driver links it as `libanti_rt.a` on Linux and macOS and as `anti_rt.lib` on Windows, the names that the static libraries of each toolchain carry.

### Linux

The glibc start file `crt1.o` holds the ELF entry point `_start`[^17]. On x86_64 and on ARM64, `_start` calls `__libc_start_main`[^17], which calls `main` and passes its return value to `exit`[^18]. A dynamically linked executable names its program interpreter, the dynamic linker. On x86_64 Linux it is `/lib64/ld-linux-x86-64.so.2`[^19], and GCC's configuration for ARM64 Linux names `/lib/ld-linux-aarch64.so.1`[^20]. The C library itself is `libc`. These facts hold for a link with the platform linker. With lld, chapter 16 links a Linux program statically against musl, whose start file `rcrt1.o` calls `main` in the same way, and the executable names no interpreter.

### macOS

The source of Apple's linker ld64 uses `_main` as the default entry name for recent OS versions and records it with `LC_MAIN`[^21]. The executable of the test `program_return42`, which ld64.lld links, has the load command `LC_MAIN` with the entry offset 4244. At address `0x100001094`, offset 4244 in its text segment, `nm` shows the symbol `_main`. The standard C functions live in libSystem, the main system library, and `-lSystem` links it[^22]. The test executable loads `/usr/lib/libSystem.B.dylib`.

### Windows

For a console application the default entry point is `mainCRTStartup`, which calls `main`[^23]. The C runtime comes in a static and a dynamic form. A link without a compiler option that selects the runtime uses the static libraries `libcmt.lib`, `libvcruntime.lib` and `libucrt.lib`[^24]. The dynamic form uses `msvcrt.lib`, `vcruntime.lib` and `ucrt.lib`, which import `vcruntime<version>.dll` and `ucrtbase.dll`[^24]. Chapter 16 builds the linker command lines of all six targets from these facts.

## Tests

The unit test `test_target` checks the operating system, the processor, the object format, the convention, the triple and the file suffixes in the target table. It also checks the mangled symbol forms of chapter 9 for each object format, module paths with several segments included. In `tests/unit/test_layout.c`, the test `c_types` checks the widths of `c_long` and `c_wchar` on windows-x86_64 and linux-arm64. The test `union_members` checks that `FF` has one member and `Mix` two. The test `print_targets` pins the matrix at the start of this chapter. The build has zero warnings, and every `ctest` test passes.

## Next

[Chapter 12, Instruction selection]({{% relref "/programming/writing-a-compiler/12-instruction-selection" %}}), turns IR instructions into target instructions on virtual registers. The back end first lays out the types for its target and folds the symbolic values. Its selector is table-driven, shared in structure by both back ends, with separate patterns per architecture.

## References

[^1]: Linux man-pages project, *elf(5)*, man-pages 6.19, section DESCRIPTION, https://man7.org/linux/man-pages/man5/elf.5.html

[^2]: Apple, *Mach-O Programming Topics*, Introduction, https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/MachOTopics/0-Introduction/introduction.html

[^3]: Apple, xnu source, `EXTERNAL_HEADERS/mach-o/loader.h`, definitions of `MH_MAGIC_64`, `LC_MAIN` and `entry_point_command`, https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h

[^4]: Microsoft, *PE Format*, sections "File Headers", "COFF File Header (Object and Image)" and "Symbol Name Representation", https://learn.microsoft.com/en-us/windows/win32/debug/pe-format

[^5]: H.J. Lu and others, *System V Application Binary Interface, AMD64 Architecture Processor Supplement*, version 1.0, March 12, 2025, sections 3.2.1, 3.2.2, 3.2.3 and 3.5.7, https://gitlab.com/x86-psABIs/x86-64-ABI

[^6]: Microsoft, *x64 calling convention*. Sections "Calling convention defaults", "Parameter passing", "Varargs", "Return values" and "Caller/callee saved registers", https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170

[^7]: Arm Limited, *Procedure Call Standard for the Arm 64-bit Architecture (AArch64)*, release 2025Q4, sections 5.10, 5.10.5, 6.1.1, 6.1.2, 6.4.5.2, 6.8.2, 6.9, 10.1.1 and 10.1.2, https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst

[^8]: Apple, *OS X ABI Function Call Guide*, "x86-64 Function Calling Conventions", https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/LowLevelABI/140-x86-64_Function_Calling_Conventions/x86_64.html

[^9]: Apple, *Writing ARM64 code for Apple platforms*, the sections on CPU registers, arguments, variadic functions and data types, https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms

[^10]: Microsoft, *Overview of ARM64 ABI conventions*, sections "Integer registers", "Parameter passing", "Stack" and "Addendum: Variadic functions", https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions?view=msvc-170

[^11]: Apple, *Mach-O Programming Topics*, "Executing Mach-O Files", section "Searching for Symbols", https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/MachOTopics/1-Articles/executing_files.html

[^12]: Microsoft, *Decorated names*, section "Format of a C decorated name", https://learn.microsoft.com/en-us/cpp/build/reference/decorated-names?view=msvc-170

[^13]: Apple, xnu source, `bsd/kern/mach_loader.c`, function `pie_required`, https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/mach_loader.c

[^14]: Apple, *ld(1)*, option `-pie`, https://keith.github.io/xcode-man-pages/ld.1.html

[^15]: Ubuntu, *Security/Features*, section "Built as PIE", https://wiki.ubuntu.com/Security/Features

[^16]: Microsoft, */DYNAMICBASE (Use address space layout randomization)*, section "Remarks", https://learn.microsoft.com/en-us/cpp/build/reference/dynamicbase-use-address-space-layout-randomization?view=msvc-170

[^17]: GNU C Library, source files `csu/Makefile`, `sysdeps/x86_64/start.S` and `sysdeps/aarch64/start.S`, https://sourceware.org/git/?p=glibc.git

[^18]: Linux Foundation, *Linux Standard Base Core Specification 5.0*, `__libc_start_main`, https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/baselib---libc-start-main-.html

[^19]: H.J. Lu and others, *System V ABI, AMD64 Architecture Processor Supplement*, version 1.0, `dl.tex`, section "Program Interpreter", https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/master/x86-64-ABI/dl.tex

[^20]: GNU Compiler Collection, source file `gcc/config/aarch64/aarch64-linux.h`, definition of `GLIBC_DYNAMIC_LINKER`, https://github.com/gcc-mirror/gcc/blob/master/gcc/config/aarch64/aarch64-linux.h

[^21]: Apple, ld64 source, `src/ld/Options.cpp`, the default entry name `_main` with `LC_MAIN`, https://github.com/apple-oss-distributions/ld64/blob/main/src/ld/Options.cpp

[^22]: Apple, *intro(3)*, sections DESCRIPTION and FILES, https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/intro.3.html

[^23]: Microsoft, */ENTRY (Entry-point symbol)*, section "Remarks", https://learn.microsoft.com/en-us/cpp/build/reference/entry-entry-point-symbol?view=msvc-170

[^24]: Microsoft, *C runtime (CRT) and C++ standard library (STL) .lib files*. Section "C runtime (CRT) .lib files", https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features?view=msvc-170
