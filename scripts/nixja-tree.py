# /// script
# requires-python = ">=3.13"
# dependencies = []
# ///

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import TypedDict

STORE_DIR = Path("/nix/store")


class DerivationOutput(TypedDict):
    hashAlgo: str
    method: str


class DerivationInputs(TypedDict):
    drvs: dict[str, list[str]]
    srcs: list[str]


class Derivation(TypedDict):
    args: list[str]
    builder: str
    env: dict[str, str]
    inputs: DerivationInputs
    name: str
    outputs: dict[str, DerivationOutput]
    system: str
    version: int


class DerivationFile(TypedDict):
    derivations: dict[str, Derivation]
    version: int


def get_derivation(store_object: Path) -> Path | None:
    """Return the derivation that produced a store object, if available."""

    out = subprocess.run(
        ["nix-store", "--query", "--deriver", str(store_object)],
        capture_output=True,
        text=True,
        check=True,
    )

    result = out.stdout.strip()

    if result == "unknown-deriver":
        return None

    return Path(result)


def drv_to_json(derivation_path: Path) -> DerivationFile:
    """Convert a derivation file into its JSON representation."""

    assert derivation_path.suffix == ".drv"

    out = subprocess.run(
        ["nix", "derivation", "show", str(derivation_path)],
        capture_output=True,
        text=True,
        check=True,
    )

    return json.loads(out.stdout)


def get_drv_record(data: DerivationFile, drv_path: Path) -> Derivation:
    """Extract the derivation record from nix derivation show output."""

    derivations = data.get("derivations", data)

    record = derivations.get(drv_path.name)

    if record is None:
        raise TypeError(f"record for {drv_path} is None")

    return record


def is_nixja_path(path: Path) -> bool:
    """Check whether a store path has a name beginning with nixja-."""

    _, separator, name = path.name.partition("-")

    return separator != "" and name.startswith("nixja-")


def to_store_path(path: str) -> Path:
    """Convert a store path name or absolute path into a Path."""

    store_path = Path(path)

    if store_path.is_absolute():
        return store_path

    return STORE_DIR / store_path


def symlink(path: Path, destination: Path) -> None:
    """Symlink a Nixja-owned store object into the destination."""

    if not is_nixja_path(path) or path.suffix == ".h":
        return

    link = destination / path.name

    if not path.exists():
        print(f"Warning: Missing store path: {path}", file=sys.stderr)
        return

    if link.is_symlink():
        if link.resolve() == path.resolve():
            return

        raise FileExistsError(f"Conflicting symlink: {link}")

    if link.exists():
        raise FileExistsError(f"Destination already exists: {link}")

    link.symlink_to(path)

    print(f"Linked: {path.name}")


def traverse_store_object(
    store_object: Path,
    destination: Path,
    visited: set[Path],
) -> None:
    """Follow a Nixja store object's deriver and recursively inspect it."""

    # Only traverse Nixja-owned objects.
    if not is_nixja_path(store_object):
        return

    store_object = store_object.resolve()

    if store_object in visited:
        return

    visited.add(store_object)

    symlink(store_object, destination)

    print(f"Inspecting store object: {store_object.name}")

    # Find the derivation that produced this object.
    drv_path = get_derivation(store_object)

    if drv_path is None:
        print(f"  No deriver: {store_object.name}")
        return

    print(f"  Deriver: {drv_path.name}")

    # Only follow Nixja-generated derivations.
    if not is_nixja_path(drv_path):
        return

    traverse_derivation(drv_path, destination, visited)


def traverse_derivation(
    drv_path: Path,
    destination: Path,
    visited: set[Path],
) -> None:
    """Recursively inspect a Nixja derivation and its inputs."""

    if not is_nixja_path(drv_path):
        return

    drv_path = drv_path.resolve()

    if drv_path in visited:
        return

    visited.add(drv_path)

    symlink(drv_path, destination)

    if not drv_path.exists():
        print(f"Warning: Missing derivation: {drv_path}", file=sys.stderr)
        return

    print(f"Inspecting derivation: {drv_path.name}")

    # Follow the derivation that produced this derivation.
    parent_drv = get_derivation(drv_path)

    if parent_drv is None:
        print(f"  No deriver: {drv_path.name}")
    else:
        print(f"  Deriver: {parent_drv.name}")
        if is_nixja_path(parent_drv):
            traverse_derivation(
                parent_drv,
                destination,
                visited,
            )

    data = drv_to_json(drv_path)
    drv = get_drv_record(data, drv_path)
    inputs = drv.get("inputs", {})

    # Process direct store-path inputs.
    for source in inputs.get("srcs", []):
        source_path = to_store_path(source)

        if source_path.suffix == ".drv":
            traverse_derivation(
                source_path,
                destination,
                visited,
            )
        else:
            traverse_store_object(
                source_path,
                destination,
                visited,
            )

    # Process derivation inputs.
    for input_drv in inputs.get("drvs", {}):
        traverse_derivation(
            to_store_path(input_drv),
            destination,
            visited,
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Recursively inspect Nixja store objects."
    )

    parser.add_argument(
        "store_object",
        help="Store object or derivation to inspect.",
    )

    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("nixja-store"),
        help="Destination directory for symlinks.",
    )

    args = parser.parse_args()

    store_object = Path(args.store_object)
    destination = args.output

    destination.mkdir(parents=True, exist_ok=True)

    visited: set[Path] = set()

    if store_object.suffix == ".drv":
        traverse_derivation(
            store_object,
            destination,
            visited,
        )
    else:
        traverse_store_object(
            store_object,
            destination,
            visited,
        )

    print(f"\nInspected {len(visited)} Nixja store objects and derivations.")
    print(f"Store view: {destination.resolve()}")


if __name__ == "__main__":
    main()
