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
			corePackages = with final; [
				bash
				coreutils
				findutils
				which
				stdenv.cc
				clang-tools
			];

			corePath = final.lib.makeBinPath final.corePackages;

			# C++ dependency scanner
			nixja-scan-deps =
				final.stdenv.mkDerivation {
					name = "nixja-scan-deps";

					src = self;

					nativeBuildInputs =
						final.corePackages
						++ [
							final.cmake
							final.makeWrapper
							final.pkg-config
						];

					buildInputs = [
						final.argparse
						final.fmt
						final.spdlog
						final.libblake3
						final.nlohmann_json
						final.nix
						final.llvmPackages_22.llvm
						final.llvmPackages_22.clang
						final.llvmPackages_22.clang-unwrapped
					];

					installPhase = ''
            						runHook preInstall
            						install -Dm755 src/nixja-scan-deps/nixja-scan-deps "$out/bin/nixja-scan-deps"
            						runHook postInstall
          '';

					postFixup = ''
            						wrapProgram $out/bin/nixja-scan-deps \
            							--prefix PATH : ${final.corePath}
          '';
				};

			# C++ driver
			nixja =
				final.stdenv.mkDerivation {
					name = "nixja";

					src = self;

					nativeBuildInputs =
						final.corePackages
						++ [
							final.cmake
							final.makeWrapper
							final.pkg-config
						];

					buildInputs = [
						final.argparse
						final.fmt
						final.spdlog
						final.libblake3
						final.nlohmann_json
						final.nix
						final.llvmPackages_22.llvm
						final.llvmPackages_22.clang
						final.llvmPackages_22.clang-unwrapped
					];

					cmakeBuildFlags = [
						"--target"
						"nixja"
					];

					cmakeFlags = [
						"-DnixjaScanDeps=${final.nixja-scan-deps}/bin/nixja-scan-deps"
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
								final.nixja-scan-deps
							]}:${final.corePath}
          '';
				};
		};

		pkgs = pkgsFor;

		packages =
			forAllSystems (pkgs: {
					default = pkgs.nixja;
					nixja = pkgs.nixja;
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
							packages =
								pkgs.corePackages
								++ (with pkgs;
										[
											# C++ development
											cmake
											meson
											ninja
											nlohmann_json
											pkg-config
											fmt
											ccache
											argparse
											spdlog

											# Nix
											nix
											boost # https://github.com/NixOS/nix/issues/13782

											# Utilities
											bear
											gnumake
											jq

											# Python
											uv
											python313
										]
										++ (with llvmPkgs; [
												llvm
												clang
												clang-tools
											]));

							shellHook = ''
                								export Clang_DIR="${llvmPkgs.clang-unwrapped.dev}/lib/cmake/clang"
                								export UV_PYTHON_DOWNLOADS=never
                								export UV_PYTHON="${pkgs.python313}/bin/python3"
              '';
						};
				});
	};
}
