{
	description = "Nixja C++ development environment";

	inputs = {
		self.submodules = true;
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

		pkgsFor = system:
			import nixpkgs {
				inherit system;
				overlays = [self.overlays.default];
			};

		forAllSystems = f:
			nixpkgs.lib.genAttrs systems (system:
				f (pkgsFor system));
	in {
		overlays.default = final: prev: {
			# C++ generator
			nixja-gen =
				final.stdenv.mkDerivation {
					name = "nixja-gen";

					src = self;

					nativeBuildInputs = [
						final.cmake
						final.makeWrapper
					];

					buildInputs = [
						final.inja
						final.nlohmann_json
					];

					cmakeBuildType = "Release";
					cmakeBuildFlags = [
						"--target"
						"nixja-gen"
					];

					installPhase = ''
						runHook preInstall
						install -Dm755 src/nixja-gen/nixja-gen "$out/bin/nixja-gen"
						runHook postInstall
					'';

					# postFixup = ''
					# 	wrapProgram $out/bin/nixja-gen \
					# 		--prefix PATH : ${final.lib.makeBinPath [
					# 			final.ninja
					# 			final.clang-tools
					# 		]}
					# '';
				};

			# C++ driver
			nixja =
				final.stdenv.mkDerivation {
					name = "nixja";

					src = self;

					nativeBuildInputs = [
						final.cmake
						final.makeWrapper
					];

					buildInputs = [
						final.inja
						final.nlohmann_json
					];

					cmakeBuildType = "Release";
					cmakeBuildFlags = [
						"--target"
						"nixja"
					];

					installPhase = ''
						runHook preInstall
						install -Dm755 src/nixja/nixja "$out/bin/nixja"
						runHook postInstall
					'';

					postPatch = ''
						substituteInPlace src/nixja/nixja.hpp \
						    --replace-fail "@nixjaGen@" "${final.nixja-gen}"
					'';

					# postFixup = ''
					# 	wrapProgram $out/bin/nixja \
					# 		--prefix PATH : ${final.lib.makeBinPath [
					# 			final.nix
					# 			final.nixja-gen
					# 		]}
					# '';
				};
		};

		pkgs = pkgsFor;

		packages =
			forAllSystems (pkgs: {
					default = pkgs.nixja;
					nixja = pkgs.nixja;
					nixja-gen = pkgs.nixja-gen;
				});

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

		devShells =
			forAllSystems (pkgs: {
					default =
						pkgs.mkShell {
							packages = with pkgs; [
								# C++ development
								clang
								clang-tools
								cmake
								ninja
								gcc
								inja
								nlohmann_json

								# Nix
								nix

								# Python development
								python313
								python313Packages.jinja2
								uv

								# Utilities
								bear
								gnumake
								jq
							];

							shellHook = ''
								export UV_PYTHON_DOWNLOADS=never
								export UV_PYTHON="${pkgs.python313}/bin/python3"
							'';
						};
				});
	};
}
