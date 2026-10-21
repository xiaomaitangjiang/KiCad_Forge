// 复用封装选择的实际 helper 行为（不是源码字符串扫描）：
// 同库同名的两个封装必须靠 id 区分，提交时要把 footprint_id 带上。
import test from 'node:test'
import assert from 'node:assert/strict'

import {
  reuseChoice,
  isChosen,
  choiceLabel,
  withFootprintChoice,
} from '../../plugins/lcsc_import/frontend/src/footprintChoice.js'

const sameNameA = { id: 'uuid-a', library: 'Pkg', name: 'SOT-23', pad_count: 3 }
const sameNameB = { id: 'uuid-b', library: 'Pkg', name: 'SOT-23', pad_count: 6 }

test('默认复用选择保留 id，库名与名称只用于显示', () => {
  const choice = reuseChoice(sameNameA)
  assert.equal(choice.mode, 'reuse')
  assert.equal(choice.id, 'uuid-a')
  assert.equal(choice.library, 'Pkg')
  assert.equal(choice.name, 'SOT-23')
  assert.equal(choiceLabel(choice), 'Pkg:SOT-23')
})

test('同库同名的两个封装靠 id 区分', () => {
  const choice = reuseChoice(sameNameA)
  assert.equal(isChosen(choice, sameNameA), true)
  assert.equal(isChosen(choice, sameNameB), false)

  const other = reuseChoice(sameNameB)
  assert.equal(isChosen(other, sameNameB), true)
  assert.equal(isChosen(other, sameNameA), false)
})

test('未选择或选择新建时没有候选项被选中', () => {
  assert.equal(isChosen(null, sameNameA), false)
  assert.equal(isChosen({ mode: 'new' }, sameNameA), false)
})

test('旧选择（无 id）仍按库名与名称比较', () => {
  const legacy = { mode: 'reuse', library: 'Pkg', name: 'SOT-23' }
  assert.equal(isChosen(legacy, sameNameA), true)
  assert.equal(isChosen(legacy, { id: 'uuid-c', library: 'Other', name: 'SOT-23' }), false)
})

test('提交 reuse 选择时显式带上 footprint_id', () => {
  const body = withFootprintChoice({ action: 'import', lcsc_id: 'C123' },
                                   reuseChoice(sameNameA))
  assert.equal(body.footprint_id, 'uuid-a')
  assert.equal(body.footprint_choice.id, 'uuid-a')
  assert.equal(body.lcsc_id, 'C123')
})

test('提交新建时没有 footprint_id，旧的复用 id 必须被清掉', () => {
  const body = withFootprintChoice({ action: 'import', footprint_id: 'stale' },
                                   { mode: 'new' })
  assert.equal(body.footprint_choice.mode, 'new')
  assert.equal(body.footprint_id, undefined) // 新建模式不能带着旧复用 UUID 提交
  assert.equal('footprint_id' in body, false)
})

test('旧选择（无 id）提交时不伪造 footprint_id，交给宿主唯一匹配', () => {
  const body = withFootprintChoice({ action: 'import', footprint_id: 'stale' },
                                   { mode: 'reuse', library: 'Pkg', name: 'SOT-23' })
  assert.equal(body.footprint_id, undefined)
  assert.equal(body.footprint_choice.library, 'Pkg')
  assert.equal(body.footprint_choice.name, 'SOT-23')
})

test('复用选择区分同库同名的两个封装，提交时带上各自的 id', () => {
  const bodyA = withFootprintChoice({ action: 'import' }, reuseChoice(sameNameA))
  const bodyB = withFootprintChoice({ action: 'import' }, reuseChoice(sameNameB))
  assert.equal(bodyA.footprint_id, 'uuid-a')
  assert.equal(bodyB.footprint_id, 'uuid-b')
  assert.notEqual(bodyA.footprint_id, bodyB.footprint_id)
  assert.equal(bodyA.footprint_choice.name, bodyB.footprint_choice.name)
})
