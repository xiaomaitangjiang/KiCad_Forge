// 封装/3D 存在性状态 → 界面语义的纯逻辑测试。
// 参考缺陷（用户示例 SS-0323PG-14N5WW）：引用写着 My_Library:SW-SMD_SS-0323PG-5N5WW，
// 本地没有这个库，旧界面靠「名称非空」显示成正常。这里把每种状态与
// 「旧字段不能把 unknown/失效变成正常」都钉住。
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import {
  footprintStatusOf, modelStatusOf, footprintReferenceOf,
  footprintIsAvailable, modelIsUsable, footprintView, modelView,
  footprintReasonKey, canReplaceFootprint, isProblemSymbol,
  FOOTPRINT_STATUSES, MODEL_STATUSES,
} from '../../src/lib/footprintStatus.js'

test('状态解析：只认服务端给的合法状态，其它一律是 unknown（未检查）', () => {
  for (const status of FOOTPRINT_STATUSES) {
    assert.equal(footprintStatusOf({ footprint_status: status }), status)
  }
  for (const status of MODEL_STATUSES) {
    assert.equal(modelStatusOf({ model_status: status }), status)
  }
  assert.equal(footprintStatusOf({}), 'unknown')
  assert.equal(footprintStatusOf({ footprint_status: 'AVAILABLE' }), 'unknown', '大小写不合法')
  assert.equal(footprintStatusOf({ footprint_status: 1 }), 'unknown')
  assert.equal(footprintStatusOf(null), 'unknown')
  assert.equal(modelStatusOf({ model_status: 'gone' }), 'unknown')
})

test('只有 available 算存在：名称、UUID、旧布尔字段都不能冒充', () => {
  // 用户示例：引用在、名字在、UUID 在，但服务端说 not_found
  const example = {
    footprint: 'My_Library:SW-SMD_SS-0323PG-5N5WW',
    footprint_reference: 'My_Library:SW-SMD_SS-0323PG-5N5WW',
    footprint_status: 'not_found',
    has_footprint: true,       // 旧字段说「有」
    has_3d_model: true,        // 旧字段连 3D 都说「有」
    footprint_id: 'ghost-uuid',
  }
  assert.equal(footprintIsAvailable(example), false)
  assert.equal(modelIsUsable(example), false)
  assert.equal(isProblemSymbol(example), true, '问题筛选必须包含它')
  const view = footprintView(example)
  assert.equal(view.status, 'not_found')
  assert.equal(view.available, false)
  assert.equal(view.problem, true)
  assert.equal(view.tone, 'bad')
  assert.equal(view.reference, 'My_Library:SW-SMD_SS-0323PG-5N5WW', '原始引用保留显示')
  assert.equal(view.canReplace, true, '未找到要有更换入口')
})

test('旧字段为 true 也不能把 stale/ambiguous/check_failed 显示成正常', () => {
  for (const status of ['stale', 'ambiguous', 'check_failed', 'unassigned']) {
    const entity = { footprint_status: status, has_footprint: true, has_3d_model: true }
    assert.equal(footprintIsAvailable(entity), false, `${status} 不能算可用`)
    assert.equal(modelIsUsable(entity), false)
    assert.equal(footprintView(entity).tone !== 'ok', true)
  }
})

test('缺失状态显示未检查且不算可用，加载/失败是另外两种显示', () => {
  const unknown = footprintView({ footprint: 'Pkg:Part' })
  assert.equal(unknown.status, 'unknown')
  assert.equal(unknown.labelKey, 'footprintStatus.unknown')
  assert.equal(unknown.available, false)
  assert.equal(unknown.canReplace, false, '未检查不是「未找到」，不引导更换')

  const loading = footprintView({ footprint: 'Pkg:Part' }, { loading: true })
  assert.equal(loading.status, 'loading')
  assert.equal(loading.tone, 'unknown')
  assert.equal(loading.labelKey, 'footprintStatus.loading')
  assert.equal(loading.available, false)

  const failed = footprintView({ footprint: 'Pkg:Part', footprint_status: 'available' }, { failed: true })
  assert.equal(failed.status, 'error')
  assert.equal(failed.available, false, '请求失败不能沿用上一次/本地的正常状态')
  assert.equal(failed.failed, true)
  assert.equal(failed.reference, 'Pkg:Part', '失败也保留原始引用')
})

test('available 是唯一正常态，原因码映射为稳定 i18n key', () => {
  const ok = footprintView({ footprint_status: 'available', footprint_reason: 'ok', footprint_reference: 'Pkg:Part' })
  assert.equal(ok.available, true)
  assert.equal(ok.problem, false)
  assert.equal(ok.tone, 'ok')
  assert.equal(ok.labelKey, 'footprintStatus.available')
  assert.equal(ok.reasonKey, 'footprintStatus.reason.ok')
  assert.equal(canReplaceFootprint('available'), false)

  assert.equal(footprintReasonKey('source_key_mismatch'), 'footprintStatus.reason.source_key_mismatch')
  assert.equal(footprintReasonKey(''), '')
  for (const status of ['not_found', 'stale', 'ambiguous', 'check_failed']) {
    assert.equal(canReplaceFootprint(status), true)
  }
})

test('3D 状态：封装不可用时模型绝不能显示正常', () => {
  const broken = modelView({
    footprint_status: 'stale', model_status: 'available', model_reason: 'ok', model_name: 'part.step',
  })
  assert.equal(broken.available, false)
  assert.equal(broken.usable, false)
  assert.equal(broken.tone, 'bad')

  const good = modelView({
    footprint_status: 'available', model_status: 'available', model_reason: 'ok', model_name: 'part.step',
  })
  assert.equal(good.usable, true)
  assert.equal(good.tone, 'ok')

  const noModel = modelView({ footprint_status: 'available', model_status: 'unassigned', model_reason: 'no_model_link' })
  assert.equal(noModel.usable, false)
  assert.equal(noModel.labelKey, 'footprintStatus.model.unassigned')
  assert.equal(noModel.reasonKey, 'footprintStatus.modelReason.no_model_link')

  const staleFootprint = modelView({ footprint_status: 'stale', model_status: 'stale', model_reason: 'footprint_unavailable' })
  assert.equal(staleFootprint.usable, false)
  assert.equal(staleFootprint.tone, 'bad')
})

test('封装不可用时不得输出模型正常文案（labelKey 级，不只 usable/tone）', () => {
  const normal = modelView({ footprint_status: 'available', model_status: 'available', model_reason: 'ok' })
  assert.equal(normal.labelKey, 'footprintStatus.model.available', '正常组合才是模型正常文案')
  for (const status of ['stale', 'ambiguous', 'not_found', 'check_failed', 'unassigned', 'unknown']) {
    const view = modelView({
      footprint_status: status, model_status: 'available', model_reason: 'ok', model_name: 'part.step',
    })
    assert.equal(view.usable, false, `${status}: 不能算可用`)
    assert.notEqual(view.labelKey, 'footprintStatus.model.available',
      `${status}: 封装不可用时不能给出「3D 模型正常」文案`)
    assert.notEqual(view.labelKey, normal.labelKey, `${status}: 文案必须与正常工作态不同`)
  }
})

test('封装不可用时模型文案是独立 key，且中英文都真的有翻译（不会显示原始 key）', () => {
  const broken = modelView({
    footprint_status: 'stale', model_status: 'available', model_reason: 'ok',
  })
  assert.equal(broken.labelKey, 'footprintStatus.model.footprintUnavailable')
  for (const lang of ['zh', 'en']) {
    const dict = JSON.parse(
      readFileSync(new URL(`../../src/locales/${lang}.json`, import.meta.url), 'utf8'),
    )
    const key = broken.labelKey.split('.').pop()
    assert.equal(typeof dict.footprintStatus.model[key], 'string', `${lang}: ${key} 缺翻译`)
    assert.notEqual(dict.footprintStatus.model[key], dict.footprintStatus.model.available,
      `${lang}: 封装不可用的模型文案不能与「模型正常」相同`)
  }
})

test('列表问题筛选只看状态：没有可用 3D 也算问题', () => {
  assert.equal(isProblemSymbol({ footprint_status: 'available', model_status: 'available' }), false)
  assert.equal(isProblemSymbol({ footprint_status: 'available', model_status: 'unassigned' }), true)
  assert.equal(isProblemSymbol({ footprint_status: 'available', model_status: 'stale' }), true)
  assert.equal(isProblemSymbol({ footprint_status: 'available', model_status: 'check_failed' }), true)
  assert.equal(isProblemSymbol({ footprint_status: 'not_found', model_status: 'available' }), true)
  assert.equal(isProblemSymbol({}), true, '没检查过 = 需要检查')
})

test('引用回退：服务端字段优先，列表 footprint 字段兜底', () => {
  assert.equal(footprintReferenceOf({ footprint_reference: 'A:B', footprint: 'C:D' }), 'A:B')
  assert.equal(footprintReferenceOf({ footprint: 'C:D' }), 'C:D')
  assert.equal(footprintReferenceOf({}), '')
  assert.equal(footprintReferenceOf(null), '')
})
