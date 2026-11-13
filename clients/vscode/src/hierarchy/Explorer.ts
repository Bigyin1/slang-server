import * as vscode from 'vscode'
import { randomBytes } from 'crypto'
import * as slang from '../SlangInterface'
import { HierarchyData } from './model'
import type { ProjectComponent } from '../sidebar/ProjectComponent'
import type { LanguageClient } from 'vscode-languageclient/node'

type Located = slang.Instance & { declLoc?: { uri: string; range: unknown } }
export class HierarchyExplorer implements vscode.Disposable {
  private panel?: vscode.WebviewPanel
  private ready?: Promise<void>
  private resolveReady?: () => void
  readonly data = new HierarchyData(slang.getScope)
  private expanded = new Set<string>()
  private selected = ''
  private focus = ''
  private active = ''
  private refreshTask: Promise<void> = Promise.resolve()
  private serial = 0
  private pendingSearch?: string
  private searching = false
  private disposables: vscode.Disposable[] = []
  constructor(
    private context: vscode.ExtensionContext,
    private project: ProjectComponent,
    private client: () => LanguageClient | undefined
  ) {
    this.disposables.push(
      project.onHierarchyUpdated((reset) => {
        void this.refresh(reset)
      })
    )
    this.disposables.push(
      vscode.window.registerWebviewPanelSerializer('slang.hierarchyExplorer', {
        deserializeWebviewPanel: async (panel, state: any) => {
          await this.attach(panel)
          this.expanded = new Set(Array.isArray(state?.expanded) ? state.expanded : [])
          this.selected = typeof state?.selected === 'string' ? state.selected : ''
          this.focus = typeof state?.focus === 'string' ? state.focus : ''
          await this.ready
        },
      })
    )
  }
  connectionChanged(running: boolean): void {
    if (running) {
      void this.refresh(false)
    } else {
      this.serial++
      this.data.reset()
      void this.panel?.webview.postMessage({ type: 'error', error: 'Language server disconnected' })
    }
  }
  setActive(path: string): void {
    this.active = path
    void this.publish().catch(() => {})
  }
  async open(path?: string): Promise<void> {
    if (!this.panel) {
      await this.attach(
        vscode.window.createWebviewPanel(
          'slang.hierarchyExplorer',
          'Hierarchy Explorer',
          vscode.ViewColumn.Active,
          { enableScripts: true }
        )
      )
      await this.ready
    } else {
      this.panel.reveal()
    }
    if (path) {
      await this.revealStable(path)
      this.focus = this.data.rows.has(path) ? path : ''
      await this.publish()
    }
  }
  private async attach(panel: vscode.WebviewPanel): Promise<void> {
    this.panel = panel
    this.ready = new Promise((resolve) => {
      this.resolveReady = resolve
    })
    panel.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.context.extensionUri, 'dist')],
    }
    const nonce = randomBytes(24).toString('hex')
    const uri = (file: string) =>
      panel.webview.asWebviewUri(vscode.Uri.joinPath(this.context.extensionUri, 'dist', file))
    const template = Buffer.from(
      await vscode.workspace.fs.readFile(
        vscode.Uri.joinPath(this.context.extensionUri, 'resources', 'hierarchy.html')
      )
    ).toString('utf8')
    const html = template
      .split('{{cspSource}}')
      .join(panel.webview.cspSource)
      .split('{{nonce}}')
      .join(nonce)
      .split('{{stylesheet}}')
      .join(uri('hierarchy.css').toString())
      .split('{{script}}')
      .join(uri('hierarchy.js').toString())
    const messages = panel.webview.onDidReceiveMessage((message) => {
      void this.message(message).catch((error) => {
        void panel.webview.postMessage({ type: 'error', error: String(error) })
      })
    })
    panel.webview.html = html
    panel.onDidDispose(() => {
      messages.dispose()
      this.resolveReady?.()
      if (this.panel === panel) {
        this.panel = undefined
      }
    })
  }
  refresh(reset: boolean): Promise<void> {
    const task = this.refreshTask.then(() => this.refreshData(reset))
    this.refreshTask = task.catch(() => {})
    return task
  }
  private async refreshData(reset: boolean): Promise<void> {
    this.serial++
    this.data.reset()
    if (reset) {
      this.expanded.clear()
      this.selected = ''
      this.focus = ''
    }
    if (!this.panel) {
      return
    }
    if (!this.client()?.isRunning()) {
      this.connectionChanged(false)
      return
    }
    await this.panel.webview.postMessage({ type: 'loading' })
    try {
      const serial = this.serial
      await this.data.children('')
      if (serial !== this.serial) {
        return
      }
      const roots = this.data.structure('')
      if (!this.expanded.size && roots.length === 1) {
        this.expanded.add(roots[0].path)
      }
      const previousSelection = this.selected
      // Restore only explicitly expanded branches, never crawl collapsed subtrees.
      for (const path of [...this.expanded]) {
        if (!this.data.rows.has(path)) {
          await this.reveal(path)
        }
        if (this.data.rows.has(path)) {
          await this.data.children(path)
        } else {
          this.expanded.delete(path)
        }
      }
      if (previousSelection && !this.data.rows.has(previousSelection)) {
        await this.reveal(previousSelection)
      }
      this.selected = this.data.rows.has(previousSelection) ? previousSelection : ''
      if (!this.data.rows.has(this.focus)) {
        this.focus = ''
      }
      await this.publish()
    } catch (error) {
      await this.panel?.webview.postMessage({ type: 'error', error: String(error) })
    }
  }
  private async revealStable(path: string): Promise<void> {
    for (;;) {
      await this.refreshTask
      const generation = this.data.generation
      await this.reveal(path)
      if (generation === this.data.generation) {
        return
      }
    }
  }
  private async reveal(path: string): Promise<void> {
    const generation = this.data.generation
    const scopes = await slang.getScopes(path)
    if (generation !== this.data.generation) {
      return
    }
    for (const scope of scopes) {
      this.data.seed(scope.path, scope.children)
      if (scope.path) {
        this.expanded.add(scope.path)
      }
    }
    this.selected = this.data.rows.has(path) ? path : ''
  }
  private async publish(): Promise<void> {
    if (!this.panel) {
      return
    }
    const generation = this.data.generation
    const selected = this.selected
    const details = selected ? await this.data.children(selected) : []
    if (generation !== this.data.generation || selected !== this.selected) {
      return
    }
    await this.panel?.webview.postMessage({
      type: 'state',
      rows: [...this.data.rows.values()]
        .filter((r) => ['Instance', 'InstanceArray', 'Scope', 'ScopeArray'].includes(r.item.kind))
        .map((row) => ({
          ...row,
          expandable: this.data.loaded(row.path)
            ? this.data.structure(row.path).length > 0
            : (row.item as slang.Instance).hasChildren !== false,
        })),
      expanded: [...this.expanded],
      selected: this.selected,
      focus: this.focus,
      active: this.active,
      details,
      available: !!this.project.unit,
    })
  }
  // Webview can request actions only on paths obtained from this build's adapter.
  private async message(m: {
    type?: string
    path?: string
    query?: string
    state?: { expanded?: string[]; selected?: string; focus?: string }
  }): Promise<void> {
    if (m.type === 'ready') {
      if (m.state) {
        this.expanded = new Set((m.state.expanded ?? []).filter((p) => typeof p === 'string'))
        this.selected = typeof m.state.selected === 'string' ? m.state.selected : ''
        this.focus = typeof m.state.focus === 'string' ? m.state.focus : ''
      }
      try {
        await this.refresh(false)
      } finally {
        this.resolveReady?.()
      }
      return
    }
    if (m.type === 'refresh') {
      await this.refresh(false)
      return
    }
    if (m.type === 'search' && typeof m.query === 'string') {
      this.pendingSearch = m.query.slice(0, 1024)
      if (this.searching) {
        return
      }
      this.searching = true
      try {
        while (this.pendingSearch !== undefined) {
          const query = this.pendingSearch
          this.pendingSearch = undefined
          const serial = this.serial
          const result = await vscode.commands.executeCommand<slang.HierarchySearchResult>(
            'slang.searchHierarchyStructure',
            query
          )
          if (serial === this.serial && this.pendingSearch === undefined) {
            await this.panel?.webview.postMessage({ type: 'search', query, result })
          }
        }
      } finally {
        this.searching = false
      }
      return
    }
    if (m.type === 'reveal' && typeof m.path === 'string') {
      await this.revealStable(m.path)
      this.focus = ''
      await this.publish()
      return
    }
    if (m.type === 'root') {
      this.focus = ''
      await this.publish()
      return
    }
    await this.refreshTask
    const row = typeof m.path === 'string' ? this.data.rows.get(m.path) : undefined
    if (!row) {
      return
    }
    const path = row.path
    switch (m.type) {
      case 'expand':
        if (this.expanded.has(path)) {
          this.expanded.delete(path)
        } else {
          await this.data.children(path)
          this.expanded.add(path)
        }
        break
      case 'select':
        this.selected = path
        break
      case 'focus':
        this.focus = path
        this.expanded.add(path)
        await this.data.children(path)
        break
      case 'activate':
        if (
          await vscode.commands.executeCommand('slang.activateInstance', {
            hierPath: path,
            interactionSource: 'editor',
          })
        ) {
          this.active = path
        }
        break
      case 'instance':
        await slang.showHierLocation(path, true)
        break
      case 'definition': {
        const location = (row.item as Located).declLoc
        if (location) {
          await vscode.commands.executeCommand('slang.showLocation', location)
        }
        break
      }
      case 'copy':
        await vscode.env.clipboard.writeText(path)
        break
      case 'sidebar':
        await this.project.showInHierarchy({
          hierPath: path,
          interactionSource: slang.InteractionSource.Editor,
        })
        break
    }
    await this.publish()
  }
  dispose(): void {
    this.panel?.dispose()
    this.disposables.forEach((d) => d.dispose())
  }
}
