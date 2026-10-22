# Copyright 2014 Dirk Pranke. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from typing import Optional

from pyn.pyn_exceptions import PynException


class Graph:
    def __init__(self, name: str) -> None:
        self.name: str = name
        self.defaults: list[str] = []
        self.nodes: dict[str, Node] = {}
        self.rules: dict[str, "Scope"] = {}
        self.pools: dict[str, "Scope"] = {}
        self.scopes: dict[str, "Scope"] = {}
        self.subninjas: list[str] = []
        self.includes: list[str] = []
        self.is_dirty: bool = False

    def __repr__(self) -> str:
        return 'Graph(name="%s")' % self.name

    def roots(self) -> list[str]:
        """Find all the outputs that are not inputs of other outputs."""
        all_deps: set[str] = set()
        roots: set[str] = set()
        for n in self.nodes.values():
            for d in n.explicit_deps:
                all_deps.add(d)
        for n in self.nodes.values():
            for d in n.outputs:
                if not d in all_deps:
                    roots.add(d)
        return list(roots)

    def closure(self, targets: list[str]) -> set[str]:
        """Return the set of the targets plus all their dependencies."""
        unvisited_nodes = targets[:]
        unvisited_set: set[str] = set(unvisited_nodes)
        nodes_to_build: set[str] = set()
        while unvisited_nodes:
            node_name = unvisited_nodes.pop(0)
            node = self.nodes[node_name]
            unvisited_set.remove(node_name)
            nodes_to_build.add(node_name)
            for d in node.deps():
                if (
                    d not in nodes_to_build
                    and d not in unvisited_set
                    and d in self.nodes
                ):
                    unvisited_nodes.append(d)
                    unvisited_set.add(d)
        return nodes_to_build

    def tsort(self, nodes_to_build: set[str]) -> list[str]:
        """Sort a list of nodes based on their dependencies (leaves first)."""

        # This performs a topological sort of the nodes in the graph by
        # picking nodes arbitrarily, and then doing depth-first searches
        # across the graph from there. It should run in O(|nodes|) time.
        # See http://en.wikipedia.org/wiki/Topological_sorting and
        # Tarjan (1976).
        #
        # This algorithm diverges a bit from the Wikipedia algorithm by
        # inserting new nodes at the tail of the sorted node list instead
        # of the head, because we want to ultimately do a bottom-up traversal.
        def visit(
            node_name,
            visited_nodes,
            sorted_nodes,
            unvisited_nodes,
            unvisited_set,
            sorted_set,
        ):
            if node_name in visited_nodes:
                raise PynException("'%s' is part of a cycle" % node_name)

            visited_nodes.add(node_name)
            for d in self.nodes[node_name].deps(include_order_only=True):
                if d in self.nodes and d not in sorted_set:
                    visit(
                        d,
                        visited_nodes,
                        sorted_nodes,
                        unvisited_nodes,
                        unvisited_set,
                        sorted_set,
                    )
            if node_name in unvisited_nodes:
                unvisited_nodes.remove(node_name)
                unvisited_set.remove(node_name)
            sorted_nodes.append(node_name)
            sorted_set.add(node_name)

        visited_nodes = set()
        unvisited_nodes = [node_name for node_name in nodes_to_build]
        sorted_nodes = []
        sorted_set = set(sorted_nodes)
        unvisited_set = set(unvisited_nodes)
        while unvisited_nodes:
            visit(
                unvisited_nodes[0],
                visited_nodes,
                sorted_nodes,
                unvisited_nodes,
                unvisited_set,
                sorted_set,
            )
        return sorted_nodes


class Node:
    def __init__(
        self,
        name: str,
        scope: "Scope",
        outputs: list[str],
        rule_name: str,
        explicit_deps: Optional[list[str]] = None,
        implicit_deps: Optional[list[str]] = None,
        order_only_deps: Optional[list[str]] = None,
        depsfile_deps: Optional[list[str]] = None,
    ) -> None:
        # 'too many arguments' pylint: disable=R0913
        self.name: str = name
        self.scope: Scope = scope
        self.outputs: list[str] = outputs
        self.rule_name: str = rule_name
        self.explicit_deps: list[str] = explicit_deps or []
        self.implicit_deps: list[str] = implicit_deps or []
        self.order_only_deps: list[str] = order_only_deps or []
        self.depsfile_deps: list[str] = depsfile_deps or []
        self.running: bool = False

    def deps(self, include_order_only: bool = False) -> list[str]:
        node_names = self.explicit_deps + self.implicit_deps + self.depsfile_deps
        if include_order_only:
            node_names += self.order_only_deps
        return node_names

    def __repr__(self) -> str:
        return 'Node(name="%s")' % self.name


class Scope:
    def __init__(self, name, parent):
        self.name = name
        self.parent = parent
        self.objs = {}

    def __repr__(self):
        return 'Scope(name="%s")' % self.name

    def __contains__(self, key):
        return key in self.objs or (self.parent and key in self.parent)

    def __setitem__(self, key, value):
        self.objs[key] = value

    def __delitem__(self, key):
        if key in self.objs:
            del self.objs[key]

    def __getitem__(self, key):
        if key in self.objs:
            return self.objs[key]
        if self.parent:
            return self.parent[key]
        return ""
