import { context } from 'esbuild'
import { fileURLToPath } from 'node:url'
const flags = new Set(process.argv.slice(2))
const common = {
  absWorkingDir: fileURLToPath(new URL('../', import.meta.url)),
  bundle: true,
  minify: flags.has('--minify'),
  sourcemap: !flags.has('--minify'),
}
const builds = await Promise.all([
  context({
    ...common,
    entryPoints: ['src/extension.ts'],
    external: ['vscode'],
    format: 'cjs',
    platform: 'node',
    outfile: 'dist/extension.js',
    mainFields: ['module', 'main'],
  }),
  context({
    ...common,
    entryPoints: ['src/hierarchy/webview.ts'],
    platform: 'browser',
    outfile: 'dist/hierarchy.js',
  }),
])
if (flags.has('--watch')) await Promise.all(builds.map((build) => build.watch()))
else {
  try {
    await Promise.all(builds.map((build) => build.rebuild()))
  } finally {
    await Promise.all(builds.map((build) => build.dispose()))
  }
}
