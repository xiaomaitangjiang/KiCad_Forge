/**
 * 中心页的内建页面外壳：没有 overlay、没有独立关闭按钮。
 * 页签、返回和面包屑由 CenterWorkspace 统一提供，页面只负责自己的内容。
 */
export default function CenterPage({ title, className = '', children }) {
  return (
    <section className={`center-page-surface ${className}`.trim()} aria-label={title}>
      {title && <h3 className="center-page-title">{title}</h3>}
      {children}
    </section>
  )
}
