# Chapter 16 notes

Choices made while writing chapter 16, Assembly emission per operating system. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 16 removes the direct path of chapter 3. Every program goes through lowering, the optimizer, selection, register allocation and the emitter in `src/emit.c`.
- One assembly file holds the whole program. Anti functions are local symbols. The main module's `main` gets the global second name `anti.rt.main` through `.globl` and `.set`, in the symbol form of the object format. A program without `main` stops with `the program has no function` and the name, unless `-S` writes only the assembly.
- Emitted names: Anti functions use `mangle`. C functions and runtime helpers such as `__chkstk` get a leading `_` on Mach-O only. Block labels are the function symbol with `.b` and the block number, prefixed with `L` on Mach-O and `.L` on ELF and COFF, which keeps them out of the symbol table. The dumps keep IR names and `b0` labels.
- Directives: `.build_version macos, 11, 0` on Mach-O, `.text`, `.p2align 2` before each ARM64 function and none on x86_64. ELF files end with `.section .note.GNU-stack,"",@progbits`. COFF files need no further directives. Chapter 19 adds a read-only data section after the functions.
- ARM64 page addresses print `sym@PAGE` and `sym@PAGEOFF` on Mach-O and `:lo12:sym` elsewhere.
- `rt/start.c` declares the Windows entry as the C identifier `_A4anti2rt_main`, because MSVC has no assembler labels. A unit test checks that `rt/start.c` spells the entry in the form `mangle` writes for each object format.
- The asm tests assemble the output of `antic -S`. The emit tests pin the assembly files of `main.anti` for six targets and of `letters.anti` for two.
- antic writes Windows unwind data. The prologue and the epilogues of a function with a frame on a COFF target carry `.seh_` directives, and `.seh_proc` and `.seh_endproc` bracket the function. llvm-mc writes `.pdata` and `.xdata` from them. Chapter 16 describes it in its section on COFF specifics.
- The x64 unwind data names no frame register, and the Windows epilogue frees the frame with `add`. The unwinder needs a frame register only when the stack pointer moves in the body, and `add rsp` is an epilogue form it recognises. If Anti ever gets dynamic stack allocation, the Windows x64 prologue must set a frame register and the epilogue must switch to `lea`.
- With unwind data the ARM64 prologue stores the frame record, allocates the save area, saves into it, sets `x29` with `add` and ends. The rest of the frame follows in the body. The epilogue frees the body part, restores the saves, frees the save area and loads the frame record between `.seh_startepilogue` and `.seh_endepilogue`, with a `.seh_nop` for each instruction of a load of `x16`. The order matches the canonical forms that the Windows unwinder decodes, and `llvm-readobj` decoding it is the static test. The runtime test is an exception that unwinds through an Anti frame on the Windows VM.
