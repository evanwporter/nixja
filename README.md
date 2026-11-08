# Nixja

Drop in (not really) replacement for ninja.

The primary goal is to combine ninja + nix. Another goal is to keep it simple (see [this article](https://grugbrain.dev/))

If it wasn't obvious this is my WIP take on nix-ninja.

This is good for CI builds where the /nix/store/ is kept between runs.

The major downside, and why its not good for local builds is that every run we need scan every source file for dependencies. This necessary for purity.

## Milestones

1. ~~Initial write in python using pyn~~

2. ~~Rewrite in C++ using ninja~~

3. Swapping out clang-scan-deps for the literal clang-scan-deps (ie: linking to llvm)

4. Providing a method to cache dependencies between runs. Probably by symlinking a file in the nix store to a local file

5. Figure out how to garbage collect old artifacts from the store automatically

6. Generate nix derivations directly instead of going through `nix-instantiate`. (Anything to reduce the overhead of using nix).

7. Optimize, optimize, optimize

8. Another goal of this project is to be minimal. Ideally we offload as much stuff to the NIX C++ API and libninja as possible.

## Rationale

I've never used nix-ninja, but I assume its excellent. I did use the design docs from nix-ninja when designing this.

Unlike nix-ninja, this is built in C++. There are two reasons for this choice:

(1) Idk a lick of rust. I would've (a) had to learn it (not fun) and even then development would be super slow or (b) would've had to use AI to generate it from start to finish (also not fun). There's enough AI slop rust projects out there and I didn't want nixja to be one of those.

(2) Eventually I want to interface directly with llvm (instead of going through `clang-scan-deps`) to parse files and discover dependencies. This looks like static linking against llvm (somehow) and modifying clang-scan-deps directly to fit my projects. Then I'll probably due a hefty strip of all the unused symbols. Idk how this would be done in rust easily.

(3) I <3 C++.

## Usage

(1) Build it

(1.5) Stage all files

(2) Navigate to `example/`

(3) Run `./nixja hello`

(4) Profit

# Release 

Add tests

## 0.2.0

This release will generate drv files directly and add them to the store with nix store add

## 0.3.0

This release will replace nix store add and interface with the C++ nix api directly.


## TO THINK ABOUT

Idk what will happen if ninja writes the output files outside of the ./ directory
