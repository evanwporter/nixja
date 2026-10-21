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

import os
import textwrap
import unittest
# import StringIO

# import main
import main_test
import parser_test

from host import Host


PATH_TO_THIS_MODULE = os.path.abspath(__file__)


#def run_under_coverage(argv, host=None):
#    host = host or Host()
#    host.stdout = StringIO.StringIO()
#    host.stderr = StringIO.StringIO()
#    returncode = main.main(host, argv)
#    return returncode, host.stdout.getvalue(), host.stderr.getvalue()


def path_to_main():
    path = os.path.join(os.path.dirname(PATH_TO_THIS_MODULE), 'main.py')
    return path.replace(' ', '\\ ')


class IntegrationTestMixin(object):
    def _files_to_ignore(self):
        # return ['.ninja_deps', '.ninja_log']
        return ['.pyn.db']

    def _host(self):
        return Host()

    def _call(self, host, args):
        cmd_prefix = [host.python_interpreter, path_to_main()]
        # return run_under_coverage(args)
        return host.call(' '.join(cmd_prefix + args))


class IntegrationTestArgs(IntegrationTestMixin, main_test.TestArgs):
    pass


class IntegrationTestBuild(IntegrationTestMixin, main_test.TestBuild):
    pass


class IntegrationTestTools(IntegrationTestMixin, main_test.TestTools):
    pass


class IntegrationTestNinjaParser(parser_test.TestNinjaParser):
    def _call(self, text, files):
        host = Host()
        cmd_prefix = [host.python_interpreter, path_to_main()]

        orig_wd = host.getcwd()
        try:
            tmpdir = str(host.mkdtemp())
            host.chdir(tmpdir)
            host.write('build.ninja', text)
            for path, contents in list(files.items()):
                host.write(path, contents)

            cmd = cmd_prefix + ['-t', 'check']
            # return run_under_coverage(['-t', 'check'], host=host)
            return host.call(' '.join(cmd))
        finally:
            host.rmtree(tmpdir)
            host.chdir(orig_wd)

    def check(self, text, _ast, dedent=True, files=None):
        files = files or {}
        if dedent:
            dedented_text = textwrap.dedent(text)
            returncode, _, _ = self._call(dedented_text, files)
        else:
            returncode, _, _ = self._call(text, files)

        # Note that we ignore what the AST is.
        self.assertEqual(returncode, 0)

    def err(self, text, files=None):
        files = files or {}
        dedented_text = textwrap.dedent(text)
        returncode, _, _ = self._call(dedented_text, files)
        self.assertNotEquals(returncode, 0)


if __name__ == '__main__':
    unittest.main()
