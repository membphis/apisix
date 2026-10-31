# SPDX-License-Identifier: Apache-2.0
"""Inputs for a differential check against the original third-party evaluator."""
from urllib.parse import urlencode


def cases():
    result = []
    for value in (None, "", "0", "02", "ABC", "abc", "-2", "x y"):
        for op, rhs in (("==", 2), ("~=", 2), (">", 0), ("<=", "2"), ("==", None),
                        ("in", ["abc", "02"]), ("~~", "^[a-z]+$"), ("~*", "^abc$")):
            args = {} if value is None else {"x": value}
            ctx = {} if value is None else {"arg_x": value}
            result.append(({"rule": [["arg_x", op, rhs]], "ctx": ctx}, urlencode(args)))
    result.extend([
        ({"rule": [["arg_x", "ABC"]], "ctx": {"arg_x": "abc"}}, "x=abc"),
        ({"rule": [["arg_missing", "=="]], "ctx": {}}, ""),
        ({"rule": ["OR", ["arg_x", "==", "a"], ["arg_x", "==", "b"]], "ctx": {"arg_x": "b"}}, "x=b"),
        ({"rule": ["!AND", ["arg_x", "==", "a"], ["arg_x", "==", "b"]], "ctx": {"arg_x": "a"}}, "x=a"),
        ({"rule": [["arg_x", "!", "==", "a"]], "ctx": {"arg_x": "a"}}, "x=a"),
        ({"rule": [["remote_addr", "ipmatch", ["127.0.0.0/8", "::1"]]], "ctx": {"remote_addr": "127.0.0.1"}}, ""),
    ])
    return result
