#!/usr/bin/env node
// Turns a protocol-v3 frame (a fixture, or anything the kiosk server sends) into the SVG that
// tvtop-expo/helpers/Kiosk/SceneCanvas.js would draw, so resvg can render a browser-equivalent
// reference image for comparing the firmware's output against.
//
//   node tools/render-reference.mjs frame.json [-o out.svg] [--static other.json]
import fs from 'node:fs'

const ICON_PATHS = {
  crown: { d: 'M4 17h16l1.6-9.6-5.9 4.8L12 4.5 8.3 12.2 2.4 7.4z M4 19h16v2.2H4z' },
  star: { d: 'M12 2.5l2.9 5.9 6.5.9-4.7 4.6 1.1 6.5L12 17.3l-5.8 3.1 1.1-6.5L2.6 9.3l6.5-.9z' },
  check: { d: 'M9.3 18.2L3.5 12.4l2.1-2.1 3.7 3.7 9-9 2.1 2.1z' },
  cross: { d: 'M19 6.4L17.6 5 12 10.6 6.4 5 5 6.4 10.6 12 5 17.6 6.4 19 12 13.4 17.6 19 19 17.6 13.4 12z' },
  person: { d: 'M12 12a4.5 4.5 0 1 0 0-9 4.5 4.5 0 0 0 0 9zm0 2c-4.4 0-8 2.2-8 5v2h16v-2c0-2.8-3.6-5-8-5z' },
  warning: { d: 'M12 2.8l10.2 18.4H1.8zM11 9h2v6h-2zm0 7.6h2v2.2h-2z', rule: 'evenodd' },
  dice: {
    d:
      'M4.5 3h15A1.5 1.5 0 0 1 21 4.5v15a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 19.5v-15A1.5 1.5 0 0 1 4.5 3z' +
      ' M7.5 6a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z M16.5 6a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z' +
      ' M12 10.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z M7.5 15a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z' +
      ' M16.5 15a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3z',
    rule: 'evenodd',
  },
  clock: { d: 'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18z M12 7v5.4l3.6 2.1', stroke: 2 },
  cast: { d: 'M3 8V5h18v14h-7 M2 20h3a3 3 0 0 0-3-3v3 M2 14a6 6 0 0 1 6 6 M2 9a11 11 0 0 1 11 11', stroke: 2 },
  wifi: { d: 'M3 9.5a13 13 0 0 1 18 0 M6.6 13a8 8 0 0 1 10.8 0 M10 16.4a3.5 3.5 0 0 1 4 0 M12 20h.01', stroke: 2 },
}
const ANCHORS = ['start', 'middle', 'end']
const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')

function decodeBitmap(modules, data) {
  const bytes = Buffer.from(String(data), 'base64')
  const bytesPerRow = Math.ceil(modules / 8)
  const grid = []
  for (let row = 0; row < modules; row++) {
    const cells = []
    for (let col = 0; col < modules; col++) cells.push(Boolean(bytes[row * bytesPerRow + (col >> 3)] & (0x80 >> (col & 7))))
    grid.push(cells)
  }
  return grid
}

function opToSvg(op, geometry, paints) {
  if (!Array.isArray(op)) return ''
  switch (op[0]) {
    case 'r': { const [, x, y, w, h, fill, radius] = op; return `<rect x="${x}" y="${y}" width="${w}" height="${h}" fill="${esc(fill)}" rx="${radius || 0}" ry="${radius || 0}"/>` }
    case 'l': { const [, x1, y1, x2, y2, color, width] = op; return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${esc(color)}" stroke-width="${width || 2}"/>` }
    case 'c': { const [, cx, cy, r, fill] = op; return `<circle cx="${cx}" cy="${cy}" r="${r}" fill="${esc(fill)}"/>` }
    case 't': {
      const [, x, y, value, size, color, align, weight] = op
      return `<text x="${x}" y="${y}" fill="${esc(color)}" font-size="${size}" font-family="Roboto" font-weight="${weight ? 'bold' : 'normal'}" text-anchor="${ANCHORS[align || 0]}">${esc(value)}</text>`
    }
    case 'b': {
      const [, x, y, size, modules, data, color] = op
      const cell = size / modules
      const cells = []
      decodeBitmap(modules, data).forEach((row, r) => row.forEach((on, c) => {
        if (on) cells.push(`<rect x="${x + c * cell}" y="${y + r * cell}" width="${cell + 0.5}" height="${cell + 0.5}" fill="${esc(color || '#000000')}"/>`)
      }))
      return `<g>${cells.join('')}</g>`
    }
    case 'i': {
      const [, x, y, size, name, color] = op
      const spec = ICON_PATHS[name]
      if (!spec) return ''
      const scale = size / 24
      return `<g transform="translate(${x}, ${y}) scale(${scale})"><path d="${spec.d}" fill="${spec.stroke ? 'none' : esc(color)}" fill-rule="${spec.rule || 'nonzero'}" stroke="${spec.stroke ? esc(color) : 'none'}" stroke-width="${spec.stroke || 0}" stroke-linecap="round" stroke-linejoin="round"/></g>`
    }
    case 'u': {
      const [, id, paintId] = op
      const definition = geometry[id]
      if (!Array.isArray(definition)) return ''
      const [fill, stroke, strokeWidth] = paints[paintId] || []
      const [, , dx, dy, sx, sy] = definition
      const attrs = `fill="${fill ? esc(fill) : 'none'}" stroke="${stroke ? esc(stroke) : 'none'}" stroke-width="${strokeWidth || 0}" transform="translate(${dx} ${dy}) scale(${sx} ${sy})"`
      if (definition[0] === 'path') return `<path d="${esc(definition[1])}" ${attrs}/>`
      if (definition[0] === 'poly') return `<polygon points="${esc(definition[1])}" ${attrs}/>`
      if (definition[0] === 'circle') return `<circle cx="${definition[1]}" cy="${definition[2]}" r="${definition[3]}" ${attrs}/>`
      return ''
    }
    default:
      return ''
  }
}

export function frameToSvg(frame, staticCache = {}) {
  const width = frame.w || 1280, height = frame.h || 720, background = frame.bg || '#0d1b2a'
  const staticSet = frame.static?.defs ? frame.static : staticCache[frame.static?.id] || {}
  const geometry = staticSet.defs || {}, paints = staticSet.paints || []
  const runs = []
  let current = { clip: null, ops: [] }
  for (const op of frame.ops || []) {
    if (op?.[0] === 'k') { runs.push(current); current = { clip: op.length > 1 ? { x: op[1], y: op[2], w: op[3], h: op[4] } : null, ops: [] }; continue }
    current.ops.push(op)
  }
  runs.push(current)
  let out = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">`
  out += `<rect x="0" y="0" width="${width}" height="${height}" fill="${esc(background)}"/>`
  runs.forEach((run, i) => {
    const clipId = `kiosk-clip-${i}`
    if (run.clip) out += `<defs><clipPath id="${clipId}"><rect x="${run.clip.x}" y="${run.clip.y}" width="${run.clip.w}" height="${run.clip.h}"/></clipPath></defs>`
    out += `<g${run.clip ? ` clip-path="url(#${clipId})"` : ''}>`
    for (const op of run.ops) out += opToSvg(op, geometry, paints)
    out += '</g>'
  })
  return out + '</svg>'
}

if (process.argv[1] && process.argv[1].endsWith('render-reference.mjs')) {
  const args = process.argv.slice(2)
  let input = null, output = null, staticFile = null
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '-o') output = args[++i]
    else if (args[i] === '--static') staticFile = args[++i]
    else input = args[i]
  }
  if (!input) { console.error('usage: render-reference.mjs frame.json [-o out.svg] [--static other.json]'); process.exit(2) }
  const frame = JSON.parse(fs.readFileSync(input, 'utf8'))
  const cache = {}
  if (staticFile) { const s = JSON.parse(fs.readFileSync(staticFile, 'utf8')); if (s.static?.id) cache[s.static.id] = s.static }
  const svg = frameToSvg(frame, cache)
  if (output) fs.writeFileSync(output, svg); else process.stdout.write(svg)
}
