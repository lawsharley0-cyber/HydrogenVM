"""Pinned Clash ByteStream primitives (not protobuf or LEB128).

Booleans pack low bit first until any byte-oriented operation resets alignment.
LogicLong helper fields use two VInts; ByteStream::readLong uses fixed BE ints.
"""
import struct

MAX_PAYLOAD = 1 << 20


def vint(value):
    if not -(1 << 31) <= value < (1 << 31):
        raise ValueError('VInt exceeds signed 32-bit range')
    negative = value < 0
    # The pinned native writer reserves the sign separately. Five-byte values
    # carry only four bits in their last byte; ordinary signed LEB128 does not.
    magnitude = abs(value)
    size = next((i+1 for i,bits in enumerate((6,13,20,27)) if magnitude < (1<<bits)),5)
    encoded = value & 0x7fffffff
    out = bytearray([(encoded & 63) | (64 if negative else 0) | (128 if size>1 else 0)])
    for i in range(1,size):
        byte = (encoded >> (6+(i-1)*7)) & (15 if i==4 else 127)
        out.append(byte | (128 if i<size-1 else 0))
    return bytes(out)


def string(value):
    if value is None:
        return struct.pack('>i', -1)
    data = value.encode('utf-8')
    if len(data) > MAX_PAYLOAD:
        raise ValueError('String exceeds payload limit')
    return struct.pack('>i', len(data)) + data


class Reader:
    def __init__(self, data):
        self.data, self.offset, self.bit, self.boolean_byte = data, 0, 0, 0

    def read(self, count):
        if count < 0 or self.offset + count > len(self.data):
            raise ValueError('Truncated message')
        self.bit = 0
        result = self.data[self.offset:self.offset + count]
        self.offset += count
        return result

    def integer(self):
        return struct.unpack('>i', self.read(4))[0]

    def string(self):
        size = self.integer()
        if size == -1:
            return None
        if size < 0 or size > MAX_PAYLOAD:
            raise ValueError('Invalid string length')
        return self.read(size).decode('utf-8')

    def vint(self):
        first = byte = self.read(1)[0]
        value, shift = first & 63, 6
        while byte & 128:
            if shift >= 34:
                raise ValueError('Overlong VInt')
            byte = self.read(1)[0]
            if shift == 27 and byte > 15:
                raise ValueError('VInt fifth byte exceeds 31 data bits')
            value |= (byte & 127) << shift
            shift += 7
        if first & 64:
            value |= -(1 << min(shift,31))
        if not -(1 << 31) <= value < (1 << 31):
            raise ValueError('VInt exceeds signed 32-bit range')
        return value

    def boolean(self):
        if self.bit == 0:
            self.boolean_byte = self.read(1)[0]
        value = bool(self.boolean_byte & (1 << self.bit))
        self.bit = (self.bit + 1) & 7
        return value

    def reference(self):
        table = self.vint()
        if table == 0:
            return 0
        index = self.vint()
        if not 1 <= table <= 999 or not 0 <= index < 1000000:
            raise ValueError('Invalid data reference')
        return table * 1000000 + index


class Writer:
    def __init__(self):
        self.data, self.bit = bytearray(), 0

    def raw(self, data):
        self.bit = 0
        self.data.extend(data)

    def vint(self, value):
        self.raw(vint(value))

    def boolean(self, value):
        if type(value) is not bool:
            raise ValueError('Boolean value required')
        if self.bit == 0:
            self.data.append(0)
        if value:
            self.data[-1] |= 1 << self.bit
        self.bit = (self.bit + 1) & 7

    def string(self, value):
        self.raw(string(value))

    def reference(self, data):
        if not (data == 0 or 1000000 <= data <= 999999999):
            raise ValueError('Invalid data reference')
        self.vint(data // 1000000)
        if data:
            self.vint(data % 1000000)

    def timer(self, value):
        for name in ('ticks', 'remaining', 'timestamp'):
            self.vint(value[name])
