#!/usr/bin/env python3
import json
import pathlib
import struct
import sys

path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'gekkopak_guest_trace.jsonl')
trace = [json.loads(x) for x in path.read_text().splitlines() if x.strip()]
expected = ['HELLO','GET_CAPS','ALLOC','UPLOAD','SUBMIT','POLL','POLL','COLLECT','FREE','COMPLETE']
assert [x['cmd'] for x in trace] == expected, [x['cmd'] for x in trace]
assert all(x['result'] == 0 for x in trace)
assert trace[3]['payload_len'] == 16
assert trace[5]['out'][0] == 0 and trace[6]['out'][0] == 1

data = struct.pack('<IIII', 0x11223344, 0x55667788, 0xAABBCCDD, 0x0BADF00D)
h = 2166136261
for byte in data:
    h ^= byte
    h = (h * 16777619) & 0xffffffff
assert h == trace[7]['out'][2]

print('GekkoPAK Azahar E2E: PASS')
print(f'  transactions : {len(trace)}')
print(f'  checksum     : 0x{h:08x}')
print(f'  modeled time : {trace[7]["out"][0]} us')
print(f'  speedup      : {trace[7]["out"][1] / 1000:.3f}x')
