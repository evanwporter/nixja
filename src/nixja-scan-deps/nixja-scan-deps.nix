pkgs: let
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
in
	mkItem {
		name = "ouput_o";
		output = "output.o";
		dependencies = [];
		command = "";
	}
