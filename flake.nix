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
			# C++ dependency scanner
			nixja-scan-deps =
				final.stdenv.mkDerivation {
					name = "nixja-scan-deps";

					src = self;

					nativeBuildInputs = [
						final.cmake
						final.pkg-config
					];

					buildInputs = [
						final.fmt
						final.nlohmann_json
						final.nix
					];

					cmakeBuildType = "Release";
					cmakeBuildFlags = [
						"--target"
						"nixja-scan-deps"
					];

					installPhase = ''
						runHook preInstall
						install -Dm755 src/nixja-scan-deps/nixja-scan-deps "$out/bin/nixja-scan-deps"
						runHook postInstall
					'';
				};

			# C++ generator
			nixja-gen =
				final.stdenv.mkDerivation {
					name = "nixja-gen";

					src = self;

					nativeBuildInputs = [
						final.cmake
						final.makeWrapper
						final.pkg-config
					];

					buildInputs = [
						final.fmt
						final.nlohmann_json
						final.nix
					];

					cmakeBuildType = "Release";
					cmakeBuildFlags = [
						"--target"
						"nixja-gen"
					];

					cmakeFlags = [
						"-DnixjaScanDeps=${final.nixja-scan-deps}"
					];

					installPhase = ''
						runHook preInstall
						install -Dm755 src/nixja-gen/nixja-gen "$out/bin/nixja-gen"
						runHook postInstall
					'';

					postFixup = ''
						wrapProgram $out/bin/nixja-gen \
							--prefix PATH : ${final.lib.makeBinPath [
								final.ninja
								final.clang-tools
							]}
					'';
				};

			# C++ driver
			nixja =
				final.stdenv.mkDerivation {
					name = "nixja";

					src = self;

					nativeBuildInputs = [
						final.cmake
						final.makeWrapper
						final.pkg-config
					];

					buildInputs = [
						final.fmt
						final.nlohmann_json
						final.nix
					];

					cmakeBuildType = "Release";
					cmakeBuildFlags = [
						"--target"
						"nixja"
					];

					cmakeFlags = [
						"-DnixjaGen=${final.nixja-gen}"
					];

					installPhase = ''
						runHook preInstall
						install -Dm755 src/nixja/nixja "$out/bin/nixja"
						runHook postInstall
					'';

					postFixup = ''
						wrapProgram $out/bin/nixja \
							--prefix PATH : ${final.lib.makeBinPath [
								final.nix
								final.nixja-gen
							]}
					'';
				};
		};

		pkgs = pkgsFor;

		packages =
			forAllSystems (pkgs: {
					default = pkgs.nixja;
					nixja = pkgs.nixja;
					nixja-gen = pkgs.nixja-gen;
					nixja-scan-deps = pkgs.nixja-scan-deps;
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

					nixja-scan-deps = {
						type = "app";
						program = "${pkgs.nixja-scan-deps}/bin/nixja-scan-deps";
					};
				});

		devShells =
			forAllSystems (pkgs: let
					llvmPkgs = pkgs.llvmPackages_22;
				in {
					default =
						(pkgs.mkShell.override {
								stdenv = llvmPkgs.stdenv;
							}) {
							packages = with pkgs; [
								# C++ development
								llvmPkgs.clang-tools
								cmake
								ninja
								nlohmann_json
								pkg-config
								fmt
								ccache

								# Nix
								nix
								boost # https://github.com/NixOS/nix/issues/13782

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
