# This file is the output of nixja-scan-deps
{pkgs}: let
	fs = pkgs.lib.fileset;

	mkItem = {
		name,
		dependencies,
		command,
	}:
		pkgs.stdenv.mkDerivation {
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
				            mkdir -p $out

				cp src/main.o $out/src/main.o

			'';
		};
in
	mkItem {
		name = "src_main_o";
		dependencies = [
			./src/main.c

			./src/world.h

			./src/hello.h
		];
		command = "clang -c src/main.c -o src/main.o";
	}
