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

from typing import Any, cast

from pyn.pyn_exceptions import PynException

type Declaration = list[Any]
type Ast = list[Declaration]
type ParseResult = tuple[Any | None, int, str | None]


def parse(msg: str, fname: str) -> Ast:
    return NinjaParser(msg, fname).parse()


class NinjaParser:
    """Parse the contents of a .ninja file and return an AST."""

    def __init__(self, msg: str, fname: str) -> None:
        self.msg = msg
        self.end = len(msg)
        self.fname = fname

    def parse(self) -> Ast:
        msg = self.msg
        value, position, error = self.grammar_(0)
        if error is not None:
            lineno = 1
            colno = 1
            i = 0
            while i < position:
                if msg[i] == "\n":
                    lineno += 1
                    colno = 1
                else:
                    colno += 1
                i += 1

            pos_str = "%s:%d:%d" % (self.fname, lineno, colno)
            raise PynException("%s %s" % (pos_str, error))
        assert value is not None
        return cast(Ast, value)

    def expect(self, start: int, substr: str) -> ParseResult:
        l = len(substr)
        if (self.end - start) < l or self.msg[start : start + l] != substr:
            return None, start, 'expecting "%s" at %d' % (substr, start)
        return substr, start + l, None

    def grammar_(self, start: int) -> ParseResult:
        """((empty_line* decl)*:ds empty_line*) end -> ds"""
        ds = []
        err = None
        p = start
        while not err and p < self.end:
            while not err and p < self.end:
                _, p, err = self.empty_line_(p)
            orig_p = p
            if p < self.end:
                v, p, err = self.decl_(orig_p)
                if not err:
                    ds.append(v)
        if not err:
            return ds, p, err
        else:
            return None, p, err

    def decl_(self, start: int) -> ParseResult:
        """build | rule | subninja | include | pool | default | var"""
        v, p, err = self.build_(start)
        if not err:
            return v, p, None

        v, p, err = self.rule_(start)
        if not err:
            return v, p, None

        v, p, err = self.subninja_(start)
        if not err:
            return v, p, None

        v, p, err = self.include_(start)
        if not err:
            return v, p, None

        v, p, err = self.pool_(start)
        if not err:
            return v, p, None

        v, p, err = self.default_(start)
        if not err:
            return v, p, None

        v, p, err = self.var_(start)
        if not err:
            return v, p, None

        return (
            None,
            p,
            (
                "expecting one of 'build', 'rule', 'subninja', "
                "'include', 'pool', 'default', or a a variable name"
            ),
        )

    def build_(self, start: int) -> ParseResult:
        """ "build" ws paths:os ws? ':' ws name:rule
        explicit_deps:eds implicit_deps:ids order_only_deps:ods eol
        ws_vars:vs -> ['build', os, rule, eds, ids, ods, vs]"""
        p = start
        v, p, err = self.expect(p, "build")
        if err:
            return v, p, err

        _, p, err = self.ws_(p)
        if err:
            return None, p, err

        os, p, err = self.paths_(p)
        if err:
            return None, p, err

        _, p, _ = self.ws_(p)

        v, p, err = self.expect(p, ":")
        if err:
            return None, p, err

        _, p, _ = self.ws_(p)

        rule, p, err = self.name_(p)
        if err:
            return None, p, err

        eds, p, _ = self.explicit_deps_(p)

        ids, p, err = self.implicit_deps_(p)
        if err:
            return None, p, err

        ods, p, err = self.order_only_deps_(p)
        if err:
            return None, p, err

        _, p, err = self.eol_(p)
        if err:
            return None, p, err

        vs, p, _ = self.ws_vars_(p)

        return ["build", os, rule, eds, ids, ods, vs], p, None

    def rule_(self, start: int) -> ParseResult:
        """ "rule" ws name:n eol ws_vars:vs -> ['rule', n, vs]"""
        p = start

        _, p, err = self.expect(p, "rule")
        if err:
            return None, p, err

        _, p, err = self.ws_(p)
        if err:
            return None, p, err

        n, p, err = self.name_(p)
        if err:
            return None, p, err

        _, p, err = self.eol_(p)
        if err:
            return None, p, err

        vs, p, _ = self.ws_vars_(p)

        return ["rule", n, vs], p, None

    def ws_vars_(self, start: int) -> ParseResult:
        """(ws var)*:vs -> vs"""
        p = start
        vs = []
        err = None
        while p < self.end and not err:
            _, p, err = self.ws_(p)
            if not err:
                v, p, err = self.var_(p)
                if not err:
                    vs.append(v)
        return vs, p, None

    def var_(self, start: int) -> ParseResult:
        """name:n ws? '=' ws? value:v eol -> ['var', n, v]"""
        n, p, err = self.name_(start)
        if err:
            return None, p, err

        _, p, _ = self.ws_(p)

        _, p, err = self.expect(p, "=")
        if err:
            return None, p, err

        _, p, _ = self.ws_(p)

        v, p, _ = self.value_(p)

        _, p, _ = self.eol_(p)
        return ["var", n, v], p, None

    def value_(self, start: int) -> ParseResult:
        """(~'\n' (('$' ' ' -> ' ')|('$' '\n' ' '+ -> '')|anything))*:vs
        -> ''.join(vs)"""
        msg, p, end = self.msg, start, self.end

        vs = []
        # _, _, err = self.eol_(p)
        while p < end and msg[p] != "\n":
            if p < (end - 1) and msg[p : p + 2] == "$ ":
                vs.append(" ")
                p += 2
            elif p < (end - 1) and msg[p : p + 2] == "$\n":
                p += 2
                _, p, _ = self.ws_(p)
            else:
                vs.append(msg[p])
                p += 1
            # _, _, err = self.eol_(p)
        return "".join(vs), p, None

    def subninja_(self, start: int) -> ParseResult:
        """ "subninja" ws path:p eol -> ['subninja', p]"""
        v, p, err = self.expect(start, "subninja")
        if err:
            return v, p, err

        v, p, err = self.ws_(p)
        if err:
            return None, p, err

        v, p, err = self.path_(p)
        if err:
            return None, p, err

        _, p, err = self.eol_(p)
        return ["subninja", v], p, None

    def include_(self, start: int) -> ParseResult:
        """ "include" ws path:p eol -> ['include', p]"""
        v, p, err = self.expect(start, "include")
        if err:
            return v, p, err

        v, p, err = self.ws_(p)
        if err:
            return None, p, err

        v, p, err = self.path_(p)
        if err:
            return None, p, err

        _, p, err = self.eol_(p)
        return ["include", v], p, None

    def pool_(self, start: int) -> ParseResult:
        """ "pool" ws name:n eol ws_vars:vs -> ['pool', n, vs]"""
        _, p, err = self.expect(start, "pool")
        if err:
            return None, p, err
        _, p, err = self.ws_(p)
        if err:
            return None, p, err
        n, p, err = self.name_(p)
        if err:
            return None, p, err
        _, p, err = self.eol_(p)
        if err:
            return None, p, err
        vs, p, _ = self.ws_vars_(p)
        return ["pool", n, vs], p, None

    def default_(self, start: int) -> ParseResult:
        """ "default" ws paths:ps eol  -> ['default', ps]"""
        v, p, err = self.expect(start, "default")
        if err:
            return v, p, err

        v, p, err = self.ws_(p)
        if err:
            return None, p, err

        v, p, err = self.paths_(p)
        if err:
            return None, p, err

        _, p, err = self.eol_(p)
        return ["default", v], p, None

    def paths_(self, start: int) -> ParseResult:
        """path:hd (ws path)*:tl -> [hd] + tl"""
        hd, p, err = self.path_(start)
        tl = []
        if err:
            return None, p, err

        while p < self.end and not err:
            v, p, err = self.ws_(p)
            if not err:
                v, p, err = self.path_(p)
                if not err:
                    tl.append(v)
        return [hd] + tl, p, None

    def path_(self, start: int) -> ParseResult:
        """(('$' ' ')|(~(' '|':'|'='|'|'|eol) anything))+:vs -> ''.join(vs)"""
        msg, p, end = self.msg, start, self.end
        vs = []

        while p < end:
            c = msg[p]
            if c.isalpha():
                vs.append(c)
                p += 1
            elif c == " " or c == "\n":
                break
            elif c == "$" and (p < end - 1) and msg[p + 1] == " ":
                vs.append(" ")
                p += 2
            elif c in (":", "=", "|"):
                break
            else:
                vs.append(c)
                p += 1
        if len(vs) == 0:
            return None, start, "expecting a path"
        else:
            return "".join(vs), p, None

    def name_(self, start: int) -> ParseResult:
        """(letter|'_'):hd (letter|digit|'_')*:tl -> ''.join([hd] + tl)"""
        if start == self.end:
            return None, start, "expecting a name"

        msg, p, end = self.msg, start, self.end
        hd = msg[p]
        if not hd.isalpha() and hd != "_":
            return None, p, 'expecting a letter or "_" to start a name'
        p += 1
        tl = []
        while p < end:
            c = msg[p]
            if c.isalpha() or c.isdigit() or c == "_":
                tl.append(c)
                p += 1
            else:
                break
        return hd + "".join(tl), p, None

    def explicit_deps_(self, start: int) -> ParseResult:
        """ws? paths:ps -> ps | -> []"""
        if start == self.end:
            return [], start, None

        _, p, _ = self.ws_(start)

        ps, p, err = self.paths_(p)
        if err:
            return [], start, None
        return ps, p, None

    def implicit_deps_(self, start: int) -> ParseResult:
        """ws? (~'|' '|') '|' ws? paths:ps -> ps | -> []"""
        msg = self.msg
        if start == self.end:
            return [], start, None

        _, p, _ = self.ws_(start)

        if (p < self.end - 1) and msg[p] == "|" and msg[p + 1] == "|":
            return [], start, None

        _, p, err = self.expect(p, "|")
        if err:
            return [], start, None

        _, p, _ = self.ws_(p)

        ps, p, err = self.paths_(p)
        if err:
            return None, p, err
        return ps, p, None

    def order_only_deps_(self, start: int) -> ParseResult:
        """ws? '|' '|' ws? paths:ps -> ps | -> []"""
        if start == self.end:
            return [], start, None

        _, p, _ = self.ws_(start)

        _, p, err = self.expect(p, "||")
        if err:
            return [], start, None

        _, p, _ = self.ws_(p)

        ps, p, err = self.paths_(p)
        if err:
            return None, p, err
        return ps, p, None

    def empty_line_(self, start: int) -> ParseResult:
        """ws? comment? ('\n'|end)"""
        msg, p, end = self.msg, start, self.end
        _, p, _ = self.ws_(start)
        if p < end:
            _, p, _ = self.comment_(p)
        if p < end and msg[p] == "\n":
            return "\n", p + 1, None
        elif p >= end:
            return None, p, None
        return None, p, "expecting a '\n' or EOF"

    def eol_(self, start: int) -> ParseResult:
        """ws? ('\n' | end)"""
        msg, p, end = self.msg, start, self.end
        if start < end and (msg[start] == " " or msg[start] == "$"):
            _, p, _ = self.ws_(start)
        else:
            p = start
        if p < end:
            if msg[p] == "\n":
                return "\n", p + 1, None
            else:
                return None, p, "expecting a newline, or EOF"
        else:
            return None, p, None

    def ws_(self, start: int) -> ParseResult:
        """(' '|('$' '\n'))+"""
        msg, p, end = self.msg, start, self.end
        if start == end:
            return None, p, "expecting either ' ' or '$\n'"
        err = None
        while not err and p < end:
            if msg[p] == " ":
                p += 1
            elif msg[p] == "$" and p < end - 1 and msg[p + 1] == "\n":
                p += 2
            else:
                err = "expecting either ' ' or '$\n'"
        if p == start:
            return None, p, err
        else:
            return None, p, None

    def comment_(self, start: int) -> ParseResult:
        """'#' (~'\n' anything)* ('\n'|end)"""
        msg, p, end = self.msg, start, self.end
        if msg[p] != "#":
            return None, start, "expecting a '#'"
        p += 1
        while p < end and msg[p] != "\n":
            p += 1
        if p < end:
            return "\n", p, None
        else:
            return None, p, None
