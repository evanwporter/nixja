import subprocess
import sys
from pathlib import Path

from jinja2 import Template

def main() -> None:
    target = sys.argv[1]
    project_dir = Path(__file__).resolve().parent
    template_path = project_dir / "nixja.nix.j2"
    output_path = Path.cwd() / "ninja.nix"

    template = Template(template_path.read_text())
    output_path.write_text(template.render(target=target))

    subprocess.run(
        [
            "nix-build",
            "--extra-experimental-features",
            "ca-derivations dynamic-derivations",
            "--show-trace",
            "-E",
            f"import {output_path} {{ pkgs = import <nixpkgs> {{}}; }}",
        ],
        check=True,
    )


if __name__ == "__main__":
    main()
