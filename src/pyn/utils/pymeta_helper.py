#/usr/bin/python
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

from __future__ import print_function

import argparse
import importlib
import os
import sys


class ParseError(Exception):
    pass


class ParserBase(object):
    name = None
    grammar = None

    def __init__(self, name=None, grammar=None,
                 src_dir=None, filename=None,
                 classname=None, force=False):
        self.grammar = grammar or self.grammar
        self.name = name or self.name
        self.src_dir = src_dir or os.path.dirname(os.path.abspath(__file__))
        self.basename = filename or self.name.lower() + '_parser.py'
        self.classname = classname or self.name.capitalize() + 'Parser'
        self.grammar_constant_name = self.name.upper() + '_GRAMMAR'
        self.filename = os.path.join(self.src_dir, self.basename)

        assert self.name
        assert self.grammar

        module_name = self.basename.replace('.py', '')
        if force or module_name not in sys.modules:
            if force or (self.generated_grammar() != self.grammar.strip()):
                self.generate_parser_module()
            self._module = importlib.import_module(module_name)
        else:
            self._module = sys.modules[module_name]

        self._cls = getattr(self._module, self.classname)

        # pylint: disable=W0212
        self._parse_error = self._module._MaybeParseError

    def parse(self, txt):
        try:
            return self._cls.parse(txt)
        except self._module.ParseError as e:
            raise ParseError(str(e))

    def generated_grammar(self):
        if not os.path.exists(self.filename):
            return None

        with open(self.filename) as fp:
            lines = fp.readlines()
            start = lines.index('%s = """\n' % self.grammar_constant_name)
            end = lines[start:].index('"""\n')
            txt = ''.join(lines[start+1:start + end])
            return txt.strip()

    def generate_parser_module(self):
        from pymeta.grammar import OMetaGrammar
        from pymeta import builder

        tree = OMetaGrammar(self.grammar).parseGrammar(self.classname,
                                                       builder.TreeBuilder)
        with open(os.path.join(self.src_dir, 'pymeta', 'runtime.py')) as fp:
            runtime_str = fp.read()
        with open(os.path.join(self.src_dir, self.filename), 'w') as fp:
            fp.write('# pylint: disable=C0103,C0301,C0302,R0201,'
                     'R0903,R0904,R0912,R0914\n\n')
            fp.write(runtime_str)
            fp.write('\n\n')
            fp.write('%s = """\n%s\n"""\n\n' % (self.grammar_constant_name,
                                                self.grammar))
            fp.write('GrammarBase = OMetaBase\n')
            fp.write('\n\n')

            parser_cls_code = builder.writePython(tree)
            fp.write(parser_cls_code)


def make_parser(grammar_file, name=None, _output=None, force=False):
    with open(grammar_file) as f:
        grammar = f.read()

    basename = os.path.basename(grammar_file).replace('.pymeta', '')
    name = name or basename.capitalize()
    return ParserBase(grammar=grammar, name=name, force=force)


def main(argv=None):
    parser = argparse.ArgumentParser(prog='pymeta_helper')
    parser.usage = '[options] grammar'
    parser.add_argument('grammar', nargs=1,
                        help=argparse.SUPPRESS)
    parser.add_argument('-o', metavar='FILE', dest='output',
                        help=('destination file (defaults to '
                              's/grammar.pymeta/grammar.py)'))
    parser.add_argument('-n', '--name',
                        help='base name of grammar')
    args = parser.parse_args(args=argv)

    try:
        make_parser(args.grammar[0], args.name, args.output, force=True)
    except IOError:
        print("Error: '%s' not found" % args.grammar, file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()
