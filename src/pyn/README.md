# pyn

pyn is a Python-based implementation of
[Ninja](http://martine.github.io/ninja/).

It is pronounced "pin." The name comes from "PYthon Ninja." Not coincidentally,
it also rhymes with the GN frontend to Ninja being developed by the Chromium
project.

Ninja is a brilliantly designed and well-implemented build system.

pyn is intended largely as an exercise to see how hard it is to (re-)implement
a full-fidelity version of Ninja, and to do it clearly, in as few lines as
possible. pyn should be a drop-in replacement for ninja.

The primary intent is to be clear, not fast (use Ninja itself if you want to be
fast); however, it is also intended to be a reference to compare how much
slower a (theoretically) well-written Python program really is to a
well-written C++ program that has been tuned for speed. Eventually there may be
multiple implementations of pyn designed for different goals in mind.

pyn is also designed to be more easily reusable and extensible, for uses
where you might want to parse and repurpose ninja files.

One version of pyn (the ometa_parser branch) uses
[PyMeta 2](https://bitbucket.org/wkornewald/pymeta/src),
as a parser generator. PyMeta 2 is an implementation of
[OMeta](https://github.com/alexwarth/ometa-js) in Python.

pyn is licensed under the Apache Source License. For more information see
the LICENSE file in the source repo.

## Additional functionality:

* '-t check' will report if the syntax of the build files is correct
* '-t question' will report if the build is up to date.
* '-t rules' will dump the list of rules in the graph
* --no-overwrite-mode will cause pyn to act as if stdout is not a terminal
  (every message will appear on its own line). (and --overwrite-mode
  will force pyn to treat the output as if it is a terminal).

 
## Things that still need to be implemented:

* support for the -k flag for load throttling
* support for pools
* restat/generator support
* rspfiles
* NINJA_STATUS option %c 
* various debug modes (-d stats, explain, keeprsp)
* various tools (-t browse, graph, compdb, recompact)
* suggest alternatives that fuzzy match the requested targets when the
  requested targets are missing.

## Also

* add hash-based build tracking in addition to mtime-based tracking?
* more tests
