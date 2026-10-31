import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

from jinja2 import Template

# Allow this example to run directly from a source checkout as well as from
# an installed package.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import shlex
from dataclasses import dataclass

from pyn.analyzer import NinjaAnalyzer
from pyn.build_graph import Graph, Node, Scope
from pyn.host import Host
from pyn.parser import parse
from pyn.var_expander import expand_vars


def command_for(graph: Graph, node: Node) -> str:
    """Return a node command with Nix references for produced inputs."""
    rule = graph.rules[node.rule_name]

    inputs: list[str] = []
    for dep in node.explicit_deps:
        if dep in graph.nodes:
            inputs.append("${" + dep.replace(".", "_") + "}")
        else:
            inputs.append(dep)

    command_scope = Scope(node.scope.name, node.scope)
    command_scope["in"] = " ".join(inputs)

    return expand_vars(rule["command"], command_scope, rule)


def load_graph(ninja_file: Path) -> Graph:
    host = Host()
    contents = ninja_file.read_text()
    ast = parse(contents, str(ninja_file))
    return NinjaAnalyzer(host, parse, expand_vars).analyze(ast, str(ninja_file))


@dataclass(frozen=True)
class Dependency:
    value: str
    is_node: bool

    def __str__(self) -> str:
        if self.is_node:
            return "${" + self.value.replace(".", "_") + "}"
        else:
            return self.value

    @property
    def source_path(self) -> str:
        """Render a source dependency relative to the generated Nix file."""
        return "./" + self.value


def dependency_for(graph: Graph, value: str) -> Dependency:
    """Classify a Ninja dependency when creating a template item.

    A value that names another graph node is a generated output and must be
    rendered as a Nix attribute reference. All other values are source files.
    """
    return Dependency(value=value, is_node=value in graph.nodes)


class StandardItem:
    output: str
    dependencies: list[Dependency]
    command: str
    # content_addressed: bool

    @property
    def attrKey(self) -> str:
        return self.output.replace(".", "_")

    def __repr__(self) -> str:
        return (
            f"StandardItem(output={self.output!r}, "
            f"dependencies={self.dependencies!r}, "
            f"command={self.command!r}, "
            # f"content_addressed={self.content_addressed!r})"
        )


class DepItem:
    output: str
    command: str

    dependencies: list[Dependency]

    # dependencies that follow `| x.o`
    implicit_dependencies: list[Dependency]

    @property
    def attrKey(self) -> str:
        return self.output.replace(".", "_")

    def __repr__(self) -> str:
        return (
            f"DepItem(output={self.output!r}, "
            f"command={self.command!r}, "
            f"dependencies={self.dependencies!r}), "
            f"implicit_dependencies={self.implicit_dependencies!r})"
        )


def has_depfile(graph: Graph, node, command: str) -> bool:
    rule = graph.rules[node.rule_name]

    # `rule` is a pyn Scope, not a dict.
    if "depfile" in rule.objs:
        return bool(expand_vars(rule["depfile"], node.scope, rule))

    arguments = shlex.split(command)
    return any(
        arg == "-MF" or (arg.startswith("-MF") and len(arg) > 3) for arg in arguments
    )


def parse_makefile(makefile_content: str) -> defaultdict[str, set[str]]:
    dependency_graph: defaultdict[str, set[str]] = defaultdict(set)

    makefile_content = re.sub(r"\\\r?\n", " ", makefile_content)

    for line in makefile_content.splitlines():
        line = line.strip()

        if not line or line.startswith("#"):
            continue

        if ":" in line:
            target, dependencies = line.split(":", 1)
            dependency_graph[target.strip()].update(dependencies.split())

    return dependency_graph


def local_dependencies(dependencies: set[str]) -> set[str]:
    """
    Filters out non local dependencies
    """
    source_root = Path.cwd().resolve()
    local: set[str] = set()

    for dependency in dependencies:
        path = Path(dependency)
        resolved = (
            path.resolve() if path.is_absolute() else (source_root / path).resolve()
        )

        try:
            local.add(resolved.relative_to(source_root).as_posix())
        except ValueError:
            # /nix/store/... system headers: compiler-provided, not source inputs.
            pass

    return local


# def output_from_command(command: list[str]) -> str:
#     for i, arg in enumerate(command):
#         if arg == "-o":
#             try:
#                 return command[i + 1]
#             except IndexError:
#                 raise ValueError("`-o` requires an output path")
#
#         if arg.startswith("-o") and len(arg) > 2:
#             return arg[2:]
#
#     raise ValueError("command has no `-o` output argument")


# TODO: we need to scan each dependency individually because, but we don't need to do that until we link against llvm
def scan_dependencies() -> dict[str, set[str]]:
    compdb = subprocess.run(
        ["ninja", "-t", "compdb", "c_COMPILER"],
        capture_output=True,
        text=True,
        check=True,
    )

    with tempfile.TemporaryDirectory(dir=Path.cwd()) as temporary_directory:
        compilation_database = Path(temporary_directory) / "compile_commands.json"
        compilation_database.write_text(compdb.stdout)
        result = subprocess.run(
            ["clang-scan-deps", f"-compilation-database={compilation_database}"],
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise RuntimeError("clang-scan-deps failed:\n" + result.stderr.strip())

    dependencies = {
        k: local_dependencies(v) for k, v in parse_makefile(result.stdout).items()
    }
    return dependencies


# Now we need to match each node to dependencies

template_path = Path(__file__).with_name("nixja-gen.nix.j2")
ninja_file = Path.cwd() / "build.ninja"

template = Template(template_path.read_text())
graph = load_graph(ninja_file)

dependency_map = scan_dependencies()

standardItems: list[StandardItem] = []
depItems: list[DepItem] = []

rendered_nodes: set[int] = set()

# TODO: some nodes can have multiple outputs

for output, node in graph.nodes.items():
    # A Ninja build edge may have multiple outputs, all pointing at the
    # same node. Render that edge just once.
    if id(node) in rendered_nodes:
        continue

    rendered_nodes.add(id(node))
    command: str = command_for(graph, node)

    if has_depfile(graph, node, command):
        item = DepItem()
        # print("OUPUT", dependency_map[output])
        item.output = output
        item.command = command

        dependencies = dependency_map[output]
        item.dependencies = [
            dependency_for(graph, dependency)
            for dependency in dependencies
            if dependency not in graph.nodes
        ]

        item.implicit_dependencies = [
            dependency_for(graph, dependency) for dependency in node.implicit_deps
        ]
        depItems.append(item)
    else:
        item = StandardItem()

        item.dependencies = [
            dependency_for(graph, dependency)
            for dependency in node.deps()
            if dependency not in graph.nodes
        ]
        item.output = output
        item.command = command
        standardItems.append(item)


# pp(standardItems)
# pp(depItems)
# pp(dependency_map)

target = sys.argv[1]

print(template.render(standardItems=standardItems, depItems=depItems, target=target))
