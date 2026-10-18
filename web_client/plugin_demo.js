/**
 * plugin_demo.js
 * 
 * FULL PROTOTYPE: Discrete Plugin Node & Precision Modal
 * Implements a sophisticated "Collapse to Route" workflow with 
 * dependency validation and route selection.
 */

(function() {
    // --- Configuration & State ---
    const DUMMY_PLUGIN_ID = 'dummy_plugin_1';
    const PLUGIN_NAME = 'Lush Reverb (Demo)';
    const IN_PORT_ID = `${DUMMY_PLUGIN_ID}_in-0`;
    const OUT_PORT_ID = `${DUMMY_PLUGIN_ID}_out-0`;
    let isModalOpen = false;
    let pluginBypassed = false;

    // --- CSS Injection ---
    const style = document.createElement('style');
    style.textContent = `
        .plugin-node {
            position: absolute;
            background: var(--panel-bg);
            border: 2px solid var(--border-color);
            border-radius: 12px;
            padding: 15px;
            width: 180px;
            box-shadow: 0 8px 25px rgba(0,0,0,0.6);
            cursor: default;
            user-select: none;
            z-index: 10;
            display: flex;
            flex-direction: column;
            gap: 12px;
            transition: border-color 0.3s, box-shadow 0.3s, opacity 0.3s;
        }
        .plugin-node.active-plugin { 
            border-color: var(--accent-blue); 
            box-shadow: 0 0 15px rgba(136, 192, 208, 0.4), 0 8px 25px rgba(0,0,0,0.6);
        }
        .plugin-node.bypassed-plugin { 
            border-color: #4c566a; 
            opacity: 0.6; 
            filter: grayscale(0.5);
        }
        .plugin-header {
            font-weight: bold;
            text-align: center;
            color: var(--text-dim);
            font-size: 12px;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 8px;
            display: flex;
            justify-content: space-between;
            align-items: center;
            cursor: grab;
        }
        .plugin-header:active { cursor: grabbing; }
        .plugin-gear {
            cursor: pointer;
            font-size: 14px;
            transition: transform 0.2s;
        }
        .plugin-gear:hover { transform: rotate(45deg); color: var(--accent-blue); }
        .plugin-io-container {
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 10px;
        }
        .plugin-vu-stack {
            display: flex;
            flex-direction: column;
            gap: 8px;
            width: 70px;
            padding: 8px;
            background: rgba(0,0,0,0.4);
            border-radius: 6px;
            border: 1px solid var(--border-color);
        }
        .vu-row {
            display: flex;
            flex-direction: column;
            align-items: center;
            gap: 2px;
        }
        .vu-label {
            font-size: 9px;
            font-weight: bold;
            color: var(--text-dim);
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        .plugin-vu-bar {
            height: 6px;
            width: 100%;
            background: #000;
            border-radius: 1px;
            position: relative;
            overflow: hidden;
        }
        .plugin-vu-fill {
            height: 100%;
            width: 0%;
            background: linear-gradient(90deg, var(--accent-green) 70%, var(--accent-yellow) 85%, var(--accent-red) 100%);
            transition: width 0.1s;
        }
        #plugin-precision-modal {
            position: fixed;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            background: var(--panel-bg);
            border: 1px solid var(--accent-blue);
            padding: 25px;
            border-radius: 12px;
            z-index: 2000;
            display: none;
            width: 480px;
            box-shadow: 0 0 80px rgba(0,0,0,0.9);
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        }
        .precision-header {
            font-size: 18px;
            font-weight: bold;
            color: var(--accent-blue);
            margin-bottom: 20px;
            border-bottom: 1px solid var(--border-color);
            padding-bottom: 10px;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .precision-param-row {
            display: flex;
            flex-direction: column;
            gap: 8px;
            margin-bottom: 20px;
        }
        .precision-label-row {
            display: flex;
            justify-content: space-between;
            color: var(--text-dim);
            font-size: 13px;
        }
        .precision-control-group {
            display: flex;
            align-items: center;
            gap: 15px;
        }
        .precision-slider {
            flex: 1;
            cursor: pointer;
        }
        .precision-input {
            background: var(--bg-color);
            border: 1px solid var(--border-color);
            color: var(--text-main);
            padding: 4px 8px;
            border-radius: 4px;
            width: 70px;
            text-align: right;
            font-family: monospace;
        }
        .precision-footer {
            display: flex;
            justify-content: flex-end;
            gap: 10px;
            margin-top: 25px;
        }
        .bypass-toggle-container {
            display: flex;
            align-items: center;
            justify-content: space-between;
            background: rgba(0,0,0,0.2);
            padding: 10px;
            border-radius: 6px;
            margin-bottom: 20px;
            border: 1px solid var(--border-color);
        }
        .collapse-section {
            margin-top: 25px;
            padding-top: 15px;
            border-top: 1px dashed var(--border-color);
            display: flex;
            flex-direction: column;
            gap: 10px;
        }
        .collapse-warning {
            font-size: 11px;
            color: var(--accent-yellow);
            text-align: center;
            margin-bottom: 5px;
        }
    `;
    document.head.appendChild(style);

    const $ = id => document.getElementById(id);

    function createPrecisionModal() {
        const modal = document.createElement('div');
        modal.id = 'plugin-precision-modal';
        modal.innerHTML = `
            <div class="precision-header">
                <span>${PLUGIN_NAME} Studio</span>
                <span style="cursor:pointer" onclick="PluginDemo.closeModal()">✕</span>
            </div>
            <div class="bypass-toggle-container">
                <span style="font-size: 13px; color: var(--text-dim);">Processing State</span>
                <select id="plugin-bypass-select" style="width: 100px;">
                    <option value="false">ACTIVE</option>
                    <option value="true">BYPASS</option>
                </select>
            </div>
            <div id="precision-params-container"></div>
            <div class="collapse-section">
                <div id="collapse-status" class="collapse-warning">Checking connectivity...</div>
                <div class="precision-control-group">
                    <select id="collapse-route-select" style="flex: 1;"></select>
                    <button id="btn-collapse-execute" class="primary" style="background: var(--accent-red); color: white;">COLLAPSE</button>
                </div>
            </div>
            <div class="precision-footer">
                <button onclick="PluginDemo.closeModal()">CANCEL</button>
                <button class="primary" onclick="PluginDemo.applyDummy()">APPLY TO ENGINE</button>
            </div>
        `;
        document.body.appendChild(modal);
        return modal;
    }

    const modalEl = createPrecisionModal();

    function createParamRow(label, min, max, step, defaultValue) {
        const row = document.createElement('div');
        row.className = 'precision-param-row';
        const labelRow = document.createElement('div');
        labelRow.className = 'precision-label-row';
        labelRow.innerHTML = `<span>${label}</span><span class="val-display">${defaultValue}</span>`;
        const controlGroup = document.createElement('div');
        controlGroup.className = 'precision-control-group';
        const slider = document.createElement('input');
        slider.type = 'range';
        slider.className = 'precision-slider';
        slider.min = min; slider.max = max; slider.step = step; slider.value = defaultValue;
        const input = document.createElement('input');
        input.type = 'number';
        input.className = 'precision-input';
        input.min = min; input.max = max; input.step = step; input.value = defaultValue;
        slider.oninput = () => {
            input.value = slider.value;
            labelRow.querySelector('.val-display').textContent = slider.value;
        };
        input.oninput = () => {
            slider.value = input.value;
            labelRow.querySelector('.val-display').textContent = input.value;
        };
        controlGroup.append(slider, input);
        row.append(labelRow, controlGroup);
        return row;
    }

    function createPluginNode() {
        const node = document.createElement('div');
        node.className = 'plugin-node active-plugin';
        node.id = `node-${DUMMY_PLUGIN_ID}`;
        node.dataset.epId = DUMMY_PLUGIN_ID;

        const header = document.createElement('div');
        header.className = 'plugin-header';
        header.innerHTML = `<span>${PLUGIN_NAME}</span>`;
        
        header.addEventListener('pointerdown', e => {
            if (e.target.classList.contains('plugin-gear')) return;
            node.dataset.dragging = 'true';
            node.dataset.startX = e.clientX;
            node.dataset.startY = e.clientY;
            const transform = new WebKitCSSMatrix(window.getComputedStyle(node).transform);
            node.dataset.initialX = transform.m41;
            node.dataset.initialY = transform.m42;
            node.setPointerCapture(e.pointerId);
            e.stopPropagation();
        });

        const gear = document.createElement('span');
        gear.className = 'plugin-gear';
        gear.title = 'Settings';
        gear.innerHTML = '⚙️';
        gear.onclick = () => {
            PluginDemo.openModal();
            modalEl.style.display = 'block';
            $('overlay').style.display = 'block';
            isModalOpen = true;
        };
        header.appendChild(gear);

        const ioContainer = document.createElement('div');
        ioContainer.className = 'plugin-io-container';

        const inPort = document.createElement('div');
        inPort.className = 'channel-row';
        inPort.id = `slot-${DUMMY_PLUGIN_ID}_in-0`;
        inPort.innerHTML = `<span class="ch-label">IN</span>`;
        
        const vuStack = document.createElement('div');
        vuStack.className = 'plugin-vu-stack';
        const preRow = document.createElement('div');
        preRow.className = 'vu-row';
        preRow.innerHTML = `<span class="vu-label">PRE</span><div class="plugin-vu-bar"><div class="plugin-vu-fill" id="vu-dummy-pre"></div></div>`;
        const postRow = document.createElement('div');
        postRow.className = 'vu-row';
        postRow.innerHTML = `<span class="vu-label">POST</span><div class="plugin-vu-bar"><div class="plugin-vu-fill" id="vu-dummy-post"></div></div>`;
        vuStack.append(preRow, postRow);

        const outPort = document.createElement('div');
        outPort.className = 'channel-row';
        outPort.id = `slot-${DUMMY_PLUGIN_ID}_out-0`;
        outPort.innerHTML = `<span class="ch-label" style="text-align:right">OUT</span>`;

        ioContainer.append(inPort, vuStack, outPort);
        node.append(header, ioContainer);

        const pos = window.nodePositions ? window.nodePositions[DUMMY_PLUGIN_ID] : { x: 600, y: 300 };
        node.style.transform = `translate(${pos.x}px, ${pos.y}px)`;
        return node;
    }

    window.PluginDemo = {
        spawnNode: function() {
            const viewport = $('patchbay-viewport');
            if (!viewport) return;
            const node = createPluginNode();
            viewport.appendChild(node);

            node.addEventListener('pointermove', e => {
                if (node.dataset.dragging !== 'true') return;
                const startX = parseFloat(node.dataset.startX);
                const startY = parseFloat(node.dataset.startY);
                const initialX = parseFloat(node.dataset.initialX);
                const initialY = parseFloat(node.dataset.initialY);
                const indicatorText = $('zoom-indicator')?.textContent || '100%';
                const scale = parseFloat(indicatorText) / 100 || 1.0;
                const dx = (e.clientX - startX) / scale;
                const dy = (e.clientY - startY) / scale;
                const newX = initialX + dx;
                const newY = initialY + dy;
                node.style.transform = `translate(${newX}px, ${newY}px)`;
                if (window.nodePositions) window.nodePositions[DUMMY_PLUGIN_ID] = { x: newX, y: newY };
                if (window.drawCables) window.drawCables();
            });

            node.addEventListener('pointerup', e => {
                node.dataset.dragging = 'false';
            });

            setInterval(() => {
                const pre = $('vu-dummy-pre');
                const post = $('vu-dummy-post');
                if (!pre || !post) return;
                const preVal = Math.random() * 60 + 20;
                pre.style.width = preVal + '%';
                if (pluginBypassed) {
                    post.style.width = preVal + '%';
                } else {
                    const postVal = Math.min(100, preVal * 1.2 + (Math.random() * 10 - 5));
                    post.style.width = postVal + '%';
                }
            }, 150);
        },
        openModal: function() {
            const container = $('precision-params-container');
            container.replaceChildren();
            container.appendChild(createParamRow('Room Size', 0, 100, 0.1, 45.5));
            container.appendChild(createParamRow('Decay Time', 0.1, 10, 0.01, 2.4));
            container.appendChild(createParamRow('Damping', 0, 1, 0.01, 0.7));
            container.appendChild(createParamRow('Mix', 0, 1, 0.01, 0.3));
            
            // Setup Bypass
            const bypassSelect = $('plugin-bypass-select');
            bypassSelect.value = pluginBypassed.toString();
            bypassSelect.onchange = (e) => {
                pluginBypassed = e.target.value === 'true';
                const node = $('node-dummy_plugin_1');
                if (node) {
                    node.classList.toggle('active-plugin', !pluginBypassed);
                    node.classList.toggle('bypassed-plugin', pluginBypassed);
                }
            };

            // Handle Collapse Logic
            this.updateCollapseOptions();

            modalEl.style.display = 'block';
            $('overlay').style.display = 'block';
            isModalOpen = true;
        },
        updateCollapseOptions: function() {
            const status = $('collapse-status');
            const select = $('collapse-route-select');
            const btn = $('btn-collapse-execute');
            
            if (!window.graph || !window.graph.routes) {
                status.textContent = 'Graph data unavailable';
                btn.disabled = true;
                return;
            }

            // Find all inputs to plugin and all outputs from plugin
            const inputs = window.graph.routes.filter(r => r.destination === IN_PORT_ID);
            const outputs = window.graph.routes.filter(r => r.source === OUT_PORT_ID);

            if (inputs.length === 0 || outputs.length === 0) {
                status.textContent = `Missing connections: ${inputs.length === 0 ? 'Inputs' : ''} ${outputs.length === 0 ? 'Outputs' : ''}`;
                select.replaceChildren();
                btn.disabled = true;
                return;
            }

            status.textContent = 'Valid connections found. Select collapse path:';
            btn.disabled = false;
            select.replaceChildren();

            // Generate all possible valid pairs (Combinations)
            // We use the endpoint names from graph.endpoints for readability
            const endpoints = window.graph.endpoints;
            
            inputs.forEach((inRoute, inIdx) => {
                outputs.forEach((outRoute, outIdx) => {
                    const srcName = endpoints.find(e => e.id === inRoute.source)?.name || inRoute.source;
                    const dstName = endpoints.find(e => e.id === outRoute.destination)?.name || outRoute.destination;
                    
                    const option = document.createElement('option');
                    option.value = `in${inIdx}_out${outIdx}`;
                    option.textContent = `${srcName} → ${dstName}`;
                    select.appendChild(option);
                });
            });

            btn.onclick = () => {
                if (confirm('Are you sure you want to collapse this plugin? This will remove the node and merge settings into the selected route.')) {
                    const selectedPath = select.value;
                    const [inIdx, outIdx] = selectedPath.split('_').map(s => parseInt(s.replace(/\D/g, '')));
                    const inRoute = inputs[inIdx];
                    const outRoute = outputs[outIdx];
                    
                    alert(`Simulating Collapse:\n\nRoute: ${inRoute.source} → ${outRoute.destination}\nEffect: Lush Reverb\nStatus: Merged into route settings.`);
                    
                    // Remove the dummy node from canvas
                    const node = $('node-dummy_plugin_1');
                    if (node) node.remove();
                    this.closeModal();
                }
            };
        },
        closeModal: function() {
            modalEl.style.display = 'none';
            $('overlay').style.display = 'none';
            isModalOpen = false;
        },
        applyDummy: function() {
            alert('DUMMY APPLY: Settings sent to engine (mocked)');
            this.closeModal();
        }
    };
})();
