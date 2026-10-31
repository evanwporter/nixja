# nix-build --show-trace -E 'import ./hello/nixja.nix { pkgs = import <nixpkgs> {}; }'
{pkgs}: let
	fs = pkgs.lib.fileset;

	mkItem = {
		name,
		output,
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

			installPhase = ''
				cp ${output} $out
			'';
		};

	main_o =
		mkItem {
			name = "main_o";
			output = "main.o";
			dependencies = [
				./src/main.c
				./src/hello.h
				./src/world.h
			];
			command = "gcc -MD -MQ main.o -MF main.o.d -o main.o -c src/main.c";
		};

	hello_o =
		mkItem {
			name = "hello_o";
			output = "hello.o";
			dependencies = [
				./src/hello.c
				./src/hello.h
			];
			command = "gcc -MD -MQ hello.o -MF hello.o.d -o hello.o -c src/hello.c";
		};

	world_o =
		mkItem {
			name = "world_o";
			output = "world.o";
			dependencies = [
				./src/world.c
				./src/world.h
			];
			command = "gcc -MD -MQ world.o -MF world.o.d -o world.o -c src/world.c";
		};

	hello =
		mkItem {
			name = "hello";
			output = "hello";
			dependencies = [
			];
			command = "gcc ${main_o} ${hello_o} ${world_o} -o hello";
		};
in
	hello
