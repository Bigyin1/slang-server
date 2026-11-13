import tape from 'tape'
import { HierarchyData, childPath, filterRows, visibleWindow } from '../../src/hierarchy/model'
import type { Item } from '../../src/SlangInterface'
const item = (name: string): Item => ({ kind: 'Instance' as Item['kind'], instName: name })
tape('Hierarchy adapter deduplicates requests and rejects obsolete generations', async (t) => {
  let resolve!: (items: Item[]) => void
  let count = 0
  const model = new HierarchyData(() => {
    count++
    return new Promise((r) => {
      resolve = r
    })
  })
  const first = model.children('top')
  const second = model.children('top')
  t.equal(count, 1)
  resolve([item('child')])
  await Promise.all([first, second])
  t.equal(model.structure('top')[0].path, 'top.child')
  await model.children('top')
  t.equal(count, 1, 'cached level does not query the server again')
  model.reset()
  const old = model.children('top')
  model.reset()
  resolve([item('obsolete')])
  try {
    await old
    t.fail('stale response accepted')
  } catch {
    t.pass('stale response rejected')
  }
  t.equal(model.rows.size, 0)
  t.end()
})
tape('Embedded array scopes preserve paths without requests', async (t) => {
  const model = new HierarchyData(async () => {
    throw new Error('Unexpected request')
  })
  model.seed('top', [
    {
      kind: 'ScopeArray',
      instName: 'gen',
      children: [{ kind: 'Scope', instName: '[3]', children: [item('child')] }],
    } as Item,
  ])
  await model.children('top.gen[3]')
  t.ok(model.rows.has('top.gen[3].child'))
  t.equal(childPath('top', 'gen[7]'), 'top.gen[7]')
  t.end()
})
tape('500-child top is lazy and viewport stays bounded', async (t) => {
  const requests: string[] = []
  const model = new HierarchyData(async (path) => {
    requests.push(path)
    return Array.from({ length: 500 }, (_, i) => item('u' + i))
  })
  await model.children('top')
  t.deepEqual(requests, ['top'])
  const rows = model.structure('top')
  t.equal(filterRows(rows, 'u49', 'source').length, 11)
  t.equal(filterRows(rows, '', 'name')[2].item.instName, 'u2')
  const range = visibleWindow(rows.length, 3000, 600)
  t.ok(range.end - range.start < 40)
  t.equal(rows.length, 500)
  t.end()
})
