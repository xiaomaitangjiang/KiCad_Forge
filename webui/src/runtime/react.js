// Shared React identity entry. The import map points `react` here so plugin
// bundles and the host resolve the exact same module instance.
//
// Named exports are listed explicitly: an entry that re-exports `*` can be
// tree-shaken down to the names the host itself uses, which would leave
// plugins without useState/useEffect at runtime.
export { default } from 'react'
export {
  Children,
  Component,
  Fragment,
  Profiler,
  PureComponent,
  StrictMode,
  Suspense,
  cloneElement,
  createContext,
  createElement,
  createFactory,
  createRef,
  forwardRef,
  isValidElement,
  lazy,
  memo,
  startTransition,
  use,
  useActionState,
  useCallback,
  useContext,
  useDebugValue,
  useDeferredValue,
  useEffect,
  useId,
  useImperativeHandle,
  useInsertionEffect,
  useLayoutEffect,
  useMemo,
  useOptimistic,
  useReducer,
  useRef,
  useState,
  useSyncExternalStore,
  useTransition,
  version,
} from 'react'
