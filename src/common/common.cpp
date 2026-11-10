#include "common.hpp"

#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>

nix::StorePath writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec) {

    json env = spec.extraEnv;
    env["name"] = spec.name;
    env["PATH"] = spec.path;
    env["linkPhase"] = spec.linkPhase;
    env["buildPhase"] = spec.buildPhase;
    env["installPhase"] = spec.installPhase;
    env["outputs"] = "out";
    env["requiredSystemFeatures"] = "recursive-nix";
    env["out"] = nix::hashPlaceholder("out");

    const json drv = {
        { "name", spec.name },
        { "system", spec.system },
        { "builder", spec.builder },
        { "version", 4u },
        {
            "args",
            json::array({ "-e", "-c", "eval \"$linkPhase\"; eval \"$buildPhase\"; eval \"$installPhase\"" }),
        },
        { "env", env },
        { "inputs", {
                        { "srcs", spec.inputSrcs },
                        { "drvs", spec.inputDrvs },
                    } },
        { "outputs", {
                         { "out", {
                                      { "method", spec.outputMethod },
                                      { "hashAlgo", "sha256" },
                                  } },
                     } },
    };

    const auto derivation = nix::Derivation::parseJsonAndValidate(*store, drv);

    return store->writeDerivation(derivation);
}
