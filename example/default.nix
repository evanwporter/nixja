pkgs: {
	bash = "${pkgs.bash}/bin/bash";
	bashStorePath = "${pkgs.bash}";
	gcc = "${pkgs.gcc}";
	coreutils = "${pkgs.coreutils}";
	clangTools = "${pkgs.clang-tools}";
	clangScanDeps = "${pkgs.clang-tools}/bin/clang-scan-deps";
}
