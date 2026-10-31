"""Console-script wrapper for the bundled Nixja generator."""

import runpy
from pathlib import Path


def main() -> None:
    runpy.run_path(Path(__file__).with_name("nixja-gen.py"), run_name="__main__")
