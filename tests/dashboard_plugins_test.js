'use strict';
// Focused DOM/logic checks using Node's built-in modules. No dependencies or audio service.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class Element {
    constructor(tag = 'div') {
        this.tagName = tag.toUpperCase(); this.children = []; this.listeners = {};
        this.style = {}; this.dataset = {}; this.attributes = {}; this.value = '';
        this.disabled = false; this.hidden = false; this.type = ''; this.required = false;
        this.textContent = ''; this.className = ''; this.validationMessage = '';
        this.classList = { add() {}, remove() {}, toggle() {} };
    }
    set value(value) { this._value = String(value); }
    get value() { return this._value ?? ''; }
    appendChild(child) { child.parent = this; this.children.push(child); return child; }
    remove() { if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this); }
    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
    append(...children) { children.forEach(child => this.appendChild(child)); }
    replaceChildren(...children) { this.children = []; this.append(...children); }
    addEventListener(event, callback) { (this.listeners[event] ??= []).push(callback); }
    setAttribute(name, value) { this.attributes[name] = value; }
    querySelectorAll(selector) {
        const tags = selector.split(',').map(tag => tag.trim().toUpperCase());
        const matches = child => tags.includes(child.tagName) ||
            (selector.startsWith('.') && child.className.split(' ').includes(selector.slice(1)));
        return this.children.flatMap(child => [
            ...(matches(child) ? [child] : []), ...child.querySelectorAll(selector)
        ]);
    }
    setCustomValidity(message) { this.validationMessage = message; }
    checkValidity() {
        if (this.validationMessage) return false;
        if (this.required && String(this.value) === '') return false;
        if (this.type === 'number' && this.value !== '') {
            const number = Number(this.value);
            if (!Number.isFinite(number)) return false;
            if (this.min != null && number < Number(this.min)) return false;
            if (this.max != null && number > Number(this.max)) return false;
        }
        return true;
    }
    reportValidity() { return this.checkValidity(); }
}

const elements = new Map();
const document = {
    getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
    createElement: tag => new Element(tag),
    createElementNS: (_, tag) => new Element(tag),
    querySelectorAll: () => [],
};
const html = fs.readFileSync(path.join(__dirname, '../web_client/dashboard.html'), 'utf8');
const source = html.match(/<script>([\s\S]*?)<\/script>/)[1].replace(/\binit\(\);\s*$/, '');
assert(!html.includes('<option value="gain">'));
assert(!source.includes("type === 'compressor'"));
const context = vm.createContext({
    document, console, window: {},
    localStorage: { getItem: () => null, setItem() {}, removeItem() {} },
    ResizeObserver: class { observe() {} }, setInterval() {}, setTimeout() {}, clearTimeout() {},
    AbortController, URLSearchParams, alert() {},
});
vm.runInContext(source, context); // Also validates syntax of the entire dashboard script.
const run = js => vm.runInContext(js, context);
const state = js => JSON.parse(run('JSON.stringify(' + js + ')'));
const descriptor = {
    type: 'new_cpp_plugin', name: '<New C++ Plugin>', description: 'No UI effect-specific code',
    defaults: { amount: 0, count: 2, enabled: false, mode: 'a', label: '' },
    parameters: [
        { id: 'amount', name: 'Amount', type: 'number', default: 0, min: 0, max: 8, step: 0.01, unit: 'linear' },
        { id: 'count', name: 'Count', type: 'integer', default: 2, min: 0, max: 10 },
        { id: 'enabled', name: 'Enabled', type: 'boolean', default: false },
        { id: 'mode', name: 'Mode', type: 'enum', default: 'a', choices: [{ label: 'A', value: 'a' }, { label: 'B', value: 'b' }] },
        { id: 'label', name: 'Label', type: 'string', default: '' },
    ],
};
context.discoveryResponse = { schema_version: 1, max_chain_length: 16, processors: [descriptor] };

(async () => {
    run('fetchAPI = async () => discoveryResponse;');
    await run('loadDspAvailable()');
    assert.equal(state('dspDiscovery.status'), 'ready');
    assert.equal(elements.get('new-processor-type').children[0].textContent, '<New C++ Plugin>');
    assert.equal(elements.get('new-processor-type').children[0].value, 'new_cpp_plugin');
    elements.get('new-processor-type').value = descriptor.type;
    run('editingRouteId = 1; currentRouteDspState = {active:false,gain:1,chain:[]}; addProcessorToChain();');
    assert.deepEqual(state('currentRouteDspState.chain[0].params'), descriptor.defaults);
    const bindings = run('dspParameterBindings');
    assert.equal(bindings.length, 5);
    assert.equal(bindings[0].control.type, 'number');
    assert.equal(Number(bindings[0].control.min), 0);
    assert.equal(bindings[1].control.step, 1);
    assert.equal(bindings[2].control.tagName, 'SELECT');
    assert.equal(bindings[3].control.tagName, 'SELECT');
    assert.equal(bindings[4].control.type, 'text');
    elements.get('modal-gain').value = '0';
    elements.get('modal-dsp-gain').value = '0';
    elements.get('modal-enabled').value = '1';
    elements.get('modal-dsp-active').value = 'true';
    const draft = state('validatedRouteDraft()');
    assert.equal(draft.dsp.gain, 0);
    assert.equal(draft.gain, 0);
    assert.equal(draft.dsp.chain[0].params.enabled, false);
    // Ordinary edits are draft-only. No POST runs until APPLY.
    run('fetchAPI = async () => { throw new Error("unexpected network call"); };');
    bindings[0].control.value = '0.25';
    bindings[0].control.listeners.change[0]();
    assert.equal(state('currentRouteDspState.chain[0].params.amount'), 0.25);
    assert.equal(descriptor.defaults.amount, 0);
    run('addProcessorToChain(); moveProcessor(1, -1);');
    assert.equal(state('currentRouteDspState.chain[1].params.amount'), 0.25);
    run('removeProcessor(0);');
    assert.equal(state('currentRouteDspState.chain.length'), 1);
    run('closeModal();');
    assert.equal(state('currentRouteDspState'), null);
    assert.equal(elements.get('route-modal').style.display, 'none');

    run('editingRouteId = 1; currentRouteDspState = {active:true,gain:0,chain:[{type:"missing_plugin",params:{x:1}}]}; renderDspChain();');
    assert.throws(() => run('validatedRouteDraft()'), /unavailable/);
    run('removeProcessor(0);');
    assert.equal(state('currentRouteDspState.chain.length'), 0);
    // Server rejection must not close the inspector or silently appear successful.
    run('fetchAPI = async () => { throw new Error("server rejected draft"); };');
    await run('saveRoute()');
    assert.equal(state('editingRouteId'), 1);
    assert.match(elements.get('route-error').textContent, /server rejected draft/);
    assert.equal(state('modalBusy'), false);
    // A successful APPLY is one whole-state request, confirmed before closing.
    run('fetchAPI = async () => discoveryResponse;');
    await run('loadDspAvailable()');
    context.requests = [];
    run('loadGraph = async () => {}; fetchAPI = async (url, options) => { requests.push({url, options}); const body = JSON.parse(options.body); return {...body.dsp,status:"ok",adopted:true}; };');
    await run('saveRoute()');
    assert.equal(context.requests.length, 1);
    assert.equal(context.requests[0].url, '/api/routes/1/apply');
    assert.equal(state('editingRouteId'), null);
    const appliedBody = JSON.parse(context.requests[0].options.body);
    assert.equal(appliedBody.gain, 0);
    assert.equal(appliedBody.enabled, true);
    assert.equal(appliedBody.dsp.gain, 0);
    // Accepted-but-unconfirmed state stays open, with no success claim.
    run('editingRouteId = 1; currentRouteDspState = {active:false,gain:1,chain:[]}; fetchAPI = async () => ({status:"pending",adopted:false});');
    await run('saveRoute()');
    assert.equal(state('editingRouteId'), 1);
    assert.match(elements.get('route-error').textContent, /not confirm/);
    run('closeModal();');
    context.requests.length = 0;
    context.alerts = [];
    context.alert = value => context.alerts.push(value);
    run('fetchAPI = async (url, options) => { requests.push({url,options}); return {status:"ok",adopted:true,persisted:true}; };');
    await run('saveSettings()');
    assert.equal(context.requests.length, 1);
    assert.equal(context.requests[0].url, '/api/settings/save');
    assert.equal(context.requests[0].options.body, undefined);
    assert.deepEqual(context.alerts, ['Applied configuration saved']);
    run('fetchAPI = async () => ({status:"ok",adopted:true,persisted:false});');
    await run('saveSettings()');
    assert.match(context.alerts[1], /not confirmed/);
    // Original transport functions serialize mutations; failed calls do not
    // poison subsequent updates, and SAVE cannot overtake an earlier APPLY.
    run('fetchAPI = ' + source.match(/function fetchAPI\([\s\S]*?\n        }/)[0].replace('function fetchAPI', 'function'));
    context.calls = [];
    context.releaseFirst = null;
    context.fetch = async url => {
        context.calls.push(url);
        if (url === '/first') await new Promise(resolve => { context.releaseFirst = resolve; });
        return {ok:true,status:200,json:async () => ({status:'ok'})};
    };
    const first = run('fetchAPI("/first", {method:"POST"})');
    const second = run('fetchAPI("/second", {method:"POST"})');
    await Promise.resolve(); await Promise.resolve();
    assert.deepEqual(context.calls, ['/first']);
    context.releaseFirst();
    await first; await second;
    assert.deepEqual(context.calls, ['/first', '/second']);
    // Discovery failure disables both adding and APPLY; no hardcoded fallback list.
    run('fetchAPI = async () => { throw new Error("discovery unavailable"); };');
    await run('loadDspAvailable()');
    assert.equal(state('dspDiscovery.status'), 'error');
    assert.equal(elements.get('dsp-add').disabled, true);
    assert.equal(elements.get('route-apply').disabled, true);
    assert.equal(elements.get('new-processor-type').children.length, 0);
    // A reconnect with the same identity must replace channel rows, not retain
    // the first width forever. Keep canvas position while rebuilding the rows.
    run('graph = {endpoints:[{id:"hardware/card/with/slash/playback",direction:"destination",channels:32}],routes:[]}; renderPatchbay();');
    assert.equal(run('endpointBlocks.get(graph.endpoints[0].id).children.length'), 33);
    run('nodePositions[graph.endpoints[0].id] = {x:50,y:90}; graph.endpoints[0].channels = 3; renderPatchbay();');
    assert.equal(run('endpointBlocks.get(graph.endpoints[0].id).children.length'), 4);
    assert.equal(run('endpointBlocks.get(graph.endpoints[0].id).style.transform'), 'translate(50px, 90px)');
    run('graph.endpoints[0].channels = 24; renderPatchbay();');
    assert.equal(run('endpointBlocks.get(graph.endpoints[0].id).children.length'), 25);
    context.deviceResponse = [
        {id:"card/quoted'",name:'<Card>',backend:'alsa',is_capture:true,active:false,capture_channels:12,playback_channels:24,max_channels:254},
        {id:"card/quoted'",name:'<Card>',backend:'alsa',is_capture:false,active:false,capture_channels:12,playback_channels:24,max_channels:254},
    ];
    run('fetchAPI = async () => deviceResponse;');
    await run('loadDevices()');
    const rows = elements.get('device-list').children.filter(child => child.className.startsWith('device-item'));
    assert.equal(rows.length, 1); // Paired directions grouped by exact opaque ID.
    assert.equal(rows[0].children[0].children[0].textContent, '<Card>');
    const hardwareInputs = rows[0].querySelectorAll('input');
    assert.deepEqual(hardwareInputs.map(input => input.value), ['12','24']);
    assert(hardwareInputs.every(input => Number(input.max) === 254 && !input.disabled));
    context.requests.length = 0;
    run('loadDevices = async () => {}; fetchAPI = async (url, options) => { requests.push({url,options}); return {status:"ok"}; };');
    await rows[0].children[1].onclick();
    assert.match(context.requests[0].url, /capture_channels=12/);
    assert.match(context.requests[0].url, /playback_channels=24/);
    console.log('PASS: plugin UI, drafts/apply/save, variable-width reconnect rows and hardware channel configuration');
})().catch(error => { console.error(error); process.exitCode = 1; });
