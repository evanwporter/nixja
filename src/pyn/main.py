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

import cPickle
import sys

from analyzer import NinjaAnalyzer
from args import parse_args
from builder import Builder
from host import Host
from parser import parse
from pyn_exceptions import PynException
from tools import tool_names, list_tools, run_tool
from var_expander import expand_vars


VERSION = '0.7'


def main(host, argv=None):
    started_time = host.time()

    returncode, args = parse_args(host, argv, VERSION, tool_names())
    if returncode is not None:
        return returncode

    if args.version:
        host.print_out(VERSION)
        return 0

    if args.tool == 'list':
        list_tools(host)
        return 0

    if args.dir:
        if not host.exists(args.dir):
            host.print_err('"%s" not found' % args.dir)
            return 2
        host.chdir(args.dir)
    if not host.exists(args.file):
        host.print_err('"%s" not found' % args.file)
        return 2

    try:
        old_graph, graph = _load_graphs(host, args)

        if args.tool:
            return run_tool(host, args, old_graph, graph, started_time)

        builder = Builder(host, args, expand_vars, started_time)
        nodes_to_build = builder.find_nodes_to_build(old_graph, graph)

        if nodes_to_build:
            res = builder.build(graph, nodes_to_build)
            if graph.is_dirty:
                graph_str = cPickle.dumps(graph)
                host.write('.pyn.db', graph_str)
        else:
            host.print_out('pyn: no work to do.')
            res = 0
        return res
    except PynException as e:
        host.print_err(str(e))
        return 1
    except KeyboardInterrupt as e:
        host.print_err('Interrupted, exiting ..')
        return 130  # SIGINT


def _load_graphs(host, args):
    old_graph = None
    needs_rescan = True
    if host.exists('.pyn.db'):
        graph_str = host.read('.pyn.db')
        old_graph = cPickle.loads(graph_str)

        graph_mtime = host.mtime('.pyn.db')
        file_mtime = host.mtime(args.file)
        needs_rescan = (file_mtime > graph_mtime or
                        any(host.mtime(f) > graph_mtime for
                            f in old_graph.includes) or
                        any(host.mtime(f) > graph_mtime for
                            f in old_graph.subninjas))

    if needs_rescan:
        ast = parse(host.read(args.file), args.file)
        analyzer = NinjaAnalyzer(host, args, parse, expand_vars)
        graph = analyzer.analyze(ast, args.file)
        graph.is_dirty = True
    else:
        graph = old_graph

    return old_graph, graph


if __name__ == '__main__':
    sys.exit(main(Host()))
