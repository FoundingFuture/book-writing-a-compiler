# Chapter 5 notes

Choices made while writing chapter 5, The parser and the syntax tree. They describe the inside of the
compiler. `docs/decisions.md` holds what a reader of the language or a user of
the tools can observe.

- Chapter 5 parser: recursive descent with precedence climbing and panic-mode recovery. Inside a block it resynchronises after a semicolon, or at a closing brace or a statement keyword. At the top level it resynchronises at an item keyword outside braces. After an error in an import it skips the rest of the import's line up to and with its `;`. A keyword in the path then starts no item.
- `--dump-ast` names each node after its grammar rule. The test `dump_ast_scale` pins the tree listing of chapter 1.
