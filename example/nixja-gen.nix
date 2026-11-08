# This file is the output of nixja-gen
# nix-build --show-trace -E 'import ./hello/nixja.nix { pkgs = import <nixpkgs> {}; }'
{pkgs}: let
	fs = pkgs.lib.fileset;

	# Dependencies are just explicit and implicit declared source files
	# Nodes should be declared directly in the command
	# TODO: We also should cover the case when a node is an implicit dependency
	mkItem = {
		name,
		installPhase,
		dependencies,
		command,
	}:
		pkgs.stdenvNoCC.mkDerivation {
			inherit name;

			src =
				fs.toSource {
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

			inherit installPhase;
		};

	# The dynamically generated derivation for a single edge.
	# TODO: we need to bring back the dependencies for implicit dependencies,
	# they are currently being passed to the jinja file, but they are unused
	mkDepItem = {
		name,
		outputs,
		command,
	}:
		builtins.outputOf
		(pkgs.stdenvNoCC.mkDerivation {
				# This derivation's only output is the .drv generated at build time.
				# Nix reserves the .drv suffix for precisely this kind of output.
				name = "${name}.drv";

				src = ./.;

				buildPhase = let
					outputs_str = "${builtins.concatStringsSep " " outputs}";
				in ''
					export NIXJA_BASH="${pkgs.bash}/bin/bash"
					export NIXJA_PATH="${pkgs.coreutils}/bin:${pkgs.gcc}/bin"
					NIXJA_SCAN_DEP_DRV="$(./nixja-scan-deps -i TODO -o ${outputs_str} -- ${command})"
					echo $NIXJA_SCAN_DEP_DRV

				'';

				installPhase = ''
					# nix-instantiate prints the generated .drv path. copy its
					# contents so that $out itself is a valid .drv file.
					cp "$NIXJA_SCAN_DEP_DRV" "$out"
				'';

				__contentAddressed = true;
				outputHashMode = "text";
				outputHashAlgo = "sha256";

				requiredSystemFeatures = ["recursive-nix"];

				nativeBuildInputs = with pkgs; [
					clang-tools
					gcc
					nix
					ninja
				];

				dontFixup = true;
			}).outPath "out";

	world_o = builtins.outputOf (builtins.storePath "f0pxc6dqw7sin1fjisz652kvc1xjw4ma-world_o.drv").outPath "out";

	hello_o = builtins.outputOf (builtins.storePath "grpl1xslsfrk19j02468716xgy1vdqrx-hello_o.drv").outPath "out";

	main_o = builtins.outputOf (builtins.storePath "0szmd4gfmakksasyaplicc1dai57j3dy-main_o.drv").outPath "out";

	hello =
		mkItem {
			name = "hello";

			dependencies = [
			];
			command = "cc ${main_o}/main.o ${hello_o}/hello.o ${world_o}/world.o -o hello";
			installPhase = ''
				mkdir -p $out

				cp hello $out/hello

			'';
		};
in
	hello
