import { after, test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'vite';
const server = await createServer({ logLevel:'error', server:{middlewareMode:true, hmr:false, ws:false, watch:null}, appType:'custom' });
after(() => server.close());
const model = await server.ssrLoadModule('/src/shared/model/workspaceModel.ts');
const { createConsoleTransport } = await server.ssrLoadModule('/src/shared/api/consoleApi.ts');
const { parseWordImport } = await server.ssrLoadModule('/src/features/vocabulary/wordImport.ts');
const books = await server.ssrLoadModule('/src/shared/model/wordbooks.ts');
const device = { id:'one', name:'one', location:'', color:'lilac', online:true };

test('real workspaces start without invented todos, schedules or learning statistics', () => {
  const data = model.emptyLiveData(device);
  assert.deepEqual(data.todos, []); assert.deepEqual(data.schedules, []);
  assert.equal(data.features.modules.connected, null);
  assert.equal(data.features.vocabulary.reviewedCount, 0);
  assert.ok(data.wordbooks.books.every(book => book.words.every(word => word.review.reviewCount === 0)));
});
test('device responses stay scoped and UUID image selections survive normalization', () => {
  const data = model.emptyLiveData(device), id='12345678-1234-1234-1234-123456789abc';
  const snapshot = model.parseWorkspace({ device_id:'one', revision:'r1', data, reported:{ modules:{connected:'pen', pen:{imageId:id}} } },'one');
  assert.equal(model.workspaceData(snapshot, device).features.modules.pen.imageId, id);
  assert.throws(() => model.parseWorkspace({...snapshot, device_id:'two'}, 'one'), /不一致/);
});
test('saving a face never writes a stale module selection, focus countdown or learning counters', () => {
  const before=model.emptyLiveData(device), next=structuredClone(before);
  next.features.watchFace.idleSeconds=300;
  next.features.vocabulary.reviewedCount=999;
  next.features.modules.connected='light';
  assert.deepEqual(model.workspacePatch(before,next), {features:{watchFace:next.features.watchFace}});
});
test('live settings use versioned authenticated server writes and propagate failures', async () => {
  let sent;
  const api=createConsoleTransport(async (path, options) => { sent={path,options}; return Response.json({device_id:'one',revision:'r2',data:{}}); });
  await api.saveWorkspace('one','r1',{deviceName:'书房'},'operation-one');
  assert.equal(sent.options.method,'PUT'); assert.equal(sent.options.credentials,'same-origin');
  assert.deepEqual(JSON.parse(sent.options.body),{revision:'r1',data:{deviceName:'书房'},event_id:'operation-one'});
  const failed=createConsoleTransport(async()=>Response.json({code:'WORKSPACE_ERROR',error:'设备离线'}, {status:409}));
  await assert.rejects(failed.workspaceAction('one','microphone.set',{enabled:false},'one'), /设备离线/);
});

test('word import handles exported headers, quoted CSV examples, duplicate words and TSV', () => {
  const cards=parseWordImport('单词,音标,释义,英文例句,例句翻译\nfocus,,专注,"Focus, and rest.",集中\nfocus,,重复,,');
  assert.equal(cards.length,1); assert.equal(cards[0].example,'Focus, and rest.');
  assert.equal(parseWordImport('word\tmeaning\nserene\t安静')[0].word,'serene');
  assert.throws(()=>parseWordImport('word,meaning\nserene,"missing'), /引号/);
  assert.throws(()=>parseWordImport('word,meaning\nserene,'), /第 2 行/);
});

test('compact polling preserves the same device vocabulary and switching to CET4 sends only its identifier', () => {
  const data = { wordbooks: { activeBookId: 'daily-english', dailyNewLimit: 20, books: [
    { id: 'cet4', name: '大学英语四级', catalog: 'cet4:1', progress: [] },
  ] } };
  const initial = model.parseWorkspace({ device_id: 'one', revision: 'r1', wordbooks_revision: 'w1', data }, 'one');
  const poll = model.parseWorkspace({ device_id: 'one', revision: 'r2', wordbooks_revision: 'w1', data: {} }, 'one');
  const combined = model.mergeWorkspaceSnapshot(poll, initial);
  const restored = books.normalizeWordbooks(combined.data.wordbooks);
  const cet4 = restored.books.find((book) => book.id === 'cet4');
  assert.equal(cet4.words.length, 4413);
  assert.equal(cet4.words.filter((card) => card.word.toLowerCase() === 'polish').length, 2);
  assert.deepEqual(model.workspaceRequestPatch(restored, { wordbooks: { ...restored, activeBookId: 'cet4' } }), {
    wordbookChanges: { activeBookId: 'cet4' },
  });
  assert.throws(() => model.mergeWorkspaceSnapshot({ ...poll, device_id: 'two' }, initial), /词本缓存/);
});
