#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes a trivial ONNX model that fulfils AudioStudio's model contract.

    python3 tools/make_test_model.py test.onnx [--samples N]

Input "audio": float32 [1, N] (N = 16000 by default; --samples 0 leaves it
open as [1, "N"]), samples of 16 kHz audio in -1..1.  Output "scores":
float32 [1, 3], raw scores (not probabilities), for the classes in the
metadata key "labels": "silence,low,high".  The scores are linear in the
logarithm of the mean square of the samples (the energy, e) and of the
mean square of their first difference (d), whose ratio d/e is 4 sin^2(pi
f / 16000) for a sine of frequency f:

    silence  -ln e - 14            (above about -60 dBFS RMS it loses)
    low/high  0.5 (ln e + 14) -/+ 2 (ln(d/e) + 1.88)   (split near 1 kHz)

Needs nothing but the Python standard library: the protobuf is written by
hand (onnx.proto3, IR version 7, opset 13).
"""

import argparse
import struct
import sys


def varint(n):
    out = bytearray()
    n &= (1 << 64) - 1
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def field_varint(number, value):
    return varint(number << 3) + varint(value)


def field_bytes(number, data):
    if isinstance(data, str):
        data = data.encode()
    return varint(number << 3 | 2) + varint(len(data)) + data


def field_float(number, value):
    return varint(number << 3 | 5) + struct.pack("<f", value)


# TensorProto.DataType
FLOAT = 1
INT64 = 7
# AttributeProto.AttributeType
ATTR_INT = 2
ATTR_INTS = 7


def tensor(name, dtype, dims, raw):
    return (b"".join(field_varint(1, d) for d in dims) + field_varint(2, dtype)
            + field_bytes(8, name) + field_bytes(9, raw))


def floats(name, dims, values):
    return tensor(name, FLOAT, dims, struct.pack(f"<{len(values)}f", *values))


def int64s(name, values):
    return tensor(name, INT64, [len(values)], struct.pack(f"<{len(values)}q", *values))


def attr_int(name, value):
    return field_bytes(1, name) + field_varint(3, value) + field_varint(20, ATTR_INT)


def attr_ints(name, values):
    return field_bytes(1, name) + b"".join(field_varint(8, v) for v in values) + field_varint(20, ATTR_INTS)


def node(op, inputs, outputs, attrs=()):
    return (b"".join(field_bytes(1, i) for i in inputs) + b"".join(field_bytes(2, o) for o in outputs)
            + field_bytes(3, outputs[0]) + field_bytes(4, op) + b"".join(field_bytes(5, a) for a in attrs))


def value_info(name, dims):
    """A float tensor; a dimension given as a string is symbolic."""
    shape = b"".join(field_bytes(1, field_bytes(2, d) if isinstance(d, str) else field_varint(1, d))
                     for d in dims)
    tensor_type = field_varint(1, FLOAT) + field_bytes(2, shape)
    return field_bytes(1, name) + field_bytes(2, field_bytes(1, tensor_type))


def model(samples):
    n = samples if samples > 0 else "N"
    nodes = [
        node("Mul", ["audio", "audio"], ["sq"]),
        node("ReduceMean", ["sq"], ["e"], [attr_ints("axes", [1]), attr_int("keepdims", 1)]),
        node("Slice", ["audio", "one", "big", "axis1"], ["tail"]),
        node("Slice", ["audio", "zero", "minus_one", "axis1"], ["head"]),
        node("Sub", ["tail", "head"], ["diff"]),
        node("Mul", ["diff", "diff"], ["dsq"]),
        node("ReduceMean", ["dsq"], ["d"], [attr_ints("axes", [1]), attr_int("keepdims", 1)]),
        node("Concat", ["e", "d"], ["features"], [attr_int("axis", 1)]),
        node("Add", ["features", "epsilon"], ["features_eps"]),
        node("Log", ["features_eps"], ["log_features"]),
        node("MatMul", ["log_features", "weights"], ["linear"]),
        node("Add", ["linear", "bias"], ["scores"]),
    ]
    initializers = [
        int64s("zero", [0]), int64s("one", [1]), int64s("minus_one", [-1]),
        int64s("big", [2 ** 62]), int64s("axis1", [1]),
        floats("epsilon", [1], [1e-10]),
        # Rows: ln e, ln d; columns: silence, low, high.
        floats("weights", [2, 3], [-1.0, 2.5, -1.5,
                                   0.0, -2.0, 2.0]),
        floats("bias", [3], [-14.0, 3.24, 10.76]),
    ]
    graph = (b"".join(field_bytes(1, x) for x in nodes) + field_bytes(2, "energy_bands")
             + b"".join(field_bytes(5, t) for t in initializers)
             + field_bytes(11, value_info("audio", [1, n]))
             + field_bytes(12, value_info("scores", [1, 3])))
    opset = field_bytes(1, "") + field_varint(2, 13)
    labels = field_bytes(1, "labels") + field_bytes(2, "silence,low,high")
    return (field_varint(1, 7) + field_bytes(2, "AudioStudio make_test_model.py")
            + field_bytes(7, graph) + field_bytes(8, opset) + field_bytes(14, labels))


def main():
    parser = argparse.ArgumentParser(description="Write a trivial model that fulfils AudioStudio's contract")
    parser.add_argument("output", help="the .onnx file to write")
    parser.add_argument("--samples", type=int, default=16000,
                        help="samples per window; 0 leaves the length open (default 16000)")
    args = parser.parse_args()
    if args.samples < 0:
        print("make_test_model: --samples must be 0 or more", file=sys.stderr)
        return 2
    with open(args.output, "wb") as f:
        f.write(model(args.samples))
    return 0


if __name__ == "__main__":
    sys.exit(main())
