"""policy_net.py -- the RL networks, for the sketch's runner (policy_run.cpp).

firmware-dev runs omniRL's policies on the ESP32-S3 (policy_core.c pol_forward:
dense layers, ELU between them). The UNO Q's MCU has no room for their weights
(0.13 to 0.77 MB each), so it keeps the files without them (port_from_esp.py)
and asks here at every policy step (policy_remote.cpp):

  pol_fwd(id, seq, flag, obs)  -> 12 float32 (the raw action)
      id    FNV-1a of the policy's name (the header's)
      flag  0: obs is this step's observation (45 floats), pushed on the history
            1: the same, a fresh start: every slot of the history is it
            2: the whole stacked input came first (pol_obs_put pieces)
  pol_obs_put(offset, bytes)   -> True

The history (contract.py HistoryStacker, policy_core.c pol_hist_stack): each
term's frames oldest to newest, the terms concatenated."""
import glob
import os
import struct
import threading

import numpy as np

from arduino.app_utils import Bridge

HERE = os.path.dirname(os.path.abspath(__file__))
OBS = 45
ACT = 12
TERMS = (0, 3, 6, 9, 21, 33, 45)


def fnv1a(name):
    x = 2166136261
    for c in name.encode():
        x = ((x ^ c) * 16777619) & 0xFFFFFFFF
    return x


def to_i32(u):
    return u - (1 << 32) if u >= (1 << 31) else u


class Net:
    def __init__(self, path):
        data = open(path, "rb").read()
        magic, ver = struct.unpack_from("<II", data, 0)
        if magic != 0x5058504D:
            raise ValueError("not an MPXP file")
        self.name = data[16:64].split(b"\0")[0].decode()
        n_layers = struct.unpack_from("<I", data, 80)[0]
        dims = struct.unpack_from("<9I", data, 84)[:n_layers + 1]
        self.history = max(1, struct.unpack_from("<I", data, 132)[0])
        self.layers = []
        off = 640
        for l in range(n_layers):
            i, o = dims[l], dims[l + 1]
            W = np.frombuffer(data, dtype="<f4", count=i * o, offset=off).reshape(o, i)
            off += 4 * i * o
            b = np.frombuffer(data, dtype="<f4", count=o, offset=off)
            off += 4 * o
            self.layers.append((W.astype(np.float32), b.astype(np.float32)))
        self.frames = np.zeros((self.history, OBS), dtype=np.float32)
        self.dims = dims

    def forward(self, x):
        x = x.astype(np.float32)
        last = len(self.layers) - 1
        for l, (W, b) in enumerate(self.layers):
            x = W @ x + b
            if l < last:
                x = np.where(x > 0, x, np.expm1(np.minimum(x, 0))).astype(np.float32)   # ELU
        return x

    def stacked(self):
        L = self.history
        return np.concatenate([self.frames[:, TERMS[t]:TERMS[t + 1]].reshape(-1) for t in range(6)])

    def unstack(self, x):
        L = self.history
        o = 0
        for t in range(6):
            n = TERMS[t + 1] - TERMS[t]
            self.frames[:, TERMS[t]:TERMS[t + 1]] = x[o:o + n * L].reshape(L, n)
            o += n * L


class Policies:
    def __init__(self, log):
        self.log = log
        self.nets = {}
        self.lock = threading.Lock()
        self.staging = bytearray()
        for p in sorted(glob.glob(os.path.join(HERE, "policies", "*.mpxp"))):
            try:
                n = Net(p)
                self.nets[to_i32(fnv1a(n.name))] = n
                self.nets[fnv1a(n.name)] = n
            except Exception as e:
                log("policy %s not loaded: %s" % (os.path.basename(p), e))
        names = sorted({n.name for n in self.nets.values()})
        log("RL networks on Linux: %s" % (", ".join(names) or "none"))
        Bridge.provide("pol_fwd", self.fwd)
        Bridge.provide("pol_obs_put", self.obs_put)

    def obs_put(self, offset, data):
        data = bytes(data)
        with self.lock:
            offset = int(offset)
            if offset == 0:
                self.staging = bytearray()
            if offset != len(self.staging):
                return False
            self.staging += data
        return True

    def fwd(self, net_id, seq, flag, obs):
        net = self.nets.get(int(net_id))
        if net is None:
            return b""
        flag = int(flag)
        with self.lock:
            if net.history <= 1:
                x = np.frombuffer(bytes(obs), dtype="<f4")
                if x.size != net.dims[0]:
                    return b""
            else:
                if flag == 2:
                    x = np.frombuffer(bytes(self.staging), dtype="<f4")
                    if x.size != net.dims[0]:
                        return b""
                    net.unstack(x)
                else:
                    f = np.frombuffer(bytes(obs), dtype="<f4")
                    if f.size != OBS:
                        return b""
                    if flag == 1:
                        net.frames[:] = f
                    else:
                        net.frames[:-1] = net.frames[1:]
                        net.frames[-1] = f
                x = net.stacked()
            return net.forward(x).astype("<f4").tobytes()
