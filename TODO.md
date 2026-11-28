# TODO

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

9. ~~Add tests~~

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

19. ~~Test if I can integrate this into `meson`~~

20. Use the nix socket inside build sandboxes as described in nix-ninja

21. Define a way to pass outside env into the derivation

22. ~~Find a way to batch scan dependencies so I can utilize a shared cache~~

23. Integrate this into `meson` & `cmake`

24. ~~Add `-C` flag~~

25. ~~Make `nixja-tree.py` script only link folders and drv files (also drv files should be pprinted json)~~

26. `nixja-tree.py` should have option to output graphviz like the ninja tool

27. ~~Clean up multiple source directories being added to store~~

28. `nixja-tree.py` should remove current symlinks

29. Add clean option / python script to clean up (related goal #5)

30. ~~Make sure we are only compiling stuff in the `target` closure~~

31. ~~Im pretty sure nixja-gen and nixja can be combine. I just need to think about what would be lost by doing that.~~

32. ~~Have nixja-scan-deps write directly to the store again and return the store path to the drv.~~

33. Define a convention for how I define functions and var (ie: snake case or camel case)

34. Look at whether I can use the [ninja test script](./external/ninja/misc/output_test.py) to test my own stuff in integration

35. Think more seriously about how to handle the nuances of a shell command in NINJA
    - `libdash`
    - treesitter-bash

36. Integrate [NIX #16851](https://github.com/NixOS/nix/issues/16851) into nixja (assuming this gets solved)

37. Provide list of clear limitations for nixja (such as using clang-scan-deps which requires a clear build command)

38. ~~Look into how `bear` seperates the compile commands from all other commands.~~
    - Will just be using clang to detech compilation commands from other commands

39. ~~Patch ELF with `so` location in nix store~~

40. ~~For nixja-gen static deps; build files in a env with only the input objects (as opposed to the entire source root directory)~~

41. Figure out how to cache a dep list between builds
    - Such that the build remains pure
    - And a incorrect dependency list won't cause the build to fail

42. Create global reference of which files have been added to the store so I don't try and add a file twice (primarily in nixja-gen)
    - Maybe its valuable to pass this onto nixja-scan-deps?

43. Add browse tool (like `ninja -t browse`)

44. Look into how phony rules are parsed

45. Add support for `-f` argument to nixja

46. Link against bash treesitter and use it to parse rudimentary bash commands
    - `: & clang ... & :`
        - Where `:`, `clang ...`, `:` would get parsed as seperate commands by `parseCommand`

47. Tweak `ClangScanDeps.cpp` to pass options instead of passing args

48. Support ninja `dyndeps`

49. Add fortran tests (after #48)

50. Add basic C++ module test (after #48)

51. Strip `ClangScanDeps.cpp` some more of unused stuff
