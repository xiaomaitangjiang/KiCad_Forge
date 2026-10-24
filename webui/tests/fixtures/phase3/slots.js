import * as React from 'react'
import {
  cardAccessory,
  contextMenuItem,
  definePlugin,
  inspectorTab,
  settingsSection,
  sidebarSection,
  toolbarAction,
} from '@kiforge/plugin-sdk'

export const __react = React
export const __reactUseState = React.useState
export const __reactVersion = React.version

function text(testid, label) {
  return React.createElement('div', { 'data-testid': testid }, label)
}

export default definePlugin({
  name: 'test.slots',
  apply(ctx) {
    ctx.slots.register(sidebarSection, {
      id: 'side',
      title: '侧栏贡献',
      render: () => text('slot-sidebar-item', '侧栏贡献'),
    })
    ctx.slots.register(toolbarAction, {
      id: 'tool',
      title: '工具栏贡献',
      render: () => text('slot-toolbar-item', '工具栏贡献'),
    })
    ctx.slots.register(cardAccessory, {
      id: 'card',
      title: '卡片贡献',
      render: (props) => text('slot-card-item', `卡片 ${props.symbol?.id ?? props.symbolId}`),
    })
    ctx.slots.register(contextMenuItem, {
      id: 'menu',
      title: '菜单贡献',
      render: () => text('slot-menu-item', '菜单贡献'),
    })
    ctx.slots.register(settingsSection, {
      id: 'settings',
      title: '设置贡献',
      render: () => text('slot-settings-item', '设置贡献'),
    })

    ctx.slots.register(inspectorTab, {
      id: 'ok',
      key: 'ok',
      title: '正常页签',
      render: () => text('slot-tab-ok', '正常页签内容'),
    })
    ctx.slots.register(inspectorTab, {
      id: 'boom',
      key: 'boom',
      title: '异常页签',
      render: () => {
        if (globalThis.__KF_BOOM__) throw new Error('fixture tab boom')
        return text('slot-tab-boom', '异常页签内容')
      },
    })
  },
})
