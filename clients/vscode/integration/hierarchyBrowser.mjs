import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawn } from 'node:child_process'
const root = fileURLToPath(new URL('../', import.meta.url))
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'slang-hierarchy-browser-'))
const started = performance.now()
const template = fs.readFileSync(root + '/resources/hierarchy.html', 'utf8')
const body = template.slice(template.indexOf('<body>') + 6, template.indexOf('<script'))
const js = fs.readFileSync(root + '/dist/hierarchy.js', 'utf8')
const css = fs.readFileSync(root + '/dist/hierarchy.css', 'utf8')
let rows = Array.from({ length: 500 }, (_, i) => ({
  path: 'top.u' + i,
  parent: 'top',
  item: {
    kind: 'Instance',
    instName: 'u' + i,
    declName: 'child',
    declKind: 'Module',
    hasChildren: false,
  },
}))
rows.unshift({
  path: 'top',
  parent: '',
  item: {
    kind: 'Instance',
    instName: 'top',
    declName: 'top',
    declKind: 'Module',
    hasChildren: true,
  },
})
if (process.argv[2]) rows = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'))
fs.writeFileSync(
  path.join(temporary, 'index.html'),
  `<!doctype html><style>${css}</style>${body}<script>
const messages=[]; function acquireVsCodeApi(){return {getState:()=>null,setState:()=>{},postMessage:m=>messages.push(m)}}
</script><script>${js}</script><script>
window.dispatchEvent(new MessageEvent('message',{data:${JSON.stringify({ type: 'state', rows, expanded: [rows[0].path], selected: '', focus: '', active: '', details: [], available: true })}}));
setTimeout(()=>{const before=document.querySelectorAll('.row').length; document.querySelector('.row').click();
const viewport=document.getElementById('viewport');viewport.scrollTop=9000;viewport.dispatchEvent(new Event('scroll'));
setTimeout(()=>{const after=document.querySelectorAll('.row').length;document.body.dataset.test=JSON.stringify({before,after,total:${rows.length},height:viewport.clientHeight,selected:messages.some(m=>m.type==='select'),activated:messages.some(m=>m.type==='activate'),heap:performance.memory?.usedJSHeapSize});},100);},100);
</script>`
)
const chrome = spawn(
  process.env.CHROME_PATH ??
    (process.platform === 'darwin'
      ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
      : 'google-chrome'),
  [
    '--headless',
    '--enable-precise-memory-info',
    '--no-first-run',
    '--no-default-browser-check',
    '--user-data-dir=' + path.join(temporary, 'profile'),
    '--window-size=1400,900',
    '--virtual-time-budget=2000',
    '--dump-dom',
    'file://' + path.join(temporary, 'index.html'),
  ]
)
chrome.on('error', (error) => {
  console.error(error)
  process.exitCode = 1
})
let output = ''
chrome.stdout.on('data', (d) => (output += d))
chrome.stderr.resume()
chrome.on('exit', () => {
  const result = output.match(/data-test="([^"]*)"/)
  if (!result) {
    console.log(output.slice(-1500))
    process.exitCode = 1
    return
  }
  const data = JSON.parse(result[1].replaceAll('&quot;', '"').replaceAll('&amp;', '&'))
  console.log({ ...data, elapsedMs: performance.now() - started })
  fs.rmSync(temporary, { recursive: true, force: true })
  if (data.before > 60 || data.after > 60 || data.activated || !data.selected) process.exitCode = 1
})
