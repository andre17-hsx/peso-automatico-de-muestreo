# Lector/escritor minimo de expresiones-S de KiCad y extraccion de simbolos de librerias.
import os
import re

SYMDIR = r"C:\Program Files\KiCad\10.0\share\kicad\symbols"
_TOK = re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()"]+')


def parse(text):
    stack, cur = [], []
    for t in _TOK.findall(text):
        if t == "(":
            stack.append(cur); cur = []
        elif t == ")":
            done = cur; cur = stack.pop(); cur.append(done)
        else:
            cur.append(t)
    return cur[0]


def dump(e, ind=0):
    if not isinstance(e, list):
        return e
    if all(not isinstance(x, list) for x in e):
        return "(" + " ".join(e) + ")"
    pad = "\t" * (ind + 1)
    head = [x for x in e if not isinstance(x, list)]
    out = "(" + " ".join(head)
    for x in e:
        if isinstance(x, list):
            out += "\n" + pad + dump(x, ind + 1)
    return out + "\n" + "\t" * ind + ")"


def q(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def uq(s):
    return s[1:-1] if s.startswith('"') else s


def find(e, key):
    return [x for x in e if isinstance(x, list) and x and x[0] == key]


_libs = {}


def lib(name):
    if name not in _libs:
        with open(os.path.join(SYMDIR, name + ".kicad_sym"), encoding="utf-8") as f:
            root = parse(f.read())
        _libs[name] = {uq(s[1]): s for s in find(root, "symbol")}
    return _libs[name]


def symbol(lib_id):
    """Simbolo listo para lib_symbols: con nombre 'Lib:Nombre' y 'extends' aplanado."""
    libname, name = lib_id.split(":")
    syms = lib(libname)
    s = syms[name]
    ext = find(s, "extends")
    if ext:
        parent = symbol(libname + ":" + uq(ext[0][1]))
        pname = uq(parent[1]).split(":")[1]
        props = {uq(p[1]): p for p in find(s, "property")}
        out = [s[0], q(lib_id)]
        for x in parent[2:]:
            if isinstance(x, list) and x[0] == "property" and uq(x[1]) in props:
                out.append(props.pop(uq(x[1])))
            elif isinstance(x, list) and x[0] == "symbol":
                sub = list(x); sub[1] = q(uq(x[1]).replace(pname, name, 1)); out.append(sub)
            else:
                out.append(x)
        out[2:2] = []
        for p in props.values():
            out.insert(len(out) - len([y for y in out if isinstance(y, list) and y[0] == "symbol"]), p)
        return out
    out = list(s)
    out[1] = q(lib_id)
    return out


def pins(sym):
    """{numero: (x, y, angulo)} en coordenadas de libreria (Y hacia arriba)."""
    res = {}

    def walk(e):
        for x in e:
            if isinstance(x, list):
                if x[0] == "pin":
                    at = find(x, "at")[0]
                    num = uq(find(x, "number")[0][1])
                    res[num] = (float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0)
                else:
                    walk(x)
    walk(sym)
    return res
