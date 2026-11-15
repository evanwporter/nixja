# Nixja

Drop in (not really) replacement for ninja.

The primary goal is to combine ninja + nix. Another goal is to keep it simple (see [this article](https://grugbrain.dev/))

If it wasn't obvious this is my WIP take on nix-ninja.

This is good for CI builds where the /nix/store/ is kept between runs.

The major downside, and why its not good for local builds is that every run we need scan every source file for dependencies. This necessary for purity.

## Todo

1. ~~Initial write in python using pyn~~

2. ~~Rewrite in C++ using ninja~~

3. Swapping out clang-scan-deps for the literal clang-scan-deps (ie: linking to llvm)

4. Providing a method to cache dependencies between runs. Probably by symlinking a dep file in the nix store to a local file
    - NOTE: doing this is impure
    - I could add a deps.json to `$out/misc/deps.json`. This would get linked to `./result`

5. Figure out how to garbage collect old artifacts from the store automatically

6. ~~Generate nix derivations directly instead of going through `nix-instantiate`. (Anything to reduce the overhead of using nix).~~

7. Optimize, optimize, optimize

8. Another goal of this project is to be minimal. Ideally we offload as much stuff to the NIX C++ API and libninja as possible.

9. Add tests

10. Perhaps switch over to using stable? Nix C-API (kinda likely -- switching from 2.34 -> 2.35 was a pain)

11. Perhaps rewrite in C99 using SAMU instead of NINJA (unlikely) (related goal #10)

12. CMAKE -> MESON

13. ~~Create DRV files instead of converting from JSON~~

14. ~~Create common library with common functions~~

15. Think about how to handle `$$(pkg-config xxx)` in ninja files
    - Perhaps its best to perform dep discovery w/i the outer derivation of nixja-scan-deps

16. Simplify (see goal #8)

17. Figure out a longer term solution to linking the three binaries to my target workspace directory for cmake builds

18. Visualize build tree [Nix#16823](https://github.com/NixOS/nix/issues/16823)

19. Test if I can integrate this into `meson`

20. Use the nix socket inside build sandboxes as described in nix-ninja

21. Define a way to pass outside env into the derivation

22. Find a way to batch scan dependencies so I can utilize a shared cache

23. Integrate this into `meson` & `cmake`

24. Add `-C` flag

25. Make `nixja-tree.py` script only link folders and drv files (also drv files should be pprinted json)

26. `nixja-tree.py` should have option to output graphviz like the ninja tool

27. Clean up multiple source directories being added to store

28. `nixja-tree.py` should remove current symlinks

29. Add clean option

30. Make sure we are only compiling stuff in the `target` closure

31. ~~Im pretty sure nixja-gen and nixja can be combine. I just need to think about what would be lost by doing that.~~

32. Have nixja-scan-deps write directly to the store again and return the store path to the drv.

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

TODO: Add tests

## TO THINK ABOUT

Idk what will happen if ninja writes the output files outside of the ./ directory
