import assert from 'assert'
import * as vscode from 'vscode'
import type { LanguageClient } from 'vscode-languageclient/node'

const extensionId = 'Hudson-River-Trading.vscode-slang'
const timeoutMs = 15_000

interface HierarchyItem {
  inst: { instName: string }
}

interface ModuleViewItem {
  data: { declName?: string; instPath?: string }
}

interface InstancesView {
  getChildren(item?: ModuleViewItem): Promise<ModuleViewItem[]>
  revealPath(moduleName: string, instancePath: string): Promise<void>
}

interface ProjectComponent {
  refreshSlangCompilation(options?: { preserveFocusedPath?: boolean }): Promise<void>
  includeData: boolean
  includeParams: boolean
  instancesView: InstancesView
  top: HierarchyItem | undefined
  getChildren(item?: HierarchyItem): Promise<HierarchyItem[]>
  getTreeItem(item: HierarchyItem): Promise<vscode.TreeItem>
}

async function waitFor(description: string, predicate: () => boolean): Promise<void> {
  const deadline = Date.now() + timeoutMs
  while (!predicate()) {
    if (Date.now() >= deadline) {
      throw new Error(`Timed out waiting for ${description}`)
    }
    await new Promise((resolve) => setTimeout(resolve, 25))
  }
}

async function executeAndExpectLocation(
  command: string,
  args: unknown,
  uri: vscode.Uri,
  line: number
): Promise<void> {
  await vscode.commands.executeCommand(command, args)
  await waitFor(`${command} to select line ${line + 1}`, () => {
    const editor = vscode.window.activeTextEditor
    return (
      editor?.document.uri.toString() === uri.toString() && editor.selection.start.line === line
    )
  })
}

export async function run(): Promise<void> {
  const workspace = vscode.workspace.workspaceFolders?.[0]
  assert.ok(workspace, 'test workspace was not opened')

  const extension = vscode.extensions.getExtension(extensionId)
  assert.ok(extension, `${extensionId} was not loaded`)
  const extensionApi = (await extension.activate()) as {
    project: ProjectComponent
    client: LanguageClient
    hierarchyExplorer: {
      open(path?: string): Promise<void>
      refresh(reset: boolean): Promise<void>
      data: { rows: Map<string, unknown>; children(path: string): Promise<unknown[]> }
    }
  }

  const uri = vscode.Uri.joinPath(workspace.uri, 'hierarchy.sv')
  const document = await vscode.workspace.openTextDocument(uri)
  const editor = await vscode.window.showTextDocument(document)
  const lines = document.getText().split('\n')
  const moduleLine = lines.findIndex((line) => line.includes('module child'))
  const instanceLine = lines.findIndex((line) => line.includes('child u_child'))
  const nestedInstanceLine = lines.findIndex((line) => line.includes('leaf nested_leaf'))
  const generatedInstanceLine = lines.findIndex((line) => line.includes('leaf generated_leaf'))
  assert.notEqual(moduleLine, -1)
  assert.notEqual(instanceLine, -1)
  assert.notEqual(nestedInstanceLine, -1)
  assert.notEqual(generatedInstanceLine, -1)

  try {
    await vscode.commands.executeCommand('slang.project.setTopLevel', uri)

    const project = extensionApi.project
    assert.equal(project.includeData, true, 'data signals should be visible by default')
    assert.ok(project.top, 'top-level instance was not populated')
    const topChildren = await project.getChildren(project.top)
    assert.ok(
      topChildren.some((item) => item.inst.instName === 'data_signal'),
      'default hierarchy should include data signals'
    )

    for (const name of ['u_child', 'params_only']) {
      const item = topChildren.find((child) => child.inst.instName === name)
      assert.ok(item, `missing hierarchy item ${name}`)
      assert.equal(
        (await project.getTreeItem(item)).collapsibleState,
        vscode.TreeItemCollapsibleState.None,
        `${name} should not be expandable without visible children`
      )
    }

    const emptyPackage = (await project.getChildren()).find(
      (item) => item.inst.instName === 'empty_pkg'
    )
    assert.ok(emptyPackage, 'missing empty package')
    assert.equal(
      (await project.getTreeItem(emptyPackage)).collapsibleState,
      vscode.TreeItemCollapsibleState.None,
      'empty package should not be expandable'
    )

    assert.equal(project.includeParams, false, 'module parameters should be hidden by default')
    const paramsPackage = (await project.getChildren()).find(
      (item) => item.inst.instName === 'params_pkg'
    )
    assert.ok(paramsPackage, 'missing parameter package')
    assert.ok(
      (await project.getChildren(paramsPackage)).some(
        (item) => item.inst.instName === 'PACKAGE_PARAM'
      ),
      'package parameters should remain visible'
    )

    const modules = await project.instancesView.getChildren()
    const childModule = modules.find((item) => item.data.declName === 'child')
    assert.ok(childModule, 'missing child module in Modules view')
    const childInstance = (await project.instancesView.getChildren(childModule)).find(
      (item) => item.data.instPath === 'top.u_child'
    )
    assert.ok(childInstance, 'missing top.u_child in Modules view')

    const revealCalls: [string, string][] = []
    const revealPath = project.instancesView.revealPath.bind(project.instancesView)
    project.instancesView.revealPath = async (moduleName, instancePath) => {
      revealCalls.push([moduleName, instancePath])
      await revealPath(moduleName, instancePath)
    }
    try {
      await vscode.commands.executeCommand(
        'slang.project.instancesView.gotoInstantiation',
        childInstance
      )
    } finally {
      project.instancesView.revealPath = revealPath
    }
    assert.deepEqual(
      revealCalls[revealCalls.length - 1],
      ['top', 'top'],
      'Go to Instantiation should reveal the parent module instance'
    )

    const lenses = await vscode.commands.executeCommand<vscode.CodeLens[]>(
      'vscode.executeCodeLensProvider',
      uri
    )
    const gotoInstantiation = lenses.find((lens) => {
      const params = lens.command?.arguments?.[0] as { hierPath?: string } | undefined
      return lens.command?.title === 'Go to Instantiation' && params?.hierPath === 'top.u_child'
    })
    assert.ok(gotoInstantiation?.command, 'missing child module CodeLens')
    editor.selection = new vscode.Selection(moduleLine, 0, moduleLine, 0)
    await executeAndExpectLocation(
      gotoInstantiation.command.command,
      gotoInstantiation.command.arguments?.[0],
      uri,
      instanceLine
    )

    editor.selection = new vscode.Selection(moduleLine, 0, moduleLine, 0)
    await executeAndExpectLocation(
      'slang.showHierLocation',
      { hierPath: 'top.u_child', takeFocus: true },
      uri,
      instanceLine
    )

    await executeAndExpectLocation(
      'slang.project.setInstance',
      'top.branch_array[0].nested_leaf',
      uri,
      nestedInstanceLine
    )
    await executeAndExpectLocation(
      'slang.project.setInstance',
      'top.generated[1].generated_leaf',
      uri,
      generatedInstanceLine
    )

    const edit = new vscode.WorkspaceEdit()
    edit.insert(
      uri,
      new vscode.Position(instanceLine, 0),
      '// unsaved shift 1\n// unsaved shift 2\n'
    )
    assert.equal(await vscode.workspace.applyEdit(edit), true)
    assert.equal(document.isDirty, true)

    editor.selection = new vscode.Selection(moduleLine, 0, moduleLine, 0)
    await executeAndExpectLocation(
      'slang.showHierLocation',
      { hierPath: 'top.u_child', takeFocus: true },
      uri,
      instanceLine + 2
    )

    const scope = await vscode.commands.executeCommand<unknown[]>('slang.getScope', 'top')
    assert.ok(Array.isArray(scope) && scope.length > 0, 'language server stopped responding')

    const originalClient = extensionApi.client
    const stateChanges: unknown[] = []
    const stateSubscription = originalClient.onDidChangeState((change) => stateChanges.push(change))
    const config = vscode.workspace.getConfiguration('slang')
    const originalPath = config.inspect<string>('path')?.workspaceValue
    const originalOpacity = config.inspect<number>('inactiveRegions.opacity')?.workspaceValue
    try {
      for (const serverPath of ['temporary-server-path', undefined, 'restored-server-path']) {
        await config.update('path', serverPath, vscode.ConfigurationTarget.Workspace)
      }
      await config.update('inactiveRegions.opacity', 0.42, vscode.ConfigurationTarget.Workspace)
      await originalClient.sendRequest('textDocument/documentHighlight', {
        textDocument: { uri: uri.toString() },
        position: { line: 0, character: 0 },
      })
      assert.equal(extensionApi.client, originalClient, 'settings changes must retain the client')
      assert.deepEqual(stateChanges, [], 'settings changes must leave the server running')
    } finally {
      await config.update('path', originalPath, vscode.ConfigurationTarget.Workspace)
      await config.update(
        'inactiveRegions.opacity',
        originalOpacity,
        vscode.ConfigurationTarget.Workspace
      )
      stateSubscription.dispose()
    }

    // Inspecting the same module at another hierarchy path must not activate it.
    const activeBefore = await originalClient.sendRequest('workspace/executeCommand', {
      command: 'slang.getActiveInstance',
      arguments: ['branch'],
    })
    await extensionApi.hierarchyExplorer.open('top.branch_array[1]')
    assert.ok(extensionApi.hierarchyExplorer.data.rows.has('top.branch_array[1]'))
    assert.deepEqual(
      await originalClient.sendRequest('workspace/executeCommand', {
        command: 'slang.getActiveInstance',
        arguments: ['branch'],
      }),
      activeBefore,
      'Explorer selection must not activate an instance'
    )
    await extensionApi.hierarchyExplorer.refresh(false)
    assert.ok(extensionApi.hierarchyExplorer.data.rows.has('top.branch_array[1]'))

    await originalClient.sendRequest('workspace/executeCommand', {
      command: 'slang.setBuildFile',
      arguments: [vscode.Uri.joinPath(workspace.uri, 'explorer.f').fsPath],
    })
    await project.refreshSlangCompilation({ preserveFocusedPath: false })
    await extensionApi.hierarchyExplorer.refresh(true)
    assert.ok(extensionApi.hierarchyExplorer.data.rows.has('explorer_a'))
    assert.ok(extensionApi.hierarchyExplorer.data.rows.has('explorer_b'), 'multiple tops retained')
    for (const [name, width] of [
      ['narrow', '8'],
      ['wide', '16'],
    ]) {
      await extensionApi.hierarchyExplorer.open(`explorer_a.${name}`)
      const details = await extensionApi.hierarchyExplorer.data.children(`explorer_a.${name}`)
      const parameter = details.find(
        (item) => (item as { instName: string }).instName === 'WIDTH'
      ) as { value: string }
      assert.ok(parameter.value.includes(width), 'details use the selected instance parameters')
    }
    const fixture = await vscode.workspace.openTextDocument(
      vscode.Uri.joinPath(workspace.uri, 'explorer.sv')
    )
    const originalFixture = fixture.getText()
    const replaceFixture = async (content: string) => {
      const edit = new vscode.WorkspaceEdit()
      edit.replace(
        fixture.uri,
        new vscode.Range(fixture.positionAt(0), fixture.positionAt(fixture.getText().length)),
        content
      )
      assert.ok(await vscode.workspace.applyEdit(edit))
      assert.ok(await fixture.save())
    }
    try {
      await extensionApi.hierarchyExplorer.open('explorer_a.wide')
      await replaceFixture(originalFixture.replace("    explorer_cell #(.WIDTH(16)) wide('0);", ''))
      await extensionApi.hierarchyExplorer.refresh(false)
      assert.equal(
        extensionApi.hierarchyExplorer.data.rows.has('explorer_a.wide'),
        false,
        'removed instance must not survive refresh'
      )
    } finally {
      await replaceFixture(originalFixture)
      await project.refreshSlangCompilation()
    }
    await extensionApi.hierarchyExplorer.open('explorer_a.external_child')
    const externalRow = extensionApi.hierarchyExplorer.data.rows.get(
      'explorer_a.external_child'
    ) as { item: { declLoc: { uri: string } } }
    assert.ok(
      externalRow.item.declLoc.uri.endsWith('/external_definition_library/external_only.sv')
    )
    await vscode.commands.executeCommand('slang.project.setTopLevel', uri)

    console.log('Hierarchy Explorer integration checks passed')
    const outputChannel = originalClient.outputChannel
    await vscode.commands.executeCommand('slang.restartLanguageServer')
    assert.notEqual(extensionApi.client, originalClient, 'explicit restart must replace the client')
    assert.equal(
      extensionApi.client.outputChannel,
      outputChannel,
      'restart must retain the output channel'
    )
    await extensionApi.client.sendRequest('textDocument/documentHighlight', {
      textDocument: { uri: uri.toString() },
      position: { line: 0, character: 0 },
    })
  } finally {
    if (document.isDirty) {
      await vscode.commands.executeCommand('workbench.action.files.revert')
    }
    await vscode.commands.executeCommand('workbench.action.closeAllEditors')
  }
}
