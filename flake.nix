{
	description = "Python development environment with uv";

	inputs = {
		nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
	};

	outputs = {
		self,
		nixpkgs,
	}: let
		systems = ["x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin"];
		forAllSystems = f:
			nixpkgs.lib.genAttrs systems (system:
				f nixpkgs.legacyPackages.${system});
	in {
		devShells =
			forAllSystems (pkgs: {
					default =
						pkgs.mkShell {
							packages = with pkgs; [
								python313
								uv
							];

							shellHook = ''
								export UV_PYTHON_DOWNLOADS=never
								export UV_PYTHON="${pkgs.python313}/bin/python3"
								echo "Python + uv development shell"
							'';
						};
				});
	};
}
