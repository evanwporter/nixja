#!/usr/bin/python

import cProfile
import pstats

from host import Host
from main import main

def profile():
    return main(Host())

if __name__ == '__main__':
    cProfile.run('profile()', '.profiler_data')
    p = pstats.Stats('.profiler_data')
    p.strip_dirs()
    p.sort_stats('cumtime')
    p.print_stats(30)
