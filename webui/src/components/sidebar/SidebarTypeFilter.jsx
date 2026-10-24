import { useState } from 'react'
import { useTranslation } from 'react-i18next'
import { typeColorOf, typeIconOf, typeLabelOf } from '../../lib/typeMetadata'

/// 元件类型筛选：items 是 [id, 展示名, 计数]，metadata 是全应用共用的类型配置。
/// 展开状态归本区块自己（不受工作库切换、类型表刷新影响 —— 组件没有 key，
/// 重新渲染不会重置展开）。
/// 筛选一律按稳定 ID 提交：点击只发选择意图，选中由 App 的 handler 清。
export default function SidebarTypeFilter({ items, metadata, selectedId, onSelect }) {
  const { t } = useTranslation()
  const [expanded, setExpanded] = useState(false)

  return (
    <div className="section">
      <div className="section-title" onClick={() => setExpanded(v => !v)}
        style={{cursor:'pointer',userSelect:'none'}}>
        {expanded ? '▾' : '▸'} {t('sidebar.componentTypes')}
      </div>
      {/* 图标/名称/颜色一律取自同一份类型配置；认不出的历史 ID 原样显示 */}
      {expanded && items.map(([id, name, n]) => {
        const color = typeColorOf(metadata, id)
        return (
          <div key={id} className={`nav-item ${selectedId === id ? 'active' : ''}`} onClick={() => onSelect(id)}>
            <span className="type-dot" aria-hidden="true"
              style={color ? { color } : undefined}>{typeIconOf(metadata, id)}</span>
            {' '}{typeLabelOf(metadata, id, name)} <span className="badge">{n}</span>
          </div>
        )
      })}
    </div>
  )
}
