#pragma once

#include <string>

const std::string nixja_gen_template = R"(
# nix-build --show-trace -E 'import ./hello/nixja.nix { pkgs = import <nixpkgs> {}; }'
{pkgs}: 
let
    fs = pkgs.lib.fileset;

    mkItem = {name, output, dependencies, command}: pkgs.stdenvNoCC.mkDerivation {
        inherit name;

        src = fs.toSource {
            root = ./.;

            # Explicit and Implicit dependencies
            fileset = fs.unions dependencies;
        };

        nativeBuildInputs = [pkgs.gcc];
        dontFixup = true;

        __contentAddressed = true;

        buildPhase = ''
            echo ${command}
            ${command}
        '';

        installPhase = ''
            cp ${output} $out
        '';
    };

{% for item in depItems  %}
    {{item.attrKey}} = mkItem {
        name = "{{item.attrKey}}";
        output = "{{item.output}}";
        dependencies = [
            {% for dependency in item.dependencies %}
            {{ dependency.source_path }}
            {% endfor %}
        ];
        command = "{{ item.command }}";
    };
{% endfor %}


{% for item in standardItems  %}
    {{item.attrKey}} = mkItem {
        name = "{{item.attrKey}}";
        output = "{{item.output}}";
        dependencies = [
            {% for dependency in item.dependencies %}
            {{ dependency.source_path }}
            {% endfor %}
        ];
        command = "{{ item.command }}";
    };
{% endfor %}

in
    {{ target }}
)";
