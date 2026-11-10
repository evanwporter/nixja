#pragma once
#include <string>

#include <nlohmann/json.hpp>
#include <nix/store/store-api.hh>

using json = nlohmann::json;

struct DerivationSpec {
    std::string name;
    std::string system;

    /// Path to bash
    std::string builder;

    /// PATH env variable
    std::string path;

    std::string linkPhase;
    std::string buildPhase;
    std::string installPhase;

    /// `nar` or `text`
    std::string outputMethod;

    json inputSrcs;
    json inputDrvs;
    json extraEnv;
};

nix::StorePath writeDerivation(
    const nix::ref<nix::Store>& store,
    const DerivationSpec& spec);
