import './webview.css'
import { filterRows, visibleWindow, type Row } from './model'
import type { Item, Instance, Var } from '../SlangInterface'
declare function acquireVsCodeApi(): {
  postMessage(m: unknown): void
  getState(): any
  setState(s: unknown): void
}
const api = acquireVsCodeApi()
const element = (id: string) => document.getElementById(id)!
const send = (type: string, path?: string) => api.postMessage({ type, path })
let state = {
  rows: [] as Row[],
  expanded: [] as string[],
  selected: '',
  focus: '',
  active: '',
  details: [] as Item[],
  available: false,
}
const saved = api.getState() ?? {}
let restoredScroll = false
let tab = saved.tab ?? 'Port'
let widths: number[] = saved.widths ?? [320, 220, 150, 260]
let detailWidth = saved.detailWidth ?? 340
let view: (Row & { depth: number })[] = []
const viewport = element('viewport')
const filter = element('filter') as HTMLInputElement
const sort = element('sort') as HTMLSelectElement
filter.value = saved.filter ?? ''
sort.value = saved.sort ?? 'source'
function persist() {
  api.setState({
    expanded: state.expanded,
    selected: state.selected,
    focus: state.focus,
    tab,
    widths,
    detailWidth,
    filter: filter.value,
    sort: sort.value,
    scroll: viewport.scrollTop,
  })
}
function button(text: string, action: () => void, parent: HTMLElement) {
  const b = document.createElement('button')
  b.textContent = text
  b.onclick = action
  parent.append(b)
  return b
}
function text(tag: string, value: string, parent: HTMLElement) {
  const e = document.createElement(tag)
  e.textContent = value
  parent.append(e)
  return e
}
function flatten() {
  view = []
  const groups = new Map<string, Row[]>()
  for (const row of state.rows) {
    const children = groups.get(row.parent) ?? []
    children.push(row)
    groups.set(row.parent, children)
  }
  const expanded = new Set(state.expanded)
  const visit = (parent: string, depth: number) => {
    for (const row of filterRows(groups.get(parent) ?? [], filter.value, sort.value)) {
      view.push({ ...row, depth })
      if (expanded.has(row.path)) {
        visit(row.path, depth + 1)
      }
    }
  }
  if (state.focus) {
    const root = state.rows.find((r) => r.path === state.focus)
    if (root) {
      view.push({ ...root, depth: 0 })
      visit(root.path, 1)
    }
  } else {
    visit('', 0)
  }
}
function renderRows() {
  const canvas = element('rows')
  canvas.replaceChildren()
  canvas.style.height = `${view.length * 28}px`
  const window = visibleWindow(view.length, viewport.scrollTop, viewport.clientHeight)
  viewport.setAttribute('aria-rowcount', String(view.length))
  for (let i = window.start; i < window.end; i++) {
    const row = view[i]
    const line = document.createElement('div')
    line.className = 'row columns'
    line.style.top = `${i * 28}px`
    line.setAttribute('role', 'row')
    line.setAttribute('aria-level', String(row.depth + 1))
    line.setAttribute('aria-rowindex', String(i + 1))
    line.setAttribute('aria-selected', String(state.selected === row.path))
    line.title = row.path
    if (row.path === state.selected) {
      line.classList.add('selected')
    }
    const cell = document.createElement('span')
    cell.setAttribute('role', 'gridcell')
    cell.style.paddingLeft = `${row.depth * 18}px`
    const inst = row.item as Instance
    const expandable =
      row.expandable ??
      (inst.hasChildren !== false || state.rows.some((r) => r.parent === row.path))
    if (expandable) {
      line.setAttribute('aria-expanded', String(state.expanded.includes(row.path)))
      button(state.expanded.includes(row.path) ? '▾' : '▸', () => send('expand', row.path), cell)
    }
    text('span', `${row.path === state.active ? '● ' : ''}${row.item.instName}`, cell)
    line.append(cell)
    const uri = (inst as Instance & { declLoc?: { uri: string } }).declLoc?.uri
    for (const value of [
      inst.declName ?? '',
      inst.declKind ?? row.item.kind,
      uri ? decodeURI(uri).split('/').pop()! : '—',
    ]) {
      const e = text('span', value, line)
      e.setAttribute('role', 'gridcell')
    }
    line.onclick = (e) => {
      if (!(e.target as HTMLElement).closest('button')) {
        send('select', row.path)
      }
    }
    line.ondblclick = () => send('focus', row.path)
    canvas.append(line)
  }
}
function render() {
  document.documentElement.style.setProperty('--columns', widths.map((w) => `${w}px`).join(' '))
  document.documentElement.style.setProperty('--details', `${detailWidth}px`)
  flatten()
  renderRows()
  element('crumbs').replaceChildren()
  const chain: Row[] = []
  let current = state.rows.find((r) => r.path === state.focus)
  while (current) {
    chain.unshift(current)
    current = state.rows.find((r) => r.path === current!.parent)
  }
  for (const row of chain) {
    button(row.item.instName, () => send('focus', row.path), element('crumbs'))
  }
  element('selected').textContent = state.selected || 'Select a block'
  const actions = element('actions')
  actions.replaceChildren()
  if (state.selected) {
    for (const [label, action] of [
      ['Make active', 'activate'],
      ['Instantiation', 'instance'],
      ['Definition', 'definition'],
      ['Copy path', 'copy'],
      ['Show in sidebar', 'sidebar'],
      ['Focus subtree', 'focus'],
    ]) {
      button(label, () => send(action, state.selected), actions)
    }
  }
  element('tabs').replaceChildren()
  for (const [label, kind] of [
    ['Ports', 'Port'],
    ['Parameters', 'Param'],
    ['Signals', 'Logic'],
  ]) {
    const b = button(
      label,
      () => {
        tab = kind
        render()
        persist()
      },
      element('tabs')
    )
    b.setAttribute('aria-pressed', String(tab === kind))
  }
  const details = element('details')
  details.replaceChildren()
  const items = state.details.filter(
    (item) =>
      item.kind === tab ||
      (tab === 'Port' && ['InterfacePort', 'InterfacePortArray'].includes(item.kind))
  )
  for (const item of items) {
    const row = document.createElement('div')
    row.className = 'detail'
    text('strong', item.instName, row)
    text('code', (item as Var).type ?? '', row)
    if ((item as Var).value !== undefined) {
      text('code', (item as Var).value!, row)
    }
    details.append(row)
  }
  if (!items.length) {
    text(
      'p',
      state.selected ? 'No members in this category.' : 'Select a block to inspect its members.',
      details
    )
  }
  element('status').textContent = state.available
    ? 'Hierarchy from the latest global compilation; local edits may not be reflected. Use Refresh to reload the view.'
    : 'Select a build file or top level first.'
  persist()
}
viewport.onscroll = () => {
  renderRows()
  persist()
}
new ResizeObserver(renderRows).observe(viewport)
filter.oninput = () => {
  viewport.scrollTop = 0
  render()
}
sort.onchange = render
viewport.onkeydown = (e) => {
  const index = Math.max(
    0,
    view.findIndex((r) => r.path === state.selected)
  )
  let row = view[index]
  if (!row) {
    return
  }
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    row = view[Math.max(0, Math.min(view.length - 1, index + (e.key === 'ArrowDown' ? 1 : -1)))]
    send('select', row.path)
    const pos = view.indexOf(row) * 28
    if (pos < viewport.scrollTop || pos + 28 > viewport.scrollTop + viewport.clientHeight) {
      viewport.scrollTop = pos
    }
  } else if (e.key === 'ArrowRight' && !state.expanded.includes(row.path)) {
    send('expand', row.path)
  } else if (e.key === 'ArrowLeft') {
    if (state.expanded.includes(row.path)) {
      send('expand', row.path)
    } else {
      send('select', row.parent)
    }
  } else if (e.key === 'Enter') {
    send('focus', row.path)
  } else {
    return
  }
  e.preventDefault()
}
element('root').onclick = () => send('root')
element('up').onclick = () => {
  const row = state.rows.find((r) => r.path === state.focus)
  send(row?.parent ? 'focus' : 'root', row?.parent)
}
element('refresh').onclick = () => send('refresh')
let timer: ReturnType<typeof setTimeout>
const search = element('search') as HTMLInputElement
search.oninput = () => {
  clearTimeout(timer)
  element('results').replaceChildren()
  if (search.value.trim()) {
    timer = setTimeout(() => api.postMessage({ type: 'search', query: search.value }), 250)
  }
}
window.addEventListener('message', (e) => {
  const message = e.data
  if (message.type === 'state') {
    const changed = state.selected !== message.selected
    state = message
    render()
    if (!restoredScroll) {
      viewport.scrollTop = saved.scroll ?? 0
      restoredScroll = true
      renderRows()
    }
    if (changed && state.selected) {
      const index = view.findIndex((row) => row.path === state.selected)
      if (
        index >= 0 &&
        (index * 28 < viewport.scrollTop ||
          (index + 1) * 28 > viewport.scrollTop + viewport.clientHeight)
      ) {
        viewport.scrollTop = index * 28
        renderRows()
      }
    }
  } else if (message.type === 'loading') {
    element('status').textContent = 'Loading hierarchy…'
  } else if (message.type === 'error') {
    element('status').textContent = `${message.error} — use Refresh to retry.`
  } else if (message.type === 'search' && message.query === search.value) {
    const results = element('results')
    results.replaceChildren()
    text(
      'div',
      `${message.result?.matches.length ?? 0} of ${message.result?.totalResults ?? 0} matches`,
      results
    )
    for (const match of message.result?.matches ?? []) {
      button(
        `${match.path} — ${match.description ?? ''}`,
        () => {
          filter.value = ''
          send('reveal', match.path)
          results.replaceChildren()
        },
        results
      )
    }
  }
})
function drag(handle: HTMLElement, move: (dx: number) => void) {
  handle.onpointerdown = (e) => {
    let start = e.clientX
    handle.setPointerCapture(e.pointerId)
    handle.onpointermove = (event) => {
      move(event.clientX - start)
      start = event.clientX
    }
    handle.onpointerup = () => {
      handle.onpointermove = null
      persist()
    }
  }
}
const divider = element('divider')
divider.onkeydown = (e) => {
  if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
    detailWidth = Math.max(220, detailWidth + (e.key === 'ArrowLeft' ? 20 : -20))
    render()
    e.preventDefault()
  }
}
drag(divider, (dx) => {
  detailWidth = Math.max(220, detailWidth - dx)
  document.documentElement.style.setProperty('--details', `${detailWidth}px`)
})
// Install persistent resize handles in the header; virtual rows never own listeners.
for (const [i, cell] of Array.from(
  document.querySelectorAll<HTMLElement>('#tree > .columns > span')
).entries()) {
  const handle = document.createElement('i')
  cell.append(handle)
  handle.onpointerdown = (e) => {
    const initial = widths[i]
    const start = e.clientX
    handle.setPointerCapture(e.pointerId)
    handle.onpointermove = (event) => {
      widths[i] = Math.max(90, initial + event.clientX - start)
      document.documentElement.style.setProperty('--columns', widths.map((w) => `${w}px`).join(' '))
    }
    handle.onpointerup = () => {
      handle.onpointermove = null
      persist()
    }
  }
}
api.postMessage({ type: 'ready', state: saved })
viewport.scrollTop = saved.scroll ?? 0
