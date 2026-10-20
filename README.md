# Slang

A compiled language with Go-like simplicity, C#-style namespaces and
`var` type inference, methods inside structs, `T!` error types instead of
`if err != nil`, and built-in `async`/`await`. Backend: LLVM.

See [DESIGN.md](DESIGN.md) for the language design.

## Layout

- `compiler/` — the `slang` compiler (Rust + inkwell/LLVM 18)
- `runtime/` — runtime library in C (allocation, strings, slices, maps, tasks)
- `examples/` — sample `.sl` programs with expected output
- `scripts/setup.sh` — installs build dependencies on Ubuntu

## Build

```
./scripts/setup.sh      # once
cargo build --release
./target/release/slang parse examples/hello.sl
```
