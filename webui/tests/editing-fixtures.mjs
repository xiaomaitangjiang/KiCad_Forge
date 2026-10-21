// 编辑命令真实 WebView2 夹具的共享种子：一个工作空间 + 两个可写符号库 + 四个符号。
// 只用于测试临时数据目录，不读取/写入开发者的真实 data。
import { mkdirSync, writeFileSync } from 'node:fs'
import { DatabaseSync } from 'node:sqlite'
import path from 'node:path'

export function symbolNode(name) {
  return `(symbol "${name}"
  (pin_names (offset 0.254))
  (exclude_from_sim no)
  (in_bom yes)
  (on_board yes)
  (property "Reference" "U" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Value" "${name}" (at 0 0 0) (effects (font (size 1.27 1.27))))
  (property "Footprint" "" (at 0 0 0) (effects (font (size 1.27 1.27)) hide))
  (symbol "${name}_0_1"
    (rectangle (start -1.016 -1.016) (end 1.016 1.016)
      (stroke (width 0.254) (type default)) (fill (type none))))
  (symbol "${name}_1_1"
    (pin passive line (at -2.54 0 0) (length 2.54)
      (name "1" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27)))))))`
}

export function libraryText(names) {
  return ['(kicad_symbol_lib (version 20231120) (generator editing-e2e)', ...names.map(symbolNode), ')\n'].join('\n')
}

export function seedEditingFixture(root, {
  namesA = ['Alpha', 'Beta'],
  namesB = ['Gamma', 'Delta'],
} = {}) {
  const libDir = path.join(root, 'data', 'e2e-libs')
  mkdirSync(libDir, { recursive: true })
  const fileA = path.join(libDir, 'lib-a.kicad_sym')
  const fileB = path.join(libDir, 'lib-b.kicad_sym')
  writeFileSync(fileA, libraryText(namesA))
  writeFileSync(fileB, libraryText(namesB))
  const db = new DatabaseSync(path.join(root, 'data', 'meta.db'))
  try {
    db.exec(`
      INSERT OR REPLACE INTO component_libraries
        (id, name, symbol_path, footprint_path, model_3d_path, enabled, sort_order, locked, protected)
        VALUES ('e2e-workspace', 'E2E Workspace', '${libDir.replaceAll("'", "''")}', '', '', 1, 0, 0, 0);
      INSERT OR REPLACE INTO libraries (id, name, file_path, component_library_id)
        VALUES ('e2e-lib-a', 'LibA', '${fileA.replaceAll("'", "''")}', 'e2e-workspace');
      INSERT OR REPLACE INTO libraries (id, name, file_path, component_library_id)
        VALUES ('e2e-lib-b', 'LibB', '${fileB.replaceAll("'", "''")}', 'e2e-workspace');
    `)
    const insert = db.prepare(`INSERT OR REPLACE INTO symbols
      (id, library_id, name, lib_id, reference_prefix, pin_count, Kicad_Forge_ID)
      VALUES (?, ?, ?, ?, 'U', 1, ?)`)
    const rows = [
      ...namesA.map(name => [`e2e-sym-${name.toLowerCase()}`, 'e2e-lib-a', name, `LibA:${name}`,
        `KF-E2E-${name.replace(/[^A-Za-z0-9]/g, '').toUpperCase()}`]),
      ...namesB.map(name => [`e2e-sym-${name.toLowerCase()}`, 'e2e-lib-b', name, `LibB:${name}`,
        `KF-E2E-${name.replace(/[^A-Za-z0-9]/g, '').toUpperCase()}`]),
    ]
    for (const row of rows) insert.run(...row)
  } finally {
    db.close()
  }
  return { fileA, fileB }
}
