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

import argparse


def parse_args(host, argv, version, tool_names):

    class ReturningArgParser(argparse.ArgumentParser):
        returncode = None

        # 'Redefining built-in "file" pylint: disable=W0622
        def print_help(self, file=None):
            super(ReturningArgParser, self).print_help(file or host.stdout)

        def error(self, message):
            self.exit(2, message)

        def exit(self, status=0, message=None):
            self.returncode = status
            if message:
                host.print_err(message)

    overwrite_by_default = host.stderr.isatty()

    ap = ReturningArgParser(prog='pyn')
    ap.usage = '%(prog)s [options] [targets...]'
    ap.description = ("if targets are unspecified, builds the 'default' "
                      "targets (see manual).")
    ap.add_argument('--version', action='store_true',
                    help='print pyn version ("%s")' % version)
    ap.add_argument('-C', metavar='DIR', dest='dir',
                    help='change to DIR before doing anything else')
    ap.add_argument('-f', metavar='FILE', dest='file', default='build.ninja',
                    help='specify input build file [default=%(default)s]')
    ap.add_argument('-j', metavar='N', type=int, dest='jobs',
                    default=host.cpu_count(),
                    help=('run N jobs in parallel [default=%(default)s, '
                          'derived from CPUs available]'))
    ap.add_argument('-l', metavar='N', type=float,
                    help=('do not start new jobs if the load average '
                          'is greater than N'))
    ap.add_argument('-k', metavar='N', type=int, dest='errors', default=1,
                    help='keep going until N jobs fail [default=default]')
    ap.add_argument('-n', action='store_true', dest='dry_run',
                    help=('dry run (don\'t run commands but act like they '
                          'succeeded)'))
    ap.add_argument('-v', action='count', dest='verbose', default=0,
                    help='show all command lines while building')
    ap.add_argument('-d', metavar='MODE', dest='debug',
                    help='enable debugging (use -d list to list modes)')
    ap.add_argument('-t', metavar='TOOL', dest='tool',
                    help='run a subtool (use -t list to list subtools)')
    ap.add_argument('--overwrite-status', action='store_true',
                    default=overwrite_by_default,
                    help=('status updates will overwrite each other%s' %
                          ' (on by default)' if overwrite_by_default else ''))
    ap.add_argument('--no-overwrite-status', action='store_false',
                    dest='overwrite_status',
                    help=('status updates will not overwrite each other%s' %
                          '' if overwrite_by_default else ' (off by default)'))
    ap.add_argument('targets', nargs='*', default=[],
                    help=argparse.SUPPRESS)
    args = ap.parse_args(args=argv)
    if ap.returncode is not None:
        return ap.returncode, None

    if args.debug:
        host.print_err('-d is not supported yet')
        return 2, None
    if args.tool and args.tool not in tool_names:
        host.print_err('unsupported tool "%s"' % args.tool)
        return 2, None

    return None, args
