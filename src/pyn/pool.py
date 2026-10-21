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

import threading
import Queue


Empty = Queue.Empty  # "Invalid name" pylint: disable=C0103


class Pool(object):
    def __init__(self, num_processes, callback):
        self.num_processes = num_processes
        self.callback = callback
        self.requests = Queue.Queue()
        self.responses = Queue.Queue()
        self.workers = []
        for _ in range(num_processes):
            w = threading.Thread(target=_loop,
                                 args=(callback, self.requests,
                                       self.responses))
            w.start()
            self.workers.append(w)

    def send(self, msg):
        self.requests.put(msg)

    def get(self, block=True, timeout=None):
        return self.responses.get(block, timeout)

    def close(self):
        for _ in self.workers:
            self.requests.put(None)

    def join(self):
        for w in self.workers:
            w.join()


def _loop(callback, requests, responses):
    while True:
        args = requests.get(block=True)
        if not args:
            break
        resp = callback(args)
        responses.put(resp)
