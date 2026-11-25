/* =========================================================
   CO2 Gadget – Charts page
   ========================================================= */

/**
 * Converts a timestamp (ms) to a 'yyyy-MM-ddTHH:mm' string in the browser's local time.
 * Used to populate the datetime-local inputs when the user enables the filter.
 */
function tsToDatetimeLocal(ts) {
    const d = new Date(ts);
    const pad = n => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
        'T' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}

/** Downloads a text file in the browser */
function downloadText(filename, content) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([content], { type: 'text/plain' }));
    a.download = filename;
    a.click();
    URL.revokeObjectURL(a.href);
}

/**
 * Builds (timestamp, value) points from the API response.
 *
 * The ESP32 uses millis() (uptime since boot), NOT wall-clock time (epoch).
 * Therefore data.lastTimestamp is the uptime of the first point, not
 * directly convertible to wall-clock time.
 *
 * Correct strategy:
 *   - The point at index `data.end` (the most recent) was added to the buffer
 *     approximately when this HTTP response was received.
 *   - We assume: wall_time(data.end) ≈ Date.now()
 *   - Each preceding point is one intervalDuration earlier:
 *     wall_time(i) = Date.now() + (i - data.end) * intervalDuration
 *
 * If the user configured a timezone different from the browser's in
 * preferences, we apply the extra offset (tzExtraMs) so that
 * Highcharts (useUTC:false) displays the saved timezone's time.
 *
 * The buffer stores averages of movingAverageInterval seconds (default
 * 60 s). Each point is already a 1-minute average bucket. With 1440 points
 * (default capacity = 24 h) Highcharts renders smoothly and no additional
 * downsampling is needed in the browser.
 */
function buildPoints(data) {
    const values = data.data;
    const interval = data.intervalDuration;  // ms between consecutive points
    const end = data.end;                     // index of the most recent point

    // The most recent point was added approximately when the response was received.
    // Highcharts with useUTC:false already applies the browser's timezone when displaying.
    const newestTs = Date.now();

    return values.map((v, i) => [newestTs + (i - end) * interval, v]);
}

/** Filters points by timestamp range (both bounds inclusive) */
function filterPoints(points, fromTs, toTs) {
    return points.filter(([ts]) => ts >= fromTs && ts <= toTs);
}

const CO2_ZONES = [
    { from: 0,    to: 450,   color: 'rgba(56, 189, 248, 0.16)', swatch: '#38bdf8', label: '< 450 ppm: Outdoor' },
    { from: 450,  to: 800,   color: 'rgba(34, 197, 94, 0.16)',  swatch: '#22c55e', label: '450\u2013800 ppm: Fresh (Recommended)' },
    { from: 800,  to: 1000,  color: 'rgba(234, 179, 8, 0.18)',  swatch: '#eab308', label: '800\u20131000 ppm: Acceptable (Upper limit)' },
    { from: 1000, to: 1500,  color: 'rgba(249, 115, 22, 0.20)', swatch: '#f97316', label: '1000\u20131500 ppm: Poor / Stuffy' },
    { from: 1500, to: 10000, color: 'rgba(239, 68, 68, 0.20)',  swatch: '#ef4444', label: '> 1500 ppm: Bad' }
];

/** Injects the control panel above the chart container */
function injectControls() {
    const container = document.getElementById('container');
    if (!container || document.getElementById('charts-controls')) return;

    const panel = document.createElement('div');
    panel.id = 'charts-controls';
    panel.innerHTML = `
        <div class="charts-filter-group">
            <label class="charts-filter-toggle" title="Enable date range filter">
                <input type="checkbox" id="charts-filter-enabled">
                <span>Date range:</span>
            </label>
            <div id="charts-range-inputs" class="charts-range-inputs charts-range-disabled">
                <input type="datetime-local" id="charts-from" title="From">
                <span class="charts-range-sep">→</span>
                <input type="datetime-local" id="charts-to" title="To">
                <button id="charts-apply" class="btn-charts">Apply</button>
            </div>
        </div>
        <div class="charts-export-btns">
            <span class="charts-export-label">Export:</span>
            <button id="charts-csv"  class="btn-charts" title="Download data as CSV">CSV</button>
            <button id="charts-json" class="btn-charts" title="Download data as JSON">JSON</button>
            <button id="charts-png"  class="btn-charts" title="Download chart as PNG image">PNG</button>
            <button id="charts-svg"  class="btn-charts" title="Download chart as SVG vector">SVG</button>
        </div>
    `;

    const style = document.createElement('style');
    style.textContent = `
        #charts-controls {
            display: flex;
            flex-wrap: wrap;
            align-items: center;
            gap: 8px 16px;
            margin-bottom: 14px;
            padding: 10px 16px;
            border-radius: var(--r-md, 10px);
            background: var(--surface-2, #f5f5f7);
            border: 1px solid var(--border, #d1d1d6);
            font-size: .85rem;
        }
        /* Filter group: checkbox + date pickers always visually together */
        .charts-filter-group {
            display: flex;
            align-items: center;
            flex-wrap: wrap;
            gap: 8px;
            flex: 1;
            min-width: 0;
        }
        .charts-filter-toggle {
            display: flex;
            align-items: center;
            gap: 7px;
            cursor: pointer;
            user-select: none;
            white-space: nowrap;
        }
        .charts-filter-toggle input[type=checkbox] {
            accent-color: var(--accent, #007aff);
            width: 15px; height: 15px;
            cursor: pointer;
        }
        .charts-range-inputs {
            display: flex;
            flex-wrap: wrap;
            align-items: center;
            gap: 6px;
            transition: opacity .2s;
        }
        .charts-range-inputs input[type=datetime-local] {
            font-size: .82rem;
            padding: 3px 6px;
            border: 1px solid var(--border, #d1d1d6);
            border-radius: var(--r-sm, 6px);
            background: var(--surface, #fff);
            color: var(--text-1, #333);
        }
        .charts-range-sep {
            color: var(--text-3, #aaa);
            user-select: none;
        }
        .charts-range-disabled {
            opacity: 0.38;
            pointer-events: none;
        }
        /* Export buttons group */
        .charts-export-btns {
            display: flex;
            align-items: center;
            gap: 6px;
            flex-shrink: 0;
            border-left: 1px solid var(--border, #d1d1d6);
            padding-left: 16px;
        }
        .charts-export-label {
            font-size: .78rem;
            color: var(--text-3, #aaa);
            white-space: nowrap;
            user-select: none;
        }
        .btn-charts {
            padding: 4px 11px;
            cursor: pointer;
            border: 1px solid var(--border, #d1d1d6);
            border-radius: var(--r-sm, 6px);
            background: var(--surface, #fff);
            color: var(--text-1, #333);
            font-size: .82rem;
            transition: background .15s;
        }
        .btn-charts:hover { background: var(--surface-3, #ebebed); }
        @media (max-width: 600px) {
            .charts-export-btns {
                border-left: none;
                padding-left: 0;
                border-top: 1px solid var(--border, #d1d1d6);
                padding-top: 8px;
                width: 100%;
            }
        }
        .charts-band-legend {
            display: flex;
            flex-wrap: wrap;
            justify-content: center;
            gap: 8px 16px;
            margin-top: 10px;
            padding: 8px 12px;
            border-radius: var(--r-md, 10px);
            background: var(--surface-2, #f5f5f7);
            border: 1px solid var(--border, #d1d1d6);
            font-size: .8rem;
            color: var(--font-color, #333);
        }
        .charts-band-item {
            display: inline-flex;
            align-items: center;
            gap: 6px;
            white-space: nowrap;
        }
        .charts-band-swatch {
            width: 12px;
            height: 12px;
            border-radius: 3px;
            display: inline-block;
            flex-shrink: 0;
        }
        .co2-reference-wrap {
            margin-top: 16px;
            padding: 12px 16px;
            border-radius: var(--r-md, 10px);
            background: var(--surface-2, #f5f5f7);
            border: 1px solid var(--border, #d1d1d6);
        }
        .co2-ref-title {
            margin: 0 0 10px 0;
            font-size: .95rem;
            color: var(--title-color, #222);
        }
        .co2-ref-table {
            width: 100%;
            border-collapse: collapse;
            font-size: .84rem;
            color: var(--font-color, #333);
        }
        .co2-ref-table th, .co2-ref-table td {
            padding: 8px 10px;
            text-align: left;
            border-bottom: 1px solid var(--border, #d1d1d6);
        }
        .co2-ref-table th {
            font-weight: 600;
            color: var(--title-color, #222);
        }
        .co2-ref-table tbody tr:last-child td { border-bottom: none; }
        .co2-row-outdoor    { background-color: rgba(56, 189, 248, 0.16); }
        .co2-row-fresh      { background-color: rgba(34, 197, 94, 0.16); }
        .co2-row-acceptable { background-color: rgba(234, 179, 8, 0.18); }
        .co2-row-poor       { background-color: rgba(249, 115, 22, 0.20); }
        .co2-row-bad        { background-color: rgba(239, 68, 68, 0.20); }
    `;
    document.head.appendChild(style);
    container.parentNode.insertBefore(panel, container);

    const legendEl = document.getElementById('charts-band-legend');
    if (legendEl) {
        legendEl.innerHTML = CO2_ZONES.map(z =>
            `<span class="charts-band-item"><span class="charts-band-swatch" style="background:${z.swatch}"></span>${z.label}</span>`
        ).join('');
    }
}

/**
 * Creates/updates the Highcharts chart and registers all control logic.
 */
function CreateChart() {
    if (typeof Highcharts !== 'undefined') {
        Highcharts.setOptions({ time: { useUTC: false } });
    }

    injectControls();

    let allPoints = [];
    let chart = null;

    const cs = prop => getComputedStyle(document.documentElement).getPropertyValue(prop).trim();

    function buildOptions(points) {
        return {
            chart: {
                type: 'line',
                backgroundColor: cs('--bg-color'),
                reflow: true,
                animation: false
            },
            title: {
                text: 'CO\u2082 \u2013 Recent Measurements',
                style: { color: cs('--title-color') }
            },
            xAxis: {
                type: 'datetime',
                labels: { format: '{value:%d/%m %H:%M}', style: { color: cs('--font-color') } }
            },
            yAxis: {
                title: { text: 'CO\u2082 (ppm)', style: { color: cs('--font-color') } },
                labels: { style: { color: cs('--font-color') } },
                plotBands: CO2_ZONES.map(({ from, to, color }) => ({ from, to, color }))
            },
            series: [{
                name: 'CO\u2082',
                data: points,
                color: cs('--title-color'),
                marker: { enabled: points.length < 120 }
            }],
            tooltip: {
                xDateFormat: '%d/%m/%Y %H:%M:%S'
            },
            exporting: { enabled: false },
            legend: { itemStyle: { color: cs('--font-color') } },
            credits: { enabled: false },
            responsive: {
                rules: [{
                    condition: { maxWidth: 500 },
                    chartOptions: {
                        xAxis: { labels: { format: '{value:%H:%M}' } }
                    }
                }]
            }
        };
    }

    function renderChart(points) {
        if (chart) {
            chart.series[0].setData(points, true, false, false);
        } else {
            chart = Highcharts.chart('container', buildOptions(points));
        }
    }

    function isFilterEnabled() {
        const cb = document.getElementById('charts-filter-enabled');
        return cb && cb.checked;
    }

    function getRange() {
        const fromInput = document.getElementById('charts-from');
        const toInput   = document.getElementById('charts-to');
        const fromTs = fromInput && fromInput.value ? new Date(fromInput.value).getTime() : -Infinity;
        const toTs   = toInput   && toInput.value   ? new Date(toInput.value).getTime()   :  Infinity;
        return { fromTs, toTs };
    }

    function applyFilter() {
        if (!allPoints.length) return;
        if (!isFilterEnabled()) {
            renderChart(allPoints);
            return;
        }
        const { fromTs, toTs } = getRange();
        const pts = filterPoints(allPoints, fromTs, toTs);
        renderChart(pts.length ? pts : allPoints);
    }

    function populateRangeInputs() {
        if (!allPoints.length) return;
        const fromInput = document.getElementById('charts-from');
        const toInput   = document.getElementById('charts-to');
        if (fromInput) fromInput.value = tsToDatetimeLocal(allPoints[0][0]);
        if (toInput)   toInput.value   = tsToDatetimeLocal(allPoints[allPoints.length - 1][0]);
    }

    // Checkbox: enables/disables the range filter
    const filterCb = document.getElementById('charts-filter-enabled');
    const rangeDiv  = document.getElementById('charts-range-inputs');
    if (filterCb) {
        filterCb.addEventListener('change', () => {
            if (filterCb.checked) {
                rangeDiv.classList.remove('charts-range-disabled');
                populateRangeInputs();
            } else {
                rangeDiv.classList.add('charts-range-disabled');
                renderChart(allPoints);
            }
        });
    }

    document.getElementById('charts-apply').addEventListener('click', applyFilter);

    function exportCSV() {
        const { fromTs, toTs } = getRange();
        const pts = isFilterEnabled() ? filterPoints(allPoints, fromTs, toTs) : allPoints;
        const rows = [['Timestamp', 'CO2_ppm']].concat(
            pts.map(([ts, v]) => [new Date(ts).toISOString(), v])
        );
        downloadText('co2_data.csv', rows.map(r => r.join(',')).join('\n'));
    }

    function exportJSON() {
        const { fromTs, toTs } = getRange();
        const pts = isFilterEnabled() ? filterPoints(allPoints, fromTs, toTs) : allPoints;
        downloadText('co2_data.json', JSON.stringify(
            pts.map(([ts, v]) => ({ timestamp: new Date(ts).toISOString(), co2_ppm: v })),
            null, 2
        ));
    }

    document.getElementById('charts-csv').addEventListener('click', exportCSV);
    document.getElementById('charts-json').addEventListener('click', exportJSON);
    document.getElementById('charts-png').addEventListener('click', () => {
        if (chart) chart.exportChart({ type: 'image/png', filename: 'co2_data' });
    });
    document.getElementById('charts-svg').addEventListener('click', () => {
        if (chart) chart.exportChart({ type: 'image/svg+xml', filename: 'co2_data' });
    });

    // Update colors when theme changes
    document.addEventListener('themeChange', () => {
        if (!chart) return;
        chart.update({
            chart: { backgroundColor: cs('--bg-color') },
            title: { style: { color: cs('--title-color') } },
            xAxis: { labels: { style: { color: cs('--font-color') } } },
            yAxis: {
                title: { style: { color: cs('--font-color') } },
                labels: { style: { color: cs('--font-color') } }
            },
            series: [{ color: cs('--title-color') }],
            legend: { itemStyle: { color: cs('--font-color') } }
        }, true, false, false);
    });

    window.addEventListener('resize', () => { if (chart) chart.reflow(); });

    // Initial load and automatic refresh every 60 s.
    // If the filter is disabled (default), all points are always shown.
    // If the filter is active, the inputs are already set by the user -> do not overwrite them.
    function fetchAndRender() {
        fetchWithTimeout('/circularBufferData', {}, 10000)
            .then(r => {
                if (!r.ok) throw new Error('Response not OK: ' + r.status);
                return r.json();
            })
            .then(data => {
                allPoints = buildPoints(data);
                applyFilter();
            })
            .catch(err => console.error('Error fetching chart data:', err));
    }

    fetchAndRender();
    setInterval(fetchAndRender, 60000);
}

document.addEventListener('DOMContentLoaded', function () {
    if (window.location.href.includes('charts.html') || window.location.pathname === '/charts.html') {
        if (typeof highlightCurrentPage === 'function') highlightCurrentPage();
        CreateChart();
    }
});
