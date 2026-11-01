# Design

Running the ninja command is the same as the nixja command.

Checks if the dep.nix file exists.

If it does then we use the file to generate dynamic derivations for each command that specifies a dep file.

If it doesn't then we full compile.

After compilation all of the generated dep files are collected, parsed and the dep file is updated to reflect this.

After running the dep.nix file is symlinked to the newly generated one in the nix store

## Idea

Basically if the src file (`*.c`) or any of the other input files change (`*.h`) we rebuild and rescan.

So the `src` consists of each file (source and header)

(1) is to compile it. Then we scan the dependencies and update it


# Scratch The Above Actually

On every build the entire project is scanned with `clang-scan-deps`.

Using the resulting dependency tree, independent derivations for each command are generated.

## Rationale

This project is primarily for CI builds. As long as the the /nix/store/ is cached between runs the entire project can be built incrementally (instead of a full rebuild as is typically done).

Furthermore its wholly reproducible, and it provides all the guarantees of the nix ecosystem, like immutability etc..

Each generator, is dependent on the root of the project. So when anything is changed in the project then every derivation needs to be rebuilt from scratch. This includes scanning the entire project directory and building a deps.nix file

A generator takes the outputted `.d` file and each component becomes an entry under `src` in the generated drv (`*.o.drv.drv`).

The generated result looks like

```jinja
{{ item.attrKey }} = pkgs.stdenvNoCC.mkDerivation {
    name = "{{ output }}";
    src =
        fs.toSource {
            root = ./.;
            fileset =
                fs.unions [
                {% for dependency in dependencies %}
                    {{ dependency }}
                {% endfor %}
                ];
        };
    nativeBuildInputs = [pkgs.gcc];
    dontFixup = true;
    buildPhase = ''
        # Echo the command like in ninja
        echo "{{ command }}"
        {{ command }}
    '';
    installPhase = ''
        cp {{ output }} $out
    '';
}
```

where `dependencies` is a list of each file the output depends on as given by `clang-scan-deps`.

Otherwise the command looks like:

```jinja
{{ item.attrKey }} = pkgs.stdenvNoCC.mkDerivation {
    name = "{{ item.output }}";

    src = fs.toSource {
        root = ./.;
        fileset = fs.unions [
            # Explicit and Implicit dependencies
            {% for dependency in item.dependencies %}
            ./src/{{ dependency }}
            {% endfor %}
        ];
    };

    nativeBuildInputs = [pkgs.gcc];
    dontFixup = true;

    buildPhase = ''
        {{ item.command }}
    '';

    installPhase = ''
        cp {{ item.output }} $out
    '';
};
```

A dependency can be either a file or a node.
