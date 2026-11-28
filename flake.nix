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

		tree-sitter-bash = pkgs:
			pkgs.stdenv.mkDerivation {
				inherit
					(pkgs.tree-sitter.grammars.tree-sitter-bash)
					version
					src
					;

				pname = "tree-sitter-bash";

				makeFlags = [
					"PREFIX=$(out)"
				];
			};
	in {
		overlays.default = final: prev: {
			corePackages = with final; [
				bash
				coreutils
				findutils
				which
				patchelf
				stdenv.cc
				clang-tools
			];

			corePath = final.lib.makeBinPath (final.corePackages ++ [final.llvmPackages_22.clang]);

			# Builder for a single scanned Ninja edge.
			nixja-task =
				final.stdenv.mkDerivation {
					name = "nixja-task";
					src = self;

					nativeBuildInputs =
						final.corePackages
						++ [
							final.cmake
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

					buildFlags = ["nixja-task"];
					cmakeFlags = ["-DNIXJA_BUILD_TESTS=OFF"];

					installPhase = ''
            					runHook preInstall
            					install -Dm755 src/nixja-task/nixja-task "$out/bin/nixja-task"
            					runHook postInstall
          '';
				};

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

					cmakeFlags = [
						"-DNIXJA_BUILD_TESTS=OFF"
						"-DnixjaTask=${final.nixja-task}/bin/nixja-task"
					];
					buildFlags = ["nixja-scan-deps"];

					installPhase = ''
            						runHook preInstall
            						install -Dm755 src/nixja-scan-deps/nixja-scan-deps "$out/bin/nixja-scan-deps"
            						runHook postInstall
          '';

					postFixup = ''
                                    # Match the system include environment used by nixpkgs' clang tools.
                                    includePaths() {
                                        local language="$1" path="" after="" flag
                                        shift
                                        while (( $# )); do
                                            flag="$1"
                                            shift
                                            case "$flag" in
                                                -isystem) path="''${path:+$path:}$1"; shift ;;
                                                -cxx-isystem)
                                                    if [[ "$language" == c++ ]]; then
                                                        path="''${path:+$path:}$1"
                                                    fi
                                                    shift ;;
                                                -idirafter) after="''${after:+$after:}$1"; shift ;;
                                            esac
                                        done
                                        printf '%s' "''${path}''${path:+''${after:+:}}''${after}"
                                    }
                                    libcFlags=$(cat ${final.llvmPackages_22.clang}/nix-support/libc-cflags)
                                    cxxFlags=$(cat ${final.llvmPackages_22.clang}/nix-support/libcxx-cxxflags)
                                    cIncludes=$(includePaths c $libcFlags)
                                    cxxIncludes=$(includePaths c++ $cxxFlags $libcFlags)
            						wrapProgram $out/bin/nixja-scan-deps \
            							--prefix PATH : ${final.corePath} \
                                                        --prefix C_INCLUDE_PATH : "$cIncludes" \
                                                        --prefix CPLUS_INCLUDE_PATH : "$cxxIncludes"
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

					buildInputs = with final; [
						argparse
						fmt
						spdlog
						libblake3
						nlohmann_json
						nix
						llvmPackages_22.llvm
						llvmPackages_22.clang
						llvmPackages_22.clang-unwrapped
						tree-sitter
						(tree-sitter-bash final)
					];

					buildFlags = ["nixja"];

					cmakeFlags = [
						"-DNIXJA_BUILD_TESTS=OFF"
						"-DnixjaScanDeps=${final.nixja-scan-deps}/bin/nixja-scan-deps"
						"-DnixjaTask=${final.nixja-task}/bin/nixja-task"
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
								final.nixja-task
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
					nixja-task = pkgs.nixja-task;
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

					nixja-task = {
						type = "app";
						program = "${pkgs.nixja-task}/bin/nixja-task";
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
											argparse
											spdlog
											gtest
											tree-sitter
											(tree-sitter-bash pkgs)

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
