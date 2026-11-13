import type { Item, Instance, Scope } from '../SlangInterface'

export interface Row {
  path: string
  parent: string
  expandable?: boolean
  item: Item
}
export function structural(item: Item): boolean {
  return ['Instance', 'InstanceArray', 'Scope', 'ScopeArray'].includes(item.kind)
}
export function childPath(parent: string, name: string): string {
  return !parent ? name : name.startsWith('[') ? parent + name : parent + '.' + name
}
export function filterRows(rows: Row[], filter: string, sort: string): Row[] {
  const query = filter.toLowerCase()
  const result = rows.filter((r) =>
    `${r.item.instName} ${(r.item as Instance).declName ?? ''}`.toLowerCase().includes(query)
  )
  return sort === 'source'
    ? result
    : result.sort((a, b) => {
        const key = (r: Row) =>
          sort === 'module' ? ((r.item as Instance).declName ?? '') : r.item.instName
        return key(a).localeCompare(key(b), undefined, { numeric: true })
      })
}
/** No dependency on VS Code or on the sidebar's mutable TreeItems. */
export class HierarchyData {
  generation = 0
  readonly rows = new Map<string, Row>()
  private cache = new Map<string, Item[]>()
  private pending = new Map<string, Promise<Item[]>>()
  constructor(private fetch: (path: string) => Promise<Item[]>) {}
  reset(): void {
    this.generation++
    this.rows.clear()
    this.cache.clear()
    this.pending.clear()
  }
  seed(path: string, items: Item[]): void {
    this.cache.set(path, items)
    for (const item of items) {
      const key = childPath(path, item.instName)
      this.rows.set(key, { path: key, parent: path, item })
      const children = (item as Scope).children
      if (children && (item.kind !== 'Instance' || children.length)) {
        this.seed(key, children)
      }
    }
  }
  async children(path: string): Promise<Item[]> {
    const cached = this.cache.get(path)
    if (cached) {
      return cached
    }
    const pending = this.pending.get(path)
    if (pending) {
      return pending
    }
    const generation = this.generation
    const request = this.fetch(path)
      .then((items) => {
        if (this.generation !== generation) {
          throw new Error('Hierarchy changed; retry')
        }
        this.seed(path, items)
        return items
      })
      .finally(() => {
        if (this.generation === generation) {
          this.pending.delete(path)
        }
      })
    this.pending.set(path, request)
    return request
  }
  loaded(path: string): boolean {
    return this.cache.has(path)
  }
  structure(path: string): Row[] {
    return (this.cache.get(path) ?? [])
      .filter(structural)
      .map((item) => this.rows.get(childPath(path, item.instName))!)
  }
}
export function visibleWindow(count: number, scroll: number, height: number, rowHeight = 28) {
  return {
    start: Math.max(0, Math.floor(scroll / rowHeight) - 8),
    end: Math.min(count, Math.ceil((scroll + height) / rowHeight) + 8),
  }
}
