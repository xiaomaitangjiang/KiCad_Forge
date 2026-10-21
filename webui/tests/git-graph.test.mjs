import test from 'node:test'
import assert from 'node:assert/strict'
import { GRAPH_LANE_STEP, GRAPH_RAIL_WIDTH, GRAPH_VISIBLE_LANES, groupRefs, laneCenter, layoutGraph, localBranches, matchCommits } from '../../plugins/git_version/frontend/src/graph.ts'

const commit = (oid, parents = []) => ({ oid, parents })

test('linear history keeps a single lane and a single parent edge', () => {
  const layout = layoutGraph([commit('a', ['b']), commit('b', ['c']), commit('c')])
  assert.equal(layout.laneCount, 1)
  assert.deepEqual(layout.rows.map((row) => row.lane), [0, 0, 0])
  assert.deepEqual(layout.rows[0].parents.map((parent) => parent.lane), [0])
  assert.equal(layout.rows[2].parents.length, 0)
  assert.equal(layout.rows[0].continues, false)
  assert.equal(layout.rows[2].continues, false)
})

test('a merge commit fans the second parent into its own lane and rejoins at the base', () => {
  const layout = layoutGraph([
    commit('merge', ['a', 'b']),
    commit('a', ['base']),
    commit('b', ['base']),
    commit('base'),
  ])
  const [merge, a, b, base] = layout.rows
  assert.equal(merge.merge, true)
  assert.deepEqual(merge.parents.map((parent) => parent.lane), [0, 1])
  assert.equal(a.lane, 0)
  assert.equal(b.lane, 1)
  assert.equal(base.lane, 0)
  assert.deepEqual(a.passThrough, [{ lane: 1, oid: 'b' }])
  assert.equal(layout.laneCount, 2)
  assert.equal(merge.continues, false)
})

test('parents outside the loaded page are marked as continuations', () => {
  const layout = layoutGraph([commit('top', ['missing']), commit('bottom')])
  const top = layout.rows[0]
  assert.equal(top.parents[0].continuation, true)
  assert.equal(top.continues, true)
  const bottom = layout.rows[1]
  assert.equal(bottom.parents.length, 0)
  assert.equal(bottom.continues, false)
})

test('recomputing layout from all loaded commits keeps lanes stable across appended pages', () => {
  const all = [
    commit('merge', ['a', 'b']),
    commit('a', ['base']),
    commit('b', ['base']),
    commit('base', ['root']),
    commit('root'),
  ]
  const firstPage = layoutGraph(all.slice(0, 2))
  const secondPageAlone = layoutGraph(all.slice(2, 4))
  const combined = layoutGraph(all)

  assert.equal(combined.rows[1].lane, firstPage.rows[1].lane)
  assert.equal(combined.rows[2].lane, 1)
  // A page appended without the earlier lane state cannot know about lane 1;
  // the model therefore recomputes from every loaded commit instead of
  // trusting page-local coordinates.
  assert.equal(secondPageAlone.rows[0].lane, 0)
  assert.equal(combined.rows[3].parents[0].continuation, false)
  assert.equal(combined.rows[4].parents.length, 0)
})

test('more than eight lanes widen the rail and keep every lane at a unique x', () => {
  // Nine merges create eighteen lanes; none may be clamped onto another lane's x.
  const layout = layoutGraph(Array.from({ length: 9 }, (_, index) => commit(`m${index}`, [`a${index}`, `b${index}`])))
  assert.equal(layout.laneCount, 18)
  assert.ok(layout.laneCount > GRAPH_VISIBLE_LANES)
  assert.equal(layout.laneOverflow, layout.laneCount - GRAPH_VISIBLE_LANES)
  assert.ok(layout.railWidth > GRAPH_RAIL_WIDTH)
  assert.equal(layout.railWidth, 163)

  const centers = Array.from({ length: layout.laneCount }, (_, lane) => laneCenter(lane, layout.laneCount))
  assert.equal(new Set(centers).size, layout.laneCount)
  assert.ok(centers.every((x) => x >= 0 && x <= layout.railWidth))
  assert.deepEqual(
    centers.slice(1).map((x, index) => x - centers[index]),
    Array(layout.laneCount - 1).fill(GRAPH_LANE_STEP),
  )
  for (const row of layout.rows) {
    assert.ok(row.lane >= 0 && row.lane < layout.laneCount)
    assert.equal(laneCenter(row.lane, layout.laneCount), centers[row.lane])
    for (const parent of row.parents) {
      assert.ok(parent.lane >= 0 && parent.lane < layout.laneCount)
      // A first parent may continue in the same lane (straight line), but a
      // second parent must never share that lane's x coordinate.
      if (parent.merge) {
        assert.notEqual(laneCenter(parent.lane, layout.laneCount), laneCenter(row.lane, layout.laneCount))
      }
    }
  }

  // Eight lanes still fit the regular 74 px viewport without a scroll hint.
  const fitting = layoutGraph(Array.from({ length: 4 }, (_, index) => commit(`n${index}`, [`c${index}`, `d${index}`])))
  assert.equal(fitting.laneCount, 8)
  assert.equal(fitting.railWidth, GRAPH_RAIL_WIDTH)
  assert.equal(fitting.laneOverflow, 0)
})

test('refs are grouped per oid with HEAD first, then local, remote and tags', () => {
  const refs = groupRefs([
    { name: 'v1.0', kind: 'tag', target_oid: 'a' },
    { name: 'origin/main', kind: 'remote', target_oid: 'a' },
    { name: 'main', kind: 'local', target_oid: 'a', is_head: true },
    { name: 'dev', kind: 'local', target_oid: 'b' },
    { name: 'origin/dev', kind: 'remote', target_oid: 'b' },
  ])
  assert.deepEqual(refs.get('a').map((ref) => ref.name), ['main', 'origin/main', 'v1.0'])
  assert.equal(refs.get('a')[0].is_head, true)
  assert.deepEqual(refs.get('b').map((ref) => ref.name), ['dev', 'origin/dev'])
  assert.deepEqual(localBranches([...refs.get('a'), ...refs.get('b')]), ['dev', 'main'])
})

test('find only dims matches and never rewrites the graph topology', () => {
  const commits = [
    { oid: 'abc1234', parents: [], subject: 'Refactor power symbol', author_name: 'Xiaom', author_time: '', committer_time: '' },
    { oid: 'def5678', parents: [], subject: 'Fix power symbol', author_name: 'Chen', author_time: '', committer_time: '' },
    { oid: '987f', parents: [], subject: 'Docs', author_name: 'Xiaom', author_time: '', committer_time: '' },
  ]
  assert.deepEqual([...matchCommits(commits, 'POWER')], ['abc1234', 'def5678'])
  assert.deepEqual([...matchCommits(commits, 'chen')], ['def5678'])
  assert.deepEqual([...matchCommits(commits, '987F')], ['987f'])
  assert.equal(matchCommits(commits, '   ').size, 0)
  const layout = layoutGraph(commits)
  assert.deepEqual(layout.rows.map((row) => row.lane), [0, 0, 0])
})
