{
	description = "Python development environment";

	inputs = {
		nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
	};

	outputs = {
		self,
		nixpkgs,
	}: let
		systems = [
			"x86_64-linux"
			"aarch64-linux"
			"aarch64-darwin"
			"x86_64-darwin"
		];

		# Construct a package set with our overlay.
		pkgsFor = system:
			import nixpkgs {
				inherit system;
				overlays = [self.overlays.default];
			};

		# Apply a function to every supported system.
		forAllSystems = f:
			nixpkgs.lib.genAttrs systems (system:
				f (pkgsFor system));
	in {
		overlays.default = final: prev: {
			nixja-gen =
				final.python313Packages.buildPythonApplication {
					name = "nixja-gen";

					src = self;
					pyproject = true;

					build-system = [
						final.python313Packages.setuptools
					];

					dependencies = [
						final.python313Packages.jinja2
					];
				};

			nixja =
				final.python313Packages.buildPythonApplication {
					name = "nixja";

					src = self;
					pyproject = true;

					build-system = [
						final.python313Packages.setuptools
					];

					postPatch = ''
						substituteInPlace src/nixja/nixja.nix.j2 \
						    --replace-fail "python ../src/nixja/nixja-gen.py" \
						    "${final.nixja-gen}/bin/nixja-gen"
					'';

					dependencies = [
						final.python313Packages.jinja2
					];
				};
		};

		# Expose customized package sets.
		pkgs = pkgsFor;

		# Packages.
		packages =
			forAllSystems (pkgs: {
					default = pkgs.nixja;
					nixja = pkgs.nixja;
					nixja-gen = pkgs.nixja-gen;
				});

		# Applications.
		apps =
			forAllSystems (pkgs: {
					default = {
						type = "app";
						program = "${pkgs.nixja}/bin/nixja";
					};

					nixja = {
						type = "app";
						program = "${pkgs.nixja}/bin/nixja";
					};

					nixja-gen = {
						type = "app";
						program = "${pkgs.nixja-gen}/bin/nixja-gen";
					};
				});

		# Development shells.
		devShells =
			forAllSystems (pkgs: {
					default =
						pkgs.mkShell {
							packages = with pkgs; [
								python313
								python313Packages.jinja2
								uv
								clang-tools
								ninja
							];

							shellHook = ''
								export UV_PYTHON_DOWNLOADS=never
								export UV_PYTHON="${pkgs.python313}/bin/python3"
							'';
						};
				});
	};
}
