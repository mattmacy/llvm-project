# Memory-bounded clangd patches (branch `clangd-patched`)

This branch carries 5 patches on top of LLVM 19.1.7 release/19.x to keep clangd's resident memory bounded on large C++ codebases (~150 K files, multi-GB per-TU header sets). On a representative workload, indexing at `-j=10` without these patches peaks around **40 GB RSS** in burst windows; with all three knobs tuned, peak drops to ~**14-18 GB**.

## New clangd flags

| Flag | What it does | Default |
|---|---|---|
| `--background-index-memory-limit=<bytes>[K\|M\|G]` | Caps merged background-index resident size by evicting the LRU file's symbol/ref/relation slabs from `FileSymbols`. Disk shards remain authoritative; the next time the indexer revisits an evicted file it re-populates the in-memory snapshot via the normal `update()` path. | `0` (=unlimited, upstream behavior) |
| `--max-concurrent-preamble-builds=<N>` | Bounds in-flight preamble parses via the existing `PreambleThrottler` interface. Each preamble holds a full Clang AST + temp parse buffers (~3-5 GB unprunned on heavy TUs); unbounded at `-j=10` peaks ~40 GB. Capping at N admits `N` preambles concurrently, queues the rest. | `0` (=unbounded, upstream behavior) |
| `--preamble-ast-pruning=<off\|conservative\|aggressive>` | Prunes preamble PCH to AST decls reachable from the TU body's name-lookup roots. `conservative` is query-result-equivalent to `off` on the existing test corpus; `aggressive` additionally drops implicit template instantiations and macro definitions not name-reachable from kept decls (can regress hover/signature on highly-templated code). | `off` |

`--help` on the patched binary prints the full description text for each.

## Tuning matrix (`-j=10`)

| RSS budget | Recommended |
|---|---|
| 16 GB | `--background-index-memory-limit=8G --max-concurrent-preamble-builds=2 --preamble-ast-pruning=aggressive` |
| 24 GB | `--background-index-memory-limit=12G --max-concurrent-preamble-builds=3 --preamble-ast-pruning=conservative` |
| 32 GB | `--background-index-memory-limit=16G --max-concurrent-preamble-builds=4 --preamble-ast-pruning=conservative` |
| 48 GB+ | `--background-index-memory-limit=20G --max-concurrent-preamble-builds=6 --preamble-ast-pruning=off` (let it spread) |

Aggressive tier is currently stub-only (flag wired, reachability pass deferred to v0.2 — PreprocessingRecord-based macro filter is in flight). Until v0.2 lands, use `conservative` and budget for the upper end of each range above.

## Operator levers beyond these flags

To stay under target RSS even with the patched binary, also do:

1. **Set `--pch-storage=disk`.** Default `memory` keeps preamble PCHs in RAM; `disk` writes them to a temp file and mmaps. Cuts steady-state RSS by ~`N × 100-300 MB` per stale preamble. Mandatory on RSS-bounded hosts.
2. **`LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 MALLOC_CONF=dirty_decay_ms:0,muzzy_decay_ms:0`.** glibc malloc holds dirty pages indefinitely after free; jemalloc with aggressive page return halves observed RSS for the same allocation pattern.
3. **`--clang-tidy=false`.** Tidy diagnostics fire per parse and hold their own AST copy. Disable unless you need clang-tidy output.
4. **`--header-insertion=never`.** Disables include suggestions at completion time, dropping a per-completion AST traversal that allocates transient include-graph state.
5. **`--background-index-priority=background`.** Lowers indexer thread priority so it yields to interactive completion. No RSS impact directly; reduces the wall-clock duration of the burst window so `-j` × per-preamble peak overlaps less.
6. **Don't oversize `-j`.** Benefits saturate around `-j=10` on the workloads tested. Going higher just multiplies the preamble-pool peak with diminishing throughput. If 16 GB is your budget and you can't get aggressive yet, prefer `-j=6` over softer caps.
7. **Cap container RSS.** Even with all the knobs, glibc / driver allocations can occasionally spike. Run clangd under a cgroup with hard `memory.max` so a runaway parse OOM-kills only clangd, not the surrounding editor + indexing pipeline.

## Patch series

In linear order on this branch:

1. `717e83554` — `--background-index-memory-limit` + LRU eviction (4 unit tests)
2. `56bd9a91d` — `--max-concurrent-preamble-builds` via `BoundedPreambleThrottler`
3. `396690588` — PCH AST pruning Commit 1: flag plumbing + off-by-default no-op stub
4. `cb593fab4` — PCH AST pruning Commit 2: conservative reachability pass (seed + numbered closure steps 1-8 + type-memo)
5. `a9b3becb9` — PCH AST pruning Commit 2.1: closure rule deltas (FieldDecl init walk, full TemplateArgument referent table, NestedNameSpecifier chain, complete RecursiveASTVisitor enumeration, ADL customization-point seed; +5 mutation-pin tests)

41/41 tests pass: 9 PreamblePruningTest + 4 FileSymbols MemoryLimit + 28 FileSymbols/FileIndex regression.

## Building

```
cmake -G Ninja -S llvm -B build \
  -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_OPTIMIZED_TABLEGEN=ON
ninja -C build clangd ClangdTests
sudo install -m755 build/bin/clangd /usr/local/bin/clangd-patched
```

Prebuilt binary attached to GitHub Releases: https://github.com/mattmacy/llvm-project/releases.

---

# The LLVM Compiler Infrastructure

[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/llvm/llvm-project/badge)](https://securityscorecards.dev/viewer/?uri=github.com/llvm/llvm-project)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/8273/badge)](https://www.bestpractices.dev/projects/8273)
[![libc++](https://github.com/llvm/llvm-project/actions/workflows/libcxx-build-and-test.yaml/badge.svg?branch=main&event=schedule)](https://github.com/llvm/llvm-project/actions/workflows/libcxx-build-and-test.yaml?query=event%3Aschedule)

Welcome to the LLVM project!

This repository contains the source code for LLVM, a toolkit for the
construction of highly optimized compilers, optimizers, and run-time
environments.

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and convert them into
object files. Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer.

C-like languages use the [Clang](https://clang.llvm.org/) frontend. This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

## Getting the Source Code and Building LLVM

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm)
page for information on building and running LLVM.

For information on how to contribute to the LLVM project, please take a look at
the [Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting in touch

Join the [LLVM Discourse forums](https://discourse.llvm.org/), [Discord
chat](https://discord.gg/xS7Z362),
[LLVM Office Hours](https://llvm.org/docs/GettingInvolved.html#office-hours) or
[Regular sync-ups](https://llvm.org/docs/GettingInvolved.html#online-sync-ups).

The LLVM project has adopted a [code of conduct](https://llvm.org/docs/CodeOfConduct.html) for
participants to all modes of communication within the project.
