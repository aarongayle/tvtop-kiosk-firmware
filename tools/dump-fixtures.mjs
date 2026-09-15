import fs from 'node:fs'
import path from 'node:path'
import { availableGames, renderGame } from '/Users/aaron/Documents/dev/tvtop/tvtop-kiosk-server/src/games/index.js'
import { toWire, checkBudget } from '/Users/aaron/Documents/dev/tvtop/tvtop-kiosk-server/src/scene/protocol.js'
import { buildGameScene, buildIdleScene } from '/Users/aaron/Documents/dev/tvtop/tvtop-kiosk-server/src/scene/scene.js'
import { pairingFrame, handoverFrame } from '/Users/aaron/Documents/dev/tvtop/tvtop-kiosk-server/src/scenes.js'

const out = process.argv[2]
const players = {
  aaron: { player_id: 'aaron', order: 1, color: 1, display_name: 'Aaron' },
  devon: { player_id: 'devon', order: 2, color: 2, display_name: 'Devon' },
  marie: { player_id: 'marie', order: 3, color: 3, display_name: 'Marie' },
}
const wire = (scene) => toWire(scene, { rev: 1, nextUrl: 'https://kiosk.tvtop.games/v1/frame/tok?rev=1', nextMs: 2000 })
const save = (name, scene) => {
  const frame = wire(scene)
  const b = checkBudget(scene)
  fs.writeFileSync(path.join(out, name + '.json'), JSON.stringify(frame))
  const kinds = {}
  for (const op of frame.ops) kinds[op[0]] = (kinds[op[0]] || 0) + 1
  console.log(name.padEnd(28), 'ops', String(frame.ops.length).padStart(4), 'dyn', String(b.bytes).padStart(6), 'static', String(b.staticBytes).padStart(7), JSON.stringify(kinds))
}

save('pairing', pairingFrame({ code: 'k7f2qm3x', deviceId: 'vy6kx4' }))
save('handover', handoverFrame())
save('idle', buildIdleScene({ name: 'Living room' }))
save('generic-game', buildGameScene({ gameName: 'Frontier Island', gameId: 'frontierisland', sessionId: '7fk2', session: { started: true }, players: Object.values(players).map(p => ({ ...p, id: p.player_id, score: 3 })), activePlayerId: 'devon' }))
save('generic-finished', buildGameScene({ gameName: 'Frontier Island', gameId: 'frontierisland', sessionId: '7fk2', session: { started: true, finished: true }, players: Object.values(players).map((p, i) => ({ ...p, id: p.player_id, score: 3 + i, finished: i === 1 })) }))

const globalState = (map, count) => ({
  session: { started: true, initialized: true, map, cards: 0, bonus: 0 },
  players,
  territories: Object.fromEntries(Array.from({ length: count }, (_, index) => [index, { player: (index % 3) + 1, armies: (index % 9) + 1 }])),
  turns: {},
  cards: {},
})
for (const [map, count] of Object.entries({ classic: 149, 'classic-balanced': 149, us: 50, europe: 52, top: 100 })) {
  try { save('gc-' + map, renderGame({ gameId: 'globalconquest', sessionId: 'kiosk-' + map, state: globalState(map, count) })) } catch (e) { console.log('gc-' + map, 'FAILED', e.message) }
}
const minimal = { session: { started: true, initialized: true, map: 'us', cards: 0, bonus: 0 }, players, territories: {}, turns: {}, cards: {} }
for (const gameId of availableGames()) {
  try { save('game-' + gameId, renderGame({ gameId, sessionId: 'kiosk-test', state: minimal })) } catch (e) { console.log(gameId, 'FAILED', e.message) }
}
