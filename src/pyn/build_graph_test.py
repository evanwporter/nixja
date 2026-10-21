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

from pyn_exceptions import PynException
from build_graph import Graph, Node, Scope


# 'too many public methods' pylint: disable=R0904


class TestGraph(unittest.TestCase):
    def test_closure_simple(self):
        g = Graph('build.ninja')
        n1 = Node(name='foo.so', scope='build.ninja', outputs=['foo.so'],
                  rule_name='shlib', explicit_deps=['foo.o'])
        n2 = Node(name='foo.o', scope='build.ninja', outputs=['foo.so'],
                  rule_name='cc', explicit_deps=['foo.c'])
        g.nodes[n1.name] = n1
        g.nodes[n2.name] = n2
        self.assertEqual(g.closure([n1.name]), set([n1.name, n2.name]))

    def test_repr(self):
        self.assertEqual(repr(Graph('build.ninja')),
                         'Graph(name="build.ninja")')

    def test_tsort_cycle(self):
        g = Graph('build.ninja')
        n1 = Node(name='foo.so', scope='build.ninja', outputs=['foo.so'],
                  rule_name='shlib', explicit_deps=['bar.so'])
        n2 = Node(name='bar.so', scope='build.ninja', outputs=['bar.so'],
                  rule_name='shlib', explicit_deps=['foo.so'])
        g.nodes[n1.name] = n1
        g.nodes[n2.name] = n2
        self.assertRaises(PynException, g.tsort, ['foo.so'])

    def test_tsort_simple(self):
        g = Graph('build.ninja')
        n1 = Node(name='foo.so', scope='build.ninja', outputs=['foo.so'],
                  rule_name='shlib', explicit_deps=['foo.o'])
        n2 = Node(name='foo.o', scope='build.ninja', outputs=['foo.o'],
                  rule_name='cc', explicit_deps=['foo.c'])
        g.nodes[n1.name] = n1
        g.nodes[n2.name] = n2
        self.assertEqual(g.tsort([n1.name]), [n2.name, n1.name])


class TestNode(unittest.TestCase):
    def test_repr(self):
        self.assertEqual(repr(Node('foo.o', ['foo.o'], Scope('build.ninja',
                                                             None),
                                   'cc', [])),
                         'Node(name="foo.o")')


class TestScope(unittest.TestCase):
    def setUp(self):
        # 'Invalid name' pylint: disable=C0103
        self.p = Scope('build.ninja', None)
        self.c = Scope('foo.subninja', self.p)
        self.p['foo'] = 'p-foo'
        self.p['bar'] = 'p-bar'
        self.c['foo'] = 'c-foo'

    def test_repr(self):
        self.assertEqual(repr(self.p), 'Scope(name="build.ninja")')
        self.assertEqual(repr(self.c), 'Scope(name="foo.subninja")')

    def test_basic(self):
        self.assertEqual(self.p['foo'], 'p-foo')
        self.assertEqual(self.p['baz'], '')
        self.assertTrue('foo' in self.p)
        self.assertEqual(self.c['foo'], 'c-foo')
        self.assertEqual(self.c['bar'], 'p-bar')

    def test_update_child(self):
        self.c['foo'] = 'c-foo2'
        self.assertEqual(self.p['foo'], 'p-foo')
        self.assertEqual(self.c['foo'], 'c-foo2')

    def test_delete_child(self):
        del self.c['foo']
        self.assertEqual(self.p['foo'], 'p-foo')
        self.assertEqual(self.c['foo'], 'p-foo')

        del self.c['foo']
        self.assertEqual(self.p['foo'], 'p-foo')
        self.assertEqual(self.c['foo'], 'p-foo')
