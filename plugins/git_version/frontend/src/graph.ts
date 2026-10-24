/**
 * Pure graph layout for the commit history list.
 *
 * The backend returns a flat, newest-first commit page (graph_v1). Lane state
 * is recomputed from the commits currently loaded, so a page appended with a
 * cursor keeps the same lanes as an uninterrupted read of the same range.
 * Nothing here knows about React or the host.
 */

export interface GraphCommitInput {
  oid: string
  parents: readonly string[]
}

export interface GraphParentEdge {
  oid: string
  /** Lane used by the parent below this row. */
  lane: number
  /** The parent is not part of the loaded commits (off-page or shallow clone). */
  continuation: boolean
  /** Not the first parent of a merge commit. */
  merge: boolean
}

export interface GraphPassThrough {
  lane: number
  oid: string
}

export interface GraphRow {
  oid: string
  lane: number
  merge: boolean
  /** Lane slots at the top of the row. */
  lanesBefore: (string | null)[]
  /** Lane slots at the bottom of the row. */
  lanesAfter: (string | null)[]
  /** Unrelated lanes whose edge crosses this row. */
  passThrough: GraphPassThrough[]
  parents: GraphParentEdge[]
  /** At least one parent is outside the loaded commits. */
  continues: boolean
}

/** Width of the rail viewport in CSS pixels. */
export const GRAPH_RAIL_WIDTH = 74
/** Horizontal distance between two lane centers. */
export const GRAPH_LANE_STEP = 9
/** Distance from the rail edge to the outermost lane center. */
const GRAPH_LANE_PADDING = 5
/** Lanes that fit the 74 px viewport without scrolling. */
export const GRAPH_VISIBLE_LANES = 1 + Math.floor((GRAPH_RAIL_WIDTH - 2 * GRAPH_LANE_PADDING) / GRAPH_LANE_STEP)

export interface GraphLayout {
  rows: GraphRow[]
  /** Real number of lane slots, including lanes beyond the 74 px viewport. */
  laneCount: number
  /** Content width needed to draw every lane without clamping. */
  railWidth: number
  /** Lanes beyond the visible rail width; a hint for the scrollbar, never a clamp. */
  laneOverflow: number
}

function railWidthFor(laneCount: number): number {
  return Math.max(
    GRAPH_RAIL_WIDTH,
    2 * GRAPH_LANE_PADDING + (Math.max(1, laneCount) - 1) * GRAPH_LANE_STEP,
  )
}

/** Center x for a lane on a rail wide enough for every lane. Lanes never collapse. */
export function laneCenter(lane: number, laneCount: number): number {
  const width = railWidthFor(laneCount)
  return width / 2 + (lane - (Math.max(1, laneCount) - 1) / 2) * GRAPH_LANE_STEP
}

/** Lane 0 is reserved for the current branch; extra parents fan out right. */
export function layoutGraph(commits: readonly GraphCommitInput[]): GraphLayout {
  const remaining = new Set(commits.map((commit) => commit.oid))
  const rows: GraphRow[] = []
  let lanes: (string | null)[] = []
  let laneCount = 1

  for (const commit of commits) {
    remaining.delete(commit.oid)
    let lane = lanes.indexOf(commit.oid)
    if (lane < 0) {
      const free = lanes.indexOf(null)
      if (free >= 0) {
        lane = free
        lanes[free] = commit.oid
      } else {
        lane = lanes.length
        lanes.push(commit.oid)
      }
    }
    const lanesBefore = lanes.slice()
    const after = lanes.slice()
    after[lane] = null
    const parents: GraphParentEdge[] = []
    commit.parents.forEach((parent, index) => {
      // An already expected lane wins: two children of the same parent share it.
      let target = after.indexOf(parent)
      if (target < 0 && index === 0) target = lane
      if (target < 0) {
        const free = after.indexOf(null)
        if (free < 0) {
          target = after.length
          after.push(parent)
        } else {
          target = free
          after[free] = parent
        }
      }
      if (target >= 0) after[target] = parent
      parents.push({ oid: parent, lane: target, continuation: !remaining.has(parent), merge: index > 0 })
    })
    while (after.length > 1 && after[after.length - 1] === null) after.pop()
    const passThrough: GraphPassThrough[] = []
    const slots = Math.max(lanesBefore.length, after.length)
    for (let slot = 0; slot < slots; slot++) {
      const before = lanesBefore[slot] ?? null
      const now = after[slot] ?? null
      if (before && before === now && slot !== lane) passThrough.push({ lane: slot, oid: before })
    }
    laneCount = Math.max(laneCount, lanesBefore.length, after.length)
    rows.push({
      oid: commit.oid,
      lane,
      merge: commit.parents.length > 1,
      lanesBefore,
      lanesAfter: after.slice(),
      passThrough,
      parents,
      continues: parents.some((parent) => parent.continuation),
    })
    lanes = after
  }

  return {
    rows,
    laneCount,
    railWidth: railWidthFor(laneCount),
    laneOverflow: Math.max(0, laneCount - GRAPH_VISIBLE_LANES),
  }
}

/** Ref badges ordered HEAD first, then local, remote and tags. */
export function groupRefs(refs: readonly { name: string; kind: string; target_oid: string; is_head?: boolean }[]) {
  const byOid = new Map<string, { name: string; kind: string; is_head: boolean }[]>()
  for (const ref of refs) {
    const list = byOid.get(ref.target_oid) ?? []
    list.push({ name: ref.name, kind: ref.kind, is_head: ref.is_head === true })
    byOid.set(ref.target_oid, list)
  }
  for (const list of byOid.values()) {
    list.sort((left, right) => {
      if (left.is_head !== right.is_head) return left.is_head ? -1 : 1
      const rank = (kind: string) => (kind === 'local' ? 0 : kind === 'remote' ? 1 : 2)
      if (rank(left.kind) !== rank(right.kind)) return rank(left.kind) - rank(right.kind)
      return left.name.localeCompare(right.name)
    })
  }
  return byOid
}

/** Local branch names used by the history range filter. */
export function localBranches(refs: readonly { name: string; kind: string }[]): string[] {
  return refs
    .filter((ref) => ref.kind === 'local')
    .map((ref) => ref.name)
    .filter((name, index, list) => list.indexOf(name) === index)
    .sort((left, right) => left.localeCompare(right))
}

/** Only matching commits are dimmed; the DAG itself never changes. */
export function matchCommits(
  commits: readonly { oid: string; subject: string; author_name: string }[],
  query: string,
): Set<string> {
  const needle = query.trim().toLowerCase()
  if (!needle) return new Set()
  const matches = new Set<string>()
  for (const commit of commits) {
    if (commit.subject.toLowerCase().includes(needle)
      || commit.oid.toLowerCase().includes(needle)
      || commit.author_name.toLowerCase().includes(needle)) {
      matches.add(commit.oid)
    }
  }
  return matches
}
