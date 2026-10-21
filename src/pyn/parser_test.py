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

import textwrap
import unittest

from parser import parse
from pyn_exceptions import PynException


class TestNinjaParser(unittest.TestCase):
    # unused argument 'files'  pylint:disable=W0613
    def check(self, text, ast, dedent=True, files=None):
        if dedent:
            dedented_text = textwrap.dedent(text)
            actual_ast = parse(dedented_text, 'build.ninja')
        else:
            actual_ast = parse(text, 'build.ninja')
        self.assertEqual(actual_ast, ast)

    def err(self, text, files=None):
        dedented_text = textwrap.dedent(text)
        self.assertRaises(PynException, parse, dedented_text,
                          'build.ninja')

    # pylint:enable=W0613

    def test_blanks(self):
        self.check('', [])
        self.check('  \n', [], dedent=False)
        self.check('  $\n\n', [], dedent=False)
        self.check('$\n\n', [], dedent=False)
        self.check('  $\n  $\n\n', [], dedent=False)
        self.check('\n', [])
        self.check('\n\n', [])

    def test_spaces_in_paths(self):
        self.check('build foo$ bar : cc foo.c',
                   [['build', ['foo bar'], 'cc', ['foo.c'],
                              [], [], []]])
        self.check('subninja foo$ bar',
                   [['subninja', 'foo bar']],
                   files={'foo bar': ''})

    def test_comments(self):
        self.check('# comment', [])
        self.check('# comment\n', [])
        self.check('\n# comment', [])

        # Note that end-of-line comments are *not* allowed
        self.check('cflags = -Wall # comment',
                   [['var', 'cflags', '-Wall # comment']])

        # Note here that the comment gets parsed as if it is two targets.
        self.check('build foo.o : cc foo.c # comment',
                   [['build', ['foo.o'], 'cc', ['foo.c', '#', 'comment'],
                     [], [], []]])

    def test_vars(self):
        self.check('f = bar', [['var', 'f', 'bar']])
        self.check('foo = bar', [['var', 'foo', 'bar']])
        self.check('foo=bar', [['var', 'foo', 'bar']])
        self.check('foo_123=bar', [['var', 'foo_123', 'bar']])
        self.check('foo = ba$ r', [['var', 'foo', 'ba r']])
        self.check('foo = ba$\n  r', [['var', 'foo', 'bar']])
        self.check('foo = ba $\n  r', [['var', 'foo', 'ba r']])
        self.check('foo = bar  \n', [['var', 'foo', 'bar  ']])

    def test_include(self):
        self.check('include foo.ninja', [['include', 'foo.ninja']],
                   files={'foo.ninja': ''})
        self.check('include foo.ninja\n', [['include', 'foo.ninja']],
                   files={'foo.ninja': ''})
        self.err('include')
        self.err('include ')

    def test_rule_errs(self):
        self.err('rulefoo')
        self.err('rule 1234')
        self.err('rule foo 1234')

    def test_subninja(self):
        self.check('subninja foo.ninja', [['subninja', 'foo.ninja']],
                   files={'foo.ninja': ''})
        self.check('subninja foo.ninja\n', [['subninja', 'foo.ninja']],
                   files={'foo.ninja': ''})
        self.err('subninja')
        self.err('subninja ')

    def test_pool(self):
        self.check('''pool foo
                        depth = 1
                   ''', [['pool', 'foo',
                          [['var', 'depth', '1']]]])
        self.err('pool')
        self.err('pool:')
        self.err('pool 123')
        self.err('pool foo bar')

    def test_syntax_err(self):
        self.err('syntaxerror')
        self.err('foo = 4\nsyntaxerror')

    def test_sample(self):
        self.check('''
                   cflags = -Wall

                   rule cc
                       command = gcc $cflags -c $in $
                                 -o $out
                       deps = gcc
                       depsfile = $out.d

                   build foo.o : cc foo.c
                   ''',
                   [['var', 'cflags', '-Wall'],
                    ['rule', 'cc',
                     [['var', 'command', 'gcc $cflags -c $in -o $out'],
                      ['var', 'deps', 'gcc'],
                      ['var', 'depsfile', '$out.d']]],
                    ['build', ['foo.o'], 'cc', ['foo.c'], [], [], []]])

    def test_simple_build(self):
        self.check('build foo.o : cc foo.c\n',
                   [['build', ['foo.o'], 'cc', ['foo.c'], [], [], []]])

    def test_simple_cflags(self):
        self.check('cflags = -Wall -O1\n',
                   [['var', 'cflags', '-Wall -O1']])

    def test_default(self):
        self.check('default foo bar\n',
                   [['default', ['foo', 'bar']]])
        self.check('default foo bar\n',
                   [['default', ['foo', 'bar']]])
        self.err('default')
        self.err('default ')

    def test_simple_rule(self):
        self.check('''
                   rule cc
                       command = gcc $cflags -c $in -o $out
                   ''',
                   [['rule', 'cc',
                     [['var', 'command', 'gcc $cflags -c $in -o $out']]]])

    def test_build_with_deps(self):
        self.check('build foo : cc',
                   [['build', ['foo'], 'cc', [], [], [], []]])
        self.check('build foo : cc | foo.h',
                   [['build', ['foo'], 'cc', [], ['foo.h'], [], []]])
        self.check('build foo : cc || foo.h',
                   [['build', ['foo'], 'cc', [], [], ['foo.h'], []]])
        self.check('build foo.o : cc foo.c | foo.h\n',
                   [['build', ['foo.o'], 'cc', ['foo.c'],
                              ['foo.h'], [], []]])
        self.check('build foo.o : cc foo.c || foo.idl\n',
                   [['build', ['foo.o'], 'cc', ['foo.c'],
                              [], ['foo.idl'], []]])
        self.check('build foo.o : cc foo.c | foo.h || foo.idl\n',
                   [['build', ['foo.o'], 'cc', ['foo.c'],
                              ['foo.h'], ['foo.idl'], []]])

    def test_build_errs(self):
        self.err('buildfoo')
        self.err('build ')
        self.err('build :')
        self.err('build foo.# foo')
        self.err('build foo.o : cc foo.c :')
        self.err('build foo.o|')
        self.err('build foo.o:|')
        self.err('build foo.o: cc foo.c |')
        self.err('build foo.o: cc foo.c ||')
        self.err('build foo.o:')
        self.err('build foo.o:|||')

    def test_no_space_between_output_and_colon(self):
        self.check('build foo.o: cc foo.c\n',
                   [['build', ['foo.o'], 'cc', ['foo.c'], [], [], []]])

    def test_trailing_dollar_sign(self):
        self.check('build foo.o: cc foo.c $\n\n',
                   [['build', ['foo.o'], 'cc', ['foo.c'], [], [], []]])

    def test_var_errs(self):
        # not a legal var name
        self.err('123')

        # no equals sign
        self.err('foo ')
