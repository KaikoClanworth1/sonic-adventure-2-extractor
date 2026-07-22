"""Walk NJS_OBJECT trees in SA2 event files and build meshes."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from chunk import parse_vertex_chunks, parse_poly_chunks

EVENT_BASE = 0x8125FE60
OBJ_SIZE = 0x34
ATTACH_SIZE = 0x18


class Node:
    __slots__ = ("ptr", "offset", "flags", "attach_ptr", "pos", "rot", "scale",
                 "children", "name", "index", "parent", "world")

    def __init__(self):
        self.children = []
        self.name = ""
        self.index = -1
        self.parent = None


class Model:
    __slots__ = ("vlist_ptr", "plist_ptr", "center", "radius",
                 "vertices", "polys", "node")

    def __init__(self):
        self.vertices = {}
        self.polys = []
        self.node = None


class NinjaFile:
    def __init__(self, data, base=EVENT_BASE):
        self.d = data
        self.base = base
        self.n = len(data)

    def ok(self, p):
        # p == base means a NULL pointer in base-0 containers (MDL/MTN), which
        # must never be followed - it would parse from file offset 0.
        return p != 0 and self.base <= p < self.base + self.n

    def off(self, p):
        return p - self.base

    def u32(self, o): return struct.unpack_from(">I", self.d, o)[0]
    def i32(self, o): return struct.unpack_from(">i", self.d, o)[0]
    def f32(self, o): return struct.unpack_from(">f", self.d, o)[0]

    def read_object(self, ptr, seen=None, depth=0, order=None):
        if seen is None: seen = set()
        if order is None: order = []
        if not self.ok(ptr) or ptr in seen or depth > 64:
            return None
        o = self.off(ptr)
        if o + OBJ_SIZE > self.n:
            return None
        seen.add(ptr)
        nd = Node()
        nd.ptr = ptr
        nd.offset = o
        nd.flags = self.u32(o)
        nd.attach_ptr = self.u32(o + 4)
        nd.pos = (self.f32(o + 8), self.f32(o + 12), self.f32(o + 16))
        nd.rot = (self.i32(o + 0x14), self.i32(o + 0x18), self.i32(o + 0x1C))
        nd.scale = (self.f32(o + 0x20), self.f32(o + 0x24), self.f32(o + 0x28))
        nd.index = len(order)
        order.append(nd)
        child = self.u32(o + 0x2C)
        sibling = self.u32(o + 0x30)
        if self.ok(child):
            c = self.read_object(child, seen, depth + 1, order)
            while c is not None:
                c.parent = nd
                nd.children.append(c)
                c = c.__dict__.get("_next") if hasattr(c, "__dict__") else None
                break
            # siblings of the child chain
            cur = child
            guard = 0
            while True:
                guard += 1
                if guard > 4096: break
                co = self.off(cur)
                sib = self.u32(co + 0x30)
                if not self.ok(sib) or sib in seen:
                    break
                s = self.read_object(sib, seen, depth + 1, order)
                if s is None: break
                s.parent = nd
                nd.children.append(s)
                cur = sib
        return nd

    def read_tree(self, ptr):
        """Read an object tree in depth-first order (matching Ninja node index)."""
        order = []
        seen = set()

        def rec(p, parent, depth):
            if not self.ok(p) or p in seen or depth > 96:
                return
            o = self.off(p)
            if o + OBJ_SIZE > self.n:
                return
            pos = (self.f32(o + 8), self.f32(o + 12), self.f32(o + 16))
            scale = (self.f32(o + 0x20), self.f32(o + 0x24), self.f32(o + 0x28))
            # Reject structurally impossible nodes: a real NJS_OBJECT never has
            # non-finite or astronomically large position/scale. Without this a
            # single garbage node poisons every descendant's world transform.
            for c in pos + scale:
                if c != c or abs(c) > 1e6:
                    return
            seen.add(p)
            nd = Node()
            nd.ptr = p
            nd.offset = o
            nd.flags = self.u32(o)
            nd.attach_ptr = self.u32(o + 4)
            nd.pos = pos
            nd.rot = (self.i32(o + 0x14), self.i32(o + 0x18), self.i32(o + 0x1C))
            nd.scale = scale
            nd.parent = parent
            nd.index = len(order)
            order.append(nd)
            if parent is not None:
                parent.children.append(nd)
            child = self.u32(o + 0x2C)
            sibling = self.u32(o + 0x30)
            if self.ok(child):
                rec(child, nd, depth + 1)
            if self.ok(sibling):
                rec(sibling, parent, depth)

        rec(ptr, None, 0)
        return order

    def read_model(self, attach_ptr):
        if not self.ok(attach_ptr):
            return None
        a = self.off(attach_ptr)
        if a + ATTACH_SIZE > self.n:
            return None
        m = Model()
        m.vlist_ptr = self.u32(a)
        m.plist_ptr = self.u32(a + 4)
        m.center = (self.f32(a + 8), self.f32(a + 12), self.f32(a + 16))
        m.radius = self.f32(a + 20)
        if self.ok(m.vlist_ptr):
            m.vertices = parse_vertex_chunks(self.d, self.off(m.vlist_ptr))
        if self.ok(m.plist_ptr):
            m.polys = parse_poly_chunks(self.d, self.off(m.plist_ptr))
        return m
