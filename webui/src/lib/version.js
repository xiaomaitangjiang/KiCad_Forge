// Version helpers shared by the About dialog and the update check.
// Accepts tags like "v1.2.3" and plain "1.2.3"; numeric parts only.
export function normalizeVersion(value) {
  const match = String(value ?? '').trim().match(/^v?(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$/)
  return match ? [Number(match[1]), Number(match[2]), Number(match[3])] : null
}

export function compareVersions(left, right) {
  const a = normalizeVersion(left)
  const b = normalizeVersion(right)
  if (!a || !b) return 0
  for (let i = 0; i < 3; i += 1) {
    if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1
  }
  return 0
}
