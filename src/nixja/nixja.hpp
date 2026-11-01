
// nix-build \
//   --extra-experimental-features 'ca-derivations dynamic-derivations' \
//   --show-trace \
//   -E 'import ./nixja-full.nix { pkgs = import <nixpkgs> {}; }'

#include <string>

const std::string nixja_template = R"NIXJA(
{pkgs}: let
	nixjaGen = builtins.storePath "@nixjaGen@";
	{{ target }}_drv =
		builtins.outputOf
		(pkgs.stdenvNoCC.mkDerivation
			{
				name = "{{ target }}.drv";

				src = ./.;

				outputHashMode = "text";
				outputHashAlgo = "sha256";

				buildInputs = [pkgs.nix];
                
                # TODO: figure out what I need
				nativeBuildInputs = with pkgs; [
					python3
					python314Packages.jinja2
					clang-tools
					gcc
					ninja
				] ++ [ nixjaGen ];

				# This is necessary so we can invoke `nix-instantiate` below
				requiredSystemFeatures = ["recursive-nix"];

				buildPhase = ''
					ninja -t compdb c_COMPILER > compile_commands.json
					./nixja-gen {{ target }} > derivation.nix
					# cat derivation.nix
				'';

				installPhase = ''
					# nix-instantiate prints the generated .drv path. copy its
					# contents so that $out itself is a valid .drv file.
					cp "$(
					  nix-instantiate \
					    --extra-experimental-features 'ca-derivations dynamic-derivations' \
					    derivation.nix \
					    --arg pkgs 'import ${pkgs.path} {}'
					)" "$out"
				'';
			}).outPath
		"out";

	{{target}} =
		pkgs.runCommand "{{ target }}" {
			__contentAddressed = true;
		} ''
			cp ${ {{target}}_drv } "$out"
		'';
in
	{{target}}
)NIXJA";
