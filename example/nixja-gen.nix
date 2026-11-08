
# This file is the output of nixja-gen
# nix-build --show-trace -E 'import ./hello/nixja.nix { pkgs = import <nixpkgs> {}; }'
{pkgs}: 
let
    fs = pkgs.lib.fileset;

    # Dependencies are just explicit and implicit declared source files
    # Nodes should be declared directly in the command
    # TODO: We also should cover the case when a node is an implicit dependency
    mkItem = {name, installPhase, dependencies, command}: pkgs.stdenvNoCC.mkDerivation {
        inherit name;

        src = fs.toSource {
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

        inherit installPhase;
    };


    
    world_o = builtins.outputOf (builtins.outputOf (builtins.storePath "/nix/store/469wgv06cflr2l2wdywwmcjmzws3jsf4-world_o.drv.drv") "out") "out";
    

    
    hello_o = builtins.outputOf (builtins.outputOf (builtins.storePath "/nix/store/7bgp640hd1l9iaf7mvz3c1f7vyv2ffg2-hello_o.drv.drv") "out") "out";
    

    
    main_o = builtins.outputOf (builtins.outputOf (builtins.storePath "/nix/store/9k4ap2s5q4mfjafgciy4jly1mfbqq1cb-main_o.drv.drv") "out") "out";
    



    
    hello = (builtins.storePath "/nix/store/ygzyw1kz4bsnk6pjn5ijxgwf14l2nvbs-hello.drv");
    





 #}
 #}
 #}
 #}
 #}
















in
    hello

