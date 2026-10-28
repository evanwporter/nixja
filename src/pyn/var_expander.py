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

from pyn.build_graph import Scope
from pyn.pyn_exceptions import PynException

type ParseResult = tuple[str | None, int, str | None]


def expand_vars(msg: str, scope: Scope, rule_scope: Scope | None = None) -> str:
    return VarExpander(scope, rule_scope).parse(msg)


class VarExpander:
    """Expand the variables in a string.

    grammar = chunk*:cs end         -> ''.join(cs)
    chunk   = ~'$' anything:c       -> c
            | '$' (' '|':'|'$'):c   -> c
            | '$' '{' varname:v '}' -> scope[v]
            | '$' varname:v         -> scope[v]
    varname = (letter|'_')+:ls      -> ''.join(ls)
    """

    def __init__(self, scope: Scope, rule_scope: Scope | None = None) -> None:
        self.scope = scope
        self.rule_scope = rule_scope

    def parse(self, msg: str) -> str:
        value, position, error = self.grammar_(msg, 0, len(msg))
        if error is not None:
            raise PynException("%s at %d" % (error, position))
        assert value is not None
        return value

    def lookup(self, var: str) -> str:
        if var in self.scope.objs:
            return self.scope.objs[var]
        if self.rule_scope and var in self.rule_scope.objs:
            return self.rule_scope.objs[var]
        if self.scope.parent:
            return self.scope.parent[var]
        return ""

    def grammar_(self, msg: str, start: int, end: int) -> ParseResult:
        values: list[str] = []
        if start == end:
            return "", end, None
        value, position, error = self.chunk_(msg, start, end)
        if error is None and value:
            values.append(value)
        while error is None and position < end:
            value, position, error = self.chunk_(msg, position, end)
            if error is None and value:
                values.append(value)
        if error is not None:
            return None, position, error
        return "".join(values), position, None

    def chunk_(self, msg: str, start: int, end: int) -> ParseResult:
        if msg[start] == "$":
            if end - start == 1:
                return (None, start + 1, "expecting a varname or a '{'")
            elif msg[start + 1] in (" ", ":", "$"):
                return (msg[start + 1], start + 2, None)
            elif msg[start + 1] == "{":
                value, position, error = self.varname_(msg, start + 2, end)
                if error is not None:
                    return None, position, error
                assert value is not None
                if position > end - 1:
                    return None, position, "expecting a closing }"
                if msg[position] == "}":
                    return self.lookup(value), position + 1, None
                else:
                    return None, position, "expecting a closing }"
            else:
                value, position, error = self.varname_(msg, start + 1, end)
                if error is not None:
                    return None, position, error
                assert value is not None
                return self.lookup(value), position, None
        else:
            return msg[start], start + 1, None

    def varname_(self, msg: str, start: int, end: int) -> ParseResult:
        values: list[str] = []
        position = start
        while position < end and (msg[position].isalpha() or msg[position] == "_"):
            values.append(msg[position])
            position += 1
        if position > start:
            return "".join(values), position, None
        return None, start, "expecting a varname"
