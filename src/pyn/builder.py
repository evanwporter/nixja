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

from pyn_exceptions import PynException
from stats import Stats
from pool import Pool, Empty
from printer import Printer


class Builder(object):
    def __init__(self, host, args, expand_vars, started_time):
        self.host = host
        self.args = args
        self._should_overwrite = args.overwrite_status and not args.verbose
        self.expand_vars = expand_vars
        self.stats = Stats(host.getenv('NINJA_STATUS', '[%s/%t] '),
                           host.time, started_time)
        self._printer = Printer(host.print_out, self._should_overwrite)
        self._mtimes = {}
        self._failures = 0
        self._pool = None

    def find_nodes_to_build(self, old_graph, graph):
        node_names = self.args.targets or graph.defaults or graph.roots()
        try:
            nodes_to_build = graph.closure(node_names)
        except KeyError as e:
            raise PynException('error: unknown target %s' % str(e))

        sorted_nodes = graph.tsort(nodes_to_build)
        sorted_nodes = [n for n in sorted_nodes
                        if graph.nodes[n].rule_name != 'phony']

        nodes_to_build = []
        for node_name in sorted_nodes:
            n = graph.nodes[node_name]
            my_stat = self._stat(node_name)
            if my_stat is None or any(self._stat(d) > my_stat
                                      for d in n.deps()):
                nodes_to_build.append(node_name)
                continue

            if old_graph and node_name in old_graph.nodes:
                if (self._command(old_graph, node_name) !=
                        self._command(graph, node_name)):
                    nodes_to_build.append(node_name)
                    continue

        return nodes_to_build

    def build(self, graph, nodes_to_build):
        stats = self.stats
        stats.total = len(nodes_to_build)
        stats.started = 0
        stats.started_time = self.host.time()

        running_jobs = []
        self._pool = Pool(self.args.jobs, _call)
        try:
            while nodes_to_build and self._failures < self.args.errors:
                while stats.started - stats.finished < self.args.jobs:
                    n = self._find_next_available_node(graph, nodes_to_build)
                    if n:
                        self._build_node(graph, n)
                        running_jobs.append(n)
                    else:
                        break
                did_work = self._process_completed_jobs(graph, running_jobs)
                if (not did_work and nodes_to_build and
                        self._failures < self.args.errors):
                    did_work = self._process_completed_jobs(graph,
                                                            running_jobs,
                                                            block=True)

            while running_jobs:
                did_work = self._process_completed_jobs(graph, running_jobs,
                                                        block=True)
        finally:
            self._pool.close()
            self._pool.join()

        self._printer.flush()
        return 1 if self._failures else 0

    def _find_next_available_node(self, graph, nodes_to_build):
        next_node = None
        for node_name in nodes_to_build:
            n = graph.nodes[node_name]
            if not any(d in graph.nodes and graph.nodes[d].running
                       for d in n.deps(include_order_only=True)):
                next_node = node_name
                break

        if next_node:
            # Ensure all of the dependencies actually exist.
            # FIXME: is there a better place for this check?
            for d in n.deps():
                if not self.host.exists(d):
                    raise PynException("error: '%s', needed by '%s', %s" %
                                       (d, next_node,
                                        "missing and no known rule to make "
                                        "it"))
            nodes_to_build.remove(next_node)
        return next_node

    def _command(self, graph, node_name):
        node = graph.nodes[node_name]
        rule_scope = graph.rules[node.rule_name]
        return self.expand_vars(rule_scope['command'], node.scope, rule_scope)

    def _description(self, graph, node_name):
        node = graph.nodes[node_name]
        rule_scope = graph.rules[node.rule_name]
        desc = rule_scope['description'] or rule_scope['command']
        return self.expand_vars(desc, node.scope, rule_scope)

    def _build_node(self, graph, node_name):
        node = graph.nodes[node_name]
        desc = self._description(graph, node_name)
        command = self._command(graph, node_name)
        self._build_node_started(node, desc, command)

        dry_run = node.rule_name == 'phony' or self.args.dry_run
        if not dry_run:
            for o in node.outputs:
                self.host.maybe_mkdir(self.host.dirname(o))
        self._pool.send((node.name, desc, command, dry_run, self.host))

    def _process_completed_jobs(self, graph, running_jobs, block=False):
        did_work = False
        while True:
            try:
                resp = self._pool.get(block=block)
                running_jobs.remove(resp[0])
                did_work = True
                self._build_node_done(graph, resp)
                if block:
                    break
            except Empty:
                break
        return did_work

    def _build_node_started(self, node, desc, command):
        node.running = True
        self.stats.started += 1
        if self.args.verbose > 1:
            self._update(command, elide=False)
        else:
            self._update(desc)

    def _build_node_done(self, graph, result):
        node_name, desc, command, ret, out, err = result
        n = graph.nodes[node_name]
        rule_scope = graph.rules[n.rule_name]
        n.running = False

        if n.scope['depfile'] and n.scope['deps'] == 'gcc':
            path = self.expand_vars(n.scope['depfile'], n.scope, rule_scope)
            if self.host.exists(path):
                depsfile_deps = self.host.read(path).split()[2:]
                self.host.remove(path)
                if n.depsfile_deps != depsfile_deps:
                    n.depsfile_deps = depsfile_deps
                    graph.dirty = True

        self.stats.finished += 1

        if ret:
            self._failures += 1
            self._update(command, prefix='FAILED: ', elide=False)
        elif self.args.verbose > 1:
            self._update(command, elide=False)
        elif self._should_overwrite:
            self._update(desc)
        if out or err:
            self._printer.flush()
        if out:
            self.host.print_out(out, end='')
        if err:
            self.host.print_err(err, end='')

    def _update(self, msg, prefix=None, elide=True):
        prefix = prefix or self.stats.format()
        self._printer.update(prefix + msg, elide=elide)

    def _stat(self, name):
        if not name in self._mtimes:
            self._restat(name)
        return self._mtimes.get(name, None)

    def _restat(self, name):
        if self.host.exists(name):
            self._mtimes[name] = self.host.mtime(name)
        else:
            self._mtimes[name] = None


def _call(request):
    node_name, desc, command, dry_run, host = request
    if dry_run:
        ret, out, err = 0, '', ''
    else:
        ret, out, err = host.call(command)
    return (node_name, desc, command, ret, out, err)
