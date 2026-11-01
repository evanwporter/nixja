{pkgs}: let
	nixjaGen = builtins.storePath "/nix/store/33ji5p4cb377wr1has68bggnq2la4hsk-nixja-gen";
	hello_drv =
		builtins.outputOf
		(pkgs.stdenvNoCC.mkDerivation
			{
				name = "hello.drv";

				src = ./.;

				outputHashMode = "text";
				outputHashAlgo = "sha256";

				buildInputs = [pkgs.nix];

				# TODO: figure out what I need
				nativeBuildInputs = with pkgs;
					[
						python3
						python314Packages.jinja2
						clang-tools
						gcc
						ninja
					]
					++ [nixjaGen];

				# This is necessary so we can invoke `nix-instantiate` below
				requiredSystemFeatures = ["recursive-nix"];

				buildPhase = ''
					ninja -t compdb c_COMPILER > compile_commands.json
					${nixjaGen}/bin/nixja-gen hello > derivation.nix
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

	hello =
		pkgs.runCommand "hello" {
			__contentAddressed = true;
		} ''
			cp ${hello_drv} "$out"
		'';
in
	hello
