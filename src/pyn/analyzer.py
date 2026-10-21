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

from build_graph import Graph, Node, Scope
from pyn_exceptions import PynException


class NinjaAnalyzer(object):
    def __init__(self, host, args, parse, expand_vars):
        self.host = host
        self.args = args
        self.parse = parse
        self.expand_vars = expand_vars

    def analyze(self, ast, filename, parent_scope=None):
        graph = Graph(filename)
        scope = Scope(filename, parent_scope)
        graph.scopes[filename] = scope
        graph = self._add_ast(graph, scope, ast)
        graph = self._add_subninjas(graph)
        return graph

    def _add_ast(self, graph, scope, ast):
        for decl in ast:
            graph = getattr(self, '_decl_' + decl[0])(graph, scope, decl)
        return graph

    def _add_subninjas(self, graph):
        for path in graph.subninjas:
            if not self.host.exists(path):
                raise PynException("'%s' not found." % path)
            ast = self.parse(self.host.read(path), path)
            subgraph = self.analyze(ast, path, graph.scopes[graph.name])
            graph = self._merge_graphs(graph, subgraph)
        return graph

    def _merge_graphs(self, graph, subgraph):
        for rule_name, rule_scope in subgraph.rules.items():
            if rule_name in graph.rules:
                raise PynException("rule '%s' declared in multiple files " %
                                   rule_name)
            graph.rules[rule_name] = rule_scope
        for name, scope in subgraph.scopes.items():
            if name in graph.scopes:
                raise PynException("scope '%s' declared in multiple files " %
                                   name)
            graph.scopes[name] = scope

        self._add_nodes_to_graph(subgraph.nodes, graph)
        return graph

    def _exp(self, scope, paths):
        return [self.expand_vars(p, scope) for p in paths]

    def _add_vars_to_scope(self, var_decls, scope, expand=True):
        for _, name, val in var_decls:
            if expand:
                scope.objs[name] = self.expand_vars(val, scope)
            else:
                scope.objs[name] = val

    def _add_nodes_to_graph(self, nodes, graph):
        for name, node in nodes.items():
            if name in graph.nodes:
                raise PynException("build output '%s' declared more than "
                                   "once " % name)
            graph.nodes[name] = node

    def _decl_build(self, graph, scope, decl):
        _, outs, rule_name, edeps, ideps, odeps, build_vars = decl

        exp_outs = self._exp(scope, outs)
        quoted_outs = ' '.join(('"%s"' % o if ' ' in o else o)
                               for o in exp_outs)
        exp_edeps = self._exp(scope, edeps)
        quoted_edeps = ' '.join(('"%s"' % o if ' ' in o else o)
                                for o in exp_edeps)
        build_scope = Scope(quoted_outs, scope)
        build_scope['out'] = quoted_outs
        build_scope['in'] = quoted_edeps
        self._add_vars_to_scope(build_vars, build_scope)

        # FIXME: using exp_outs instead of quoted_outs might get us
        # into trouble; we should probably have a different kind of name
        # entirely.
        n = Node(' '.join(exp_outs), build_scope, exp_outs, rule_name,
                 exp_edeps, self._exp(scope, ideps), self._exp(scope, odeps))
        nodes = {}
        for o in exp_outs:
            nodes[o] = n
        self._add_nodes_to_graph(nodes, graph)

        return graph

    def _decl_default(self, graph, scope, decl):
        _, defaults = decl

        graph.defaults = graph.defaults + self._exp(scope, defaults)
        return graph

    def _decl_include(self, graph, scope, decl):
        _, path = decl
        full_path = self.expand_vars(path, scope)
        if not self.host.exists(full_path):
            raise PynException("'%s' not found." % full_path)
        ast = self.parse(self.host.read(full_path), full_path)
        graph = self._add_ast(graph, graph.scopes[graph.name], ast)
        graph.includes.append(full_path)
        return graph

    def _decl_pool(self, graph, scope, decl):
        _, name, pool_vars = decl

        if name in graph.pools:
            raise PynException("pool '%s' already declared" % name)
        if not pool_vars:
            raise PynException("pool '%s' has no depth variable" % name)
        if len(pool_vars) > 1:
            raise PynException("pool '%s' has too many variables" % name)

        _, var_name, var_value = pool_vars[0]
        if var_name != 'depth':
            raise PynException("pool '%s' has a variable named %s, not "
                               "'depth'" % (name, var_name))
        try:
            depth = int(self.expand_vars(var_value, scope))
        except ValueError:
            raise PynException("pool '%s'\'s depth value, '%s', is not an int"
                               % (name, var_value))

        graph.pools[name] = depth
        return graph

    def _decl_rule(self, graph, scope, decl):
        _, rule_name, rule_vars = decl

        if rule_name in graph.rules:
            raise PynException("'rule %s' declared more than once" % rule_name)

        rule_scope = Scope(rule_name, scope)
        self._add_vars_to_scope(rule_vars, rule_scope, expand=False)
        graph.rules[rule_name] = rule_scope
        return graph

    def _decl_subninja(self, graph, scope, decl):
        _, path = decl
        graph.subninjas.append(self.expand_vars(path, scope))
        return graph

    def _decl_var(self, graph, scope, decl):
        self._add_vars_to_scope([decl], scope)
        return graph
