# Chapter 4 notes

Choices made while writing chapter 4, The lexer. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- After a lexical error the lexer records a diagnostic, emits `TOKEN_ERROR` and continues. A doc comment block without `*/` reports `unterminated block comment` alone, even with text on its opener line. The driver prints every diagnostic and stops after the lexer.
- `--dump-tokens` prints `line:column`, the token group and the source text, the format of the listing in chapter 1. The test `dump_tokens_scale` pins that listing.
