'use strict';

var stationsModule = require('./stations');
var arrivalsModule = require('./arrivals');

var WORKER_BASE = 'https://nexttrainworker.sloccy.workers.dev';

var OP = { GET_STATIONS_VERSION: 1, GET_STATIONS_FULL: 2,
           GET_ARRIVALS: 3, REFRESH_STATIONS: 4 };
var DATA_TYPE = { STATIONS_VERSION: 1, STATIONS_CHUNK: 2, ARRIVALS: 3 };
var STATUS    = { OK: 0, OFFLINE: 1, NO_DATA: 2, ERROR: 3 };

// ─── In-flight serialization queue ────────────────────────────────────────────
// Phone serializes its own outbound sends via the success-callback chain.
// We also need to guard against overlapping XHR requests when the watch
// fires multiple OP_GET_ARRIVALS in quick succession (background prefetch).

var s_in_flight = false;
var s_queue     = [];

function enqueue(fn) {
  s_queue.push(fn);
  if (!s_in_flight) drain();
}

function drain() {
  if (s_queue.length === 0) { s_in_flight = false; return; }
  s_in_flight = true;
  var fn = s_queue.shift();
  fn();
}

// ─── Send helpers ──────────────────────────────────────────────────────────────

function sendStatus(queryIndex, code) {
  Pebble.sendAppMessage({ MSG_STATUS: code, MSG_QUERY_INDEX: queryIndex },
    function() {}, function(e) { console.warn('[pkjs] send failed: ' + JSON.stringify(e)); });
}

function sendDict(dict, cb) {
  Pebble.sendAppMessage(dict,
    function() { cb && cb(); },
    function(e) { console.warn('[pkjs] send failed: ' + JSON.stringify(e)); cb && cb(); });
}

// ─── Stations version ─────────────────────────────────────────────────────────

function handleGetStationsVersion() {
  enqueue(function() {
    stationsModule.load(WORKER_BASE, function(err, data) {
      if (err) { sendStatus(0, STATUS.OFFLINE); drain(); return; }
      sendDict({ MSG_DATA_TYPE: DATA_TYPE.STATIONS_VERSION,
                 MSG_STATIONS_VERSION: (data.g | 0) },
               drain);
    });
  });
}

// ─── Stations full sync ────────────────────────────────────────────────────────

var CHUNK_SIZE = 500; // bytes per AppMessage chunk

function handleGetStationsFull() {
  enqueue(function() {
    stationsModule.load(WORKER_BASE, function(err, data) {
      if (err) { sendStatus(0, STATUS.OFFLINE); drain(); return; }
      var blob = stationsModule.pack(data);
      if (!blob) { sendStatus(0, STATUS.ERROR); drain(); return; }

      var total  = Math.ceil(blob.byteLength / CHUNK_SIZE);
      var index  = 0;

      function sendChunk() {
        if (index >= total) { drain(); return; }
        var start   = index * CHUNK_SIZE;
        var end     = Math.min(start + CHUNK_SIZE, blob.byteLength);
        var slice   = blob.slice(start, end);
        var i       = index;
        index++;
        sendDict({
          MSG_DATA_TYPE:   DATA_TYPE.STATIONS_CHUNK,
          MSG_CHUNK_INDEX: i,
          MSG_CHUNK_TOTAL: total,
          MSG_PAYLOAD:     slice,
        }, sendChunk);
      }
      sendChunk();
    });
  });
}

// ─── Arrivals ─────────────────────────────────────────────────────────────────

function handleGetArrivals(queryIndex, stationSlug, routesStr) {
  enqueue(function() {
    stationsModule.load(WORKER_BASE, function(err, stationsData) {
      if (err || !stationsData) { sendStatus(queryIndex, STATUS.OFFLINE); drain(); return; }

      var station = null;
      (stationsData.s || []).forEach(function(s) {
        if (s.k === stationSlug) station = s;
      });
      if (!station) { sendStatus(queryIndex, STATUS.NO_DATA); drain(); return; }

      var url = WORKER_BASE + '/arrivals'
              + '?station=' + encodeURIComponent(stationSlug)
              + '&routes='  + encodeURIComponent(routesStr || '');

      var xhr = new XMLHttpRequest();
      xhr.open('GET', url, true);
      xhr.timeout = 8000;

      xhr.onload = function() {
        if (xhr.status === 503) { sendStatus(queryIndex, STATUS.NO_DATA); drain(); return; }
        if (xhr.status < 200 || xhr.status >= 300) { sendStatus(queryIndex, STATUS.ERROR); drain(); return; }

        var body;
        try { body = JSON.parse(xhr.responseText); }
        catch(e) { sendStatus(queryIndex, STATUS.ERROR); drain(); return; }

        var routes = (routesStr || '').split(',');
        var buf = arrivalsModule.pack(body.a || [], station, routes);

        sendDict({
          MSG_DATA_TYPE:    DATA_TYPE.ARRIVALS,
          MSG_QUERY_INDEX:  queryIndex,
          MSG_STATION_NAME: station.n,
          MSG_NEXT_REFRESH: (body.n | 0),
          MSG_PAYLOAD:      buf,
        }, drain);
      };

      xhr.onerror   = function() { sendStatus(queryIndex, STATUS.OFFLINE); drain(); };
      xhr.ontimeout = function() { sendStatus(queryIndex, STATUS.OFFLINE); drain(); };
      xhr.send();
    });
  });
}

// ─── Refresh stations ─────────────────────────────────────────────────────────

function handleRefreshStations() {
  enqueue(function() {
    stationsModule.invalidate();
    stationsModule.load(WORKER_BASE, function(err) {
      if (err) { sendStatus(0, STATUS.OFFLINE); drain(); return; }
      sendDict({ MSG_DATA_TYPE: DATA_TYPE.STATIONS_VERSION,
                 MSG_STATIONS_VERSION: 0 }, // force watch re-sync
               drain);
    });
  });
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

Pebble.addEventListener('ready', function() {
  console.log('[pkjs] ready');
  // Pre-warm the /stations cache so first watch request is fast
  stationsModule.load(WORKER_BASE, function(err) {
    if (err) console.warn('[pkjs] stations pre-warm failed: ' + err.message);
    else console.log('[pkjs] stations cached');
  });
});

Pebble.addEventListener('appmessage', function(e) {
  var op           = e.payload.MSG_OP;
  var queryIndex   = e.payload.MSG_QUERY_INDEX !== undefined ? e.payload.MSG_QUERY_INDEX : 0;
  var stationSlug  = e.payload.MSG_QUERY_STATION || '';
  var routesStr    = e.payload.MSG_QUERY_ROUTES  || '';

  console.log('[pkjs] op=' + op + ' idx=' + queryIndex + ' sta=' + stationSlug);

  switch (op) {
    case OP.GET_STATIONS_VERSION: handleGetStationsVersion();                          break;
    case OP.GET_STATIONS_FULL:    handleGetStationsFull();                             break;
    case OP.GET_ARRIVALS:         handleGetArrivals(queryIndex, stationSlug, routesStr); break;
    case OP.REFRESH_STATIONS:     handleRefreshStations();                             break;
    default: console.warn('[pkjs] unknown op: ' + op);
  }
});
