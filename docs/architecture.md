## Scanner Derivation

Derivation: `<out>.drv.drv`

This is the derivation to generate the compilation derivation with all dependencies:

Derivation name: <out>.drv.drv

- Finds dependencies by running <command> -M internally
- Parses the depfile
- Generates a complete derivation with all discovered dependencies

Input: `$in` (the source file)

Command: nixja-scanner (internally runs gcc -MM, parses deps, generates .drv)

`-MM` returns the makefile without system headers. `-M` returns the file with headers.

Output: `<out>.drv` <- A derivation file with all header dependencies baked in!

outputHashMode: text <- This makes it a dynamic derivation

## Compilation Derivation

Derivation: `<out>.drv`

This IS the .drv file that was output by the Scanner Derivation above.

Derivation name: `<out>.drv` (from Scanner output)

Input: all include files + `$in`

Command: `<command>` (the actual compilation command)

Output: `$out` / `<out>`

### Restructure

We need to create the build directory under `$NIX_BUILD_TOP` with symlinks creating the build directory.

So given `${PROJECT_ROOT} / <relative path to dependency>` we replace `$PROJECT_ROOT` with `${NIX_BUILD_TOP}`

ie:

`${PROJECT_ROOT} / <path to dependency>` -> `$NIX_BUILD_TOP / <path to dependency>`

(this is a symlink)
