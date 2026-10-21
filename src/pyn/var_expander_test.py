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


from build_graph import Scope
from pyn_exceptions import PynException
from var_expander import expand_vars


class TestExpandVars(unittest.TestCase):
    # 'too many public methods' pylint: disable=R0904

    def setUp(self):  # 'invalid name' pylint: disable=C0103
        self.scope = Scope('base', None)
        self.scope['foo'] = 'a'
        self.scope['bar'] = 'b'

    def check(self, inp, out, rule_scope=None):
        self.assertEqual(expand_vars(inp, self.scope, rule_scope), out)

    def err(self, inp):
        self.assertRaises(PynException, expand_vars, inp, self.scope)

    def test_noop(self):
        self.check('xyz', 'xyz')

    def test_empty(self):
        self.check('', '')

    def test_simple(self):
        self.check('$foo', 'a')
        self.check('$foo bar', 'a bar')
        self.check('c$foo', 'ca')

    def test_escapes(self):
        self.check('$$', '$')
        self.check('$ ', ' ')
        self.check('$:', ':')

    def test_curlies(self):
        self.check('${foo}', 'a')
        self.check('${foo}bar', 'abar')

    def test_undefined_var_expands_to_nothing(self):
        self.check('$baz', '')

    def test_periods_terminate_variable_names(self):
        self.check('$foo.bar', 'a.bar')

    def test_errors(self):
        self.err('${')
        self.err('${baz')
        self.err('$')
        self.err('$123')
        self.err('${baz foo')

    def test_scope_with_rules(self):
        r = Scope('cc', self.scope)
        r['baz'] = 'c'

        # ensure that a rule isn't required.
        self.check('$baz', '')

        # ensure that we look at the rule.
        self.check('$baz', 'c', r)

        # ensure that the main scope trumps the rule.
        r['foo'] = 'r'
        self.check('$foo', 'a', r)

        # ensure that the rule trumps the parent scope.
        c = Scope('child', self.scope)
        self.assertEqual(expand_vars('$foo', c, r), 'r')

    def test_multiple_vars(self):
        # ensure that we handle back-to-back variables properly
        # and don't insert any whitespace.
        self.check('$foo$bar', 'ab')
        self.check('$foo $bar', 'a b')

        # ensure that undefined variables are handled properly and
        # don't result in whitespace, either.
        self.check('$foo$baz$bar', 'ab')
