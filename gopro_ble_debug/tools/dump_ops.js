// Dump ops.js tables as JSON (run under jsc from www/).
var window = globalThis;
load('ops.js');
const o = window.OPSDEF;
print(JSON.stringify({ CH: o.CH, RSP_OF: o.RSP_OF, PACKETIZED: [...o.PACKETIZED], OPS: o.OPS, PB_RX: o.PB_RX, CMD_RSP: o.CMD_RSP }));
