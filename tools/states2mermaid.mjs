#!/usr/bin/env node
// states2mermaid - decode a GSystem state machine (*.states, chunk family 0x05) and print it as JSON or Mermaid.
//
//   0x05000000 machine
//     0x05010000 state list
//       0x05010100 state (44 B): name[32] kind:u32 param:u32 layout:i8 x, i8 y, u8 w, u8 h
//         kind 0 = named state, 1 = unnamed junction/choice, 2 = timed (param = f32 seconds),
//         6 = timed variant (Intro), 8 = entry pseudo-state
//     0x05020000 transition list
//       0x05020100 transition (48 B): name[32] kind:u32 branch:u32 from:u16 to:u16 layout[4]
//         kind 0 = edge from -> to; on an unnamed junction `branch` selects the outcome (1, 2, ...)
//         kind 1 = "return to previous state" edge, only `from` is meaningful (from == to)
//
// Usage: node states2mermaid.mjs <file.states> [--json]

import fs from 'node:fs';

const TAG = { MACHINE: 0x05000000, STATES: 0x05010000, STATE: 0x05010100, TRANSITIONS: 0x05020000, TRANSITION: 0x05020100 };

function chunks(buf, start, end) {
  const out = [];
  for (let pos = start; pos < end; ) {
    const tag = buf.readUInt32LE(pos);
    const size = buf.readUInt32LE(pos + 4);
    out.push({ tag, body: pos + 8, size });
    pos += 8 + size;
  }
  return out;
}

const name32 = (buf, at) => buf.toString('latin1', at, at + 32).split('\0')[0];

export function readStates(buf) {
  const [root] = chunks(buf, 0, buf.length);
  if (root.tag !== TAG.MACHINE) throw new Error('not a state machine file');
  const machine = { states: [], transitions: [] };
  for (const list of chunks(buf, root.body, root.body + root.size)) {
    for (const c of chunks(buf, list.body, list.body + list.size)) {
      if (c.tag === TAG.STATE) {
        const kind = buf.readUInt32LE(c.body + 32);
        const state = { id: machine.states.length, name: name32(buf, c.body), kind };
        if (kind & 2) state.seconds = +buf.readFloatLE(c.body + 36).toFixed(3);
        machine.states.push(state);
      } else if (c.tag === TAG.TRANSITION) {
        machine.transitions.push({
          name: name32(buf, c.body),
          kind: buf.readUInt32LE(c.body + 32),
          branch: buf.readUInt32LE(c.body + 36),
          from: buf.readUInt16LE(c.body + 40),
          to: buf.readUInt16LE(c.body + 42),
        });
      } else {
        throw new Error(`unexpected chunk 0x${c.tag.toString(16)}`);
      }
    }
  }
  return machine;
}

function toMermaid({ states, transitions }) {
  const id = (s) => `s${s}`;
  const lines = ['stateDiagram-v2'];
  for (const s of states) {
    if (s.kind === 8) lines.push(`  state "entry" as ${id(s.id)}`);
    else if (!s.name) lines.push(`  state ${id(s.id)} <<choice>>`);
    else lines.push(`  state "${s.name}${s.seconds ? ` (${s.seconds}s)` : ''}" as ${id(s.id)}`);
  }
  for (const t of transitions) {
    if (t.kind === 1) lines.push(`  ${id(t.from)} --> ${id(t.from)} : ${t.name || '?'} (return to previous)`);
    else lines.push(`  ${id(t.from)} --> ${id(t.to)}${t.name ? ` : ${t.name}` : t.branch ? ` : branch ${t.branch}` : ''}`);
  }
  return lines.join('\n');
}

const [file, flag] = process.argv.slice(2);
if (!file) {
  console.error('usage: states2mermaid.mjs <file.states> [--json]');
  process.exit(2);
}
const machine = readStates(fs.readFileSync(file));
console.log(flag === '--json' ? JSON.stringify(machine, null, 1) : toMermaid(machine));
