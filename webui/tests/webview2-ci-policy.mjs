// WebView2 150+ ignores environment overrides in elevated hosts. This adapter
// uses the documented HKLM alternative only on explicitly opted-in CI runners.
import { execFileSync } from 'node:child_process'

function machineRegistry() {
  return function command(action, key, entry) {
    const payload = JSON.stringify({ action, key, entry }).replaceAll("'", "''")
    const script = `
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$inputData = '${payload}' | ConvertFrom-Json
$root = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
$subkey = $null
try {
  $location = 'SOFTWARE\\Policies\\Microsoft\\Edge\\WebView2\\' + $inputData.key
  if ($inputData.action -eq 'set') {
    $subkey = $root.CreateSubKey($location)
    $kind = [Enum]::Parse([Microsoft.Win32.RegistryValueKind], [string]$inputData.entry.kind)
    $value = $inputData.entry.value
    switch ($kind) {
      'DWord' { $value = [int]$value }
      'QWord' { $value = [long]$value }
      'Binary' { $value = [byte[]]$value }
      'MultiString' { $value = [string[]]$value }
      default { $value = [string]$value }
    }
    $subkey.SetValue('KiCad_Forge.exe', $value, $kind)
  } else {
    $subkey = $root.OpenSubKey($location, $inputData.action -eq 'remove')
    if ($inputData.action -eq 'remove') {
      if ($subkey) { $subkey.DeleteValue('KiCad_Forge.exe', $false) }
    } elseif (!$subkey -or !($subkey.GetValueNames() -contains 'KiCad_Forge.exe')) {
      'null'
    } else {
      @{ kind = $subkey.GetValueKind('KiCad_Forge.exe').ToString(); value = $subkey.GetValue('KiCad_Forge.exe', $null, 'DoNotExpandEnvironmentNames') } | ConvertTo-Json -Compress
    }
  }
} finally {
  if ($subkey) { $subkey.Dispose() }
  $root.Dispose()
}
`
    const result = execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], {
      encoding: 'utf8', windowsHide: true, timeout: 15000,
    })
    return action === 'get' ? JSON.parse(result.trim()) : undefined
  }
}

export function installWebView2CiPolicy({
  platform = process.platform,
  githubActions = process.env.GITHUB_ACTIONS,
  enabled = process.env.KFORGE_WEBVIEW2_MACHINE_POLICY,
  port, profile, registry,
} = {}) {
  if (platform !== 'win32' || githubActions !== 'true' || enabled !== '1') return () => {}
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('invalid WebView2 debugging port')
  if (typeof profile !== 'string' || !profile) throw new Error('missing fixture WebView2 profile')
  if (!registry) {
    const command = machineRegistry()
    registry = { get: (key) => command('get', key), set: (key, entry) => command('set', key, entry), remove: (key) => command('remove', key) }
  }
  const settings = {
    AdditionalBrowserArguments: { kind: 'String', value: `--remote-debugging-port=${port}` },
    UserDataFolder: { kind: 'String', value: profile },
  }
  const previous = Object.keys(settings).map((key) => [key, registry.get(key)])
  const restore = () => {
    const failures = []
    for (const [key, entry] of previous) {
      try { if (entry === null) registry.remove(key); else registry.set(key, entry) } catch (error) { failures.push(error) }
    }
    if (failures.length) throw new AggregateError(failures, 'cannot restore WebView2 CI policies')
  }
  try {
    for (const [key, entry] of Object.entries(settings)) registry.set(key, entry)
  } catch (error) {
    try { restore() } catch (rollback) { throw new AggregateError([error, rollback], 'WebView2 CI policy installation and restoration failed') }
    throw error
  }
  return restore
}
