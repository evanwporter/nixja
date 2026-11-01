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
					./nixja-scan-deps -i TODO -o ${outputs_str} -- ${command} > derivation.nix
					cat derivation.nix
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

	main_o =
		mkDepItem {
			name = "main_o";
			outputs = [
				"main.o"
			];
			command = "cc -MD -MQ main.o -MF main.o.d -o main.o -c src/main.c";
			# TODO: Handle dependencies and implicit dependencies
		};

	hello_o =
		mkDepItem {
			name = "hello_o";
			outputs = [
				"hello.o"
			];
			command = "cc -MD -MQ hello.o -MF hello.o.d -o hello.o -c src/hello.c";
			# TODO: Handle dependencies and implicit dependencies
		};

	world_o =
		mkDepItem {
			name = "world_o";
			outputs = [
				"world.o"
			];
			command = "cc -MD -MQ world.o -MF world.o.d -o world.o -c src/world.c";
			# TODO: Handle dependencies and implicit dependencies
		};

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
