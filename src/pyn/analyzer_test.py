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

import unittest

from analyzer import NinjaAnalyzer
from host_fake import FakeHost
from parser import parse
from pyn_exceptions import PynException
from var_expander import expand_vars


class TestAnalyzer(unittest.TestCase):
    # 'too many public methods' pylint: disable=R0904
    # 'Invalid name 'setUp' pylint: disable=C0103

    def setUp(self):
        self.host = FakeHost()
        self.args = None

    def check(self, ast):
        analyzer = NinjaAnalyzer(self.host, self.args, parse, expand_vars)
        return analyzer.analyze(ast, 'build.ninja')

    def err(self, ast):
        analyzer = NinjaAnalyzer(self.host, self.args, parse, expand_vars)
        self.assertRaises(PynException, analyzer.analyze, ast, 'build.ninja')

    def test_empty(self):
        self.check([])

    def test_build(self):
        self.check([['build', ['foo.o'], 'cc', ['foo.c'], [], [], []]])
        self.err([['build', ['foo.o'], 'cc', ['foo.c'], [], [], []],
                  ['build', ['foo.o'], 'cc', ['foo.c'], [], [], []]])
        self.check([['build', ['foo.o'], 'cc', ['foo.c'], [], [],
                    [['var', 'foo', 'bar'],
                     ['var', 'foo', 'bar']]]])

    def test_vars(self):
        self.check([['var', 'foo', 'bar']])
        self.check([['var', 'foo', 'bar'],
                    ['var', 'foo', 'bar']])

    def test_vars_are_expanded_immediately(self):
        graph = self.check([['var', 'foo', 'bar'],
                            ['var', 'baz', '$foo']])
        self.assertEquals(graph.scopes[graph.name]['baz'],
                          'bar')

    def test_subninja_missing(self):
        self.err([['subninja', 'missing.ninja']])

    def test_subninja(self):
        self.host.files['/tmp/sub.ninja'] = ''
        self.check([['subninja', 'sub.ninja']])

    def test_scope_of_subninjas(self):
        self.host.files = {
            '/tmp/build.ninja': ('foo = 1\n'
                                 'bar = 2\n'
                                 'rule echo\n'
                                 '  command = echo $foo $bar\n'
                                 'build one : echo\n'
                                 'subninja sub.ninja\n'),
            '/tmp/sub.ninja': ('foo = s1\n'
                               'build two: echo\n'),
        }
        ast = parse(self.host.read('build.ninja'), 'build.ninja')
        analyzer = NinjaAnalyzer(self.host, self.args, parse, expand_vars)
        graph = analyzer.analyze(ast, 'build.ninja')

        n = graph.nodes['one']
        rule_scope = graph.rules[n.rule_name]
        self.assertEqual(expand_vars(rule_scope['command'], n.scope,
                                     rule_scope),
                         'echo 1 2')

        n = graph.nodes['two']
        rule_scope = graph.rules[n.rule_name]
        self.assertEqual(expand_vars(rule_scope['command'], n.scope,
                                     rule_scope),
                         'echo s1 2')

    def test_include(self):
        self.host.files['/tmp/sub.ninja'] = ''
        self.check([['include', 'sub.ninja']])

    def test_include_missing(self):
        self.err([['include', 'missing.ninja']])

    def test_pool(self):
        self.check([['pool', 'foo', [['var', 'depth', '4']]]])
        self.err([['pool', 'foo', [['var', 'depth', 'foo']]]])
        self.err([['pool', 'foo', [['var', 'depth', '4']]],
                  ['pool', 'foo', [['var', 'depth', '4']]]])
        self.err([['pool', 'foo', []]])
        self.err([['pool', 'foo', [['var', 'foo', '4']]]])
        self.err([['pool', 'foo', [['var', 'depth', '4'],
                                   ['var', 'foo', 'bar']]]])

    def test_rule(self):
        # Note that the var values are *not* expanded.
        self.check([['rule', 'cc', [['var', 'command', 'cc -o $out $in']]]])

        self.err([['rule', 'cc', [['var', 'command', 'cc -o $out $in']]],
                  ['rule', 'cc', [['var', 'command', 'cc -o $out $in']]]])

        # Note that duplicate variables are okay.
        self.check([['rule', 'cc', [['var', 'command', 'cc -o $out $in'],
                                    ['var', 'command', 'touch $out']]]])

    def test_default(self):
        self.check([['default', ['all']]])
